// Hardware renderer (SDL_GPU): draws a frame of the Model 2 3D layer on the
// GPU instead of the CPU rasterizer (src/runtime/raster.cpp, the exact
// reference), around the tilemap layers the CPU still draws (Video's
// external-3D mode: background and foreground layers).
//
// Stage 1: geometry. Polygons are projected exactly as model2_3d_project,
// drawn as a fan from their first vertex in the rasterizer's order (window,
// then z, newest first) and clipped to their window; the depth test stands in
// for the rasterizer's "first polygon to fill a pixel wins". Colour is the
// polygon's palette colour through the luma translation, as the rasterizer's
// solid renderer; textures come in stage 2.
#pragma once

#include "runtime/video.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace app {

class GpuRenderer {
public:
    // Creates the pipelines for drawing into textures of `format`. False (with
    // error()) if the GPU or its shader format is not usable.
    bool init(SDL_GPUDevice *dev, SDL_GPUTextureFormat format);
    void shutdown();
    bool ok() const { return poly_pipe_ != nullptr; }
    const std::string &error() const { return error_; }

    // Draws the frame into `target` (w x h, created with COLOR_TARGET usage):
    // the background layer, the 3D polygons, the foreground layer, all from
    // `video` in external-3D mode.
    void render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, int w, int h, const rt::Video &video);

private:
    struct PolyVertex {
        float x, y, depth;
        float r, g, b, a;
        float checker;
    };
    struct QuadVertex {
        float x, y, u, v;
    };
    struct Batch { // consecutive polygons sharing one clip rectangle
        uint32_t first, count;
        SDL_Rect clip;
    };

    SDL_GPUDevice *dev_ = nullptr;
    SDL_GPUGraphicsPipeline *poly_pipe_ = nullptr, *quad_pipe_ = nullptr;
    SDL_GPUSampler *sampler_ = nullptr;
    SDL_GPUTexture *layers_[2] = {nullptr, nullptr}; // background, foreground
    SDL_GPUTexture *depth_ = nullptr;
    int depth_w_ = 0, depth_h_ = 0;
    SDL_GPUBuffer *vbuf_ = nullptr, *qbuf_ = nullptr;
    uint32_t vbuf_size_ = 0;
    SDL_GPUTransferBuffer *upload_ = nullptr;
    uint32_t upload_size_ = 0;
    std::vector<PolyVertex> verts_;
    std::vector<Batch> batches_;
    std::vector<size_t> order_;
    uint8_t gamma_[256];
    std::string error_;

    SDL_GPUShader *shader(const unsigned char *spv, size_t spv_len, const unsigned char *dxil, size_t dxil_len,
                          const char *msl, const char *dxil_entry, SDL_GPUShaderStage stage, int uniforms, int samplers);
    bool ensure(int w, int h, uint32_t vert_bytes);
    void build(const rt::Video &video, int w, int h);
};

} // namespace app
