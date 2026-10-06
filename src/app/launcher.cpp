#include "app/launcher.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace app {

Launcher::Launcher(Config &cfg, SDL_Window *window) : cfg_(cfg), window_(window) {
    std::snprintf(path_buf_, sizeof path_buf_, "%s", cfg_.rom_path.c_str());
    std::snprintf(link_next_buf_, sizeof link_next_buf_, "%s", cfg_.link_next.c_str());
    check_rom();
}

void Launcher::check_rom() {
    checks_.clear();
    rom_ok_ = false;
    if (cfg_.rom_path.empty()) {
        rom_message_ = "Choose your " M2_ROMSET " ROM set (.zip or .7z).";
        return;
    }
    // Keep the path absolute, so the saved setting works from any directory.
    std::error_code ec;
    const auto abs = std::filesystem::absolute(cfg_.rom_path, ec);
    if (!ec && abs.string() != cfg_.rom_path && std::filesystem::exists(abs, ec)) {
        cfg_.rom_path = abs.lexically_normal().string();
        std::snprintf(path_buf_, sizeof path_buf_, "%s", cfg_.rom_path.c_str());
        cfg_.save();
    }
    try {
        checks_ = rt::check_rom_set(cfg_.rom_path);
        int good = 0;
        for (const auto &c : checks_) good += c.ok;
        rom_ok_ = good == int(checks_.size());
        rom_message_ = rom_ok_ ? "All " + std::to_string(good) + " files verified."
                               : std::to_string(int(checks_.size()) - good) + " of " + std::to_string(checks_.size()) +
                                     " files missing or wrong: this is not the " M2_ROMSET " set.";
    } catch (const std::exception &e) {
        rom_message_ = e.what();
    }
}

void SDLCALL Launcher::dialog_done(void *self, const char *const *files, int) {
    auto *l = static_cast<Launcher *>(self);
    std::lock_guard<std::mutex> g(l->dialog_mutex_);
    l->dialog_pending_ = false;
    if (files && files[0]) l->dialog_result_ = files[0];
    else if (!files) l->error_ = std::string("File dialog unavailable (") + SDL_GetError() + "); type the path instead.";
}

void Launcher::browse() {
    static const SDL_DialogFileFilter filters[] = {{"ROM set (zip, 7z)", "zip;7z"}, {"All files", "*"}};
    dialog_pending_ = true;
    SDL_ShowOpenFileDialog(dialog_done, this, window_, filters, 2, cfg_.rom_path.empty() ? nullptr : cfg_.rom_path.c_str(), false);
}

void Launcher::start_capture(int action, CaptureKind kind, const Devices &devices) {
    capture_action_ = action;
    capture_kind_ = kind;
    joy_tracking_ = false;
    joy_rest_.clear();
    if (kind == CaptureJoy)
        for (SDL_Joystick *j : devices.joys)
            for (int i = 0; i < SDL_GetNumJoystickAxes(j); i++) joy_rest_[{SDL_GetJoystickID(j), i}] = SDL_GetJoystickAxis(j, i);
}

bool Launcher::handle_event(const SDL_Event &e) {
    if (capture_action_ < 0) return false;
    Binding &b = cfg_.controls.bind[capture_action_];
    auto done = [&] {
        capture_action_ = -1;
        joy_tracking_ = false;
        cfg_.save();
    };
    auto guid_of = [](SDL_JoystickID id) {
        char g[64] = "";
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), g, sizeof g);
        return std::string(g);
    };
    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
            capture_action_ = -1; // cancel
            joy_tracking_ = false;
        } else if (capture_kind_ == CaptureKey) {
            b.key = (e.key.scancode == SDL_SCANCODE_BACKSPACE || e.key.scancode == SDL_SCANCODE_DELETE) ? SDL_SCANCODE_UNKNOWN
                                                                                                         : e.key.scancode;
            done();
        }
        return true;
    }
    if (capture_kind_ == CapturePad && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        b.pad.kind = PadInput::Button;
        b.pad.index = e.gbutton.button;
        done();
        return true;
    }
    if (capture_kind_ == CapturePad && e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && std::abs(int(e.gaxis.value)) > 20000) {
        b.pad.kind = PadInput::Axis;
        b.pad.index = e.gaxis.axis;
        b.pad.dir = e.gaxis.value > 0 ? 1 : -1;
        done();
        return true;
    }
    if (capture_kind_ == CaptureJoy && e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN && !joy_tracking_) {
        b.joy = JoyInput{};
        b.joy.kind = JoyInput::Button;
        b.joy.guid = guid_of(e.jbutton.which);
        b.joy.index = e.jbutton.button;
        done();
        return true;
    }
    if (capture_kind_ == CaptureJoy && e.type == SDL_EVENT_JOYSTICK_HAT_MOTION && !joy_tracking_) {
        const int v = e.jhat.value;
        if (v == SDL_HAT_UP || v == SDL_HAT_DOWN || v == SDL_HAT_LEFT || v == SDL_HAT_RIGHT) {
            b.joy = JoyInput{};
            b.joy.kind = JoyInput::Hat;
            b.joy.guid = guid_of(e.jhat.which);
            b.joy.index = e.jhat.hat;
            b.joy.mask = v;
            done();
        }
        return true;
    }
    if (capture_kind_ == CaptureJoy && e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION) {
        // An axis binds once it has moved well away from where it rested and
        // come back: its rest value and the furthest it went are its range.
        const auto key = std::make_pair(e.jaxis.which, int(e.jaxis.axis));
        const auto it = joy_rest_.find(key);
        const int rest = it != joy_rest_.end() ? it->second : 0, v = e.jaxis.value;
        if (!joy_tracking_) {
            if (std::abs(v - rest) > 12000) {
                joy_tracking_ = true;
                joy_id_ = e.jaxis.which, joy_axis_ = e.jaxis.axis, joy_from_ = rest, joy_extreme_ = v;
            }
        } else if (e.jaxis.which == joy_id_ && e.jaxis.axis == joy_axis_) {
            if (std::abs(v - joy_from_) > std::abs(joy_extreme_ - joy_from_)) joy_extreme_ = v;
            if (std::abs(v - joy_from_) * 10 < std::abs(joy_extreme_ - joy_from_) * 3) { // let go: back within 30%
                b.joy = JoyInput{};
                b.joy.kind = JoyInput::Axis;
                b.joy.guid = guid_of(joy_id_);
                b.joy.index = joy_axis_;
                b.joy.rest = joy_from_;
                b.joy.full = joy_extreme_;
                done();
            }
        }
        return true;
    }
    return e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP || e.type == SDL_EVENT_JOYSTICK_BUTTON_UP;
}

Launcher::Result Launcher::draw(bool game_running, const Devices &devices) {
    SDL_Gamepad *pad = devices.pad;
    {
        std::lock_guard<std::mutex> g(dialog_mutex_);
        if (!dialog_result_.empty()) {
            cfg_.rom_path = dialog_result_;
            dialog_result_.clear();
            std::snprintf(path_buf_, sizeof path_buf_, "%s", cfg_.rom_path.c_str());
            cfg_.save();
            check_rom();
        }
    }

    Result result = Stay;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowBgAlpha(game_running ? 0.85f : 1.0f);
    ImGui::Begin("Daytona USA", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextUnformatted("DAYTONA USA");
    ImGui::SameLine();
    ImGui::TextDisabled("  static recompilation, native runtime");
    ImGui::Separator();

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Game")) {
            ImGui::Spacing();
            ImGui::TextUnformatted("ROM set");
            ImGui::SetNextItemWidth(-200);
            if (ImGui::InputText("##rom", path_buf_, sizeof path_buf_, ImGuiInputTextFlags_EnterReturnsTrue)) {
                cfg_.rom_path = path_buf_;
                cfg_.save();
                check_rom();
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse...", ImVec2(90, 0)) && !dialog_pending_) browse();
            ImGui::SameLine();
            if (ImGui::Button("Check", ImVec2(90, 0))) {
                cfg_.rom_path = path_buf_;
                cfg_.save();
                check_rom();
            }
            ImGui::TextColored(rom_ok_ ? ImVec4(0.4f, 0.9f, 0.4f, 1) : ImVec4(1, 0.6f, 0.3f, 1), "%s", rom_message_.c_str());
            if (!checks_.empty() && ImGui::TreeNode("Files")) {
                if (ImGui::BeginTable("files", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    for (const auto &c : checks_) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(c.file.c_str());
                        ImGui::TableNextColumn();
                        if (c.ok) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "ok");
                        else ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%s", c.problem.c_str());
                    }
                    ImGui::EndTable();
                }
                ImGui::TreePop();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted("Display");
            static const char *apis[] = {"Automatic", "Vulkan", "Direct3D 12", "Metal"};
            static const char *api_ids[] = {"", "vulkan", "direct3d12", "metal"};
            int api = 0;
            for (int i = 0; i < 4; i++)
                if (cfg_.gpu == api_ids[i]) api = i;
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo("Graphics API (Restart Required)", &api, apis, 4)) {
                cfg_.gpu = api_ids[api];
                cfg_.save();
            }
            static const char *renderers[] = {"Software (exact)", "Hardware (Experimental)"};
            int rd = cfg_.renderer == "hardware" ? 1 : 0;
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo("Renderer", &rd, renderers, 2)) {
                cfg_.renderer = rd ? "hardware" : "software";
                cfg_.save();
            }
            ImGui::TextDisabled("Software draws the 3D on the CPU, as the arcade board. Hardware uses the GPU:\n"
                                "in development, close to but not yet pixel-exact.");
            if (ImGui::Checkbox("Fullscreen", &cfg_.fullscreen)) {
                SDL_SetWindowFullscreen(window_, cfg_.fullscreen);
                cfg_.save();
            }
            if (ImGui::Checkbox("Skip launcher", &cfg_.skip_launcher)) cfg_.save();
            ImGui::SameLine();
            ImGui::TextDisabled("(starts the game straight away; Esc opens this launcher)");
            static const char *draw_modes[] = {"Double buffered", "Single buffered", "Every third frame"};
            ImGui::SetNextItemWidth(200);
            int dm = std::clamp(cfg_.draw_mode, 0, 2);
            if (ImGui::Combo("Draw mode", &dm, draw_modes, 3)) {
                cfg_.draw_mode = dm;
                cfg_.save();
            }
            ImGui::TextDisabled("Double buffered draws every frame, as the game does. Single buffered draws\n"
                                "every 2nd frame, every third frame every 3rd: faster, the game itself is not slowed.");
            static const char *supersampling[] = {"Off", "2x", "3x", "4x"};
            ImGui::BeginDisabled(cfg_.renderer != "hardware");
            ImGui::SetNextItemWidth(200);
            int ss = std::clamp(cfg_.supersampling, 1, 4) - 1;
            if (ImGui::Combo("Super sampling", &ss, supersampling, 4)) {
                cfg_.supersampling = ss + 1;
                cfg_.save();
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled("Hardware renderer: the 3D drawn at 2 to 4 times the original resolution, with\n"
                                "sharper textures; the window shows it scaled to fit, which smooths the edges.");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted("Enhancements");
            static const char *aspects[] = {"Original (4:3)", "16:10", "16:9", "21:9"};
            static const char *aspect_ids[] = {"", "16:10", "16:9", "21:9"};
            int aspect = 0;
            for (int i = 0; i < 4; i++)
                if (cfg_.aspect == aspect_ids[i]) aspect = i;
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo("Widescreen", &aspect, aspects, 4)) {
                cfg_.aspect = aspect_ids[aspect];
                cfg_.save();
            }
            ImGui::BeginDisabled(cfg_.aspect.empty());
            if (ImGui::Checkbox("HUD at the screen edges (Experimental)", &cfg_.hud_edges)) cfg_.save();
            if (ImGui::Checkbox("Stretch tile background (Experimental)", &cfg_.stretch_backdrop)) cfg_.save();
            ImGui::EndDisabled();
            ImGui::TextDisabled("Shows more of the scene at the sides. The HUD stays 4:3 in the centre, or its\n"
                                "lap times, position and maps move out to the edges. In-game the sky at the\n"
                                "sides is plain blue, or the game's sky picture stretched across the screen.");
            static const char *distances[] = {"Shortest", "Shorter", "Default", "Further", "Furthest"};
            ImGui::SetNextItemWidth(200);
            int dd = std::clamp(cfg_.draw_distance, -2, 2);
            if (ImGui::SliderInt("Draw distance", &dd, -2, 2, distances[dd + 2], ImGuiSliderFlags_AlwaysClamp)) {
                cfg_.draw_distance = dd;
                cfg_.save();
            }
            ImGui::TextDisabled("Scenery around the course. Default is the game's own; shorter is faster,\n"
                                "further shows more trees and buildings ahead (not more road).");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted("Link play (Experimental, Reset Required)");
            if (ImGui::Checkbox("Link to other cabinets", &cfg_.link)) cfg_.save();
            ImGui::BeginDisabled(!cfg_.link);
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputInt("This cabinet's port", &cfg_.link_port, 0, 0)) {
                cfg_.link_port = std::clamp(cfg_.link_port, 1, 65535);
                cfg_.save();
            }
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputText("Next cabinet (host:port)", link_next_buf_, sizeof link_next_buf_)) {
                cfg_.link_next = link_next_buf_;
                cfg_.save();
            }
            if (ImGui::Checkbox("Frame sync (every cabinet waits for the master)", &cfg_.link_framesync)) cfg_.save();
            ImGui::EndDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 640);
            ImGui::TextWrapped("Link: %s", link_status_.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Cabinets link in a ring over the network (Wi-Fi or wired): each listens on its\n"
                                "port and connects to the next; with two, each one's next is the other. Every\n"
                                "computer can use the same port: e.g. 15112, and Next cabinet = the other\n"
                                "computer's address:15112. In test mode (F2) > GAME SYSTEM set LINK ID (one\n"
                                "MASTER, the others SLAVE) and a different CAR NUMBER on each. The 1994 set\n"
                                "(Revision A) has link play.");

            ImGui::Spacing();
            ImGui::Separator();
            if (!error_.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.4f, 1));
                ImGui::TextWrapped("%s", error_.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::BeginDisabled(!rom_ok_);
            if (game_running) {
                if (ImGui::Button("Resume", ImVec2(140, 40))) result = Resume;
                ImGui::SameLine();
                if (ImGui::Button("Reset", ImVec2(140, 40))) result = Reset;
            } else if (ImGui::Button("Start", ImVec2(140, 40))) {
                result = StartGame;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Quit", ImVec2(140, 40))) result = Quit;
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Controls")) {
            Controls &c = cfg_.controls;
            ImGui::Spacing();
            ImGui::Text("Gamepad: %s", pad ? SDL_GetGamepadName(pad) : "none connected");
            std::string joys;
            for (SDL_Joystick *j : devices.joys) {
                const char *n = SDL_GetJoystickName(j);
                joys += (joys.empty() ? "" : ", ") + std::string(n ? n : "joystick");
            }
            ImGui::TextWrapped("Wheels and joysticks: %s", joys.empty() ? "none connected" : joys.c_str());
            // live meters
            const bool *keys = SDL_GetKeyboardState(nullptr);
            const float steer = c.value(SteerRight, keys, devices) - c.value(SteerLeft, keys, devices);
            char label[32];
            std::snprintf(label, sizeof label, "%+.2f", c.steer_invert ? -steer : steer);
            ImGui::ProgressBar(0.5f + 0.5f * (c.steer_invert ? -steer : steer), ImVec2(200, 0), label);
            ImGui::SameLine();
            ImGui::TextUnformatted("Steering");
            const float acc = c.value(Accelerate, keys, devices), brk = c.value(Brake, keys, devices);
            ImGui::ProgressBar(acc, ImVec2(200, 0));
            ImGui::SameLine();
            ImGui::TextUnformatted("Accelerator");
            ImGui::ProgressBar(brk, ImVec2(200, 0));
            ImGui::SameLine();
            ImGui::TextUnformatted("Brake");
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderFloat("Dead zone", &c.deadzone, 0.0f, 0.4f, "%.2f")) cfg_.save();
            ImGui::SameLine();
            if (ImGui::Checkbox("Invert steering", &c.steer_invert)) cfg_.save();
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderFloat("Wheel dead zone (Experimental)", &c.joy_deadzone, 0.0f, 0.4f, "%.2f")) cfg_.save();
            ImGui::TextDisabled("Triggers, sticks, wheels and pedals are analogue. To bind a wheel or pedal axis,\n"
                                "click its button, then turn the wheel or press the pedal as far as you want full\n"
                                "lock or full travel to be, and let go: that sets its range.");
#ifndef _WIN32 // SDL's own Logitech driver is off on Windows already
            if (ImGui::Checkbox("Legacy Logitech wheel support (Restart Required)", &cfg_.legacy_logitech_wheels)) cfg_.save();
            ImGui::TextDisabled("For a Logitech wheel that is listed but does nothing when you bind it (the original\n"
                                "Driving Force). After restarting, bind the wheel's controls again.");
#endif

            ImGui::SetNextItemWidth(200);
            int ffb = int(cfg_.ffb_strength * 100.0f + 0.5f);
            if (ImGui::SliderInt("Force feedback (Experimental)", &ffb, 0, 100, ffb ? "%d%%" : "Off")) {
                cfg_.ffb_strength = float(ffb) / 100.0f;
                cfg_.save();
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("Invert force", &cfg_.ffb_invert)) cfg_.save();
            ImGui::SameLine();
            if (ImGui::Checkbox("Log force feedback", &cfg_.ffb_log)) cfg_.save();
            ImGui::TextDisabled("The arcade wheel's motor (centring, resistance, the wheel pulling), on the\n"
                                "device steering is bound to; a gamepad rumbles when the car is pushed. Now: %s.\n"
                                "Wheels and force feedback are untested on real hardware so far: reports welcome.\n"
                                "The log goes to daytona.log beside launcher.ini on Windows, else the terminal.", ffb_device_);

            if (ImGui::BeginTable("binds", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthFixed, 130);
                ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthFixed, 130);
                ImGui::TableSetupColumn("Gamepad", ImGuiTableColumnFlags_WidthFixed, 250);
                ImGui::TableSetupColumn("Wheel / joystick (Experimental)");
                ImGui::TableHeadersRow();
                for (int a = 0; a < kNumActions; a++) {
                    ImGui::PushID(a);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(action_name(Action(a)));
                    ImGui::TableNextColumn();
                    const bool cap_key = capture_action_ == a && capture_kind_ == CaptureKey;
                    const char *kn = c.bind[a].key == SDL_SCANCODE_UNKNOWN ? "-" : SDL_GetScancodeName(c.bind[a].key);
                    if (ImGui::Button(cap_key ? "Press a key..." : kn, ImVec2(120, 0))) start_capture(a, CaptureKey, devices);
                    ImGui::TableNextColumn();
                    const bool cap_pad = capture_action_ == a && capture_kind_ == CapturePad;
                    const std::string pn = cap_pad ? "Press or move..." : c.bind[a].pad.describe();
                    if (ImGui::Button(pn.c_str(), ImVec2(190, 0))) start_capture(a, CapturePad, devices);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("clear")) {
                        c.bind[a].pad = PadInput{};
                        cfg_.save();
                    }
                    ImGui::TableNextColumn();
                    const bool cap_joy = capture_action_ == a && capture_kind_ == CaptureJoy;
                    const std::string jn = !cap_joy ? c.bind[a].joy.describe(devices)
                                           : joy_tracking_ ? "As far as you want, then let go..."
                                                           : "Press, or move fully and let go...";
                    if (ImGui::Button(jn.c_str(), ImVec2(300, 0))) start_capture(a, CaptureJoy, devices);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("clear##joy")) {
                        c.bind[a].joy = JoyInput{};
                        cfg_.save();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Esc cancels a binding; Backspace clears a key.");
            if (ImGui::Button("Reset to defaults")) {
                c.set_defaults();
                cfg_.save();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
            ImGui::Spacing();
            auto percent = [&](const char *label, float &value) { // a 0..1 setting as 0-100%
                ImGui::SetNextItemWidth(200);
                int v = int(value * 100.0f + 0.5f);
                if (ImGui::SliderInt(label, &v, 0, 100, "%d%%")) {
                    value = float(v) / 100.0f;
                    cfg_.save();
                }
            };
            percent("Volume", cfg_.volume);
            ImGui::SameLine();
            if (ImGui::Checkbox("Mute", &cfg_.mute)) cfg_.save();
            ImGui::Spacing();
            percent("Music", cfg_.music_volume);
            percent("Effects", cfg_.effects_volume);
            ImGui::TextDisabled("The balance between the music and everything else (the engine, skids, crashes).\n"
                                "100%% and 100%% is the game as the arcade's sound board mixes it.");

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Checkbox("Native audio (Experimental, Reset Required)", &cfg_.native_audio)) cfg_.save();
            ImGui::TextDisabled("Shared native sequencer/mixer; reference audio remains available for comparison.");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::End();
    return result;
}

} // namespace app
