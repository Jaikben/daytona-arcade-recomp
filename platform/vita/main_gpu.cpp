#define SDL_MAIN_HANDLED
#include "audio.h"
#include "sound_worker.h"
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
#include <psp2/power.h>
#include <vita2d.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

struct VitaSettings {
    int cpu_clock = 333;
    int gpu_clock = 111;
    int volume = 80;
    int deadzone = 12;
    bool mute = false;
    bool steer_invert = false;

    void defaults() { *this = VitaSettings{}; }
    void sanitize() {
        const auto valid = [](int value, const int *choices, int count, int fallback) {
            for (int i = 0; i < count; ++i) if (value == choices[i]) return value;
            return fallback;
        };
        static const int cpus[] = {111, 222, 333, 444};
        static const int gpus[] = {41, 77, 111, 166};
        cpu_clock = valid(cpu_clock, cpus, 4, 333);
        gpu_clock = valid(gpu_clock, gpus, 4, 111);
        volume = std::clamp(volume, 0, 100);
        deadzone = std::clamp(deadzone, 0, 40);
    }
    void load() {
        FILE *f = std::fopen("ux0:data/daytona93/vita.cfg", "r");
        if (!f) return;
        char line[96], key[40]; int value = 0;
        while (std::fgets(line, sizeof line, f)) {
            if (std::sscanf(line, "%39[^=]=%d", key, &value) != 2) continue;
            if (!std::strcmp(key, "cpu_clock")) cpu_clock = value;
            else if (!std::strcmp(key, "gpu_clock")) gpu_clock = value;
            else if (!std::strcmp(key, "volume")) volume = value;
            else if (!std::strcmp(key, "mute")) mute = value != 0;
            else if (!std::strcmp(key, "deadzone")) deadzone = value;
            else if (!std::strcmp(key, "steer_invert")) steer_invert = value != 0;
        }
        std::fclose(f); sanitize();
    }
    bool save() const {
        FILE *f = std::fopen("ux0:data/daytona93/vita.cfg.tmp", "w");
        if (!f) return false;
        std::fprintf(f, "cpu_clock=%d\ngpu_clock=%d\nvolume=%d\nmute=%d\ndeadzone=%d\nsteer_invert=%d\n",
                     cpu_clock, gpu_clock, volume, int(mute), deadzone, int(steer_invert));
        bool ok = std::fflush(f) == 0;
        if (std::fclose(f) != 0) ok = false;
        if (!ok) { std::remove("ux0:data/daytona93/vita.cfg.tmp"); return false; }
        std::remove("ux0:data/daytona93/vita.cfg");
        return std::rename("ux0:data/daytona93/vita.cfg.tmp", "ux0:data/daytona93/vita.cfg") == 0;
    }
};

int cycle_value(int value, const int *choices, int count, int direction) {
    int index = 0;
    for (int i = 0; i < count; ++i) if (choices[i] == value) index = i;
    return choices[(index + (direction < 0 ? count - 1 : 1)) % count];
}

void draw_menu(bool have_game, bool options, int selection, const VitaSettings &settings,
               const std::string &status, double fps) {
    const unsigned white = RGBA8(235,235,235,255), yellow = RGBA8(255,200,70,255);
    vita::gpu_text(options ? "DAYTONA RECOMP - OPTIONS" : "DAYTONA RECOMP - GPU18", 30, 24, white, 3, 49, 1);
    char line[128];
    if (!options) {
        std::snprintf(line, sizeof line, "FPS %.1F  CPU %d MHz  GPU %d MHz  GXM", fps,
                      scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency());
        vita::gpu_text(line, 30, 64, white, 2, 70, 1);
        const char *labels[4] = {have_game ? "RESUME GAME" : "START GAME", "RESET GAME", "OPTIONS", "SAVE AND QUIT"};
        for (int i = 0; i < 4; ++i) {
            std::string label = std::string(i == selection ? "> " : "  ") + labels[i];
            vita::gpu_text(label, 46, 126 + i * 44, i == selection ? yellow : white, 2, 70, 1);
        }
        vita::gpu_text(status, 30, 342, white, 2, 74, 6);
        vita::gpu_text("CROSS SELECT  CIRCLE RESUME  START+SELECT MENU", 30, 516, white, 2, 74, 1);
        return;
    }
    const char *values[12];
    char cpu[32], gpu[32], volume[32], mute[32], deadzone[32], invert[32];
    std::snprintf(cpu, sizeof cpu, "CPU CLOCK: %d MHz", settings.cpu_clock);
    std::snprintf(gpu, sizeof gpu, "GPU CLOCK: %d MHz", settings.gpu_clock);
    std::snprintf(volume, sizeof volume, "VOLUME: %d%%", settings.volume);
    std::snprintf(mute, sizeof mute, "MUTE: %s", settings.mute ? "ON" : "OFF");
    std::snprintf(deadzone, sizeof deadzone, "DEAD ZONE: %d%%", settings.deadzone);
    std::snprintf(invert, sizeof invert, "INVERT STEERING: %s", settings.steer_invert ? "ON" : "OFF");
    values[0]=cpu; values[1]=gpu; values[2]=volume; values[3]=mute; values[4]=deadzone; values[5]=invert;
    values[6]="GRAPHICS API: GXM"; values[7]="FULLSCREEN: ON"; values[8]="ROM: DAYTONA93.ZIP";
    values[9]="BINDINGS: VITA FIXED"; values[10]="RESET DEFAULTS"; values[11]="BACK";
    for (int i = 0; i < 12; ++i) {
        std::string label = std::string(i == selection ? "> " : "  ") + values[i];
        vita::gpu_text(label, 42, 68 + i * 35, i == selection ? yellow : white, 2, 70, 1);
    }
    vita::gpu_text(status, 30, 489, white, 1, 112, 2);
    vita::gpu_text("LEFT/RIGHT CHANGE  CROSS SELECT  CIRCLE BACK", 30, 526, white, 1, 112, 1);
}
} // namespace

int main(int, char **) {
    sceIoMkdir(kDirectory, 0777);
    vita::DiagnosticLog log;
    log.begin();
    log.literal("GPU18: configurable clocks and GXM System24 fast path starting\n");
    void *heap_probe = std::malloc(1024);
    if (!heap_probe) { log.literal("GPU18: heap unavailable\n"); sceKernelExitProcess(1); }
    std::free(heap_probe);

    VitaSettings settings;
    settings.load();
    const int arm_clock_before = scePowerGetArmClockFrequency();
    const int gpu_clock_before = scePowerGetGpuClockFrequency();
    const int cpu_clock_result = scePowerSetArmClockFrequency(settings.cpu_clock);
    const int gpu_clock_result = scePowerSetGpuClockFrequency(settings.gpu_clock);
    auto restore_clocks = [&] {
        if (arm_clock_before > 0) scePowerSetArmClockFrequency(arm_clock_before);
        if (gpu_clock_before > 0) scePowerSetGpuClockFrequency(gpu_clock_before);
    };
    const int arm_clock = scePowerGetArmClockFrequency();
    const int bus_clock = scePowerGetBusClockFrequency();
    const int gpu_clock = scePowerGetGpuClockFrequency();
    const int xbar_clock = scePowerGetGpuXbarClockFrequency();
    log.log("GPU18 clocks: requested_cpu=%d requested_gpu=%d arm=%d bus=%d gpu=%d xbar=%d cpu_before=%d gpu_before=%d set_cpu=%d set_gpu=%d\n",
            settings.cpu_clock, settings.gpu_clock, arm_clock, bus_clock, gpu_clock, xbar_clock,
            arm_clock_before, gpu_clock_before, cpu_clock_result, gpu_clock_result);
    log.literal("GPU18 stage: SDL timer/audio init begin\n");
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        log.log("GPU18: SDL timer/audio init failed: %s\n", SDL_GetError());
        restore_clocks();
        return 1;
    }
    log.literal("GPU18 stage: SDL init done; vita2d init begin\n");
    if (vita2d_init_advanced(8 * 1024 * 1024) < 0) {
        log.literal("GPU18: vita2d/GXM initialization failed\n");
        SDL_Quit();
        restore_clocks();
        return 1;
    }
    log.literal("GPU18 stage: vita2d init done; framebuffer textures begin\n");
    vita2d_set_vblank_wait(1);
    vita2d_set_clear_color(RGBA8(0,0,0,255));
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    vita::GpuFastRenderer gpu;
    if (!gpu.ok()) {
        log.literal("GPU18: framebuffer texture allocation failed\n");
        gpu.shutdown(); vita2d_fini(); SDL_Quit(); restore_clocks(); return 1;
    }
    log.log("GPU18 stage: GPU texture arenas ready: reserved_mb=%.2f; audio begin\n", double(gpu.reserved_bytes()) / (1024.0 * 1024.0));
    vita::Audio audio;
    if (!audio.open()) log.log("GPU18: audio unavailable: %s\n", SDL_GetError());
    audio.volume(float(settings.volume) / 100.0f);
    audio.mute(settings.mute);
    log.literal("GPU18 stage: audio done; main loop ready\n");

    std::unique_ptr<rt::GameLoop> game;
    // Declared after game: its destructor drains work before game is destroyed.
    vita::SoundWorker sound_worker;
    sound_worker.open();
    log.log("GPU18 sound worker: threaded=%d affinity_result=%d affinity_mask=0x%x\n",
            int(sound_worker.threaded()), sound_worker.affinity_result(), sound_worker.affinity_mask());
    vita::Controls controls;
    vita::FrameClock clock(rt::GameLoop::kFrameHz, 1);
    bool running = true, menu = true, options = false, wait_release = true, gpu_fast = true;
    int selection = 0;
    uint32_t previous_buttons = 0;
    std::string status = "CROSS SELECT. OPTIONS INCLUDE CLOCKS, AUDIO, DISPLAY AND CONTROLS.";
    uint64_t last = ticks_us(), perf_start = last, perf_frames = 0, perf_run_us = 0, perf_gpu_us = 0;
    uint64_t perf_core_us = 0, perf_geo_us = 0, perf_video_us = 0, perf_sound_us = 0;
    uint64_t perf_presents = 0, perf_wait_us = 0, perf_encode_us = 0;
    uint64_t perf_sound_wait_us = 0, perf_audio_queue_us = 0;
    double display_fps = 0.0;

    auto apply_preferences = [&] {
        audio.volume(float(settings.volume) / 100.0f);
        audio.mute(settings.mute);
        controls.set_deadzone(float(settings.deadzone) / 100.0f);
        controls.set_steer_invert(settings.steer_invert);
    };
    auto commit_settings = [&](bool set_clocks) {
        int cpu_result = 0, gpu_result = 0;
        if (set_clocks) {
            cpu_result = scePowerSetArmClockFrequency(settings.cpu_clock);
            gpu_result = scePowerSetGpuClockFrequency(settings.gpu_clock);
        }
        apply_preferences();
        const bool saved = settings.save();
        char message[160];
        std::snprintf(message, sizeof message, "CPU %d/%d MHz  GPU %d/%d MHz  SETTINGS %s",
                      scePowerGetArmClockFrequency(), settings.cpu_clock,
                      scePowerGetGpuClockFrequency(), settings.gpu_clock, saved ? "SAVED" : "SAVE FAILED");
        status = message;
        log.log("GPU18 settings: requested_cpu=%d requested_gpu=%d actual_cpu=%d actual_gpu=%d set_cpu=%d set_gpu=%d volume=%d mute=%d deadzone=%d invert=%d saved=%d\n",
                settings.cpu_clock, settings.gpu_clock, scePowerGetArmClockFrequency(),
                scePowerGetGpuClockFrequency(), cpu_result, gpu_result, settings.volume,
                int(settings.mute), settings.deadzone, int(settings.steer_invert), int(saved));
    };
    apply_preferences();

    auto save = [&] {
        if (!game) return true;
        const bool a = save_nv("ioboard_eeprom.bin", game->board().io().eeprom);
        const bool b = save_nv("backup_ram.bin", game->board().backup_ram());
        if (!a || !b) log.literal("GPU18: save failed\n");
        return a && b;
    };
    auto apply_mode = [&] {
        if (game) game->board().video().set_external_3d(gpu_fast);
        gpu.reset_materials();
        clock.reset();
    };
    auto start_game = [&]() -> bool {
        sound_worker.finish();
        save();
        game.reset();
        gpu.reset_materials();
        audio.pause();
        status = "LOADING DAYTONA93...";
        gpu.prepare_frame();
        vita2d_start_drawing(); vita2d_clear_screen(); draw_menu(false, false, 0, settings, status, display_fps); vita2d_end_drawing(); vita2d_swap_buffers();
        try {
            auto images = rt::import_rom_set(kRom);
            game = std::make_unique<rt::GameLoop>(std::move(images));
            game->set_profile_clock(ticks_us);
            load_nv("ioboard_eeprom.bin", game->board().io().eeprom);
            load_nv("backup_ram.bin", game->board().backup_ram());
            game->board().video().set_profile_clock(ticks_us);
            apply_mode();
            controls = vita::Controls{};
            apply_preferences();
            status = "START+SELECT MENU. TRIANGLE VIEW 4. D-PAD UP/DOWN SHIFT.";
            wait_release = true;
            return true;
        } catch (const std::exception &e) {
            status = e.what(); log.log("GPU18 start: %s\n", e.what()); return false;
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
            audio.pause(); save(); clock.reset(); wait_release = true;
        }
        if (menu && !wait_release) {
            if (options) {
                constexpr int kOptionCount = 12;
                if (pressed & vita::Up) selection = (selection + kOptionCount - 1) % kOptionCount;
                if (pressed & vita::Down) selection = (selection + 1) % kOptionCount;
                if (pressed & vita::Circle) { options = false; selection = 2; wait_release = true; }
                else {
                    const int direction = (pressed & vita::Left) ? -1 : ((pressed & vita::Right) ? 1 : 0);
                    const bool activate = (pressed & vita::Cross) != 0;
                    bool changed = false, clocks_changed = false;
                    static const int cpu_choices[] = {111, 222, 333, 444};
                    static const int gpu_choices[] = {41, 77, 111, 166};
                    if (selection == 0 && (direction || activate)) {
                        settings.cpu_clock = cycle_value(settings.cpu_clock, cpu_choices, 4, direction < 0 ? -1 : 1);
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
                    } else if (selection == 6 && activate) {
                        status = "VITA RENDERER IS FIXED TO NATIVE GXM GPU FAST.";
                    } else if (selection == 7 && activate) {
                        status = "VITA OUTPUT IS FIXED FULLSCREEN AT 960 X 544.";
                    } else if (selection == 8 && activate) {
                        FILE *rom = std::fopen(kRom, "rb");
                        status = rom ? "ROM CHECK OK: UX0:DATA/DAYTONA93/DAYTONA93.ZIP"
                                     : "ROM MISSING: UX0:DATA/DAYTONA93/DAYTONA93.ZIP";
                        if (rom) std::fclose(rom);
                    } else if (selection == 9 && activate) {
                        status = "STEER=L-STICK  PEDALS=R-STICK/L/R  SHIFT=UP/DOWN";
                    } else if (selection == 10 && activate) {
                        settings.defaults(); changed = clocks_changed = true;
                    } else if (selection == 11 && activate) {
                        options = false; selection = 2; wait_release = true;
                    }
                    if (changed) commit_settings(clocks_changed);
                }
            } else {
                if (pressed & vita::Up) selection = (selection + 3) % 4;
                if (pressed & vita::Down) selection = (selection + 1) % 4;
                if ((pressed & vita::Circle) && game) {
                    menu = false; clock.reset(); wait_release = true;
                } else if (pressed & vita::Cross) {
                    if (selection == 0) {
                        if (game || start_game()) { menu = false; clock.reset(); wait_release = true; }
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
            const int frames = clock.advance(elapsed);
            for (int n = 0; n < frames; ++n) {
                const uint64_t begin = ticks_us();
                try {
                    game->run_frame_deferred_sound(map_input(controls.sample(wait_release ? vita::Pad{} : pad)));
                    sound_worker.dispatch(*game);
                    simulated = true;
                } catch (const std::exception &e) {
                    status = e.what(); log.log("GPU18 runtime: %s\n", e.what()); menu = true; game.reset(); break;
                }
                perf_run_us += ticks_us() - begin;
                // Sound runs concurrently with command generation below.
                // FrameClock is capped at one step; join before the next step.
            }
        } else clock.reset();

        const uint64_t gpu_begin = ticks_us();
        gpu.prepare_frame();
        const uint64_t gpu_ready = ticks_us();
        vita2d_start_drawing();
        vita2d_clear_screen();
        if (menu) draw_menu(bool(game), options, selection, settings, status, display_fps);
        else if (game) {
            if (gpu_fast) gpu.draw(game->board().video()); else gpu.draw_exact(game->board().video());
        }
        vita2d_end_drawing();
        vita2d_swap_buffers();
        if (!menu && game) {
            ++perf_presents;
            perf_wait_us += gpu_ready - gpu_begin;
            perf_encode_us += uint64_t(gpu.last_gpu_ms() * 1000.0);
            perf_gpu_us += ticks_us() - gpu_begin;
        }

        if (simulated && game) {
            const uint64_t wait_begin = ticks_us();
            try { sound_worker.finish(); }
            catch (const std::exception &e) {
                status = e.what();
                log.log("GPU18 sound runtime: %s\n", e.what());
                menu = true; audio.pause(); game.reset();
            }
            perf_sound_wait_us += ticks_us() - wait_begin;
            if (game) {
                const auto &fp = game->last_profile();
                perf_core_us += fp.core(); perf_geo_us += fp.geometry;
                perf_video_us += fp.video; perf_sound_us += fp.sound;
                const uint64_t queue_begin = ticks_us();
                if (game->sound()) audio.push(*game->sound());
                perf_audio_queue_us += ticks_us() - queue_begin;
                ++perf_frames;
            }
        }
        const uint64_t after = ticks_us();
        if (after - perf_start >= 2000000) {
            const double sec = double(after - perf_start) / 1000000.0;
            display_fps = sec > 0 ? double(perf_frames) / sec : 0.0;
            const double run_ms = perf_frames ? double(perf_run_us) / perf_frames / 1000.0 : 0.0;
            const double present_divisor = perf_presents ? double(perf_presents) : 1.0;
            const double gpu_ms = double(perf_gpu_us) / present_divisor / 1000.0;
            const double frame_divisor = perf_frames ? double(perf_frames) : 1.0;
            if (game) {
                const auto &vp = game->board().video().last_profile();
                log.log("gpu18: mode=%s sim_fps=%.2f run_ms=%.2f core_ms=%.2f geo_ms=%.2f video_ms=%.2f sound_ms=%.2f gpu_submit_ms=%.2f gpu_encode_ms=%.2f gpu_wait_ms=%.2f sound_wait_ms=%.2f audio_queue_ms=%.2f cpu_raster_ms=%.2f tile_cache_ms=%.2f tile_draw_ms=%.2f compose_ms=%.2f layers=%u tiles=%u chars=%u materials=%u sources=%u builds=%u defers=%u cache_mb=%.2f cache_reserved_mb=%.2f pool_free_kb=%u pool_drops=%u material_drops=%u subdiv_polys=%u vertices=%u tile_uploads=%u cache_resets=%u tile_quads=%u draws=%u/%u clips=%u polys=%u/%u shader_setups=%u state_reuses=%u draw_errors=%u sys24_ctrl=%04x/%04x\n",
                        gpu_fast ? "GPU_FAST" : "CPU_EXACT", display_fps, run_ms,
                        double(perf_core_us)/frame_divisor/1000.0, double(perf_geo_us)/frame_divisor/1000.0,
                        double(perf_video_us)/frame_divisor/1000.0, double(perf_sound_us)/frame_divisor/1000.0, gpu_ms, double(perf_encode_us)/present_divisor/1000.0, double(perf_wait_us)/present_divisor/1000.0, double(perf_sound_wait_us)/frame_divisor/1000.0, double(perf_audio_queue_us)/frame_divisor/1000.0,
                        double(vp.raster)/1000.0, double(vp.tile_cache)/1000.0, double(vp.tile_draw)/1000.0,
                        double(vp.composite)/1000.0, unsigned(vp.layers_rebuilt), vp.tiles_rebuilt, vp.characters_changed,
                        unsigned(gpu.cached_materials()), unsigned(gpu.cached_sources()), gpu.material_builds(), gpu.material_defers(), double(gpu.cached_bytes())/(1024.0*1024.0),
                        double(gpu.reserved_bytes())/(1024.0*1024.0), gpu.min_pool_free()/1024u, gpu.pool_drops(), gpu.material_drops(), gpu.subdivided_polys(), unsigned(gpu.submitted_vertices()), gpu.system24_uploaded_tiles(), gpu.cache_resets(), gpu.system24_quads(), gpu.textured_draws(), gpu.solid_draws(), gpu.clip_changes(), gpu.textured_polys(), gpu.solid_polys(), gpu.shader_setups(), gpu.state_reuses(), gpu.draw_errors(),
                        unsigned(game->board().video().system24_word(0x5004)),
                        unsigned(game->board().video().system24_word(0x5006)));
            }
            perf_start = after; perf_frames = perf_run_us = perf_gpu_us = 0;
            perf_core_us = perf_geo_us = perf_video_us = perf_sound_us = 0;
            perf_presents = perf_wait_us = perf_encode_us = 0;
            perf_sound_wait_us = perf_audio_queue_us = 0;
        }
        if (menu || !simulated) SDL_Delay(1);
    }

    sound_worker.close();
    save();
    settings.save();
    audio.close();
    gpu.shutdown();
    restore_clocks();
    vita2d_fini();
    SDL_Quit();
    log.literal("GPU18: clean exit\n");
    return 0;
}
