// m2gpushot: the game headless with the hardware renderer, saving frames as
// raw dumps (scripts/rgb2png.py converts them). Same inputs, same frames as
// m2run, whose dumps come from the software renderer: compare the two.
//
//   m2gpushot IMAGES_DIR FRAMES --dump DIR --every N [--inputs scripts/inputs/X.txt]
//             [--aspect W:H [--hud-edges] [--stretch-backdrop]]
//   m2gpushot IMAGES_DIR FRAMES --bench [--inputs ...] [--aspect ...]
//
// --bench draws every frame through the GPU, with no readback, and reports
// the time in the game (logic, geometrizer, CPU tilemap layers), in the
// renderer on the CPU (vertices, uploads) and waiting for the GPU.
//
// SDL_GPU without a window: an offscreen texture is drawn and read back.

#include "app/gpu/gpu_renderer.h"
#include "runtime/game_loop.h"
#include "../common/input_script.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: m2gpushot IMAGES_DIR FRAMES --dump DIR --every N [--inputs FILE]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t frames = std::strtoull(argv[2], nullptr, 10);
    std::string dump_dir, inputs_path;
    uint64_t every = 0;
    double aspect = 0;
    bool hud_edges = false, stretch = false, bench = false;
    for (int i = 3; i < argc; i++) {
        if (!std::strcmp(argv[i], "--bench")) bench = true;
        if (!std::strcmp(argv[i], "--hud-edges")) hud_edges = true;
        if (!std::strcmp(argv[i], "--stretch-backdrop")) stretch = true;
    }
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--hud-edges") || !std::strcmp(argv[i], "--stretch-backdrop") ||
            !std::strcmp(argv[i], "--bench")) { i--; continue; }
        if (!std::strcmp(argv[i], "--aspect")) {
            double a = 0, b = 0;
            if (std::sscanf(argv[i + 1], "%lf:%lf", &a, &b) == 2 && b > 0) aspect = a / b;
        }
        if (!std::strcmp(argv[i], "--dump")) dump_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--every")) every = std::strtoull(argv[i + 1], nullptr, 10);
        else if (!std::strcmp(argv[i], "--inputs")) inputs_path = argv[i + 1];
    }
    if (!bench && (dump_dir.empty() || !every)) {
        std::fprintf(stderr, "m2gpushot: --dump DIR and --every N (or --bench) are needed\n");
        return 2;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "m2gpushot: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GPUDevice *dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
                                             false, nullptr);
    if (!dev) {
        std::fprintf(stderr, "m2gpushot: SDL_CreateGPUDevice: %s\n", SDL_GetError());
        return 1;
    }
    std::printf("m2gpushot: GPU driver %s\n", SDL_GetGPUDeviceDriver(dev));
    app::GpuRenderer gpu;
    if (!gpu.init(dev, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM)) {
        std::fprintf(stderr, "m2gpushot: %s\n", gpu.error().c_str());
        return 1;
    }
    constexpr int H = rt::GameLoop::kHeight;
    const int W = rt::GameLoop::kWidth + 2 * rt::GameLoop::wide_margin(aspect);
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = Uint32(W);
    ti.height = H;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    SDL_GPUTexture *target = SDL_CreateGPUTexture(dev, &ti);
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = Uint32(W) * H * 4;
    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!target || !download) {
        std::fprintf(stderr, "m2gpushot: %s\n", SDL_GetError());
        return 1;
    }

    try {
        rt::GameLoop game(dir);
        game.board().video().set_external_3d(true, true);
        game.set_aspect(aspect);
        game.set_hud_edges(hud_edges);
        game.set_stretch_backdrop(stretch);
        tools::Script script;
        if (!inputs_path.empty()) script.load(inputs_path);
        if (bench) {
            using clk = std::chrono::steady_clock;
            double t_game = 0, t_cpu = 0, t_wait = 0;
            SDL_GPUFence *last = nullptr;
            const auto t0 = clk::now();
            for (uint64_t f = 0; f < frames; f++) {
                const auto a = clk::now();
                game.run_frame(script.at(game.board().frame()));
                const auto b = clk::now();
                if (last) { // one frame in flight, as a window would have
                    SDL_WaitForGPUFences(dev, true, &last, 1);
                    SDL_ReleaseGPUFence(dev, last);
                }
                const auto c = clk::now();
                SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
                gpu.render(cmd, target, W, H, game.board().video());
                last = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
                const auto d = clk::now();
                t_game += std::chrono::duration<double>(b - a).count();
                t_wait += std::chrono::duration<double>(c - b).count();
                t_cpu += std::chrono::duration<double>(d - c).count();
            }
            if (last) SDL_WaitForGPUFences(dev, true, &last, 1), SDL_ReleaseGPUFence(dev, last);
            const double total = std::chrono::duration<double>(clk::now() - t0).count();
            std::printf("m2gpushot: %" PRIu64 " frames in %.2f s (%.0f frames/s); per frame: game %.2f ms, "
                        "renderer CPU %.2f ms, waiting for the GPU %.2f ms\n",
                        frames, total, double(frames) / total, t_game * 1e3 / double(frames),
                        t_cpu * 1e3 / double(frames), t_wait * 1e3 / double(frames));
            gpu.shutdown();
            SDL_DestroyGPUDevice(dev);
            return 0;
        }
        for (uint64_t f = 0; f < frames; f++) {
            game.run_frame(script.at(game.board().frame()));
            if (game.board().frame() % every) continue;
            SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
            gpu.render(cmd, target, W, H, game.board().video());
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureRegion src{};
            src.texture = target;
            src.w = Uint32(W);
            src.h = H;
            src.d = 1;
            SDL_GPUTextureTransferInfo dst{};
            dst.transfer_buffer = download;
            dst.pixels_per_row = Uint32(W);
            SDL_DownloadFromGPUTexture(copy, &src, &dst);
            SDL_EndGPUCopyPass(copy);
            SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            SDL_WaitForGPUFences(dev, true, &fence, 1);
            SDL_ReleaseGPUFence(dev, fence);
            char path[512];
            std::snprintf(path, sizeof path, "%s/run_%05" PRIu64 ".rgb", dump_dir.c_str(), game.board().frame());
            if (FILE *d = std::fopen(path, "wb")) {
                const void *p = SDL_MapGPUTransferBuffer(dev, download, false);
                std::fwrite(p, 4, size_t(W) * H, d);
                SDL_UnmapGPUTransferBuffer(dev, download);
                std::fclose(d);
            }
        }
        std::printf("m2gpushot: %" PRIu64 " frames\n", frames);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "m2gpushot: %s\n", e.what());
        return 1;
    }
    gpu.shutdown();
    SDL_ReleaseGPUTransferBuffer(dev, download);
    SDL_ReleaseGPUTexture(dev, target);
    SDL_DestroyGPUDevice(dev);
    SDL_Quit();
    return 0;
}
