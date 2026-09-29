// Native Vita frontend: shared board/recompiled code, SDL2 presentation/audio.
#define SDL_MAIN_HANDLED
#include "audio.h"
#include "controls.h"
#include "text.h"
#include "runtime/game_loop.h"
#include "runtime/rom_import.h"
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" { int _newlib_heap_size_user = 256 * 1024 * 1024; }
namespace {
constexpr const char *kDirectory = "ux0:data/daytona93";
constexpr const char *kRom = "ux0:data/daytona93/daytona93.zip";
constexpr int kDisplayWidth = 960, kDisplayHeight = 544;

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
    for (auto entry : map) if (native.buttons & entry.native) pad.buttons |= entry.portable;
    return pad;
}

bool load_bytes(const std::string &path, uint8_t *data, size_t size) {
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    // Bound the allocation even if a save has been replaced by a huge file.
    std::vector<uint8_t> temporary(size);
    const bool ok = std::fread(temporary.data(), 1, size, file) == size &&
                    std::fgetc(file) == EOF && !std::ferror(file);
    std::fclose(file);
    if (ok) std::copy(temporary.begin(), temporary.end(), data);
    return ok;
}
bool save_bytes(const std::string &path, const uint8_t *data, size_t size) {
    const std::string temporary = path + ".tmp", backup = path + ".bak";
    std::FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    bool ok = std::fwrite(data, 1, size, file) == size;
    if (std::fflush(file) != 0) ok = false;
    if (std::fclose(file) != 0) ok = false;
    if (!ok) return false;
    // Vita rename need not replace an existing destination. Retain one
    // complete previous generation and restore it if the final rename fails.
    bool had_old = false;
    if (auto *old = std::fopen(path.c_str(), "rb")) {
        std::fclose(old);
        std::remove(backup.c_str());
        if (std::rename(path.c_str(), backup.c_str()) != 0) return false;
        had_old = true;
    }
    if (std::rename(temporary.c_str(), path.c_str()) == 0) return true;
    if (had_old) std::rename(backup.c_str(), path.c_str());
    return false;
}
template<class C> void load_nv(const std::string &name, C &data) {
    const std::string path = std::string(kDirectory) + "/" + name;
    if (!load_bytes(path, data.data(), data.size()))
        load_bytes(path + ".bak", data.data(), data.size());
}
void sdl_check(bool ok, const char *operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}

int run_app() {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    using Window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
    using Renderer = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;
    using Texture = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;
    Window window(SDL_CreateWindow("Daytona Recomp", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                   kDisplayWidth, kDisplayHeight, SDL_WINDOW_FULLSCREEN), SDL_DestroyWindow);
    sdl_check(bool(window), "create window");
    Renderer renderer(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC), SDL_DestroyRenderer);
    if (!renderer) renderer.reset(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_ACCELERATED));
    sdl_check(bool(renderer), "create Vita renderer");
    SDL_RendererInfo info{};
    SDL_GetRendererInfo(renderer.get(), &info);
    std::fprintf(stderr, "renderer: %s\n", info.name ? info.name : "unknown");
    sdl_check(SDL_RenderSetLogicalSize(renderer.get(), kDisplayWidth, kDisplayHeight) == 0, "logical size");
    Texture screen(SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                      rt::GameLoop::kWidth, rt::GameLoop::kHeight), SDL_DestroyTexture);
    sdl_check(bool(screen), "create screen texture");
    SDL_SetTextureBlendMode(screen.get(), SDL_BLENDMODE_NONE);
    vita::Audio audio;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0 || !audio.open())
        std::fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
    uint8_t mute_setting = 0;
    const std::string settings = std::string(kDirectory) + "/mute.bin";
    if (!load_bytes(settings, &mute_setting, 1)) load_bytes(settings + ".bak", &mute_setting, 1);
    bool muted = mute_setting != 0;
    audio.mute(muted);

    std::unique_ptr<rt::GameLoop> game;
    std::vector<uint8_t> saved_eeprom, saved_backup;
    vita::Controls controls;
    vita::FrameClock clock(rt::GameLoop::kFrameHz);
    bool running = true, menu = true, have_frame = false, wait_release = true;
    bool background = false;
    int selection = 0;
    uint8_t pulse = 0;
    uint32_t previous_buttons = 0;
    const double frequency = double(SDL_GetPerformanceFrequency());
    uint64_t previous_counter = SDL_GetPerformanceCounter();
    double save_elapsed = 0;
    std::string status = "Place your daytona93.zip in ux0:data/daytona93/ then select START GAME.";

    auto save = [&]() {
        if (!game) return true;
        auto save_changed = [&](const char *name, const auto &data, std::vector<uint8_t> &previous) {
            if (data.size() == previous.size() && std::equal(data.begin(), data.end(), previous.begin())) return true;
            if (!save_bytes(std::string(kDirectory) + "/" + name, data.data(), data.size())) return false;
            previous.assign(data.begin(), data.end());
            return true;
        };
        // Do not short-circuit: attempt both saves even if one failed.
        const bool eeprom_ok = save_changed("ioboard_eeprom.bin", game->board().io().eeprom, saved_eeprom);
        const bool backup_ok = save_changed("backup_ram.bin", game->board().backup_ram(), saved_backup);
        if (!eeprom_ok || !backup_ok) {
            status = "Save failed. Check free space and ux0:data/daytona93/. Previous saves are kept as .bak.";
            std::fprintf(stderr, "%s\n", status.c_str());
        }
        return eeprom_ok && backup_ok;
    };
    auto draw_menu = [&]() {
        SDL_SetRenderDrawColor(renderer.get(), 16, 18, 24, 255);
        SDL_RenderClear(renderer.get());
        SDL_SetRenderDrawColor(renderer.get(), 235, 235, 235, 255);
        vita::text(renderer.get(), "DAYTONA RECOMP - PS VITA", 30, 28, 3, 49, 1);
        const std::string labels[] = {game ? "RESUME GAME" : "START GAME", "RESET GAME", "TEST SWITCH", "SERVICE COIN",
                                      muted ? "SOUND: MUTED" : "SOUND: ON", "SAVE AND QUIT"};
        for (int i = 0; i < 6; ++i) {
            if (i == selection) SDL_SetRenderDrawColor(renderer.get(), 255, 200, 70, 255);
            else SDL_SetRenderDrawColor(renderer.get(), 220, 220, 225, 255);
            vita::text(renderer.get(), (i == selection ? "> " : "  ") + labels[i], 42, 108 + i * 38, 2, 70, 1);
        }
        SDL_SetRenderDrawColor(renderer.get(), 220, 220, 225, 255);
        vita::text(renderer.get(), status, 30, 364, 2, 74, 7);
        vita::text(renderer.get(), "CROSS: SELECT  CIRCLE: RESUME  START+SELECT: MENU", 30, 516, 2, 74, 1);
    };
    auto reset_clock = [&]() {
        clock.reset(); previous_counter = SDL_GetPerformanceCounter(); wait_release = true;
    };
    auto open_menu = [&]() {
        menu = true; audio.pause(); save(); reset_clock();
    };
    auto start = [&]() {
        if (!save()) return false;
        game.reset(); have_frame = false; audio.pause();
        status = "Loading and checking your ROM set...";
        draw_menu(); SDL_RenderPresent(renderer.get());
        try {
            game = std::make_unique<rt::GameLoop>(rt::import_rom_set(kRom));
            load_nv("ioboard_eeprom.bin", game->board().io().eeprom);
            load_nv("backup_ram.bin", game->board().backup_ram());
            saved_eeprom.assign(game->board().io().eeprom.begin(), game->board().io().eeprom.end());
            saved_backup = game->board().backup_ram();
            controls = vita::Controls{};
            pulse = 0; save_elapsed = 0;
            status = "L/R: BRAKE/GAS. LEFT STICK: STEER. RIGHT STICK: ANALOG PEDALS. UP/DOWN: GEARS. FACE BUTTONS: VIEWS.";
            reset_clock();
            return true;
        } catch (const std::exception &error) {
            status = error.what(); std::fprintf(stderr, "start: %s\n", error.what());
            reset_clock();
            return false;
        }
    };

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            else if (event.type == SDL_APP_WILLENTERBACKGROUND) { background = true; open_menu(); }
            else if (event.type == SDL_APP_DIDENTERFOREGROUND) { background = false; reset_clock(); }
        }
        if (!running) break;
        if (background) { SDL_Delay(20); continue; }
        const auto pad = read_pad();
        uint32_t pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;
        if (wait_release) {
            pressed = 0;
            controls.latch(pad.buttons);
            if (!pad.buttons) wait_release = false;
        }
        if (!menu && !wait_release && vita::menu_chord(pad.buttons)) open_menu();
        if (menu && !wait_release) {
            if (pressed & vita::Up) selection = (selection + 5) % 6;
            if (pressed & vita::Down) selection = (selection + 1) % 6;
            if ((pressed & vita::Circle) && game) { menu = false; reset_clock(); }
            else if (pressed & vita::Cross) {
                switch (selection) {
                case 0: if (game || start()) { menu = false; reset_clock(); } break;
                case 1: if (start()) { menu = false; reset_clock(); } break;
                case 2: case 3:
                    if (game) { pulse = selection == 2 ? 0x04 : 0x08; menu = false; reset_clock(); }
                    else status = "Start the game before using the cabinet switches.";
                    break;
                case 4:
                    muted = !muted; audio.mute(muted); mute_setting = muted ? 1 : 0;
                    if (!save_bytes(settings, &mute_setting, 1)) status = "Unable to save sound setting.";
                    break;
                case 5: if (save()) running = false; break;
                }
            }
        }
        const uint64_t now = SDL_GetPerformanceCounter();
        const double elapsed = double(now - previous_counter) / frequency;
        previous_counter = now;
        if (game && !menu) {
            const int frames = clock.advance(elapsed);
            try {
                for (int n = 0; n < frames; ++n) {
                    const auto input = controls.sample(wait_release ? vita::Pad{} : pad);
                    rt::Inputs mapped;
                    mapped.steer = input.steer; mapped.accel = input.accel; mapped.brake = input.brake;
                    mapped.in0 = uint8_t(input.in0 & ~pulse); mapped.in1 = input.in1; mapped.in2 = input.in2;
                    game->run_frame(mapped); pulse = 0;
                    if (game->sound()) audio.push(*game->sound());
                    have_frame = true;
                }
                if (frames) sdl_check(SDL_UpdateTexture(screen.get(), nullptr, game->screen().data(),
                                                       rt::GameLoop::kWidth * int(sizeof(uint32_t))) == 0, "upload screen");
                save_elapsed += elapsed;
                if (save_elapsed >= 5.0) { save(); save_elapsed = 0; }
            } catch (const std::exception &error) {
                status = error.what(); std::fprintf(stderr, "runtime: %s\n", error.what());
                open_menu(); game.reset(); have_frame = false;
            }
        } else clock.reset();
        if (menu) draw_menu();
        else {
            SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 255);
            SDL_RenderClear(renderer.get());
            if (have_frame) {
                // Preserve the same square-pixel framebuffer aspect as desktop.
                const int width = kDisplayHeight * rt::GameLoop::kWidth / rt::GameLoop::kHeight;
                const SDL_Rect destination{(kDisplayWidth - width) / 2, 0, width, kDisplayHeight};
                SDL_RenderCopy(renderer.get(), screen.get(), nullptr, &destination);
            }
        }
        SDL_RenderPresent(renderer.get());
        SDL_Delay(1); // also bounds polling when the driver cannot enable vsync
    }
    save();
    return 0;
}
} // namespace

int main(int, char **) {
    sceIoMkdir(kDirectory, 0777);
    if (std::freopen("ux0:data/daytona93/vita.log", "w", stderr)) std::setvbuf(stderr, nullptr, _IOLBF, 0);
    std::fprintf(stderr, "Daytona Vita frontend starting\n");
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    int result = 0;
    try { result = run_app(); }
    catch (const std::exception &error) { std::fprintf(stderr, "fatal: %s\n", error.what()); result = 1; }
    SDL_Quit();
    return result;
}
