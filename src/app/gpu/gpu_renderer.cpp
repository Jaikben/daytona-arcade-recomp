#include "app/gpu/gpu_renderer.h"

#include "app/gpu/shaders_gen.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace app {

namespace {

inline uint16_t le16(const uint8_t *base, uint32_t index) { return uint16_t(base[index * 2] | base[index * 2 + 1] << 8); }

} // namespace

SDL_GPUShader *GpuRenderer::shader(const unsigned char *spv, size_t spv_len, const unsigned char *dxil, size_t dxil_len,
                                   const char *msl, const char *dxil_entry, SDL_GPUShaderStage stage, int uniforms,
                                   int samplers) {
    SDL_GPUShaderCreateInfo si{};
    si.stage = stage;
    si.num_uniform_buffers = Uint32(uniforms);
    si.num_samplers = Uint32(samplers);
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(dev_);
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        si.format = SDL_GPU_SHADERFORMAT_SPIRV, si.code = spv, si.code_size = spv_len, si.entrypoint = "main";
    } else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
        si.format = SDL_GPU_SHADERFORMAT_DXIL, si.code = dxil, si.code_size = dxil_len, si.entrypoint = dxil_entry;
    } else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
        si.format = SDL_GPU_SHADERFORMAT_MSL, si.code = reinterpret_cast<const Uint8 *>(msl),
        si.code_size = std::strlen(msl), si.entrypoint = "main0";
    } else {
        error_ = "no supported shader format (SPIR-V, DXIL or MSL)";
        return nullptr;
    }
    SDL_GPUShader *s = SDL_CreateGPUShader(dev_, &si);
    if (!s) error_ = std::string("SDL_CreateGPUShader: ") + SDL_GetError();
    return s;
}

bool GpuRenderer::init(SDL_GPUDevice *dev, SDL_GPUTextureFormat format) {
    dev_ = dev;
    for (int i = 0; i < 256; i++) // the rasterizer's gamma (MAME video_start)
        gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));

    SDL_GPUShader *vs_poly = shader(k_vs_poly_spv, sizeof k_vs_poly_spv, k_vs_poly_dxil, sizeof k_vs_poly_dxil,
                                    k_vs_poly_msl, "vs_poly", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
    SDL_GPUShader *ps_poly = shader(k_ps_poly_spv, sizeof k_ps_poly_spv, k_ps_poly_dxil, sizeof k_ps_poly_dxil,
                                    k_ps_poly_msl, "ps_poly", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    SDL_GPUShader *vs_quad = shader(k_vs_quad_spv, sizeof k_vs_quad_spv, k_vs_quad_dxil, sizeof k_vs_quad_dxil,
                                    k_vs_quad_msl, "vs_quad", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
    SDL_GPUShader *ps_quad = shader(k_ps_quad_spv, sizeof k_ps_quad_spv, k_ps_quad_dxil, sizeof k_ps_quad_dxil,
                                    k_ps_quad_msl, "ps_quad", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    if (!vs_poly || !ps_poly || !vs_quad || !ps_quad) {
        for (SDL_GPUShader *s : {vs_poly, ps_poly, vs_quad, ps_quad})
            if (s) SDL_ReleaseGPUShader(dev_, s);
        return false;
    }

    SDL_GPUColorTargetDescription color{};
    color.format = format;

    // Polygons: position, depth (draw order), colour, checker flag.
    SDL_GPUVertexBufferDescription pvb{};
    pvb.slot = 0;
    pvb.pitch = sizeof(PolyVertex);
    pvb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute pattr[4] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(PolyVertex, x))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, Uint32(offsetof(PolyVertex, depth))},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, Uint32(offsetof(PolyVertex, r))},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, Uint32(offsetof(PolyVertex, checker))},
    };
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vs_poly;
    pi.fragment_shader = ps_poly;
    pi.vertex_input_state.vertex_buffer_descriptions = &pvb;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = pattr;
    pi.vertex_input_state.num_vertex_attributes = 4;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.depth_stencil_state.enable_depth_test = true;
    pi.depth_stencil_state.enable_depth_write = true;
    pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS; // earlier in the order wins
    pi.target_info.color_target_descriptions = &color;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    pi.target_info.has_depth_stencil_target = true;
    poly_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &pi);

    // Layers: a textured quad; no depth test.
    SDL_GPUVertexBufferDescription qvb{};
    qvb.slot = 0;
    qvb.pitch = sizeof(QuadVertex);
    qvb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute qattr[2] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(QuadVertex, x))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, Uint32(offsetof(QuadVertex, u))},
    };
    SDL_GPUGraphicsPipelineCreateInfo qi{};
    qi.vertex_shader = vs_quad;
    qi.fragment_shader = ps_quad;
    qi.vertex_input_state.vertex_buffer_descriptions = &qvb;
    qi.vertex_input_state.num_vertex_buffers = 1;
    qi.vertex_input_state.vertex_attributes = qattr;
    qi.vertex_input_state.num_vertex_attributes = 2;
    qi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    qi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    qi.target_info.color_target_descriptions = &color;
    qi.target_info.num_color_targets = 1;
    qi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    qi.target_info.has_depth_stencil_target = true; // same pass as the polygons
    quad_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &qi);

    for (SDL_GPUShader *s : {vs_poly, ps_poly, vs_quad, ps_quad}) SDL_ReleaseGPUShader(dev_, s);
    if (!poly_pipe_ || !quad_pipe_) {
        error_ = std::string("SDL_CreateGPUGraphicsPipeline: ") + SDL_GetError();
        shutdown();
        return false;
    }

    SDL_GPUSamplerCreateInfo smp{}; // layers are drawn 1:1: nearest
    smp.min_filter = smp.mag_filter = SDL_GPU_FILTER_NEAREST;
    smp.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    smp.address_mode_u = smp.address_mode_v = smp.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(dev_, &smp);

    SDL_GPUBufferCreateInfo qb{};
    qb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    qb.size = sizeof(QuadVertex) * 6;
    qbuf_ = SDL_CreateGPUBuffer(dev_, &qb);
    if (!sampler_ || !qbuf_) {
        error_ = std::string("SDL_GPU setup: ") + SDL_GetError();
        shutdown();
        return false;
    }
    return true;
}

void GpuRenderer::shutdown() {
    if (!dev_) return;
    if (poly_pipe_) SDL_ReleaseGPUGraphicsPipeline(dev_, poly_pipe_), poly_pipe_ = nullptr;
    if (quad_pipe_) SDL_ReleaseGPUGraphicsPipeline(dev_, quad_pipe_), quad_pipe_ = nullptr;
    if (sampler_) SDL_ReleaseGPUSampler(dev_, sampler_), sampler_ = nullptr;
    for (auto &t : layers_)
        if (t) SDL_ReleaseGPUTexture(dev_, t), t = nullptr;
    if (depth_) SDL_ReleaseGPUTexture(dev_, depth_), depth_ = nullptr;
    if (vbuf_) SDL_ReleaseGPUBuffer(dev_, vbuf_), vbuf_ = nullptr;
    if (qbuf_) SDL_ReleaseGPUBuffer(dev_, qbuf_), qbuf_ = nullptr;
    if (upload_) SDL_ReleaseGPUTransferBuffer(dev_, upload_), upload_ = nullptr;
    vbuf_size_ = upload_size_ = 0;
    depth_w_ = depth_h_ = 0;
}

// Textures and buffers big enough for this frame.
bool GpuRenderer::ensure(int w, int h, uint32_t vert_bytes) {
    if (w != depth_w_ || h != depth_h_) {
        for (auto &t : layers_)
            if (t) SDL_ReleaseGPUTexture(dev_, t), t = nullptr;
        if (depth_) SDL_ReleaseGPUTexture(dev_, depth_), depth_ = nullptr;
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.width = Uint32(w);
        ti.height = Uint32(h);
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; // the layers' 0xAARRGGBB words
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        for (auto &t : layers_) t = SDL_CreateGPUTexture(dev_, &ti);
        ti.format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        depth_ = SDL_CreateGPUTexture(dev_, &ti);
        if (!layers_[0] || !layers_[1] || !depth_) return false;
        depth_w_ = w, depth_h_ = h;
    }
    if (vert_bytes > vbuf_size_) {
        if (vbuf_) SDL_ReleaseGPUBuffer(dev_, vbuf_);
        vbuf_size_ = std::max<uint32_t>(vert_bytes, 1u << 20) * 2;
        SDL_GPUBufferCreateInfo bi{};
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = vbuf_size_;
        vbuf_ = SDL_CreateGPUBuffer(dev_, &bi);
        if (!vbuf_) return false;
    }
    const uint32_t need = uint32_t(w) * uint32_t(h) * 4 * 2 + uint32_t(sizeof(QuadVertex) * 6) + vert_bytes;
    if (need > upload_size_) {
        if (upload_) SDL_ReleaseGPUTransferBuffer(dev_, upload_);
        upload_size_ = need * 2;
        SDL_GPUTransferBufferCreateInfo tb{};
        tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tb.size = upload_size_;
        upload_ = SDL_CreateGPUTransferBuffer(dev_, &tb);
        if (!upload_) return false;
    }
    return true;
}

// The polygons as triangles, in the rasterizer's order, batched by clip rectangle.
void GpuRenderer::build(const rt::Video &video, int w, int h) {
    verts_.clear();
    batches_.clear();
    const std::vector<rt::GeoPoly> &polys = video.gpu_polys();
    const rt::VideoMem &mem = video.gpu_mem();
    const int windows = video.gpu_windows();
    const int crtc_x = video.crtc_x(), crtc_y = video.crtc_y(), render_x = video.render_x(), render_y = video.render_y();
    if (polys.empty() || !mem.palram || !mem.colorxlat) return;

    // Raster::render: windows from the last down to 0, low z first, newest first within z.
    order_.resize(polys.size());
    std::iota(order_.begin(), order_.end(), size_t(0));
    std::sort(order_.begin(), order_.end(), [&](size_t a, size_t b) {
        if (polys[a].window != polys[b].window) return polys[a].window > polys[b].window;
        if (polys[a].z != polys[b].z) return polys[a].z < polys[b].z;
        return a > b;
    });
    const float depth_step = 1.0f / float(order_.size() + 2);
    uint32_t drawn = 0;
    for (size_t i : order_) {
        const rt::GeoPoly &poly = polys[i];
        if (poly.window > windows || poly.num_vertices < 3) continue;
        const int renderer = (poly.texheader[0] >> 13) & 3;
        if (renderer == 1) continue; // translucent solid: the rasterizer draws nothing

        // clip rectangle (Raster::render_one), in the renderer's offsets, within the screen
        const int x0 = std::max(poly.viewport[0] + render_x, 0), x1 = std::min(poly.viewport[2] + render_x, w - 1);
        const int y0 = std::max((384 - poly.viewport[3]) + render_y, 0), y1 = std::min((384 - poly.viewport[1]) + render_y, h - 1);
        if (x0 > x1 || y0 > y1) continue;
        const SDL_Rect clip{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
        if (batches_.empty() || std::memcmp(&batches_.back().clip, &clip, sizeof clip) != 0)
            batches_.push_back({uint32_t(verts_.size()), 0, clip});

        // colour: the solid renderer's (palette entry, luma translation, gamma)
        const uint32_t colorbase = (poly.texheader[3] >> 6) & 0x3ff;
        const uint32_t luma = poly.luma >> 2;
        const uint32_t color = le16(mem.palram, colorbase + 0x1000);
        const float r = gamma_[le16(mem.colorxlat, 0x0000 / 2 + (((color >> 0) & 0x1f) << 8) + luma) & 0xff] / 255.0f;
        const float g = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff] / 255.0f;
        const float b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff] / 255.0f;
        const float checker = (poly.texheader[0] >> 15) & 1 ? 1.0f : 0.0f;
        const float depth = float(++drawn) * depth_step;

        // model2_3d_project, then a fan from vertex 0 (the polygon is convex)
        PolyVertex v[8];
        for (int k = 0; k < poly.num_vertices; k++) {
            const rt::GeoVertex &g0 = poly.v[k];
            const float z = g0.p[0] + std::numeric_limits<float>::min();
            v[k] = {float(crtc_x + poly.center[0]) + g0.x / z, float((384 - poly.center[1]) + crtc_y) - g0.y / z, depth,
                    r, g, b, 1.0f, checker};
        }
        for (int k = 1; k + 1 < poly.num_vertices; k++) {
            verts_.push_back(v[0]);
            verts_.push_back(v[k]);
            verts_.push_back(v[k + 1]);
        }
        batches_.back().count = uint32_t(verts_.size()) - batches_.back().first;
    }
}

void GpuRenderer::render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, int w, int h, const rt::Video &video) {
    if (!ok()) return;
    build(video, w, h);
    const uint32_t vert_bytes = uint32_t(verts_.size() * sizeof(PolyVertex));
    if (!ensure(w, h, vert_bytes)) return;

    // upload: both layers, the layer quad, the polygon vertices
    const uint32_t layer_bytes = uint32_t(w) * uint32_t(h) * 4;
    auto *p = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(dev_, upload_, true));
    const std::vector<uint32_t> *layers[2] = {&video.background_layer(), &video.foreground_layer()};
    for (int l = 0; l < 2; l++) {
        const size_t n = std::min(layers[l]->size() * 4, size_t(layer_bytes));
        std::memcpy(p + l * layer_bytes, layers[l]->data(), n);
    }
    const float fw = float(w), fh = float(h);
    const QuadVertex quad[6] = {{0, 0, 0, 0}, {fw, 0, 1, 0}, {0, fh, 0, 1}, {fw, 0, 1, 0}, {fw, fh, 1, 1}, {0, fh, 0, 1}};
    std::memcpy(p + 2 * layer_bytes, quad, sizeof quad);
    if (vert_bytes) std::memcpy(p + 2 * layer_bytes + sizeof quad, verts_.data(), vert_bytes);
    SDL_UnmapGPUTransferBuffer(dev_, upload_);

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    for (int l = 0; l < 2; l++) {
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = upload_;
        src.offset = uint32_t(l) * layer_bytes;
        src.pixels_per_row = Uint32(w);
        SDL_GPUTextureRegion dst{};
        dst.texture = layers_[l];
        dst.w = Uint32(w);
        dst.h = Uint32(h);
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
    }
    SDL_GPUTransferBufferLocation qsrc{upload_, 2 * layer_bytes};
    SDL_GPUBufferRegion qdst{qbuf_, 0, sizeof quad};
    SDL_UploadToGPUBuffer(copy, &qsrc, &qdst, true);
    if (vert_bytes) {
        SDL_GPUTransferBufferLocation vsrc{upload_, 2 * layer_bytes + uint32_t(sizeof quad)};
        SDL_GPUBufferRegion vdst{vbuf_, 0, vert_bytes};
        SDL_UploadToGPUBuffer(copy, &vsrc, &vdst, true);
    }
    SDL_EndGPUCopyPass(copy);

    SDL_GPUColorTargetInfo ct{};
    ct.texture = target;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.clear_color = SDL_FColor{0, 0, 0, 1};
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dt{};
    dt.texture = depth_;
    dt.clear_depth = 1.0f;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    const float screen[4] = {fw, fh, 0, 0};
    SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof screen);
    const SDL_Rect full{0, 0, w, h};

    auto draw_layer = [&](int l) {
        SDL_BindGPUGraphicsPipeline(pass, quad_pipe_);
        SDL_SetGPUScissor(pass, &full);
        SDL_GPUBufferBinding qb{qbuf_, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &qb, 1);
        SDL_GPUTextureSamplerBinding tsb{layers_[l], sampler_};
        SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
        SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    };
    draw_layer(0); // background
    if (!verts_.empty()) {
        SDL_BindGPUGraphicsPipeline(pass, poly_pipe_);
        SDL_GPUBufferBinding vb{vbuf_, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        for (const Batch &b : batches_) {
            if (!b.count) continue;
            SDL_SetGPUScissor(pass, &b.clip);
            SDL_DrawGPUPrimitives(pass, b.count, 1, b.first, 0);
        }
    }
    draw_layer(1); // foreground
    SDL_EndGPURenderPass(pass);
}

} // namespace app
