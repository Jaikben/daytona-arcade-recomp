#define SDL_MAIN_HANDLED
#include "audio.h"
#include "controls.h"
#include "diagnostic_log.h"
#include "gpu_fast.h"
#include "gpu_text.h"
#include "runtime/game_loop.h"
#include "runtime/rom_import.h"

#include <SDL.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <vita2d.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

extern "C" { unsigned int _newlib_heap_size_user = 192 * 1024 * 1024; }

namespace {
constexpr const char *kDirectory = "ux0:data/daytona93";
constexpr const char *kRom = "ux0:data/daytona93/daytona93.zip";

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

void draw_menu(bool have_game, bool gpu_fast, int selection, const std::string &status, double fps) {
    const unsigned white = RGBA8(235,235,235,255), yellow = RGBA8(255,200,70,255);
    vita::gpu_text("DAYTONA RECOMP - GPU06", 30, 28, white, 3, 49, 1);
    char perf[96];
    std::snprintf(perf, sizeof perf, "FPS %.1F  MODE %s", fps, gpu_fast ? "GPU FAST" : "CPU EXACT");
    vita::gpu_text(perf, 30, 68, white, 2, 70, 1);
    const char *labels[4] = {have_game ? "RESUME GAME" : "START GAME", "RESET GAME",
                             gpu_fast ? "RENDERER: GPU FAST" : "RENDERER: CPU EXACT", "SAVE AND QUIT"};
    for (int i = 0; i < 4; ++i) {
        std::string label = std::string(i == selection ? "> " : "  ") + labels[i];
        vita::gpu_text(label, 46, 130 + i * 44, i == selection ? yellow : white, 2, 70, 1);
    }
    vita::gpu_text(status, 30, 350, white, 2, 74, 6);
    vita::gpu_text("CROSS SELECT  CIRCLE RESUME  START+SELECT MENU", 30, 516, white, 2, 74, 1);
}
} // namespace

int main(int, char **) {
    sceIoMkdir(kDirectory, 0777);
    vita::DiagnosticLog log;
    log.begin();
    log.literal("GPU06: deferred-texture GXM fast path starting\n");
    void *heap_probe = std::malloc(1024);
    if (!heap_probe) { log.literal("GPU06: heap unavailable\n"); sceKernelExitProcess(1); }
    std::free(heap_probe);

    log.literal("GPU06 stage: SDL timer/audio init begin\n");
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        log.log("GPU06: SDL timer/audio init failed: %s\n", SDL_GetError());
        return 1;
    }
    log.literal("GPU06 stage: SDL init done; vita2d init begin\n");
    if (vita2d_init_advanced(8 * 1024 * 1024) < 0) {
        log.literal("GPU06: vita2d/GXM initialization failed\n");
        SDL_Quit();
        return 1;
    }
    log.literal("GPU06 stage: vita2d init done; framebuffer textures begin\n");
    vita2d_set_vblank_wait(1);
    vita2d_set_clear_color(RGBA8(0,0,0,255));
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    vita::GpuFastRenderer gpu;
    if (!gpu.ok()) {
        log.literal("GPU06: framebuffer texture allocation failed\n");
        vita2d_fini(); SDL_Quit(); return 1;
    }
    log.literal("GPU06 stage: framebuffer textures ready; audio begin\n");
    vita::Audio audio;
    if (!audio.open()) log.log("GPU06: audio unavailable: %s\n", SDL_GetError());
    log.literal("GPU06 stage: audio done; main loop ready\n");

    std::unique_ptr<rt::GameLoop> game;
    vita::Controls controls;
    vita::FrameClock clock(rt::GameLoop::kFrameHz, 1);
    bool running = true, menu = true, wait_release = true, gpu_fast = true;
    int selection = 0;
    uint32_t previous_buttons = 0;
    std::string status = "GPU FAST IS APPROXIMATE: TEXTURE MIP/INTERPOLATION PARITY IS NOT YET EXACT.";
    uint64_t last = ticks_us(), perf_start = last, perf_frames = 0, perf_run_us = 0, perf_gpu_us = 0;
    double display_fps = 0.0;

    auto save = [&] {
        if (!game) return true;
        const bool a = save_nv("ioboard_eeprom.bin", game->board().io().eeprom);
        const bool b = save_nv("backup_ram.bin", game->board().backup_ram());
        if (!a || !b) log.literal("GPU06: save failed\n");
        return a && b;
    };
    auto apply_mode = [&] {
        if (game) game->board().video().set_external_3d(gpu_fast);
        gpu.reset_materials();
        clock.reset();
    };
    auto start_game = [&]() -> bool {
        save();
        game.reset();
        gpu.reset_materials();
        audio.pause();
        status = "LOADING DAYTONA93...";
        vita2d_start_drawing(); vita2d_clear_screen(); draw_menu(false, gpu_fast, selection, status, display_fps); vita2d_end_drawing(); vita2d_swap_buffers();
        try {
            auto images = rt::import_rom_set(kRom);
            game = std::make_unique<rt::GameLoop>(std::move(images));
            game->set_profile_clock(ticks_us);
            load_nv("ioboard_eeprom.bin", game->board().io().eeprom);
            load_nv("backup_ram.bin", game->board().backup_ram());
            game->board().video().set_profile_clock(ticks_us);
            apply_mode();
            controls = vita::Controls{};
            status = "START+SELECT MENU. TRIANGLE VIEW 4. D-PAD UP/DOWN SHIFT.";
            wait_release = true;
            return true;
        } catch (const std::exception &e) {
            status = e.what(); log.log("GPU06 start: %s\n", e.what()); return false;
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
            menu = true; audio.pause(); save(); clock.reset(); wait_release = true;
        }
        if (menu && !wait_release) {
            if (pressed & vita::Up) selection = (selection + 3) % 4;
            if (pressed & vita::Down) selection = (selection + 1) % 4;
            if ((pressed & vita::Circle) && game) { menu = false; clock.reset(); wait_release = true; }
            else if (pressed & vita::Cross) {
                if (selection == 0) { if (game || start_game()) { menu = false; clock.reset(); wait_release = true; } }
                else if (selection == 1) { if (start_game()) { menu = false; clock.reset(); wait_release = true; } }
                else if (selection == 2) {
                    gpu_fast = !gpu_fast; apply_mode();
                    status = gpu_fast ? "GPU FAST ENABLED - APPROXIMATE 3D." : "CPU EXACT ENABLED - SLOW REFERENCE RASTERIZER.";
                } else if (selection == 3) { save(); running = false; }
            }
        }

        if (game && !menu) {
            const int frames = clock.advance(elapsed);
            for (int n = 0; n < frames; ++n) {
                const uint64_t begin = ticks_us();
                try {
                    game->run_frame(map_input(controls.sample(wait_release ? vita::Pad{} : pad)));
                } catch (const std::exception &e) {
                    status = e.what(); log.log("GPU06 runtime: %s\n", e.what()); menu = true; game.reset(); break;
                }
                perf_run_us += ticks_us() - begin;
                if (game && game->sound()) audio.push(*game->sound());
                ++perf_frames;
            }
        } else clock.reset();

        const uint64_t gpu_begin = ticks_us();
        vita2d_start_drawing();
        vita2d_clear_screen();
        if (menu) draw_menu(bool(game), gpu_fast, selection, status, display_fps);
        else if (game) {
            if (gpu_fast) gpu.draw(game->board().video()); else gpu.draw_exact(game->board().video());
        }
        vita2d_end_drawing();
        vita2d_swap_buffers();
        // GXM executes asynchronously. Material evictions are retired during
        // drawing and physically freed only after submitted work is complete.
        gpu.reap_retired();
        perf_gpu_us += ticks_us() - gpu_begin;

        const uint64_t after = ticks_us();
        if (after - perf_start >= 2000000) {
            const double sec = double(after - perf_start) / 1000000.0;
            display_fps = sec > 0 ? double(perf_frames) / sec : 0.0;
            const double run_ms = perf_frames ? double(perf_run_us) / perf_frames / 1000.0 : 0.0;
            const double gpu_ms = perf_frames ? double(perf_gpu_us) / perf_frames / 1000.0 : 0.0;
            if (game) {
                const auto &vp = game->board().video().last_profile();
                log.log("gpu06: mode=%s sim_fps=%.2f run_ms=%.2f gpu_submit_ms=%.2f cpu_raster_ms=%.2f tile_cache_ms=%.2f tile_draw_ms=%.2f compose_ms=%.2f materials=%u cache_mb=%.2f retired_mb=%.2f pool_free_kb=%u pool_drops=%u\n",
                        gpu_fast ? "GPU_FAST" : "CPU_EXACT", display_fps, run_ms, gpu_ms,
                        double(vp.raster)/1000.0, double(vp.tile_cache)/1000.0, double(vp.tile_draw)/1000.0,
                        double(vp.composite)/1000.0, unsigned(gpu.cached_materials()), double(gpu.cached_bytes())/(1024.0*1024.0),
                        double(gpu.retired_bytes())/(1024.0*1024.0), gpu.min_pool_free()/1024u, gpu.pool_drops());
            }
            perf_start = after; perf_frames = perf_run_us = perf_gpu_us = 0;
        }
        SDL_Delay(1);
    }

    save();
    audio.pause();
    gpu.shutdown();
    vita2d_fini();
    SDL_Quit();
    log.literal("GPU06: clean exit\n");
    return 0;
}
