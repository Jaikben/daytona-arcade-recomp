// daytona: the recompiled game in a window. SDL3 for the window, input and
// GPU (SDL_GPU: Vulkan or Direct3D 12 on Windows, Vulkan on Linux, Metal on
// macOS). The game runs on the native board runtime (rt::GameLoop); each
// composed frame is uploaded to a GPU texture and scaled onto the swapchain.
//
//   daytona [--images DIR] [--gpu vulkan|direct3d12|metal] [--frames N] [--fullscreen]
//
// Controls (keyboard): Left/Right steer, Up or Z accelerate, Down or X brake,
// 1-4 gears, A S D F view buttons (VR1-VR4), 5 coin, Enter start, F2 test,
// F3 service, F11 fullscreen, Esc quit. Gamepad: left stick steer, right
// trigger accelerate, left trigger brake, shoulders shift down/up, Start
// start, Back coin, face buttons view buttons.

#include "runtime/game_loop.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace {

constexpr double kArcadeHz = 57.52; // 16 MHz / (656 x 424), the board's frame rate

struct Controls {
    float steer = 0; // -1..1
    int gear = 1;    // 1-4
    rt::Inputs to_inputs(const bool *keys, SDL_Gamepad *pad) {
        rt::Inputs in;
        // steering: keyboard ramps, the stick is direct
        float target = 0;
        if (keys[SDL_SCANCODE_LEFT]) target -= 1;
        if (keys[SDL_SCANCODE_RIGHT]) target += 1;
        float accel = (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_Z]) ? 1.f : 0.f;
        float brake = (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_X]) ? 1.f : 0.f;
        if (pad) {
            const float sx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.f;
            if (std::fabs(sx) > 0.08f) target = sx;
            accel = std::max(accel, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.f);
            brake = std::max(brake, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.f);
        }
        steer += std::clamp(target - steer, -0.12f, 0.12f);
        auto adc = [](float v) { return uint8_t(std::lround(0x80 + std::clamp(v, -1.f, 1.f) * 0x60)); }; // 0x20-0xe0
        auto pedal = [](float v) { return uint8_t(std::lround(0x20 + std::clamp(v, 0.f, 1.f) * 0xc0)); };
        in.steer = adc(steer);
        in.accel = pedal(accel);
        in.brake = pedal(brake);

        auto btn = [&](SDL_GamepadButton b) { return pad && SDL_GetGamepadButton(pad, b); };
        auto low = [](uint8_t &port, uint8_t bit, bool on) { if (on) port &= uint8_t(~bit); };
        low(in.in0, 0x01, keys[SDL_SCANCODE_5] || btn(SDL_GAMEPAD_BUTTON_BACK));   // coin
        low(in.in0, 0x04, keys[SDL_SCANCODE_F2]);                                   // test
        low(in.in0, 0x08, keys[SDL_SCANCODE_F3]);                                   // service
        low(in.in0, 0x10, keys[SDL_SCANCODE_RETURN] || btn(SDL_GAMEPAD_BUTTON_START)); // start
        low(in.in0, 0x20, keys[SDL_SCANCODE_A] || btn(SDL_GAMEPAD_BUTTON_SOUTH));  // VR1
        low(in.in0, 0x40, keys[SDL_SCANCODE_S] || btn(SDL_GAMEPAD_BUTTON_EAST));   // VR2
        low(in.in0, 0x80, keys[SDL_SCANCODE_D] || btn(SDL_GAMEPAD_BUTTON_WEST));   // VR3
        low(in.in1, 0x01, keys[SDL_SCANCODE_F] || btn(SDL_GAMEPAD_BUTTON_NORTH));  // VR4
        for (int g = 1; g <= 4; g++)
            if (keys[SDL_SCANCODE_1 + (g - 1)]) gear = g;
        static const uint8_t gearvalue[5] = {0, 2, 1, 6, 5}; // MAME daytona_gearbox_r: neutral, 1-4
        in.in1 = uint8_t((in.in1 & ~0x70) | (gearvalue[gear] << 4));
        return in;
    }
    void shift(int d) { gear = std::clamp(gear + d, 1, 4); }
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
    std::string images = "build/rom_cache/daytona93", gpu;
    uint64_t max_frames = 0;
    bool fullscreen = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--images") && i + 1 < argc) images = argv[++i];
        else if (!std::strcmp(argv[i], "--gpu") && i + 1 < argc) gpu = argv[++i];
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--fullscreen")) fullscreen = true;
    }

    if (!gpu.empty()) SDL_SetHint(SDL_HINT_GPU_DRIVER, gpu.c_str());
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return fail("SDL_Init");

    try {
        rt::GameLoop game(images);
        const std::string eeprom_path = pref_file("ioboard_eeprom.bin"), backup_path = pref_file("backup_ram.bin");
        load_file(eeprom_path, game.board().io().eeprom);
        load_file(backup_path, game.board().backup_ram());

        constexpr int W = rt::GameLoop::kWidth, H = rt::GameLoop::kHeight;
        SDL_Window *window = SDL_CreateWindow("Daytona USA", W * 2, H * 2,
                                              SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
        if (!window) return fail("SDL_CreateWindow");
        SDL_GPUDevice *dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
                                                 false, nullptr);
        if (!dev) return fail("SDL_CreateGPUDevice");
        if (!SDL_ClaimWindowForGPUDevice(dev, window)) return fail("SDL_ClaimWindowForGPUDevice");
        std::printf("daytona: GPU driver %s\n", SDL_GetGPUDeviceDriver(dev));

        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; // the screen's 0xAARRGGBB words, little-endian
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = W;
        ti.height = H;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        SDL_GPUTexture *screen = SDL_CreateGPUTexture(dev, &ti);
        SDL_GPUTransferBufferCreateInfo tbi{};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = W * H * 4;
        SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(dev, &tbi);
        if (!screen || !upload) return fail("SDL_CreateGPUTexture");

        SDL_Gamepad *pad = nullptr;
        Controls controls;
        bool running = true;
        uint64_t last = SDL_GetTicksNS();
        double pending = 0;
        const double frame_ns = 1e9 / kArcadeHz;
        bool new_frame = false;

        while (running) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) running = false;
                else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) running = false;
                else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11)
                    SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                else if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(e.gdevice.which);
                else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    if (e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) controls.shift(-1);
                    if (e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) controls.shift(+1);
                }
            }

            // Arcade speed: advance the game at 57.52 frames/s, present at the display's rate.
            const uint64_t now = SDL_GetTicksNS();
            pending += double(now - last);
            last = now;
            pending = std::min(pending, frame_ns * 4); // don't spiral after a stall
            while (pending >= frame_ns) {
                game.run_frame(controls.to_inputs(SDL_GetKeyboardState(nullptr), pad));
                pending -= frame_ns;
                new_frame = true;
                if (max_frames && game.frames() >= max_frames) running = false;
            }

            SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
            if (!cmd) return fail("SDL_AcquireGPUCommandBuffer");
            if (new_frame) {
                void *p = SDL_MapGPUTransferBuffer(dev, upload, true);
                std::memcpy(p, game.screen().data(), size_t(W) * H * 4);
                SDL_UnmapGPUTransferBuffer(dev, upload);
                SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
                SDL_GPUTextureTransferInfo src{};
                src.transfer_buffer = upload;
                SDL_GPUTextureRegion dst{};
                dst.texture = screen;
                dst.w = W;
                dst.h = H;
                dst.d = 1;
                SDL_UploadToGPUTexture(copy, &src, &dst, true);
                SDL_EndGPUCopyPass(copy);
                new_frame = false;
            }
            SDL_GPUTexture *swap = nullptr;
            Uint32 sw = 0, sh = 0;
            if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &sw, &sh)) return fail("swapchain");
            if (swap) {
                // 4:3 inside the window, letterboxed
                const double scale = std::min(double(sw) / W, double(sh) / H);
                const Uint32 dw = Uint32(W * scale), dh = Uint32(H * scale);
                SDL_GPUBlitInfo blit{};
                blit.source.texture = screen;
                blit.source.w = W;
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
            SDL_SubmitGPUCommandBuffer(cmd);
        }

        save_file(eeprom_path, game.board().io().eeprom);
        save_file(backup_path, game.board().backup_ram());
        std::printf("daytona: %llu frames\n", (unsigned long long)game.frames());
        SDL_ReleaseGPUTransferBuffer(dev, upload);
        SDL_ReleaseGPUTexture(dev, screen);
        SDL_ReleaseWindowFromGPUDevice(dev, window);
        SDL_DestroyGPUDevice(dev);
        SDL_DestroyWindow(window);
        if (pad) SDL_CloseGamepad(pad);
    } catch (const std::exception &ex) {
        std::fprintf(stderr, "daytona: %s\n", ex.what());
        SDL_Quit();
        return 1;
    }
    SDL_Quit();
    return 0;
}
