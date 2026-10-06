#define SDL_MAIN_HANDLED
#include "audio.h"
#include "native_audio.h"
#include "runtime/native_sound_engine.h"
#include "sound_worker.h"
#include "link.h"
#include "controls.h"
#include "diagnostic_log.h"
#include "async_log.h"
#include "gpu_fast.h"
#include "gpu_text.h"
#include "imgui_vita.h"
#include "runtime/game_loop.h"
#include "runtime/test_hold.h"
#include "runtime/rom_import.h"

#include <SDL.h>
#include <psp2/ctrl.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <vita2d.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>
#include <utility>

#ifndef DAYTONA_VITA_DIAGNOSTICS
#define DAYTONA_VITA_DIAGNOSTICS 0
#endif

extern "C" { unsigned int _newlib_heap_size_user = 192 * 1024 * 1024; }

namespace {
constexpr bool kDiagnostics = DAYTONA_VITA_DIAGNOSTICS != 0;
constexpr const char *kDirectory = "ux0:data/" M2_ROMSET;
constexpr const char *kRom = "ux0:data/" M2_ROMSET "/" M2_ROMSET ".zip";
constexpr const char *kConfig = "ux0:data/" M2_ROMSET "/vita.cfg";
constexpr const char *kConfigTemp = "ux0:data/" M2_ROMSET "/vita.cfg.tmp";

vita::Pad read_pad() {
    SceCtrlData native{};
    vita::Pad pad;
    if (sceCtrlPeekBufferPositive(0, &native, 1) <= 0) return pad;
    pad.lx = native.lx; pad.ry = native.ry;
    const struct { uint32_t native, portable; } map[] = {
        {SCE_CTRL_CROSS, vita::Cross}, {SCE_CTRL_CIRCLE, vita::Circle},
        {SCE_CTRL_SQUARE, vita::Square}, {SCE_CTRL_TRIANGLE, vita::Triangle},
        {SCE_CTRL_UP, vita::Up}, {SCE_CTRL_DOWN, vita::Down},
        {SCE_CTRL_LEFT, vita::Left}, {SCE_CTRL_RIGHT, vita::Right},
        {SCE_CTRL_LTRIGGER, vita::L}, {SCE_CTRL_RTRIGGER, vita::R},
        {SCE_CTRL_START, vita::Start}, {SCE_CTRL_SELECT, vita::Select}
    };
    for (auto e : map) if (native.buttons & e.native) pad.buttons |= e.portable;
    return pad;
}

bool load_bytes(const std::string &path, uint8_t *data, size_t size) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    const bool ok = std::fread(data, 1, size, f) == size && std::fgetc(f) == EOF;
    std::fclose(f);
    return ok;
}
bool save_bytes(const std::string &path, const uint8_t *data, size_t size) {
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data, 1, size, f) == size;
    if (std::fflush(f) != 0) ok = false;
    if (std::fclose(f) != 0) ok = false;
    if (!ok) { std::remove(tmp.c_str()); return false; }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

template<class C> void load_nv(const char *name, C &data) {
    load_bytes(std::string(kDirectory) + "/" + name, data.data(), data.size());
}

template<class C> bool save_nv(const char *name, const C &data) {
    return save_bytes(std::string(kDirectory) + "/" + name, data.data(), data.size());
}

rt::Inputs map_input(const vita::Input &in) {
    rt::Inputs out;
    out.steer = in.steer; out.accel = in.accel; out.brake = in.brake;
    out.in0 = in.in0; out.in1 = in.in1; out.in2 = in.in2;
    return out;
}

uint64_t ticks_us() {
    const uint64_t f = SDL_GetPerformanceFrequency();
    return f ? SDL_GetPerformanceCounter() * 1000000ull / f : 0;
}

// Measurement only; never use this clock to pace game or audio playback.
uint64_t diagnostic_ticks_us() {
    return kDiagnostics ? ticks_us() : 0;
}

struct VitaSettings {
    bool revision_a = std::strcmp(M2_ROMSET, "daytona") == 0;
    bool link_enabled = false, link_sync = false;
    int link_port = 15112, next_port = 15112;
    int next_ip[4] = {192, 168, 1, 2};
    std::string next_address() const {
        return std::to_string(next_ip[0]) + "." + std::to_string(next_ip[1]) + "." +
            std::to_string(next_ip[2]) + "." + std::to_string(next_ip[3]);
    }
    int cpu_clock = 333;
    int gpu_clock = 111;
    int volume = 80;
    int deadzone = 12;
    int aspect = 0, draw_distance = 0, steer_curve = 0;
    bool hud_edges = false;

    bool fourth_core = false;
    bool stretch_backdrop = false, skip_launcher = false;
    bool mute = false;
    bool native_audio = false; // explicitly selected while native fidelity is validated
    bool steer_invert = false;

    void defaults() { *this = VitaSettings{}; }
    void sanitize() {
        link_port = std::clamp(link_port, 1, 65535);
        next_port = std::clamp(next_port, 1, 65535);
        for (int &octet : next_ip) octet = std::clamp(octet, 0, 255);
        const auto valid = [](int value, const int *choices, int count, int fallback) {
            for (int i = 0; i < count; ++i) if (value == choices[i]) return value;
            return fallback;
        };
        static const int cpus[] = {111, 222, 333, 444, 500};
        static const int gpus[] = {41, 77, 111, 166};
        cpu_clock = valid(cpu_clock, cpus, 5, 333);
        gpu_clock = valid(gpu_clock, gpus, 4, 111);
        volume = std::clamp(volume, 0, 100);
        deadzone = std::clamp(deadzone, 0, 40);
        aspect = std::clamp(aspect, 0, 3);
        steer_curve = std::clamp(steer_curve, 0, 2);
        draw_distance = std::clamp(draw_distance, -2, 2);
    }
    void load() {
        FILE *f = std::fopen(kConfig, "r");
        if (!f) return;
        char line[96], key[40]; int value = 0;
        while (std::fgets(line, sizeof line, f)) {
            if (std::sscanf(line, "%39[^=]=%d", key, &value) != 2) continue;
            if (!std::strcmp(key, "fourth_core")) fourth_core = value != 0;
            else if (!std::strcmp(key, "link_enabled")) link_enabled = value != 0;
            else if (!std::strcmp(key, "link_sync")) link_sync = value != 0;
            else if (!std::strcmp(key, "link_port")) link_port = value;
            else if (!std::strcmp(key, "next_port")) next_port = value;
            else if (!std::strcmp(key, "next_ip0")) next_ip[0] = value;
            else if (!std::strcmp(key, "next_ip1")) next_ip[1] = value;
            else if (!std::strcmp(key, "next_ip2")) next_ip[2] = value;
            else if (!std::strcmp(key, "next_ip3")) next_ip[3] = value;
            else if (!std::strcmp(key, "stretch_backdrop")) stretch_backdrop = value != 0;
            else if (!std::strcmp(key, "skip_launcher")) skip_launcher = value != 0;
            else if (!std::strcmp(key, "steer_curve")) steer_curve = value;
            else if (!std::strcmp(key, "aspect")) aspect = value;
            else if (!std::strcmp(key, "draw_distance")) draw_distance = value;
            else if (!std::strcmp(key, "hud_edges")) hud_edges = value != 0;
            else if (!std::strcmp(key, "cpu_clock")) cpu_clock = value;
            else if (!std::strcmp(key, "gpu_clock")) gpu_clock = value;
            else if (!std::strcmp(key, "volume")) volume = value;
            else if (!std::strcmp(key, "mute")) mute = value != 0;
            else if (!std::strcmp(key, "native_audio")) native_audio = value != 0;
            else if (!std::strcmp(key, "deadzone")) deadzone = value;
            else if (!std::strcmp(key, "steer_invert")) steer_invert = value != 0;
        }
        std::fclose(f); sanitize();
    }
    bool save() const {
        FILE *f = std::fopen(kConfigTemp, "w");
        if (!f) return false;
        std::fprintf(f, "cpu_clock=%d\ngpu_clock=%d\nvolume=%d\nmute=%d\nnative_audio=%d\ndeadzone=%d\nsteer_invert=%d\n",
                     cpu_clock, gpu_clock, volume, int(mute), int(native_audio), deadzone, int(steer_invert));
        std::fprintf(f, "stretch_backdrop=%d\nskip_launcher=%d\n", int(stretch_backdrop), int(skip_launcher));
        std::fprintf(f, "fourth_core=%d\n", int(fourth_core));
        std::fprintf(f, "link_enabled=%d\nlink_sync=%d\nlink_port=%d\nnext_port=%d\n",
            int(link_enabled), int(link_sync), link_port, next_port);
        for (int i = 0; i < 4; ++i) std::fprintf(f, "next_ip%d=%d\n", i, next_ip[i]);
        std::fprintf(f, "steer_curve=%d\n", steer_curve);
        std::fprintf(f, "aspect=%d\ndraw_distance=%d\nhud_edges=%d\n", aspect, draw_distance, int(hud_edges));
        bool ok = std::fflush(f) == 0;
        if (std::fclose(f) != 0) ok = false;
        if (!ok) { std::remove(kConfigTemp); return false; }
        std::remove(kConfig);
        return std::rename(kConfigTemp, kConfig) == 0;
    }
};

int cycle_value(int value, const int *choices, int count, int direction) {
    int index = 0;
    for (int i = 0; i < count; ++i) if (choices[i] == value) index = i;
    return choices[(index + (direction < 0 ? count - 1 : 1)) % count];
}

uint32_t ui_buttons = 0;
rt::TestHold test_hold;
void draw_menu(bool have_game, bool &options, int &selection, const VitaSettings &settings,
               const std::string &status, double fps) {
    ImGui::SetNextWindowPos({12, 12});
    ImGui::SetNextWindowSize({936, 520});
    ImGui::Begin("Daytona USA", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    ImGui::Text("Daytona USA | %.1f FPS | CPU %d MHz | GPU %d MHz", fps,
                scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency());
    if (ImGui::Button("Game")) { options = false; selection = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Options")) { options = true; selection = 0; }
    ImGui::SameLine(); ImGui::TextDisabled("D-pad navigate | X select | O back");
    ImGui::Separator();
    static int previous_selection = -1;
    static bool previous_options = false;
    const bool scroll_to_selection = selection != previous_selection || options != previous_options;
    if (ImGui::BeginChild("settings", {0, -100}, ImGuiChildFlags_None)) {
        if (!options) {
            const char *labels[] = {have_game ? "Resume / launch selected ROM" : "Start game", "Reset game", "Options", "Quit"};
            for (int i = 0; i < 4; ++i) {
                if (ImGui::Selectable(labels[i], selection == i, 0, {0, 36})) { selection = i; ui_buttons = vita::Cross; }
                if (selection == i && scroll_to_selection) ImGui::SetScrollHereY();
            }
        } else {
    const char *values[29];
    char link_fields[6][64];
    for (int i = 0; i < 4; ++i) {
        std::snprintf(link_fields[i], sizeof link_fields[i], "NEXT CABINET IP OCTET %d: %d", i + 1, settings.next_ip[i]);
        values[19+i] = link_fields[i];
    }
    std::snprintf(link_fields[4], sizeof link_fields[4], "LISTEN PORT: %d", settings.link_port);
    std::snprintf(link_fields[5], sizeof link_fields[5], "NEXT CABINET PORT: %d", settings.next_port);
    values[23] = link_fields[4]; values[24] = link_fields[5];
    values[18] = settings.link_enabled ? "LINK PLAY: ON (REVISION A)" : "LINK PLAY: OFF";
    values[25] = settings.link_sync ? "LINK FRAME SYNC: ON" : "LINK FRAME SYNC: OFF";
    char cpu[64], gpu[32], volume[32], mute[32], deadzone[32], invert[32];
    std::snprintf(cpu, sizeof cpu, "CPU CLOCK: %d MHz (ACTUAL %d)", settings.cpu_clock,
                  scePowerGetArmClockFrequency());
    std::snprintf(gpu, sizeof gpu, "GPU CLOCK: %d MHz", settings.gpu_clock);
    std::snprintf(volume, sizeof volume, "VOLUME: %d%%", settings.volume);
    std::snprintf(mute, sizeof mute, "MUTE: %s", settings.mute ? "ON" : "OFF");
    std::snprintf(deadzone, sizeof deadzone, "DEAD ZONE: %d%%", settings.deadzone);
    std::snprintf(invert, sizeof invert, "INVERT STEERING: %s", settings.steer_invert ? "ON" : "OFF");
    values[0]=cpu; values[1]=gpu; values[2]=volume; values[3]=mute; values[4]=deadzone; values[5]=invert;
    values[6]=settings.native_audio ? "AUDIO ENGINE: NATIVE (TEST)" : "AUDIO ENGINE: REFERENCE";
    values[7]="GRAPHICS API: GXM"; values[8]="FULLSCREEN: ON";
    values[9]=settings.revision_a ? "ROM: DAYTONA (1994 REVISION A)" : "ROM: DAYTONA93 (1993)";
    values[10]="BINDS: SELECT+TRIANGLE TEST / SELECT+SQUARE SERVICE";
    static const char* aspects[] = {"ASPECT: ORIGINAL", "ASPECT: 16:10", "ASPECT: 16:9", "ASPECT: 21:9"};
    static const char* distances[] = {"DISTANCE: SHORTEST", "DISTANCE: SHORTER", "DISTANCE: DEFAULT", "DISTANCE: FURTHER", "DISTANCE: FURTHEST"};
    values[11]=aspects[settings.aspect]; values[12]=settings.hud_edges ? "HUD: SCREEN EDGES" : "HUD: CENTRED";
    values[13]=distances[settings.draw_distance + 2];
    values[26]=test_hold.armed() ? "HOLD TEST BUTTON: ARMED (START / RESUME)" : "HOLD TEST BUTTON: OFF";
    values[27]="RESET DEFAULTS"; values[28]="BACK";
    values[17]=!settings.fourth_core ? "4TH CORE: OFF" :
        vita::fourth_core_active() ? "4TH CORE: ENABLED" : "4TH CORE: UNAVAILABLE (PLUGIN REQUIRED)";
    values[15]=settings.stretch_backdrop ? "STRETCH TILE BACKGROUND: ON" : "STRETCH TILE BACKGROUND: OFF";
    values[16]=settings.skip_launcher ? "SKIP LAUNCHER: ON" : "SKIP LAUNCHER: OFF";
    static const char* curves[] = {"STEERING CURVE: LINEAR", "STEERING CURVE: SOFT", "STEERING CURVE: EXTRA SOFT"};
    values[14]=curves[settings.steer_curve];

            for (int i = 0; i < 29; ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(values[i], selection == i, 0, {760, 32})) { selection = i; ui_buttons = vita::Cross; }
                if (selection == i && scroll_to_selection) ImGui::SetScrollHereY();
                ImGui::SameLine();
                if (ImGui::SmallButton("<")) { selection = i; ui_buttons = vita::Left; }
                ImGui::SameLine();
                if (ImGui::SmallButton(">")) { selection = i; ui_buttons = vita::Right; }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    previous_selection = selection; previous_options = options;
    ImGui::Separator();
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::TextDisabled("START+SELECT menu | SELECT+TRIANGLE test | SELECT+SQUARE service");
    ImGui::End();
}

} // namespace

int main(int, char **) {
    sceIoMkdir(kDirectory, 0777);
    vita::DiagnosticLog log(kDiagnostics);
    log.begin();
    log.literal("GPU25: configurable clocks and GXM System24 fast path starting\n");
    void *heap_probe = std::malloc(1024);
    if (!heap_probe) { log.fault("GPU25: heap unavailable\n"); sceKernelExitProcess(1); }
    std::free(heap_probe);

    VitaSettings settings;
    settings.load();
    const int arm_clock_before = scePowerGetArmClockFrequency();
    const int gpu_clock_before = scePowerGetGpuClockFrequency();
    const int cpu_clock_result = vita::set_cpu_clock(settings.cpu_clock);
    const int gpu_clock_result = scePowerSetGpuClockFrequency(settings.gpu_clock);
    if (cpu_clock_result < 0 || gpu_clock_result < 0)
        log.fault("GPU25: clock request failed: cpu=%d gpu=%d\n", cpu_clock_result, gpu_clock_result);
    auto restore_clocks = [&] {
        if (arm_clock_before > 0) scePowerSetArmClockFrequency(arm_clock_before);
        if (gpu_clock_before > 0) scePowerSetGpuClockFrequency(gpu_clock_before);
    };
    const int arm_clock = scePowerGetArmClockFrequency();
    const int bus_clock = scePowerGetBusClockFrequency();
    const int gpu_clock = scePowerGetGpuClockFrequency();
    const int xbar_clock = scePowerGetGpuXbarClockFrequency();
    log.log("GPU25 clocks: requested_cpu=%d requested_gpu=%d arm=%d bus=%d gpu=%d xbar=%d cpu_before=%d gpu_before=%d set_cpu=%d set_gpu=%d\n",
            settings.cpu_clock, settings.gpu_clock, arm_clock, bus_clock, gpu_clock, xbar_clock,
            arm_clock_before, gpu_clock_before, cpu_clock_result, gpu_clock_result);
    log.literal("GPU25 stage: SDL timer/audio init begin\n");
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        log.fault("GPU25: SDL timer/audio init failed: %s\n", SDL_GetError());
        restore_clocks();
        return 1;
    }
    log.literal("GPU25 stage: SDL init done; vita2d init begin\n");
    if (vita2d_init_advanced(8 * 1024 * 1024) < 0) {
        log.fault("GPU25: vita2d/GXM initialization failed\n");
        SDL_Quit();
        restore_clocks();
        return 1;
    }
    log.literal("GPU25 stage: vita2d init done; framebuffer textures begin\n");
    vita2d_set_vblank_wait(1);
    vita2d_set_clear_color(RGBA8(0,0,0,255));
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    vita::ImGuiVita ui;
    if (!ui.init()) {
        log.fault("ImGui font texture allocation failed\n");
        vita2d_fini(); SDL_Quit(); restore_clocks(); return 1;
    }

    vita::GpuFastRenderer gpu;
    if (!gpu.ok()) {
        log.fault("GPU25: framebuffer texture allocation failed\n");
        gpu.shutdown(); ui.shutdown(); vita2d_fini(); SDL_Quit(); restore_clocks(); return 1;
    }
    log.log("GPU25 stage: GPU texture arenas ready: reserved_mb=%.2f; audio begin\n", double(gpu.reserved_bytes()) / (1024.0 * 1024.0));
    vita::Audio audio;
    vita::NativeAudio<snd::NativeSoundEngine> native_audio;
    bool active_native_audio = false;
    if (!audio.open()) log.fault("GPU25: audio unavailable: %s\n", SDL_GetError());
    audio.volume(float(settings.volume) / 100.0f);
    audio.mute(settings.mute);
    log.literal("GPU25 stage: audio done; main loop ready\n");

    std::unique_ptr<vita::TcpLink> link; // outlives the board's transport pointer
    std::unique_ptr<rt::GameLoop> game;
    // Destruction order keeps the detached packet, game and audio alive until
    // the worker has drained. Only one sound packet may be in flight.
    rt::GameLoop::SoundPacket active_sound;
    vita::SoundWorker sound_worker;
    sound_worker.open();
    log.log("GPU25 sound worker: threaded=%d affinity_result=%d affinity_mask=0x%x\n",
            int(sound_worker.threaded()), sound_worker.affinity_result(), sound_worker.affinity_mask());
    vita::AsyncLog perf_log;
    if constexpr (kDiagnostics) perf_log.open(log, ticks_us);
    log.log("GPU25 periodic log worker: threaded=%d; unavailable worker drops periodic records only\n",
            int(perf_log.threaded()));
    vita::Controls controls;
    vita::FrameClock clock(rt::GameLoop::kFrameHz, 1);
    bool running = true, menu = true, options = false, wait_release = true, gpu_fast = true;
    int selection = 0;
    uint32_t previous_buttons = 0;
    std::string status = "CROSS SELECT. OPTIONS INCLUDE CLOCKS, AUDIO, DISPLAY AND CONTROLS.";
    uint64_t last = ticks_us(), perf_start = last, perf_frames = 0, perf_run_us = 0, perf_gpu_us = 0;
    uint64_t perf_core_us = 0, perf_geo_us = 0, perf_video_us = 0, perf_sound_us = 0;
    uint64_t perf_presents = 0, perf_wait_us = 0, perf_encode_us = 0;
    uint64_t perf_sound_wait_us = 0, perf_audio_queue_us = 0, perf_sound_frames = 0;
    uint64_t perf_sort_us = 0, perf_polygon_us = 0, perf_tile_us = 0, perf_upload_us = 0;
    uint64_t previous_log_us = 0, perf_frame_peak_us = 0;
    bool sound_in_flight = false;
    bool native_effect_warned = false;
    double display_fps = 0.0;

    // Join only after the following frame's board work, or before an operation
    // that mutates audio/lifetime state. The worker never touches GameLoop's
    // profile or pending bytes while the next board frame runs.
    auto finish_sound = [&]() -> bool {
        if (!sound_in_flight) return true;
        sound_in_flight = false;
        const uint64_t begin = diagnostic_ticks_us();
        try { sound_worker.finish(); }
        catch (const std::exception &e) {
            perf_sound_wait_us += diagnostic_ticks_us() - begin;
            status = e.what();
            perf_log.sync([&] { log.fault("GPU25 sound runtime: %s\n", e.what()); });
            menu = true;
            audio.pause(); native_audio.pause();
            active_sound = {};
            game.reset(); // finish has joined, including its error path
            return false;
        }
        perf_sound_wait_us += diagnostic_ticks_us() - begin;
        perf_sound_us += sound_worker.last_sound_ticks();
        perf_audio_queue_us += sound_worker.last_audio_ticks();
        ++perf_sound_frames;
        return true;
    };

    int applied_core_setting = -1;
    auto apply_preferences = [&] {
        if (applied_core_setting != int(settings.fourth_core)) {
            if (!vita::configure_fourth_core(settings.fourth_core))
                log.fault("CPU affinity request rejected; using ordinary application cores\n");
            applied_core_setting = int(settings.fourth_core);
        }
        audio.volume(float(settings.volume) / 100.0f);
        audio.mute(settings.mute);
        native_audio.volume(float(settings.volume) / 100.0f);
        native_audio.mute(settings.mute);
        controls.set_deadzone(float(settings.deadzone) / 100.0f);
        controls.set_steer_invert(settings.steer_invert);
        controls.set_steer_curve(settings.steer_curve);
        rt::GameLoop::set_draw_distance(settings.draw_distance);
        if (game) {
            static constexpr double aspects[] = {0, 16.0/10, 16.0/9, 21.0/9};
            game->set_aspect(aspects[settings.aspect]);
            game->set_hud_edges(settings.hud_edges);
        }
    };
    auto commit_settings = [&](bool set_clocks) {
        if (!finish_sound()) return;
        perf_log.drain();
        int cpu_result = 0, gpu_result = 0;
        if (set_clocks) {
            cpu_result = vita::set_cpu_clock(settings.cpu_clock);
            gpu_result = scePowerSetGpuClockFrequency(settings.gpu_clock);
        }
        if (cpu_result < 0 || gpu_result < 0)
            log.fault("GPU25: clock request failed: cpu=%d gpu=%d\n", cpu_result, gpu_result);
        apply_preferences();
        const bool saved = settings.save();
        if (!saved) log.fault("GPU25: settings save failed\n");
        char message[160];
        std::snprintf(message, sizeof message, "CPU %d/%d MHz  GPU %d/%d MHz  SETTINGS %s",
                      scePowerGetArmClockFrequency(), settings.cpu_clock,
                      scePowerGetGpuClockFrequency(), settings.gpu_clock, saved ? "SAVED" : "SAVE FAILED");
        status = message;
        log.log("GPU25 settings: requested_cpu=%d requested_gpu=%d actual_cpu=%d actual_gpu=%d set_cpu=%d set_gpu=%d volume=%d mute=%d deadzone=%d invert=%d native_audio=%d saved=%d\n",
                settings.cpu_clock, settings.gpu_clock, scePowerGetArmClockFrequency(),
                scePowerGetGpuClockFrequency(), cpu_result, gpu_result, settings.volume,
                int(settings.mute), settings.deadzone, int(settings.steer_invert), int(settings.native_audio), int(saved));
    };
    apply_preferences();

    auto save = [&] {
        if (!finish_sound()) return false;
        if (!game) return true;
        const bool a = save_nv("ioboard_eeprom.bin", game->board().io().eeprom);
        const bool b = save_nv("backup_ram.bin", game->board().backup_ram());
        if (!a || !b) perf_log.sync([&] { log.fault("GPU25: save failed\n"); });
        return a && b;
    };
    auto apply_mode = [&] {
        if (game) {
            game->board().video().set_external_3d(gpu_fast);
            // Hardware reports show the wide GPU tile path slower overall.
            // Keep original-aspect GPU tiles; use CPU wide layers until total
            // device frame time, not CPU composition alone, justifies it.
            game->board().video().set_gpu_background(false);
        }
        gpu.reset_materials();
        clock.reset();
    };
    auto start_game = [&]() -> bool {
        if (!finish_sound()) return false;
        if (!save()) { status = "SAVE FAILED; ROM SWITCH CANCELLED."; return false; }
        if (settings.revision_a != (std::strcmp(M2_ROMSET, "daytona") == 0)) {
            const char *executable = settings.revision_a ? "app0:/daytona.self" : "app0:/eboot.bin";
            // Only the dual package has daytona.self. A standalone Revision A
            // eboot must not relaunch itself when the user selects daytona93.
            FILE *check = std::fopen("app0:/daytona.self", "rb");
            if (!check) { status = "SELECTED ROM EXECUTABLE NOT PACKAGED. INSTALL THE DUAL-ROM VPK."; return false; }
            std::fclose(check);
            settings.save();
            native_audio.close(); audio.close(); sound_worker.close();
            game.reset(); link.reset();
            vita2d_wait_rendering_done();
            sceGxmDisplayQueueFinish();
            const int rc = sceAppMgrLoadExec(executable, nullptr, nullptr);
            status = "ROM SWITCH FAILED: " + std::to_string(rc);
            return false;
        }
        active_sound = {};
        native_audio.close();
        audio.close();
        game.reset();
        link.reset();
        gpu.reset_materials();
        status = std::string("LOADING ") + M2_ROMSET + "...";
        gpu.prepare_frame();
        vita2d_start_drawing(); vita2d_clear_screen();
        ui.frame(1.f / 60);
        ImGui::SetNextWindowPos({24, 24}); ImGui::SetNextWindowSize({912, 496});
        ImGui::Begin("Starting Daytona USA", nullptr, ImGuiWindowFlags_NoDecoration);
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::End(); ui.render();
        vita2d_end_drawing(); vita2d_swap_buffers();
        try {
            if (settings.link_enabled && !settings.revision_a)
                throw std::runtime_error("LINK PLAY REQUIRES DAYTONA REVISION A. SELECT THAT ROM OR TURN LINK OFF.");
            auto images = rt::import_rom_set(kRom);
            active_native_audio = settings.native_audio;
            native_effect_warned = false;
            if (active_native_audio) {
                sound_worker.close();
                auto engine = std::make_unique<snd::NativeSoundEngine>(
                    std::move(images.sound_program), std::move(images.pcm1), std::move(images.pcm2));
                if (!native_audio.open(std::move(engine), kDiagnostics ? ticks_us : nullptr))
                    throw std::runtime_error("Native audio output unavailable; select Reference audio and reset");
            } else {
                if (!sound_worker.threaded()) sound_worker.open();
                if (!audio.open()) perf_log.sync([&] { log.fault("GPU25 reference audio unavailable: %s\n", SDL_GetError()); });
            }
            game = std::make_unique<rt::GameLoop>(std::move(images), !active_native_audio);
            if (test_hold.armed()) test_hold.arm();
            perf_log.sync([&] {
                log.log("GPU25 audio engine: backend=%s clock=%s reference_sound_board=%d\n",
                        active_native_audio ? "NATIVE_TEST" : "REFERENCE",
                        active_native_audio ? "AUDIO_DEVICE_48000" : "GAME_FRAME", int(game->sound() != nullptr));
            });
            game->set_profile_clock(kDiagnostics ? ticks_us : nullptr);
            load_nv("ioboard_eeprom.bin", game->board().io().eeprom);
            load_nv("backup_ram.bin", game->board().backup_ram());
            if (settings.link_enabled) {
                link = std::make_unique<vita::TcpLink>(settings.link_port, settings.next_address(), settings.next_port);
                if (!link->error().empty()) throw std::runtime_error("LINK: " + link->error());
                game->board().set_link(link.get(), settings.link_sync);
            }
            game->board().video().set_profile_clock(kDiagnostics ? ticks_us : nullptr);
            apply_mode();
            controls = vita::Controls{};
            apply_preferences();
            status = "START+SELECT MENU. TRIANGLE VIEW 4. D-PAD UP/DOWN SHIFT.";
            wait_release = true;
            return true;
        } catch (const std::exception &e) {
            native_audio.close(); audio.pause(); game.reset(); link.reset();
            status = e.what();
            perf_log.sync([&] { log.fault("GPU25 start: %s\n", e.what()); });
            return false;
        }
    };

    while (running) {
        const uint64_t now = ticks_us();
        const double elapsed = double(now - last) / 1000000.0;
        last = now;
        const vita::Pad pad = read_pad();
        uint32_t pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;
        if (wait_release) {
            pressed = 0; controls.latch(pad.buttons);
            if (!pad.buttons) wait_release = false;
        }
        if (!menu && !wait_release && vita::menu_chord(pad.buttons)) {
            menu = true; options = false; selection = 0;
            finish_sound();
            audio.pause(); native_audio.pause(); save(); clock.reset(); wait_release = true;
        }
        if (menu && !wait_release) {
            pressed |= ui_buttons;
            ui_buttons = 0;
            if (options) {
                constexpr int kOptionCount = 29;
                if (pressed & vita::Up) selection = (selection + kOptionCount - 1) % kOptionCount;
                if (pressed & vita::Down) selection = (selection + 1) % kOptionCount;
                if (pressed & vita::Circle) { options = false; selection = 2; wait_release = true; }
                else {
                    const int direction = (pressed & vita::Left) ? -1 : ((pressed & vita::Right) ? 1 : 0);
                    const bool activate = (pressed & vita::Cross) != 0;
                    bool changed = false, clocks_changed = false;
                    static const int cpu_choices[] = {111, 222, 333, 444, 500};
                    static const int gpu_choices[] = {41, 77, 111, 166};
                    if (selection == 0 && (direction || activate)) {
                        settings.cpu_clock = cycle_value(settings.cpu_clock, cpu_choices, 5, direction < 0 ? -1 : 1);
                        changed = clocks_changed = true;
                    } else if (selection == 1 && (direction || activate)) {
                        settings.gpu_clock = cycle_value(settings.gpu_clock, gpu_choices, 4, direction < 0 ? -1 : 1);
                        changed = clocks_changed = true;
                    } else if (selection == 2 && (direction || activate)) {
                        settings.volume = std::clamp(settings.volume + (direction < 0 ? -10 : 10), 0, 100);
                        changed = true;
                    } else if (selection == 3 && (direction || activate)) {
                        settings.mute = !settings.mute; changed = true;
                    } else if (selection == 4 && (direction || activate)) {
                        settings.deadzone = std::clamp(settings.deadzone + (direction < 0 ? -5 : 5), 0, 40);
                        changed = true;
                    } else if (selection == 5 && (direction || activate)) {
                        settings.steer_invert = !settings.steer_invert; changed = true;
                    } else if (selection == 6 && (direction || activate)) {
                        settings.native_audio = !settings.native_audio; changed = true;
                    } else if (selection == 7 && activate) {
                        status = "VITA RENDERER IS FIXED TO NATIVE GXM GPU FAST.";
                    } else if (selection == 8 && activate) {
                        status = "VITA OUTPUT IS FIXED FULLSCREEN AT 960 X 544.";
                    } else if (selection == 9 && (direction || activate)) {
                        settings.revision_a = !settings.revision_a;
                        status = "ROM SELECTED. CHOOSE START/RESET TO SWITCH. SAVES ARE SEPARATE.";
                    } else if (selection == 10 && activate) {
                        status = "TEST: SELECT+TRIANGLE  SERVICE: SELECT+SQUARE  MENU: CROSS NEXT / START ENTER";
                    } else if (selection == 11 && (direction || activate)) {
                        settings.aspect = (settings.aspect + (direction < 0 ? 3 : 1)) % 4; changed = true;
                    } else if (selection == 12 && (direction || activate)) {
                        settings.hud_edges = !settings.hud_edges; changed = true;
                    } else if (selection == 13 && (direction || activate)) {
                        settings.draw_distance = std::clamp(settings.draw_distance + (direction < 0 ? -1 : 1), -2, 2);
                        changed = true;
                    } else if (selection == 14 && (direction || activate)) {
                        settings.steer_curve = (settings.steer_curve + (direction < 0 ? 2 : 1)) % 3; changed = true;
                    } else if (selection == 15 && (direction || activate)) {
                        settings.stretch_backdrop = !settings.stretch_backdrop; changed = true;
                    } else if (selection == 16 && (direction || activate)) {
                        settings.skip_launcher = !settings.skip_launcher; changed = true;
                    } else if (selection == 17 && (direction || activate)) {
                        settings.fourth_core = !settings.fourth_core; changed = true;
                    } else if (selection == 18 && (direction || activate)) {
                        settings.link_enabled = !settings.link_enabled; changed = true;
                    } else if (selection >= 19 && selection <= 22 && (direction || activate)) {
                        int &octet = settings.next_ip[selection - 19];
                        octet = (octet + (direction < 0 ? 255 : 1)) % 256; changed = true;
                    } else if ((selection == 23 || selection == 24) && (direction || activate)) {
                        int &port = selection == 23 ? settings.link_port : settings.next_port;
                        port = std::clamp(port + (direction < 0 ? -1 : 1), 1, 65535); changed = true;
                    } else if (selection == 25 && (direction || activate)) {
                        settings.link_sync = !settings.link_sync; changed = true;
                    } else if (selection == 26 && (direction || activate)) {
                        if (test_hold.armed()) test_hold.cancel(); else test_hold.arm();
                    } else if (selection == 27 && activate) {
                        test_hold.cancel();
                        settings.defaults(); changed = clocks_changed = true;
                    } else if (selection == 28 && activate) {
                        options = false; selection = 2; wait_release = true;
                    }
                    if (changed) {
                        commit_settings(clocks_changed);
                        if (selection >= 18 && selection <= 25)
                            status = "LINK SETTINGS SAVED. RESET GAME TO APPLY. NEXT: " +
                                settings.next_address() + ":" + std::to_string(settings.next_port);
                        if (selection == 17) status = !settings.fourth_core
                            ? "4TH CORE OFF: ORDINARY THREE-CORE SCHEDULING."
                            : vita::fourth_core_active()
                                ? "4TH CORE ALLOWED FOR GAME AND AUDIO THREADS. NO EXTRA GAME WORKER."
                                : "4TH CORE REJECTED. REQUIRES A WORKING CORE-UNLOCK PLUGIN.";
                        if (selection == 0 && settings.cpu_clock == 500 && scePowerGetArmClockFrequency() != 500)
                            status += " 500 NOT ACTIVE: CHECK OVERCLOCK PLUGIN/PROFILE.";
                        if (selection == 13) status = "FURTHER DISTANCES ADD SCENERY AND MAY REDUCE FPS.";
                        if (selection == 6) status = "AUDIO ENGINE CHANGE SAVED. RESET GAME TO APPLY. NATIVE IS EXPERIMENTAL.";
                    }
                }
            } else {
                if (pressed & vita::Up) selection = (selection + 3) % 4;
                if (pressed & vita::Down) selection = (selection + 1) % 4;
                if ((pressed & vita::Circle) && game) {
                    menu = false; clock.reset(); wait_release = true;
                } else if (pressed & vita::Cross) {
                    if (selection == 0) {
                        const bool same_rom = settings.revision_a == (std::strcmp(M2_ROMSET, "daytona") == 0);
                        if ((game && same_rom) || start_game()) { menu = false; clock.reset(); wait_release = true; }
                    } else if (selection == 1) {
                        if (start_game()) { menu = false; clock.reset(); wait_release = true; }
                    } else if (selection == 2) {
                        options = true; selection = 0; wait_release = true;
                    } else if (selection == 3) {
                        save(); settings.save(); running = false;
                    }
                }
            }
        }

        bool simulated = false;
        if (game && !menu) {
            if (active_native_audio) native_audio.resume();
            const int frames = clock.advance(elapsed);
            for (int n = 0; n < frames; ++n) {
                const uint64_t begin = diagnostic_ticks_us();
                try {
                    auto inputs = map_input(controls.sample(wait_release ? vita::Pad{} : pad));
                    test_hold.apply(game->frames(), inputs.in0);
                    auto next_sound = game->run_frame_sound_packet(inputs);
                    perf_run_us += diagnostic_ticks_us() - begin;
                    const auto &fp = game->last_profile();
                    perf_core_us += fp.core(); perf_geo_us += fp.geometry;
                    perf_video_us += fp.video;
                    ++perf_frames;
                    if (active_native_audio) {
                        // Only commands cross into audio. The callback sequences
                        // and mixes continuously, not once per graphics frame.
                        const auto bytes = game->board().take_sound_bytes();
                        const auto health = native_audio.stats();
                        if (health.unsupported && !native_effect_warned) {
                            native_effect_warned = true;
                            log.log("Native audio: unsupported effect omitted; playback continues\n");
                        }
                        if (health.failed || health.invalid) {
                            char error[192];
                            std::snprintf(error, sizeof error,
                                "Native audio fault: callback=%u invalid=%u unsupported=%u. Reset with Reference audio.",
                                health.failed, health.invalid, health.unsupported);
                            throw std::runtime_error(error);
                        }
                        if (!native_audio.send(bytes.data(), bytes.size()))
                            throw std::runtime_error("Native audio queue overflow; use Reference audio and reset");
                    } else {
                        // Reference backend retains the bounded sound pipeline.
                        if (!finish_sound()) break;
                        active_sound = std::move(next_sound);
                        sound_worker.dispatch_packet(active_sound, audio, kDiagnostics ? ticks_us : nullptr);
                        sound_in_flight = true;
                    }
                    simulated = true;
                } catch (const std::exception &e) {
                    // A board/dispatch failure must not destroy a sound board
                    // still used by the previous job.
                    finish_sound();
                    status = e.what();
                    perf_log.sync([&] { log.fault("GPU25 runtime: %s\n", e.what()); });
                    menu = true; audio.pause(); native_audio.close(); active_sound = {}; game.reset(); break;
                }
            }
        } else clock.reset();

        const uint64_t gpu_begin = diagnostic_ticks_us();
        if (link) link->poll();
        gpu.prepare_frame();
        const uint64_t gpu_ready = diagnostic_ticks_us();
        vita2d_start_drawing();
        vita2d_clear_screen();
        if (menu) {
            std::string shown_status = status;
            if (link && game && !options) {
                const auto *comm = game->board().comm_board();
                shown_status += "\nIP " + link->local_ip() + " RX " + (link->rx_open() ? "YES" : "WAIT") +
                    " TX " + (link->tx_open() ? "YES" : "WAIT");
                if (comm) shown_status += " CABINET " + std::to_string(comm->id()) + "/" +
                    std::to_string(comm->count()) + (comm->link() == rt::CommBoard::Link::Lost ? " LOST" : "");
            }
            ui.frame(float(elapsed));
            draw_menu(bool(game), options, selection, settings, shown_status, display_fps);
            ui.render();
        }
        else if (game) {
            if (gpu_fast) gpu.draw(game->board().video()); else gpu.draw_exact(game->board().video());
        }
        vita2d_end_drawing();
        vita2d_swap_buffers();
        if (!menu && game) {
            ++perf_presents;
            perf_wait_us += gpu_ready - gpu_begin;
            perf_encode_us += uint64_t(gpu.last_gpu_ms() * 1000.0);
            perf_sort_us += gpu.last_sort_us(); perf_polygon_us += gpu.last_polygon_us();
            perf_tile_us += gpu.last_tile_us(); perf_upload_us += gpu.last_upload_us();
            perf_gpu_us += diagnostic_ticks_us() - gpu_begin;
        }

        const uint64_t after = ticks_us();
        if (simulated) perf_frame_peak_us = std::max(perf_frame_peak_us, after - now);
        if (after - perf_start >= 2000000) {
            const double sec = double(after - perf_start) / 1000000.0;
            display_fps = sec > 0 ? double(perf_frames) / sec : 0.0;
            const double run_ms = perf_frames ? double(perf_run_us) / perf_frames / 1000.0 : 0.0;
            const double present_divisor = perf_presents ? double(perf_presents) : 1.0;
            const double gpu_ms = double(perf_gpu_us) / present_divisor / 1000.0;
            const double frame_divisor = perf_frames ? double(perf_frames) : 1.0;
            const double sound_divisor = perf_sound_frames ? double(perf_sound_frames) : 1.0;
            if (kDiagnostics && game) {
                const auto &vp = game->board().video().last_profile();
                const auto log_stats = perf_log.stats();
                const auto native_stats = native_audio.stats();
                const uint64_t log_begin = ticks_us();
                perf_log.try_log("gpu25: mode=%s menu=%d frames=%u presents=%u window_ms=%.2f log_enqueue_prev_ms=%.2f log_write_prev_ms=%.2f log_write_peak_ms=%.2f log_drops=%u log_failures=%u sound_frames=%u frame_peak_ms=%.2f sim_fps=%.2f run_ms=%.2f core_ms=%.2f geo_ms=%.2f video_ms=%.2f sound_ms=%.2f gpu_submit_ms=%.2f gpu_encode_ms=%.2f gpu_wait_ms=%.2f sound_wait_ms=%.2f audio_worker_queue_ms=%.2f sort_ms=%.2f poly_ms=%.2f tiles_ms=%.2f upload_ms=%.2f cpu_mhz=%d gpu_mhz=%d cpu_raster_ms=%.2f tile_cache_ms=%.2f tile_draw_ms=%.2f compose_ms=%.2f layers=%u tiles=%u chars=%u materials=%u sources=%u builds=%u defers=%u cache_mb=%.2f cache_reserved_mb=%.2f pool_free_kb=%u pool_drops=%u material_drops=%u subdiv_polys=%u vertices=%u tile_uploads=%u cache_resets=%u tile_quads=%u draws=%u/%u clips=%u polys=%u/%u shader_setups=%u state_reuses=%u draw_errors=%u checker_polys=%u textured_checker_polys=%u sys24_ctrl=%04x/%04x audio=%s native_frames=%u native_last_ms=%.3f native_peak_ms=%.3f native_queue=%u native_overflows=%u native_failed=%u native_unsupported=%u native_invalid=%u native_notes=%u native_voices=%u\n",
                        gpu_fast ? "GPU_FAST" : "CPU_EXACT", int(menu), unsigned(perf_frames), unsigned(perf_presents),
                        sec * 1000.0, double(previous_log_us) / 1000.0,
                        double(log_stats.last_write_ticks) / 1000.0, double(log_stats.max_write_ticks) / 1000.0,
                        unsigned(log_stats.dropped), unsigned(log_stats.failures), unsigned(perf_sound_frames),
                        double(perf_frame_peak_us) / 1000.0, display_fps, run_ms,
                        double(perf_core_us)/frame_divisor/1000.0, double(perf_geo_us)/frame_divisor/1000.0,
                        double(perf_video_us)/frame_divisor/1000.0, double(perf_sound_us)/sound_divisor/1000.0, gpu_ms, double(perf_encode_us)/present_divisor/1000.0, double(perf_wait_us)/present_divisor/1000.0, double(perf_sound_wait_us)/frame_divisor/1000.0, double(perf_audio_queue_us)/sound_divisor/1000.0,
                        double(perf_sort_us)/present_divisor/1000.0, double(perf_polygon_us)/present_divisor/1000.0,
                        double(perf_tile_us)/present_divisor/1000.0, double(perf_upload_us)/present_divisor/1000.0,
                        scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency(),
                        double(vp.raster)/1000.0, double(vp.tile_cache)/1000.0, double(vp.tile_draw)/1000.0,
                        double(vp.composite)/1000.0, unsigned(vp.layers_rebuilt), vp.tiles_rebuilt, vp.characters_changed,
                        unsigned(gpu.cached_materials()), unsigned(gpu.cached_sources()), gpu.material_builds(), gpu.material_defers(), double(gpu.cached_bytes())/(1024.0*1024.0),
                        double(gpu.reserved_bytes())/(1024.0*1024.0), gpu.min_pool_free()/1024u, gpu.pool_drops(), gpu.material_drops(), gpu.subdivided_polys(), unsigned(gpu.submitted_vertices()), gpu.system24_uploaded_tiles(), gpu.cache_resets(), gpu.system24_quads(), gpu.textured_draws(), gpu.solid_draws(), gpu.clip_changes(), gpu.textured_polys(), gpu.solid_polys(), gpu.shader_setups(), gpu.state_reuses(), gpu.draw_errors(), gpu.checker_polys(), gpu.textured_checker_polys(),
                        unsigned(game->board().video().system24_word(0x5004)),
                        unsigned(game->board().video().system24_word(0x5006)),
                        active_native_audio ? "NATIVE_TEST" : "REFERENCE", native_stats.frames,
                        double(native_stats.last_us)/1000.0, double(native_stats.peak_us)/1000.0,
                        native_stats.queued, native_stats.overflows, native_stats.failed,
                        native_stats.unsupported, native_stats.invalid, native_stats.notes, native_stats.voices);
                previous_log_us = ticks_us() - log_begin;
            }
            perf_start = after; perf_frames = perf_run_us = perf_gpu_us = 0;
            perf_core_us = perf_geo_us = perf_video_us = perf_sound_us = 0;
            perf_presents = perf_wait_us = perf_encode_us = 0;
            perf_sound_wait_us = perf_audio_queue_us = perf_sound_frames = 0;
            perf_frame_peak_us = 0;
            perf_sort_us = perf_polygon_us = perf_tile_us = perf_upload_us = 0;
        }
        if (menu || !simulated) SDL_Delay(1);
    }

    finish_sound();
    sound_worker.close();
    save();
    settings.save();
    native_audio.close();
    audio.close();
    gpu.shutdown();
    ui.shutdown();
    restore_clocks();
    vita2d_fini();
    perf_log.close(); // join file I/O before destroying SDL synchronization
    SDL_Quit();
    log.literal("GPU25: clean exit\n");
    return 0;
}
