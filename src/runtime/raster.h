// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels, Ville Linde, Aaron Giles
//
// CPU reference rasterizer for the Model 2 3D layer: projects, orders and
// draws a frame's display list (Geo::polys) into a 512x512 RGB32 layer.
// Transplanted from MAME at dddd73680656e355bb2b5beecab1167c9f07bf81
// (BSD-3-Clause; notices above kept as the licence requires):
// src/mame/sega/model2_v.cpp (model2_3d_project, render_polygons,
// model2_renderer::model2_3d_render), src/mame/sega/model2rd.ipp (scanline
// shaders, bilinear texel fetch) and the parts of src/devices/video/poly.h
// they use (render_triangle, render_polygon, round_coordinate), drawn
// immediately in order instead of through MAME's work queue. This is the
// ground truth for the GPU renderer, checked against MAME frame by frame.
// See THIRD_PARTY.md.
#pragma once

#include "runtime/geo.h"

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <array>
#include <vector>

namespace rt {

// The video memories the rasterizer reads, as the i960 sees them.
struct VideoMem {
    const uint8_t *palram = nullptr;    // 0x01800000, 0x4000 bytes (u16 entries)
    const uint8_t *colorxlat = nullptr; // 0x01810000, 0xc000 bytes (u16 entries)
    const uint8_t *lumaram = nullptr;   // 0x12800000, 0x20000 bytes (byte lane 0 of each dword)
    const uint32_t *tex0 = nullptr;     // texture RAM 0, packed as MAME stores it (0x80000 dwords)
    const uint32_t *tex1 = nullptr;     // texture RAM 1
};

class Raster {
public:
    Raster();

    // MAME render_polygons: clear, then draw windows from the last down to
    // 0, each in z-bucket order (low to high z; newest first within a
    // bucket). crtc_x/crtc_y: MAME's m_crtc_xoffset/m_crtc_yoffset (the
    // projection); render_x/render_y: the renderer's offsets (the viewport;
    // equal to the CRTC's once the game has set them). clip: the visible
    // area, inclusive.
    void render(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem, int crtc_x, int crtc_y,
                int clip_minx, int clip_maxx, int clip_miny, int clip_maxy) {
        render(polys, windows, mem, crtc_x, crtc_y, crtc_x, crtc_y, clip_minx, clip_maxx, clip_miny, clip_maxy);
    }
    void render(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem, int crtc_x, int crtc_y, int render_x,
                int render_y, int clip_minx, int clip_maxx, int clip_miny, int clip_maxy);

    const uint32_t *pixels() const { return dest_.data(); } // stride() x 512, 0x00RRGGBB
    int stride() const { return stride_; }                   // 512, wider for widescreen
    // Widescreen (enhancement, 0 = off): callers shift x by `margin`; a polygon
    // whose viewport spans the 496-pixel screen may then draw `margin` pixels
    // beyond either side. The layer grows to hold the wider screen.
    void set_wide_margin(int margin);
    // Widescreen, HUD at the edges: HUD overlay polygons (the game draws them
    // at a fixed near depth, z 1536; scenery near the HUD is above 18000)
    // lying wholly inside a moved HUD group's rectangle (496-wide screen
    // coordinates) move with it: the condition panel's box and car.
    struct HudMove { int x0 = 0, x1 = 0, y0 = 0, y1 = 0, dx = 0; };
    static constexpr uint16_t kHudOverlayZ = 0x0fff; // sort z: the smallest exponent (HUD overlays are 0x0600)
    // Whether the race HUD is on screen: its condition panel's box, one
    // checker-shaded overlay polygon (texheader 0x8000, sort z <= kHudOverlayZ)
    // about 77x82 pixels at x 385..462, y 67..149 (496-wide coordinates).
    // Without it nothing is moved: car close-ups, attract and ranking screens
    // put near polygons and 2D text in the HUD's areas too.
    bool race_hud_visible(const std::vector<GeoPoly> &polys, int crtc_x, int crtc_y) const;
    void set_hud_moves(const HudMove *moves, int count) {
        hud_moves_count_ = std::min(count, 3);
        std::copy_n(moves, hud_moves_count_, hud_moves_);
    }
    uint64_t hash(int minx, int maxx, int miny, int maxy) const; // as the MAME log computes it

    struct Extra; // per-polygon shading state (MAME m2_poly_extra_data)

private:
    int stride_ = 512, margin_ = 0;
    HudMove hud_moves_[3];
    int hud_moves_count_ = 0;
    std::vector<uint32_t> dest_;
    std::vector<uint8_t> fill_;
    uint8_t gamma_[256];
    const VideoMem *mem_ = nullptr;
#ifdef M2_VITA_RENDER_OPT
    struct ShadeEntry {
        uint32_t key = 0xffffffffu;
        std::array<uint32_t, 128> colors{};
    };
    // Cleared logically every render: palette/luma/translation RAM may change
    // between frames. A pointer is used only while its polygon is being drawn.
    std::array<ShadeEntry, 64> shades_;
    const uint32_t *shade_table(const Extra &o);
    std::vector<std::size_t> order_;
#endif

    void render_one(GeoPoly poly, int crtc_x, int crtc_y, int render_x, int render_y, int clip_minx, int clip_maxx,
                    int clip_miny, int clip_maxy);
    template <bool Translucent> void draw_scanline_solid(int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o);
    template <bool Translucent> void draw_scanline_tex(int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o);
    template <bool Translucent, bool Cached> void draw_tex_span(int32_t y, int32_t x0, int32_t x1,
        const float *start, const float *dpdx, const Extra &o);
    void scanline(int renderer, int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o);
    void render_triangle(const int *clip, int renderer, const Extra &o, const GeoVertex &v1, const GeoVertex &v2, const GeoVertex &v3);
    template <int NumVerts> void render_polygon(const int *clip, int renderer, const Extra &o, const GeoVertex *v);
    template <bool Translucent> uint32_t fetch_bilinear_texel(const Extra &o, int32_t miplevel, int32_t u, int32_t v) const;
};

} // namespace rt
