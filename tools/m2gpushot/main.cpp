// m2gpushot: the game headless with the hardware renderer, saving frames as
// raw dumps (scripts/rgb2png.py converts them). Same inputs, same frames as
// m2run, whose dumps come from the software renderer: compare the two.
//
//   m2gpushot IMAGES_DIR FRAMES --dump DIR --every N [--inputs scripts/inputs/X.txt]
//
// SDL_GPU without a window: an offscreen texture is drawn and read back.

#include "app/gpu/gpu_renderer.h"
#include "runtime/game_loop.h"
#include "../common/input_script.h"

#include <SDL3/SDL.h>

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
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--dump")) dump_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--every")) every = std::strtoull(argv[i + 1], nullptr, 10);
        else if (!std::strcmp(argv[i], "--inputs")) inputs_path = argv[i + 1];
    }
    if (dump_dir.empty() || !every) {
        std::fprintf(stderr, "m2gpushot: --dump DIR and --every N are needed\n");
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
    constexpr int W = rt::GameLoop::kWidth, H = rt::GameLoop::kHeight;
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = W;
    ti.height = H;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    SDL_GPUTexture *target = SDL_CreateGPUTexture(dev, &ti);
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = W * H * 4;
    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!target || !download) {
        std::fprintf(stderr, "m2gpushot: %s\n", SDL_GetError());
        return 1;
    }

    try {
        rt::GameLoop game(dir);
        game.board().video().set_external_3d(true, true);
        tools::Script script;
        if (!inputs_path.empty()) script.load(inputs_path);
        for (uint64_t f = 0; f < frames; f++) {
            game.run_frame(script.at(game.board().frame()));
            if (game.board().frame() % every) continue;
            SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
            gpu.render(cmd, target, W, H, game.board().video());
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureRegion src{};
            src.texture = target;
            src.w = W;
            src.h = H;
            src.d = 1;
            SDL_GPUTextureTransferInfo dst{};
            dst.transfer_buffer = download;
            dst.pixels_per_row = W;
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
