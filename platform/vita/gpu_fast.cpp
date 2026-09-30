#include "gpu_fast.h"
#include "runtime/raster_texel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <psp2/kernel/processmgr.h>

namespace vita {
namespace {
constexpr float kDisplayW = 960.0f;
constexpr float kDisplayH = 544.0f;
constexpr float kSourceW = float(rt::Video::W);
constexpr float kSourceH = float(rt::Video::H);
constexpr float kScale = kDisplayH / kSourceH;
constexpr float kOffsetX = (kDisplayW - kSourceW * kScale) * 0.5f;

inline float sx(float x) { return kOffsetX + x * kScale; }
inline float sy(float y) { return y * kScale; }

inline bool key_less(const rt::GeoPoly &a, std::size_t ai, const rt::GeoPoly &b, std::size_t bi) {
    if (a.window != b.window) return a.window > b.window;
    if (a.z != b.z) return a.z < b.z;
    return ai > bi;
}
} // namespace

GpuFastRenderer::GpuFastRenderer() {
    background_ = vita2d_create_empty_texture_format(rt::Video::W, rt::Video::H, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    foreground_ = vita2d_create_empty_texture_format(rt::Video::W, rt::Video::H, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    if (background_) vita2d_texture_set_filters(background_, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    if (foreground_) vita2d_texture_set_filters(foreground_, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    for (int i = 0; i < 256; ++i) gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));
}

GpuFastRenderer::~GpuFastRenderer() {
    // main_gpu calls shutdown() before vita2d_fini(). Do not touch GXM-owned
    // allocations after libvita2d has already been torn down.
}

uint16_t GpuFastRenderer::le16(const uint8_t *base, uint32_t index) {
    return uint16_t(base[index * 2] | uint16_t(base[index * 2 + 1]) << 8);
}

uint32_t GpuFastRenderer::swap_rb(uint32_t argb) {
    const uint32_t a = argb & 0xff000000u;
    const uint32_t r = (argb >> 16) & 0xff;
    const uint32_t g = (argb >> 8) & 0xff;
    const uint32_t b = argb & 0xff;
    return a | (b << 16) | (g << 8) | r;
}

void GpuFastRenderer::reset_materials() {
    // This entry point is only used between scenes. Wait before destroying
    // textures because GXM consumes draw commands asynchronously.
    vita2d_wait_rendering_done();
    for (auto &m : materials_) if (m.texture) vita2d_free_texture(m.texture);
    for (auto *t : retired_) if (t) vita2d_free_texture(t);
    materials_.clear();
    retired_.clear();
    cached_bytes_ = retired_bytes_ = 0;
}

void GpuFastRenderer::reap_retired() {
    if (retired_.empty()) return;
    // Evictions can happen while commands for the old texture are already in
    // the current GXM scene. Defer the actual free until every submitted draw
    // has finished. GPU05 freed immediately and could hand GXM stale CDRAM.
    vita2d_wait_rendering_done();
    for (auto *t : retired_) if (t) vita2d_free_texture(t);
    retired_.clear();
    retired_bytes_ = 0;
}

void GpuFastRenderer::shutdown() {
    if (shutdown_) return;
    reset_materials();
    if (background_) vita2d_free_texture(background_), background_ = nullptr;
    if (foreground_) vita2d_free_texture(foreground_), foreground_ = nullptr;
    shutdown_ = true;
}

void GpuFastRenderer::upload_layer(vita2d_texture *texture, const std::vector<uint32_t> &pixels) {
    if (!texture || pixels.size() < size_t(rt::Video::W) * rt::Video::H) return;
    // libvita2d returns texture stride in BYTES. GPU04 accidentally treated
    // that value as a uint32_t element count, advancing each row four times
    // too far and eventually writing outside CDRAM. Keep the address
    // arithmetic byte-based, then cast only the selected row.
    auto *base = static_cast<uint8_t *>(vita2d_texture_get_datap(texture));
    const size_t stride_bytes = vita2d_texture_get_stride(texture);
    if (!base || stride_bytes < size_t(rt::Video::W) * sizeof(uint32_t) ||
        (stride_bytes & (alignof(uint32_t) - 1)) != 0) return;
    for (int y = 0; y < rt::Video::H; ++y) {
        auto *row = reinterpret_cast<uint32_t *>(base + size_t(y) * stride_bytes);
        const uint32_t *src = pixels.data() + size_t(y) * rt::Video::W;
        for (int x = 0; x < rt::Video::W; ++x) row[x] = swap_rb(src[x]);
    }
}

uint32_t GpuFastRenderer::shade_texel(const rt::GeoPoly &poly, const rt::VideoMem &mem, uint8_t texel) const {
    const uint32_t lumabase = uint32_t(poly.texheader[1] & 0xff) << 7;
    const uint32_t color_index = ((poly.texheader[3] >> 6) & 0x3ff) + 0x1000;
    const uint32_t color = le16(mem.palram, color_index) & 0x7fff;
    uint8_t luma = uint8_t(uint32_t(mem.lumaram[(lumabase + (uint32_t(texel) << 3)) * 4]) * poly.luma / 256);
    luma = std::min(luma, uint8_t(0x3f));
    const uint32_t cr = (((color >> 0) & 0x1f) << 8) + luma;
    const uint32_t cg = 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma;
    const uint32_t cb = 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma;
    const uint8_t r = gamma_[le16(mem.colorxlat, cr) & 0xff];
    const uint8_t g = gamma_[le16(mem.colorxlat, cg) & 0xff];
    const uint8_t b = gamma_[le16(mem.colorxlat, cb) & 0xff];
    return RGBA8(r, g, b, 255);
}

uint32_t GpuFastRenderer::solid_color(const rt::GeoPoly &poly, const rt::VideoMem &mem) const {
    const uint32_t color_index = ((poly.texheader[3] >> 6) & 0x3ff) + 0x1000;
    const uint32_t color = le16(mem.palram, color_index) & 0xffff;
    const uint8_t luma = poly.luma >> 2;
    const uint8_t r = gamma_[le16(mem.colorxlat, (((color >> 0) & 0x1f) << 8) + luma) & 0xff];
    const uint8_t g = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff];
    const uint8_t b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff];
    return RGBA8(r, g, b, 255);
}

void GpuFastRenderer::evict_for(std::size_t bytes) {
    while (!materials_.empty() && cached_bytes_ + bytes > kCacheLimit) {
        auto it = std::min_element(materials_.begin(), materials_.end(), [](const Material &a, const Material &b) {
            return a.stamp < b.stamp;
        });
        cached_bytes_ -= it->bytes;
        if (it->texture) {
            retired_.push_back(it->texture);
            retired_bytes_ += it->bytes;
            it->texture = nullptr;
        }
        materials_.erase(it);
    }
}

GpuFastRenderer::Material GpuFastRenderer::build_material(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
    Material out;
    out.key = {poly.texheader[0], poly.texheader[1], poly.texheader[2], poly.texheader[3], poly.luma,
               bool((poly.texheader[0] >> 13) & 1)};
    out.source_w = 32u << ((poly.texheader[0] >> 0) & 7);
    out.source_h = 32u << ((poly.texheader[0] >> 3) & 7);
    const uint32_t step_x = std::max(1u, (out.source_w + kTextureLimit - 1) / kTextureLimit);
    const uint32_t step_y = std::max(1u, (out.source_h + kTextureLimit - 1) / kTextureLimit);
    out.tex_w = std::max(1u, (out.source_w + step_x - 1) / step_x);
    out.tex_h = std::max(1u, (out.source_h + step_y - 1) / step_y);
    out.bytes = size_t(out.tex_w) * out.tex_h * 4;
    evict_for(out.bytes);
    out.texture = vita2d_create_empty_texture_format(out.tex_w, out.tex_h, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    if (!out.texture) return out;
    vita2d_texture_set_filters(out.texture, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    const bool mirror_x = (poly.texheader[0] >> 8) & 1;
    const bool mirror_y = (poly.texheader[0] >> 9) & 1;
    const bool wrap_x = ((poly.texheader[0] >> 6) & 1) && !mirror_x;
    const bool wrap_y = ((poly.texheader[0] >> 7) & 1) && !mirror_y;
    sceGxmTextureSetUAddrMode(&out.texture->gxm_tex, mirror_x ? SCE_GXM_TEXTURE_ADDR_MIRROR : (wrap_x ? SCE_GXM_TEXTURE_ADDR_REPEAT : SCE_GXM_TEXTURE_ADDR_CLAMP));
    sceGxmTextureSetVAddrMode(&out.texture->gxm_tex, mirror_y ? SCE_GXM_TEXTURE_ADDR_MIRROR : (wrap_y ? SCE_GXM_TEXTURE_ADDR_REPEAT : SCE_GXM_TEXTURE_ADDR_CLAMP));

    auto *base = static_cast<uint8_t *>(vita2d_texture_get_datap(out.texture));
    const size_t stride_bytes = vita2d_texture_get_stride(out.texture);
    if (!base || stride_bytes < size_t(out.tex_w) * sizeof(uint32_t) ||
        (stride_bytes & (alignof(uint32_t) - 1)) != 0) {
        vita2d_free_texture(out.texture);
        out.texture = nullptr;
        return out;
    }
    const uint32_t bx = 32u * ((poly.texheader[2] >> 0) & 0x3f);
    const uint32_t by = 32u * ((poly.texheader[2] >> 6) & 0x1f);
    const uint32_t *sheet = (poly.texheader[2] & 0x1000) ? mem.tex1 : mem.tex0;
    const bool transparent = out.key.translucent;
    for (uint32_t y = 0; y < out.tex_h; ++y) {
        auto *row = reinterpret_cast<uint32_t *>(base + size_t(y) * stride_bytes);
        const uint32_t sy0 = std::min(y * step_y, out.source_h - 1);
        for (uint32_t x = 0; x < out.tex_w; ++x) {
            const uint32_t sx0 = std::min(x * step_x, out.source_w - 1);
            const rt::TexelQuad q = rt::read_texel_quad(bx, by, sx0, sx0, sy0, sy0, sheet);
            const uint8_t index = uint8_t((q.t00 >> 4) & 0x0f);
            if (transparent && index == 0x0f) row[x] = RGBA8(0, 0, 0, 0);
            else row[x] = shade_texel(poly, mem, index);
        }
    }
    cached_bytes_ += out.bytes;
    return out;
}

GpuFastRenderer::Material *GpuFastRenderer::material_for(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
    const MaterialKey key{poly.texheader[0], poly.texheader[1], poly.texheader[2], poly.texheader[3], poly.luma,
                          bool((poly.texheader[0] >> 13) & 1)};
    ++stamp_;
    for (auto &m : materials_) {
        if (m.key == key) { m.stamp = stamp_; return &m; }
    }
    Material m = build_material(poly, mem);
    if (!m.texture) return nullptr;
    m.stamp = stamp_;
    materials_.push_back(std::move(m));
    return &materials_.back();
}

void GpuFastRenderer::draw_polygons(rt::Video &video) {
    const auto &polys = video.gpu_polys();
    const rt::VideoMem &mem = video.gpu_mem();
    pool_drops_ = 0;
    min_pool_free_ = vita2d_pool_free_space();
    order_.resize(polys.size());
    for (size_t i = 0; i < order_.size(); ++i) order_[i] = i;
    std::sort(order_.begin(), order_.end(), [&](size_t a, size_t b) { return key_less(polys[a], a, polys[b], b); });

    // CPU rasterizer is first-hit/front-to-back. Draw the reverse list with
    // alpha-tested textures to obtain the same basic visibility ordering.
    // Enable stencil clipping once. GPU05 enabled it again for every polygon,
    // which redundantly drew the old clip rectangle before setting the new one.
    vita2d_enable_clipping();
    int last_clip[4] = {-1, -1, -1, -1};
    for (auto oi = order_.rbegin(); oi != order_.rend(); ++oi) {
        const rt::GeoPoly &poly = polys[*oi];
        if (poly.window > video.gpu_windows() || poly.num_vertices < 3) continue;
        const int renderer = (poly.texheader[0] >> 13) & 3;
        int clip_l = std::max<int>(poly.viewport[0] + video.render_x(), 0);
        int clip_r = std::min<int>(poly.viewport[2] + video.render_x(), rt::Video::W - 1);
        int clip_t = std::max<int>((384 - poly.viewport[3]) + video.render_y(), 0);
        int clip_b = std::min<int>((384 - poly.viewport[1]) + video.render_y(), rt::Video::H - 1);
        if (clip_l > clip_r || clip_t > clip_b) continue;
        // libvita2d's clip implementation consumes temporary-pool vertices
        // and does not null-check an exhausted pool. Reserve headroom for the
        // two clip rectangles plus this polygon's vertices/tint data.
        unsigned free_pool = vita2d_pool_free_space();
        min_pool_free_ = std::min(min_pool_free_, free_pool);
        if (free_pool < 2048) { ++pool_drops_; break; }
        const int gclip[4] = {int(sx(float(clip_l))), int(sy(float(clip_t))),
                              int(sx(float(clip_r + 1))), int(sy(float(clip_b + 1)))};
        if (gclip[0] != last_clip[0] || gclip[1] != last_clip[1] ||
            gclip[2] != last_clip[2] || gclip[3] != last_clip[3]) {
            vita2d_set_clip_rectangle(gclip[0], gclip[1], gclip[2], gclip[3]);
            for (int c = 0; c < 4; ++c) last_clip[c] = gclip[c];
        }

        struct P { float x, y, u, v; } p[8];
        for (int i = 0; i < poly.num_vertices; ++i) {
            const float pz = poly.v[i].p[0] + std::numeric_limits<float>::min();
            const float x = float(video.crtc_x() + poly.center[0]) + poly.v[i].x / pz;
            const float y = float((384 - poly.center[1]) + video.crtc_y()) - poly.v[i].y / pz;
            p[i].x = sx(x); p[i].y = sy(y);
            p[i].u = poly.v[i].p[1] / 8.0f;
            p[i].v = poly.v[i].p[2] / 8.0f;
        }

        if (renderer & 2) {
            Material *m = material_for(poly, mem);
            if (!m) continue;
            const size_t n = size_t(poly.num_vertices - 2) * 3;
            auto *verts = static_cast<vita2d_texture_vertex *>(vita2d_pool_memalign(unsigned(n * sizeof(vita2d_texture_vertex)), 4));
            if (!verts) continue;
            size_t v = 0;
            for (int i = 1; i + 1 < poly.num_vertices; ++i) {
                for (int j : {0, i, i + 1}) {
                    verts[v].x = p[j].x; verts[v].y = p[j].y; verts[v].z = 0.5f;
                    verts[v].u = p[j].u / float(m->source_w);
                    verts[v].v = p[j].v / float(m->source_h);
                    ++v;
                }
            }
            vita2d_draw_array_textured(m->texture, SCE_GXM_PRIMITIVE_TRIANGLES, verts, n, RGBA8(255,255,255,255));
        } else if (!(renderer & 1)) {
            const size_t n = size_t(poly.num_vertices - 2) * 3;
            auto *verts = static_cast<vita2d_color_vertex *>(vita2d_pool_memalign(unsigned(n * sizeof(vita2d_color_vertex)), 4));
            if (!verts) continue;
            const uint32_t c = solid_color(poly, mem);
            size_t v = 0;
            for (int i = 1; i + 1 < poly.num_vertices; ++i) {
                for (int j : {0, i, i + 1}) verts[v++] = vita2d_color_vertex{p[j].x, p[j].y, 0.5f, c};
            }
            vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, verts, n);
        }
    }
    min_pool_free_ = std::min(min_pool_free_, vita2d_pool_free_space());
    vita2d_disable_clipping();
}

void GpuFastRenderer::draw_exact(rt::Video &video) {
    const uint64_t begin = sceKernelGetProcessTimeWide();
    upload_layer(background_, video.screen());
    vita2d_draw_texture_scale(background_, kOffsetX, 0.0f, kScale, kScale);
    last_gpu_ms_ = double(sceKernelGetProcessTimeWide() - begin) / 1000.0;
}

void GpuFastRenderer::draw(rt::Video &video) {
    const uint64_t begin = sceKernelGetProcessTimeWide();
    upload_layer(background_, video.background_layer());
    upload_layer(foreground_, video.foreground_layer());
    const float scale = kScale;
    vita2d_draw_texture_scale(background_, kOffsetX, 0.0f, scale, scale);
    draw_polygons(video);
    vita2d_draw_texture_scale(foreground_, kOffsetX, 0.0f, scale, scale);
    last_gpu_ms_ = double(sceKernelGetProcessTimeWide() - begin) / 1000.0;
}

} // namespace vita
