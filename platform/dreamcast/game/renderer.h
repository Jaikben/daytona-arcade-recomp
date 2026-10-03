// The Dreamcast's picture, drawn by the PVR: the background tile layers as a
// texture, the game's polygons over them, then the front tile layers (their
// empty pixels see-through), at 620x480 centred. The runtime is in external 3D
// mode (Video::set_external_3d), as platform/vita's GPU path: it hands over
// this frame's polygons and both tile layers and draws no 3D itself.
//
// Order: as the CPU reference and platform/vita/polygon_order.h, descending
// window, ascending z, newest first on ties, drawn back to front. The
// polygons and the front layer go in the translucent list with autosort off
// (pvr_init_params_t::autosort_disabled), which the PVR draws in the order
// given, blending by the texel's alpha (0 or 1: colour-keyed texels); the
// depth test is off, z carries 1/z for perspective-correct textures.
//
// Textures, as the Vita's GpuFastRenderer: a source is a texture's 4-bit
// texel indices (read_texel_quad from the board's texture RAM), at most
// kTextureLimit a side (larger ones sampled down), kept as a twiddled
// PAL4BPP texture in a cache the frontend gives (the half of each texture
// sheet the game's writes do not reach). The game shades a texel through
// luma RAM (per texel, from texheader[1]) and the polygon's colour and light
// together through the colour translation table; the PVR has 64 sixteen-
// colour palette banks a frame, far fewer than the frame's colour and light
// combinations. So it is split, an approximation: a palette bank holds the
// texels' luma as greys (keyed by the luma base and transparency, few a
// frame), and the polygon's colour at full light, scaled by its light, is
// the vertex colour the PVR multiplies the texel by. Past the build budget
// or the palette banks a polygon is drawn in its flat colour.
#pragma once

// After the runtime's headers (KOS's BIT macro; see main.cpp).
#include <kos.h>

#include "runtime/raster_texel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace dc {

class Renderer {
public:
    struct Region { uint8_t *base; size_t bytes; }; // video RAM for the texture cache

    // vertex_buffer: the PVR's vertex buffer size (pvr_init_params_t); a
    // frame's polygons stop there (skipped counts them) instead of
    // overflowing it, which stops the PVR and the game with it.
    Renderer(size_t vertex_buffer, std::vector<Region> cache)
        : budget_(vertex_buffer - 4096), regions_(std::move(cache)), background_(pvr_mem_malloc(kTexW * kTexH * 2)),
          foreground_(pvr_mem_malloc(kTexW * kTexH * 2)) {
        if (!background_ || !foreground_) throw std::runtime_error("no video RAM for the tile layer textures");
        for (int i = 0; i < 256; i++) gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));
        pvr_set_pal_format(PVR_PAL_ARGB1555);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, kTexW, kTexH,
                         background_, PVR_FILTER_BILINEAR);
        unordered(cxt);
        pvr_poly_compile(&background_header_, &cxt);
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED, kTexW, kTexH,
                         foreground_, PVR_FILTER_BILINEAR);
        unordered(cxt);
        pvr_poly_compile(&foreground_header_, &cxt);
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        unordered(cxt);
        pvr_poly_compile(&solid_header_, &cxt);
    }

    // video: after GameLoop::run_frame, in external 3D mode.
    void draw(const rt::Video &video) {
        stage = 1;
        uint64_t t = timer_us_gettime64();
        auto lap = [&](int step) {
            const uint64_t now = timer_us_gettime64();
            step_us[step] += now - t;
            t = now;
        };
#ifdef M2_DC_SPEED
        // A layer the runtime did not compose again is already in video RAM;
        // the others go by DMA once the PVR has finished the last frame
        // (below), while the polygons are prepared.
        const std::vector<uint16_t> *back_layer = nullptr, *front_layer = nullptr;
        if (video.background16_generation() != back_uploaded_) {
            back_layer = &video.background16();
            back_uploaded_ = video.background16_generation();
        }
        if (video.foreground16_generation() != front_uploaded_) {
            front_layer = &video.foreground16();
            front_uploaded_ = video.foreground16_generation();
        }
#else
        upload(video.background_layer(), background_, false);
        upload(video.foreground_layer(), foreground_, true);
#endif
        lap(0);
        stage = 2;
        const auto &polys = video.gpu_polys();
        const rt::VideoMem &mem = video.gpu_mem();
        sort(polys);

        stage = 3;
        pvr_wait_ready(); // the last frame is drawn: its palettes and textures may change
#ifdef M2_DC_SPEED
        if (back_layer) upload_layer(*back_layer, background_);
#endif
        lap(1);
        if (flush_) { // the texture cache filled up last frame
            sources_.clear();
            region_ = 0;
            region_used_ = 0;
            flush_ = false;
            ++flushes;
        }
        materials_.clear();
        banks_.clear();
        banks_used_ = 0;
        builds_ = 0;
        stage = 4;
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        quad(background_header_);
        pvr_list_finish();

        // Materials front to back first: the 64 palette banks and the build
        // budget go to what is nearest (drawn last, largest on screen).
        no_bank = no_build = 0;
        for (const Entry &e : order_) {
            const rt::GeoPoly &poly = polys[e.index];
            if (poly.window > video.gpu_windows() || poly.num_vertices < 3 || poly.num_vertices > 8) continue;
            if ((poly.texheader[0] >> 14) & 1) material_for(poly, mem);
        }
        lap(2);

#ifdef M2_DC_SPEED
        if (front_layer) upload_layer(*front_layer, foreground_); // after the back layer's DMA
#endif
        pvr_list_begin(PVR_LIST_TR_POLY);
        pvr_dr_init(&dr_);
        size_t used = sizeof(pvr_poly_hdr_t) + 4 * sizeof(pvr_vertex_t);
        drawn = textured = skipped = 0;
        const void *current = nullptr; // the header in force (polygons sharing one send it once)
        for (auto e = order_.rbegin(); e != order_.rend(); ++e) {
            const rt::GeoPoly &poly = polys[e->index];
            if (poly.window > video.gpu_windows() || poly.num_vertices < 3 || poly.num_vertices > 8) continue;
            const int renderer = (poly.texheader[0] >> 13) & 3;
            const Material *material = nullptr;
            if (renderer & 2) material = material_found(poly);
            else if (renderer & 1) continue; // as the Vita: not drawn
            const void *header = material ? static_cast<const void *>(&material->header)
                                          : static_cast<const void *>(&solid_header_);
            const size_t bytes = size_t(poly.num_vertices) * sizeof(pvr_vertex_t) +
                                 (header != current ? sizeof(pvr_poly_hdr_t) : 0);
            if (used + bytes > budget_) { ++skipped; continue; }
            if (polygon(poly, video, material ? textured_colour(poly, mem) : solid_color(poly, mem), material, header,
                        current)) {
                ++drawn;
                if (material) ++textured;
                used += bytes;
            }
        }
        quad(foreground_header_);
        pvr_list_finish();
        vertex_bytes = used;
        stage = 6;
#ifdef M2_DC_SPEED
        wait_dma(); // the layers are in video RAM before the PVR draws them
#endif
        pvr_scene_finish();
        lap(3);
        stage = 0;
    }
    // Microseconds in draw so far: the layers' conversion and upload,
    // sorting and waiting for the PVR, materials (texture builds), polygons.
    uint64_t step_us[4] = {};

    // Where draw is (the watchdog's): 0 not drawing, 1 uploading the tile
    // layers, 2 sorting, 3 waiting for the PVR, 4 submitting polygons,
    // 6 finishing the scene.
    volatile int stage = 0;
    // The last frame: polygons drawn (of them textured), left out (vertex
    // buffer full), vertex data; texture cache flushes so far.
    unsigned drawn = 0, textured = 0, skipped = 0, flushes = 0;
    unsigned no_bank = 0, no_build = 0; // textured polygons drawn flat: out of palette banks, past the build budget
    size_t vertex_bytes = 0;
    size_t sources() const { return sources_.size(); }

private:
    static constexpr int kTexW = 512, kTexH = 512, kW = 496, kH = 384;
    static constexpr float kScale = 480.0f / kH, kOffsetX = (640.0f - kW * kScale) * 0.5f;
    static constexpr uint32_t kTextureLimit = 256; // source textures at most this a side
    static constexpr unsigned kBuildsPerFrame = 32; // new source textures a frame
    static constexpr unsigned kBanks = 64;          // the PVR's 16-colour palette banks
    static constexpr unsigned kPaletteLuma = 0xf0;  // the material light palettes are shaded at (luma & 0xf0 at most)
    struct Entry { uint32_t key; size_t index; };
    struct Source { pvr_ptr_t texture; uint32_t w, h, source_w, source_h; };
    struct Material { pvr_poly_hdr_t header; float u_scale, v_scale; };

    // No depth test (the game's order), no culling (its own facing decides).
    static void unordered(pvr_poly_cxt_t &cxt) {
        cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
        cxt.gen.culling = PVR_CULLING_NONE;
    }

    static uint16_t le16(const uint8_t *p, uint32_t index) { return uint16_t(p[index * 2] | p[index * 2 + 1] << 8); }

    // ARGB8888 (W wide, the first H rows) to RGB565, or to ARGB1555 with
    // zero (no tile pixel) see-through, written through the store queues
    // straight into video RAM (32 bytes, 16 pixels, at a time; W is 31 of
    // those). The front layer is mostly see-through: a row that is all zero
    // and was all zero when last uploaded is already right in video RAM.
    static uint32_t rgb565(uint32_t c) { return ((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f); }
    static uint32_t argb1555(uint32_t c) {
        return (c ? 0x8000 : 0) | ((c >> 9) & 0x7c00) | ((c >> 6) & 0x03e0) | ((c >> 3) & 0x001f);
    }
    static_assert(kW % 16 == 0, "rows go out 16 pixels at a time");
    void upload(const std::vector<uint32_t> &layer, pvr_ptr_t texture, bool alpha) {
        auto *dst = static_cast<uint8_t *>(texture);
        sq_lock(dst); // the texture is in one 64 MB window: one lock serves every row
        for (int y = 0; y < kH; y++) {
            const uint32_t *src = &layer[size_t(y) * kW];
            if (alpha) {
                uint32_t any = 0;
                for (int x = 0; x < kW; x++) any |= src[x];
                if (!any && clear_row_[y]) continue;
                clear_row_[y] = !any;
            }
            auto *sq = reinterpret_cast<uint32_t *>(SQ_MASK_DEST(dst + size_t(y) * kTexW * 2));
            for (int x = 0; x < kW; x += 16, sq += 8) {
                if (alpha)
                    for (int k = 0; k < 8; k++) sq[k] = argb1555(src[x + 2 * k]) | argb1555(src[x + 2 * k + 1]) << 16;
                else
                    for (int k = 0; k < 8; k++) sq[k] = rgb565(src[x + 2 * k]) | rgb565(src[x + 2 * k + 1]) << 16;
                sq_flush(sq);
            }
        }
        sq_unlock();
    }

#ifdef M2_DC_SPEED
    // The runtime's 16-bit layers (rt::Video::background16/foreground16:
    // already RGB565 and ARGB1555, rows of kLayerStride = the texture's
    // width): one DMA each, while the CPU carries on. KOS flushes the cache
    // for it; the buffers are 32-byte aligned (main.cpp's operator new).
    static_assert(rt::Video::kLayerStride == kTexW, "a layer's rows are the texture's");
    static void wait_dma() {
        while (!pvr_dma_ready()) thd_pass();
    }
    void upload_layer(const std::vector<uint16_t> &layer, pvr_ptr_t texture) {
        wait_dma(); // one PVR DMA at a time
        if (pvr_txr_load_dma(layer.data(), texture, size_t(kTexW) * kH * 2, false, nullptr, nullptr) < 0)
            pvr_txr_load(layer.data(), texture, size_t(kTexW) * kH * 2); // (not aligned: by the store queues)
    }
#endif

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

    // A textured polygon's vertex colour: its colour at full light (luma
    // 0x3f through the translation table), scaled by its light.
    uint32_t textured_colour(const rt::GeoPoly &poly, const rt::VideoMem &mem) const {
        const uint32_t color = le16(mem.palram, ((poly.texheader[3] >> 6) & 0x3ff) + 0x1000);
        const uint32_t light = std::min<uint32_t>(256, uint32_t(poly.luma & 0xf0) * 256 / kPaletteLuma);
        auto channel = [&](uint32_t base, uint32_t c) {
            return uint32_t(gamma_[le16(mem.colorxlat, base + (c << 8) + 0x3f) & 0xff]) * light >> 8;
        };
        return 0xff000000u | std::min<uint32_t>(255, channel(0, color & 0x1f)) << 16 |
               std::min<uint32_t>(255, channel(0x4000 / 2, (color >> 5) & 0x1f)) << 8 |
               std::min<uint32_t>(255, channel(0x8000 / 2, (color >> 10) & 0x1f));
    }

    uint32_t solid_color(const rt::GeoPoly &poly, const rt::VideoMem &mem) const {
        const uint32_t color = le16(mem.palram, ((poly.texheader[3] >> 6) & 0x3ff) + 0x1000);
        const uint32_t luma = poly.luma >> 2;
        const uint8_t r = gamma_[le16(mem.colorxlat, (((color >> 0) & 0x1f) << 8) + luma) & 0xff];
        const uint8_t g = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff];
        const uint8_t b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff];
        return 0xff000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | b;
    }

    // A palette entry: the texel's luma (luma RAM at the material's luma
    // base, at kPaletteLuma) as a grey, ARGB1555; alpha 0 for a transparent
    // material's texel 15.
    static uint16_t texel_grey(const rt::GeoPoly &poly, const rt::VideoMem &mem, unsigned texel) {
        if (((poly.texheader[0] >> 13) & 1) && texel == 0x0f) return 0;
        const uint32_t lumabase = uint32_t(poly.texheader[1] & 0xff) << 7;
        const uint32_t luma =
            std::min<uint32_t>(uint32_t(mem.lumaram[(lumabase + (texel << 3)) * 4]) * kPaletteLuma / 256, 0x3f);
        const uint32_t g5 = luma * 31 / 0x3f;
        return uint16_t(0x8000 | g5 << 10 | g5 << 5 | g5);
    }

    // Video RAM from the cache regions (bump allocation; a full cache is
    // emptied at the start of the next frame).
    pvr_ptr_t allocate(size_t bytes) {
        bytes = (bytes + 31) & ~size_t(31);
        while (region_ < regions_.size()) {
            if (region_used_ + bytes <= regions_[region_].bytes) {
                uint8_t *p = regions_[region_].base + region_used_;
                region_used_ += bytes;
                return p;
            }
            ++region_;
            region_used_ = 0;
        }
        flush_ = true;
        return nullptr;
    }

    const Source *source_for(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
        const uint32_t key = uint32_t(poly.texheader[0] & 0x23ff) | uint32_t(poly.texheader[2] & 0x1fff) << 16;
        auto found = sources_.find(key);
        if (found != sources_.end()) return &found->second;
        if (builds_ >= kBuildsPerFrame) return nullptr;
        Source s{};
        s.source_w = 32u << (poly.texheader[0] & 7);
        s.source_h = 32u << ((poly.texheader[0] >> 3) & 7);
        const uint32_t step_x = std::max(1u, s.source_w / kTextureLimit), step_y = std::max(1u, s.source_h / kTextureLimit);
        s.w = s.source_w / step_x;
        s.h = s.source_h / step_y;
        s.texture = allocate(size_t(s.w) * s.h / 2);
        if (!s.texture) return nullptr;
        ++builds_;
        const uint32_t bx = 32u * (poly.texheader[2] & 0x3f);
        const uint32_t by = 32u * ((poly.texheader[2] >> 6) & 0x1f);
        const uint32_t *sheet = (poly.texheader[2] & 0x1000) ? mem.tex1 : mem.tex0;
        std::memset(texels_, 0, size_t(s.w) * s.h / 2);
        for (uint32_t y = 0; y < s.h; ++y)
            for (uint32_t x = 0; x < s.w; ++x) {
                const rt::TexelQuad q = rt::read_texel_quad(bx, by, x * step_x, x * step_x, y * step_y, y * step_y, sheet);
                texels_[(x + y * s.w) >> 1] |= uint8_t(((q.t00 >> 4) & 0x0f) << ((x & 1) * 4));
            }
        pvr_txr_load_ex(texels_, s.texture, s.w, s.h, PVR_TXRLOAD_4BPP);
        return &sources_.emplace(key, s).first->second;
    }

    // A material: a source texture with a palette bank (the luma base and
    // transparency; the colour is the vertex's).
    static uint64_t material_key(const rt::GeoPoly &poly) {
        return uint64_t(poly.texheader[0] & 0x23ff) | uint64_t(poly.texheader[1] & 0xff) << 14 |
               uint64_t(poly.texheader[2] & 0x1fff) << 22;
    }

    // This frame's material for the polygon, if the front-to-back pass made one.
    const Material *material_found(const rt::GeoPoly &poly) const {
        auto found = materials_.find(material_key(poly));
        return found != materials_.end() ? &found->second : nullptr;
    }

    const Material *material_for(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
        const uint64_t key = material_key(poly);
        auto found = materials_.find(key);
        if (found != materials_.end()) return &found->second;
        const uint32_t bank_key = uint32_t(poly.texheader[1] & 0xff) | uint32_t((poly.texheader[0] >> 13) & 1) << 8;
        auto bank_found = banks_.find(bank_key);
        if (bank_found == banks_.end() && banks_used_ >= kBanks) { ++no_bank; return nullptr; }
        const Source *source = source_for(poly, mem);
        if (!source) { ++no_build; return nullptr; }
        unsigned bank;
        if (bank_found != banks_.end()) {
            bank = bank_found->second;
        } else {
            bank = banks_used_++;
            banks_.emplace(bank_key, bank);
            for (unsigned i = 0; i < 16; ++i) pvr_set_pal_entry(bank * 16 + i, texel_grey(poly, mem, i));
        }
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_PAL4BPP | PVR_TXRFMT_4BPP_PAL(bank), int(source->w),
                         int(source->h), source->texture, PVR_FILTER_BILINEAR);
        unordered(cxt);
        // The CPU masks texture coordinates (repeat); the header's mirror bits flip.
        cxt.txr.uv_flip = (((poly.texheader[0] >> 8) & 1) ? PVR_UVFLIP_U : 0) | (((poly.texheader[0] >> 9) & 1) ? PVR_UVFLIP_V : 0);
        Material m;
        pvr_poly_compile(&m.header, &cxt);
        m.u_scale = 1.0f / (8.0f * float(source->source_w));
        m.v_scale = 1.0f / (8.0f * float(source->source_h));
        return &materials_.emplace(key, m).first->second;
    }

    // A convex polygon as one strip: 0, 1, n-1, 2, n-2, ... (its header first
    // when it is not the one in force).
    bool polygon(const rt::GeoPoly &poly, const rt::Video &video, uint32_t argb, const Material *material,
                 const void *header, const void *&current) {
        struct Vertex { float x, y, z, u, v; };
        Vertex v[8];
        const int n = poly.num_vertices;
        const float cx = float(video.crtc_x() + poly.center[0]), cy = float((384 - poly.center[1]) + video.crtc_y());
        for (int i = 0; i < n; i++) {
            const float pz = poly.v[i].p[0];
            if (!(pz > 0.0f) || !finite(pz)) return false;
            const float inv = 1.0f / pz;
            v[i].x = kOffsetX + (cx + poly.v[i].x * inv) * kScale;
            v[i].y = (cy - poly.v[i].y * inv) * kScale;
            v[i].z = inv;
            v[i].u = material ? poly.v[i].p[1] * material->u_scale : 0.0f;
            v[i].v = material ? poly.v[i].p[2] * material->v_scale : 0.0f;
            if (!finite(v[i].x) || !finite(v[i].y) || !finite(v[i].z)) return false;
        }
        // Straight into the store queues (KOS's direct rendering): the list
        // is open, the queues point at the TA.
        if (header != current) {
            auto *dst = reinterpret_cast<uint32_t *>(pvr_dr_target(dr_));
            const auto *src = static_cast<const uint32_t *>(header);
            for (int w = 0; w < 8; w++) dst[w] = src[w];
            pvr_dr_commit(dst);
            current = header;
        }
        int lo = 1, hi = n - 1;
        int strip[8] = {0};
        for (int s = 1; s < n; s++) strip[s] = (s & 1) ? lo++ : hi--;
        for (int k = 0; k < n; k++) {
            const Vertex &in = v[strip[k]];
            pvr_vertex_t *out = pvr_dr_target(dr_);
            out->flags = k == n - 1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            out->x = in.x;
            out->y = in.y;
            out->z = in.z;
            out->u = in.u;
            out->v = in.v;
            out->argb = argb;
            out->oargb = 0;
            pvr_dr_commit(out);
        }
        return true;
    }

    // std::isfinite without the soft-float compare helpers it costs at
    // -fno-fast-math on the SH-4: exponent not all ones.
    static bool finite(float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return (u & 0x7f800000u) != 0x7f800000u;
    }

    size_t budget_;
    std::vector<Region> regions_;
    size_t region_ = 0, region_used_ = 0;
    bool flush_ = false;
    pvr_ptr_t background_, foreground_;
    pvr_poly_hdr_t background_header_, foreground_header_, solid_header_;
    pvr_dr_state_t dr_ = 0;
    uint64_t back_uploaded_ = ~0ull, front_uploaded_ = ~0ull; // the layers' generations in video RAM
    bool clear_row_[kH] = {}; // front layer rows all see-through in video RAM
    alignas(32) uint8_t texels_[kTextureLimit * kTextureLimit / 2] = {};
    std::vector<Entry> order_;
    std::unordered_map<uint32_t, Source> sources_;
    std::unordered_map<uint64_t, Material> materials_;
    std::unordered_map<uint32_t, unsigned> banks_; // luma base and transparency -> palette bank, this frame
    unsigned banks_used_ = 0, builds_ = 0;
    uint8_t gamma_[256] = {};
};

} // namespace dc
