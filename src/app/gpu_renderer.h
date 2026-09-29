// The Model 2 screen on the GPU (SDL_GPU: Vulkan, Direct3D 12, Metal): the 3D
// layer's polygons are drawn by shaders/poly.*.hlsl from the triangles
// rt::Video prepares (rt::prepare_gpu_frame), then composed with the 2D
// tilemap layers the CPU builds (shaders/composite.*.hlsl). The render scale
// multiplies the 3D layer's resolution; the output is 496x384 times it.
#pragma once

#include "runtime/video.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace app {

class GpuRenderer {
public:
    // Creates pipelines and targets. Returns false with the reason in err.
    bool init(SDL_GPUDevice *dev, int scale, std::string &err);
    void shutdown();

    // A new game frame: upload the 2D layers (and, when the frame has one, the
    // new 3D layer's triangles, colour tables and texture RAM), draw the 3D
    // layer, compose the screen into output().
    void render(SDL_GPUCommandBuffer *cmd, const rt::Video &video, const rt::VideoMem &mem);

    SDL_GPUTexture *output() const { return out_; }
    int width() const { return rt::Video::W * scale_; }
    int height() const { return rt::Video::H * scale_; }
    int scale() const { return scale_; }

    // Test aid: the 3D layer's visible 496x384 as 0x00RRGGBB (scale 1 only),
    // read back after the command buffer has been submitted and waited on.
    bool read_layer3d(std::vector<uint32_t> &out);

private:
    SDL_GPUShader *shader(const unsigned char *spirv, size_t spirv_n, const unsigned char *dxil, size_t dxil_n,
                          const unsigned char *msl, size_t msl_n, SDL_GPUShaderStage stage, int samplers, int storage);
    void upload(SDL_GPUCopyPass *copy, SDL_GPUTransferBuffer *&tb, uint32_t &tb_size, const void *data, uint32_t size,
                SDL_GPUBuffer *dst, uint32_t offset);
    void upload_texture(SDL_GPUCopyPass *copy, SDL_GPUTransferBuffer *tb, const void *data, SDL_GPUTexture *dst, int w, int h);

    SDL_GPUDevice *dev_ = nullptr;
    int scale_ = 1;
    SDL_GPUGraphicsPipeline *poly_ = nullptr, *composite_ = nullptr;
    SDL_GPUTexture *layer3d_ = nullptr, *depth_ = nullptr, *back_ = nullptr, *front_ = nullptr, *out_ = nullptr;
    SDL_GPUSampler *nearest_ = nullptr;
    SDL_GPUBuffer *vbuf_ = nullptr, *texram_ = nullptr, *vmem_ = nullptr;
    uint32_t vbuf_size_ = 0;
    SDL_GPUTransferBuffer *tb_back_ = nullptr, *tb_front_ = nullptr, *tb_verts_ = nullptr, *tb_tex_ = nullptr, *tb_vmem_ = nullptr;
    uint32_t tb_verts_size_ = 0, tb_tex_size_ = 0, tb_vmem_size_ = 0;
    SDL_GPUTransferBuffer *tb_read_ = nullptr;
    uint32_t verts_ = 0;
    bool layer3d_clear_ = true; // the 3D layer texture currently holds nothing
};

} // namespace app
