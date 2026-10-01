// daytona: the recompiled game with its launcher, in one window. SDL3 for the
// window, input and GPU (SDL_GPU: Vulkan or Direct3D 12 on Windows, Vulkan
// on Linux, Metal on macOS); Dear ImGui for the launcher. The game runs on
// the native board runtime (rt::GameLoop), loaded straight from the user's
// ROM zip (rt::import_rom_set); each composed frame is uploaded to a GPU
// texture and scaled onto the swapchain, and the sound board's output is
// played through SDL audio.
//
//   daytona [--rom FILE.zip] [--autostart] [--gpu vulkan|direct3d12|metal]
//           (the launcher's "Skip launcher" is a saved --autostart)
//           [--fullscreen] [--frames N] [--audio native|reference]
//
// In the game: Esc opens the launcher (resume, reset, controls), F11
// toggles fullscreen. Controls are set in the launcher and saved.

#include "app/config.h"
#include "app/launcher.h"
#include "app/native_audio.h"
#include "runtime/native_sound_engine.h"
#include "runtime/game_loop.h"
#include "runtime/rom_import.h"

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include "imgui.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h> // Windows: the WinMain entry point a WIN32 (GUI) program links against

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

namespace {

constexpr double kArcadeHz = rt::GameLoop::kFrameHz; // 16 MHz / (656 x 424), the board's frame rate

// The sound board's two outputs (the YM3438, and the two MultiPCMs mixed) go
// to their own SDL audio streams at the chips' own rates; SDL resamples and
// mixes them on the device. The game advances on the display's clock and the
// device plays on its own, so a small speed trim on both streams holds the
// queue near kLatency instead of letting it drift into a gap or a backlog.
class Audio {
public:
    static constexpr double kLatency = 0.06; // seconds queued
    ~Audio() { close(); }

    bool open(double fm_rate, double pcm_rate) {
        dev_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
        if (!dev_) return false;
        fm_rate_ = int(fm_rate + 0.5);
        const SDL_AudioSpec fm{SDL_AUDIO_F32, 2, fm_rate_}, pcm{SDL_AUDIO_F32, 2, int(pcm_rate + 0.5)};
        fm_ = SDL_CreateAudioStream(&fm, nullptr);
        pcm_ = SDL_CreateAudioStream(&pcm, nullptr);
        if (!fm_ || !pcm_ || !SDL_BindAudioStream(dev_, fm_) || !SDL_BindAudioStream(dev_, pcm_)) {
            close(); return false;
        }
        return true;
    }
    void push(snd::SoundBoard &sb, float gain) {
        if (!fm_) return;
        const std::vector<float> fm = sb.take_fm(), pcm = sb.take_pcm();
        SDL_PutAudioStreamData(fm_, fm.data(), int(fm.size() * sizeof(float)));
        SDL_PutAudioStreamData(pcm_, pcm.data(), int(pcm.size() * sizeof(float)));
        const double queued = double(SDL_GetAudioStreamQueued(fm_)) / (8.0 * fm_rate_);
        if (queued > kLatency * 4) { // a stall (window drag, debugger): drop the backlog
            SDL_ClearAudioStream(fm_);
            SDL_ClearAudioStream(pcm_);
        }
        const double err = std::clamp((queued - kLatency) / kLatency, -1.0, 1.0);
        const float ratio = float(1.0 + 0.005 * err); // at most 0.5%: inaudible
        SDL_SetAudioStreamFrequencyRatio(fm_, ratio);
        SDL_SetAudioStreamFrequencyRatio(pcm_, ratio);
        SDL_SetAudioStreamGain(fm_, gain);
        SDL_SetAudioStreamGain(pcm_, gain);
    }
    void clear() {
        if (fm_) SDL_ClearAudioStream(fm_);
        if (pcm_) SDL_ClearAudioStream(pcm_);
    }
    void close() {
        if (fm_) SDL_DestroyAudioStream(fm_);
        if (pcm_) SDL_DestroyAudioStream(pcm_);
        if (dev_) SDL_CloseAudioDevice(dev_);
        fm_ = pcm_ = nullptr;
        dev_ = 0;
    }

private:
    SDL_AudioDeviceID dev_ = 0;
    SDL_AudioStream *fm_ = nullptr, *pcm_ = nullptr;
    int fm_rate_ = 1;
};

std::string pref_file(const char *name) {
    char *base = SDL_GetPrefPath("daytona-recomp", "daytona93");
    std::string p = base ? std::string(base) + name : std::string(name);
    SDL_free(base);
    return p;
}
template <typename C> void load_file(const std::string &path, C &into) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::vector<uint8_t> d{std::istreambuf_iterator<char>(f), {}};
    if (d.size() == into.size()) std::copy(d.begin(), d.end(), into.begin());
}
template <typename C> void save_file(const std::string &path, const C &from) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(from.data()), std::streamsize(from.size()));
}

int fail(const char *what) {
    std::fprintf(stderr, "daytona: %s: %s\n", what, SDL_GetError());
    return 1;
}

} // namespace

int main(int argc, char **argv) {
    app::Config cfg;
    cfg.load();
    uint64_t max_frames = 0;
    bool autostart = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--rom") && i + 1 < argc) cfg.rom_path = argv[++i];
        else if (!std::strcmp(argv[i], "--gpu") && i + 1 < argc) cfg.gpu = argv[++i];
        else if (!std::strcmp(argv[i], "--audio")) {
            if (i + 1 == argc || (std::strcmp(argv[i + 1], "native") && std::strcmp(argv[i + 1], "reference"))) {
                std::fprintf(stderr, "daytona: --audio expects native or reference\n");
                return 1;
            }
            cfg.native_audio = !std::strcmp(argv[++i], "native");
        }
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--fullscreen")) cfg.fullscreen = true;
        else if (!std::strcmp(argv[i], "--autostart")) autostart = true;
    }

    if (!cfg.gpu.empty()) SDL_SetHint(SDL_HINT_GPU_DRIVER, cfg.gpu.c_str());
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return fail("SDL_Init");
    Audio audio;
    app::NativeAudio<snd::NativeSoundEngine> native_audio;
    const bool audio_initialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
    bool have_audio = false, native_active = false, native_fault = false;
    if (audio_initialized) std::printf("daytona: audio driver %s\n", SDL_GetCurrentAudioDriver());
    else std::fprintf(stderr, "daytona: no audio output (%s)\n", SDL_GetError());

    constexpr int W = rt::GameLoop::kWidth, H = rt::GameLoop::kHeight;
    const int kMaxW = W + 2 * rt::GameLoop::wide_margin(app::Config::kMaxAspect);
    int screen_w = W; // this frame's width: W, or wider with widescreen
    SDL_Window *window = SDL_CreateWindow("Daytona USA", W * 2, H * 2,
                                          SDL_WINDOW_RESIZABLE | (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!window) return fail("SDL_CreateWindow");
    constexpr SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
    SDL_GPUDevice *dev = SDL_CreateGPUDevice(formats, false, nullptr);
    if (!dev && !cfg.gpu.empty()) {
        // The chosen API is not available here (Vulkan on a Mac without MoltenVK): use the automatic choice.
        std::fprintf(stderr, "daytona: %s; falling back to automatic\n", SDL_GetError());
        SDL_ResetHint(SDL_HINT_GPU_DRIVER);
        cfg.gpu.clear();
        dev = SDL_CreateGPUDevice(formats, false, nullptr);
    }
    if (!dev) return fail("SDL_CreateGPUDevice");
    if (!SDL_ClaimWindowForGPUDevice(dev, window)) return fail("SDL_ClaimWindowForGPUDevice");
    std::printf("daytona: GPU driver %s\n", SDL_GetGPUDeviceDriver(dev));

    // Dear ImGui on SDL3 + SDL_GPU (its shaders ship precompiled for every backend)
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(SDL_GetWindowDisplayScale(window));
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo ii;
    ii.Device = dev;
    ii.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev, window);
    ImGui_ImplSDLGPU3_Init(&ii);

    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; // the screen's 0xAARRGGBB words, little-endian
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = kMaxW; // the widest screen (widescreen); each frame uses its own width
    ti.height = H;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    SDL_GPUTexture *screen = SDL_CreateGPUTexture(dev, &ti);
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = Uint32(kMaxW) * H * 4;
    SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!screen || !upload) return fail("SDL_CreateGPUTexture");

    app::Launcher launcher(cfg, window);
    std::unique_ptr<rt::GameLoop> game;
    const std::string eeprom_path = pref_file("ioboard_eeprom.bin"), backup_path = pref_file("backup_ram.bin");
    auto save_nv = [&] {
        if (!game) return;
        save_file(eeprom_path, game->board().io().eeprom);
        save_file(backup_path, game->board().backup_ram());
    };
    auto start_game = [&] {
        save_nv();
        native_audio.close(); // joins callback before replacing its ROMs/engine
        audio.close();
        game.reset();
        have_audio = false;
        native_fault = false;
        native_active = cfg.native_audio;
        try {
            auto images = rt::import_rom_set(cfg.rom_path);
            if (native_active) {
                if (!audio_initialized) throw std::runtime_error("Native audio needs an available audio device.");
                auto engine = std::make_unique<snd::NativeSoundEngine>(std::move(images.sound_program),
                    std::move(images.pcm1), std::move(images.pcm2));
                if (!native_audio.open(std::move(engine)))
                    throw std::runtime_error(std::string("Cannot open native audio: ") + SDL_GetError());
                native_audio.volume(cfg.volume);
                native_audio.mute(cfg.mute);
            } else if (audio_initialized) {
                have_audio = audio.open(snd::SoundBoard::kYmClock / 144.0, snd::SoundBoard::kPcmClock / 224.0);
                if (!have_audio) std::fprintf(stderr, "daytona: reference audio unavailable (%s)\n", SDL_GetError());
            }
            game = std::make_unique<rt::GameLoop>(std::move(images), !native_active);
            std::printf("daytona: audio backend %s\n", native_active
                ? "native (experimental, 48000 Hz device clock; no reference sound board)" : "reference");
            load_file(eeprom_path, game->board().io().eeprom);
            load_file(backup_path, game->board().backup_ram());
            launcher.set_error("");
            return true;
        } catch (const std::exception &e) {
            native_audio.close();
            audio.close();
            game.reset();
            launcher.set_error(e.what());
            return false;
        }
    };

    bool in_launcher = true, running = true, have_frame = false, new_frame = false;
    // Skip launcher (saved) or --autostart: straight into the game when the ROM
    // set checks out; otherwise the launcher shows, with the reason.
    if ((autostart || cfg.skip_launcher) && launcher.rom_ok() && start_game()) in_launcher = false;
    auto sync_native_audio = [&] {
        if (native_fault) in_launcher = true;
        if (!native_audio.available()) return;
        native_audio.volume(cfg.volume);
        native_audio.mute(cfg.mute);
        if (in_launcher || !running) native_audio.pause();
        else if (!native_audio.resume()) {
            launcher.set_error(std::string("Native audio resume failed; reset the game: ") + SDL_GetError());
            in_launcher = native_fault = true;
            native_audio.pause();
        }
    };
    sync_native_audio();
    SDL_Gamepad *pad = nullptr;
    uint64_t last = SDL_GetTicksNS();
    double pending = 0;
    const double frame_ns = 1e9 / kArcadeHz;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && e.gdevice.which == SDL_GetGamepadID(pad)) {
                SDL_CloseGamepad(pad);
                pad = nullptr;
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11) {
                cfg.fullscreen = !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
                SDL_SetWindowFullscreen(window, cfg.fullscreen);
                cfg.save();
            }
            if (in_launcher) {
                if (!launcher.handle_event(e)) ImGui_ImplSDL3_ProcessEvent(&e);
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) {
                in_launcher = true; // pause and show the launcher
            }
        }

        sync_native_audio();
        if (native_active) {
            const auto health = native_audio.stats();
            if (health.failed || health.invalid || health.unsupported) {
                native_audio.pause();
                char message[192];
                std::snprintf(message, sizeof message,
                    "Native audio fault (callback %u, invalid %u, unsupported %u); reset or select reference audio.",
                    health.failed, health.invalid, health.unsupported);
                launcher.set_error(message);
                in_launcher = native_fault = true;
            }
        }

        // Game: arcade speed (57.52 frames/s), presented at the display's rate.
        const uint64_t now = SDL_GetTicksNS();
        pending = std::min(pending + double(now - last), frame_ns * 4);
        last = now;
        if (game && !in_launcher) {
            game->set_aspect(cfg.aspect_ratio()); // widescreen: no-op unless it changed
            game->set_hud_edges(cfg.hud_edges);
            game->set_frame_skip(cfg.draw_mode);
            game->set_stretch_backdrop(cfg.stretch_backdrop);
            rt::GameLoop::set_draw_distance(cfg.draw_distance);
            while (pending >= frame_ns) {
                game->run_frame(cfg.controls.sample(SDL_GetKeyboardState(nullptr), pad));
                if (native_active) {
                    const auto bytes = game->board().take_sound_bytes();
                    if (!native_audio.send(bytes.data(), bytes.size())) {
                        native_audio.pause();
                        launcher.set_error("Native audio command queue failed; reset the game or select reference audio.");
                        in_launcher = native_fault = true;
                        break;
                    }
                }
                pending -= frame_ns;
                new_frame = have_frame = true;
                if (max_frames && game->frames() >= max_frames) running = false;
            }
            if (game->sound()) {
                if (have_audio) audio.push(*game->sound(), cfg.mute ? 0.0f : cfg.volume);
                else game->sound()->take_fm(), game->sound()->take_pcm(); // nowhere to play it
            }
        } else {
            pending = 0;
        }

        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (!cmd) return fail("SDL_AcquireGPUCommandBuffer");
        if (new_frame) {
            void *p = SDL_MapGPUTransferBuffer(dev, upload, true);
            screen_w = game->screen_width();
            std::memcpy(p, game->screen().data(), size_t(screen_w) * H * 4);
            SDL_UnmapGPUTransferBuffer(dev, upload);
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureTransferInfo src{};
            src.transfer_buffer = upload;
            src.pixels_per_row = Uint32(screen_w);
            SDL_GPUTextureRegion dst{};
            dst.texture = screen;
            dst.w = Uint32(screen_w);
            dst.h = H;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, true);
            SDL_EndGPUCopyPass(copy);
            new_frame = false;
        }

        ImDrawData *draw = nullptr;
        if (in_launcher) {
            ImGui_ImplSDLGPU3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            switch (launcher.draw(game != nullptr, pad)) {
            case app::Launcher::StartGame:
            case app::Launcher::Reset:
                if (start_game()) in_launcher = false, have_frame = false;
                break;
            case app::Launcher::Resume: in_launcher = false; break;
            case app::Launcher::Quit: running = false; break;
            default: break;
            }
            sync_native_audio();
            ImGui::Render();
            draw = ImGui::GetDrawData();
            ImGui_ImplSDLGPU3_PrepareDrawData(draw, cmd);
        }

        SDL_GPUTexture *swap = nullptr;
        Uint32 sw = 0, sh = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &sw, &sh)) return fail("swapchain");
        if (swap) {
            if (have_frame) { // the game's screen at its own shape, letterboxed
                const double scale = std::min(double(sw) / screen_w, double(sh) / H);
                const Uint32 dw = Uint32(screen_w * scale), dh = Uint32(H * scale);
                SDL_GPUBlitInfo blit{};
                blit.source.texture = screen;
                blit.source.w = Uint32(screen_w);
                blit.source.h = H;
                blit.destination.texture = swap;
                blit.destination.x = (sw - dw) / 2;
                blit.destination.y = (sh - dh) / 2;
                blit.destination.w = dw;
                blit.destination.h = dh;
                blit.load_op = SDL_GPU_LOADOP_CLEAR;
                blit.clear_color = SDL_FColor{0, 0, 0, 1};
                blit.filter = SDL_GPU_FILTER_LINEAR;
                SDL_BlitGPUTexture(cmd, &blit);
            }
            if (draw || !have_frame) { // the launcher on top (or a clear screen)
                SDL_GPUColorTargetInfo ct{};
                ct.texture = swap;
                ct.load_op = have_frame ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
                ct.store_op = SDL_GPU_STOREOP_STORE;
                ct.clear_color = SDL_FColor{0.05f, 0.05f, 0.08f, 1};
                SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
                if (draw) ImGui_ImplSDLGPU3_RenderDrawData(draw, cmd, pass);
                SDL_EndGPURenderPass(pass);
            }
        }
        SDL_SubmitGPUCommandBuffer(cmd);
    }

    save_nv();
    cfg.save();
    if (game) std::printf("daytona: %llu frames\n", (unsigned long long)game->frames());
    if (native_audio.available()) {
        native_audio.pause();
        const auto stats = native_audio.stats();
        std::printf("daytona: native audio callbacks=%u frames=%u queued=%u overflows=%u failed=%u invalid=%u unsupported=%u\n",
            stats.callbacks, stats.frames, stats.queued, stats.overflows, stats.failed, stats.invalid, stats.unsupported);
    }
    native_audio.close();
    audio.close();
    SDL_WaitForGPUIdle(dev);
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseGPUTransferBuffer(dev, upload);
    SDL_ReleaseGPUTexture(dev, screen);
    SDL_ReleaseWindowFromGPUDevice(dev, window);
    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(window);
    if (pad) SDL_CloseGamepad(pad);
    SDL_Quit();
    return 0;
}
