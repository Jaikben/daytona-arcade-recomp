// The Dreamcast's picture, drawn by the PVR (step 4a): the background tile
// layers as a texture, the game's polygons over them, then the front tile
// layers (their empty pixels see-through), at 620x480 centred. The runtime is
// in external 3D mode (Video::set_external_3d), as platform/vita's GPU path:
// it hands over this frame's polygons and both tile layers and draws no 3D
// itself. Polygons are flat-coloured for now (solid_color, as the Vita's
// GpuFastRenderer::solid_color); textures are step 4b.
//
// Order: as the CPU reference and platform/vita/polygon_order.h, descending
// window, ascending z, newest first on ties; drawn back to front with the
// depth test off, so the PVR paints them in that order.
#pragma once

// After the runtime's headers (KOS's BIT macro; see main.cpp).
#include <kos.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace dc {

class Renderer {
public:
    // vertex_buffer: the PVR's vertex buffer size (pvr_init_params_t); a
    // frame's polygons stop there (skipped counts them) instead of
    // overflowing it, which stops the PVR and the game with it.
    explicit Renderer(size_t vertex_buffer)
        : budget_(vertex_buffer - 4096), background_(pvr_mem_malloc(kTexW * kTexH * 2)),
          foreground_(pvr_mem_malloc(kTexW * kTexH * 2)) {
        if (!background_ || !foreground_) throw std::runtime_error("no video RAM for the tile layer textures");
        for (int i = 0; i < 256; i++) gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, kTexW, kTexH,
                         background_, PVR_FILTER_BILINEAR);
        unordered(cxt);
        pvr_poly_compile(&background_header_, &cxt);
        pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED, kTexW, kTexH,
                         foreground_, PVR_FILTER_BILINEAR);
        unordered(cxt);
        pvr_poly_compile(&foreground_header_, &cxt);
        pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
        unordered(cxt);
        pvr_poly_compile(&solid_header_, &cxt);
    }

    // video: after GameLoop::run_frame, in external 3D mode.
    void draw(const rt::Video &video) {
        stage = 1;
        upload(video.background_layer(), background_, false);
        upload(video.foreground_layer(), foreground_, true);
        stage = 2;
        const auto &polys = video.gpu_polys();
        const rt::VideoMem &mem = video.gpu_mem();
        sort(polys);

        stage = 3;
        pvr_wait_ready();
        stage = 4;
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        quad(background_header_);
        // One header for every flat polygon (their colour is per vertex).
        pvr_prim(&solid_header_, sizeof solid_header_);
        size_t used = 2 * sizeof(pvr_poly_hdr_t) + 4 * sizeof(pvr_vertex_t);
        drawn = skipped = 0;
        for (auto e = order_.rbegin(); e != order_.rend(); ++e) {
            const rt::GeoPoly &poly = polys[e->index];
            if (poly.window > video.gpu_windows() || poly.num_vertices < 3 || poly.num_vertices > 8) continue;
            const size_t bytes = size_t(poly.num_vertices) * sizeof(pvr_vertex_t);
            if (used + bytes > budget_ || !polygons) { ++skipped; continue; }
            if (polygon(poly, video, solid_color(poly, mem))) ++drawn, used += bytes;
        }
        vertex_bytes = used;
        stage = 5;
        pvr_list_finish();
        pvr_list_begin(PVR_LIST_PT_POLY);
        quad(foreground_header_);
        pvr_list_finish();
        stage = 6;
        pvr_scene_finish();
        stage = 0;
    }

    // Where draw is (the watchdog's): 0 not drawing, 1 uploading the tile
    // layers, 2 sorting, 3 waiting for the PVR, 4 submitting polygons,
    // 5 the front layer, 6 finishing the scene.
    volatile int stage = 0;
    bool polygons = true; // false: the tile layers only (a diagnostic)
    unsigned drawn = 0, skipped = 0; // polygons in the last frame: drawn, and left out (vertex buffer full)
    size_t vertex_bytes = 0;          // the last frame's vertex data in the opaque list

private:
    static constexpr int kTexW = 512, kTexH = 512, kW = 496, kH = 384;
    static constexpr float kScale = 480.0f / kH, kOffsetX = (640.0f - kW * kScale) * 0.5f;
    struct Entry { uint32_t key; size_t index; };

    // No depth test (painter's order), no culling: the game's own order and
    // facing decide.
    static void unordered(pvr_poly_cxt_t &cxt) {
        cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        cxt.gen.culling = PVR_CULLING_NONE;
    }

    static uint16_t le16(const uint8_t *p, uint32_t index) { return uint16_t(p[index * 2] | p[index * 2 + 1] << 8); }

    // ARGB8888 (W wide, the first H rows) to RGB565, or to ARGB1555 with
    // zero (no tile pixel) see-through; a row at a time, main RAM is short.
    void upload(const std::vector<uint32_t> &layer, pvr_ptr_t texture, bool alpha) {
        auto *dst = static_cast<uint8_t *>(texture);
        for (int y = 0; y < kH; y++) {
            const uint32_t *src = &layer[size_t(y) * kW];
            for (int x = 0; x < kW; x++) {
                const uint32_t c = src[x];
                row_[x] = alpha ? uint16_t((c ? 0x8000 : 0) | ((c >> 9) & 0x7c00) | ((c >> 6) & 0x03e0) | ((c >> 3) & 0x001f))
                                : uint16_t(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
            }
            pvr_txr_load(row_, dst + size_t(y) * kTexW * 2, kTexW * 2);
        }
    }

    void quad(const pvr_poly_hdr_t &header) {
        pvr_prim(&header, sizeof header);
        const float x0 = kOffsetX, x1 = kOffsetX + kW * kScale, h = kH * kScale;
        const float u1 = float(kW) / kTexW, v1 = float(kH) / kTexH;
        const float corners[4][4] = {{x0, 0, 0, 0}, {x1, 0, u1, 0}, {x0, h, 0, v1}, {x1, h, u1, v1}};
        for (int i = 0; i < 4; i++) {
            pvr_vertex_t v{};
            v.flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v.x = corners[i][0];
            v.y = corners[i][1];
            v.z = 1.0f;
            v.u = corners[i][2];
            v.v = corners[i][3];
            v.argb = 0xffffffffu;
            pvr_prim(&v, sizeof v);
        }
    }

    void sort(const std::vector<rt::GeoPoly> &polys) {
        const size_t count = polys.size();
        order_.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const size_t index = count - 1 - i;
            order_[i] = {((255u - uint32_t(polys[index].window)) << 16) | uint32_t(polys[index].z), index};
        }
        std::sort(order_.begin(), order_.end(),
                  [](const Entry &a, const Entry &b) { return a.key != b.key ? a.key < b.key : a.index > b.index; });
    }

    uint32_t solid_color(const rt::GeoPoly &poly, const rt::VideoMem &mem) const {
        const uint32_t color = le16(mem.palram, ((poly.texheader[3] >> 6) & 0x3ff) + 0x1000);
        const uint32_t luma = poly.luma >> 2;
        const uint8_t r = gamma_[le16(mem.colorxlat, (((color >> 0) & 0x1f) << 8) + luma) & 0xff];
        const uint8_t g = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff];
        const uint8_t b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff];
        return 0xff000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | b;
    }

    // A convex polygon as one strip: 0, 1, n-1, 2, n-2, ...
    bool polygon(const rt::GeoPoly &poly, const rt::Video &video, uint32_t argb) {
        pvr_vertex_t v[8];
        const int n = poly.num_vertices;
        for (int i = 0; i < n; i++) {
            const float pz = poly.v[i].p[0];
            if (!(pz > 0.0f) || !std::isfinite(pz)) return false;
            const float x = float(video.crtc_x() + poly.center[0]) + poly.v[i].x / pz;
            const float y = float((384 - poly.center[1]) + video.crtc_y()) - poly.v[i].y / pz;
            v[i] = {};
            v[i].x = kOffsetX + x * kScale;
            v[i].y = y * kScale;
            v[i].z = 1.0f;
            v[i].argb = argb;
            if (!std::isfinite(v[i].x) || !std::isfinite(v[i].y)) return false;
        }
        int lo = 1, hi = n - 1, k = 0;
        int strip[8] = {0};
        for (int s = 1; s < n; s++) strip[s] = (s & 1) ? lo++ : hi--;
        for (; k < n; k++) {
            pvr_vertex_t out = v[strip[k]];
            out.flags = k == n - 1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            pvr_prim(&out, sizeof out);
        }
        return true;
    }

    size_t budget_;
    pvr_ptr_t background_, foreground_;
    pvr_poly_hdr_t background_header_, foreground_header_, solid_header_;
    alignas(32) uint16_t row_[kTexW] = {};
    std::vector<Entry> order_;
    uint8_t gamma_[256] = {};
};

} // namespace dc
