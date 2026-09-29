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

#include <cstdint>
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

// One vertex of the GPU renderer's 3D layer (src/app/gpu_renderer.cpp,
// shaders/poly.*.hlsl): the polygon projected and set up exactly as the CPU
// rasterizer sets it up, with the polygon's shading state packed alongside.
struct GpuVertex {
    float x, y, depth, scale; // 3D-layer pixels; draw-order depth (first drawn wins); render scale
    float ooz, uoz, voz;      // 1/z, u/z, v/z (textured polygons)
    uint32_t a[4], b[4], c[4]; // shading state and clip rectangle (see shaders/poly.frag.hlsl)
};
static_assert(sizeof(GpuVertex) == 76);

// A frame of the 3D layer for the GPU: triangles in draw order plus the
// colour-table, luma and gamma bytes the pixel shader reads.
struct GpuFrame {
    std::vector<GpuVertex> verts;
    std::vector<uint32_t> vmem; // colorxlat (0xc000 bytes), luma lane 0 (0x8000), gamma (0x100)
};

// Build the GPU frame for the same display list Raster::render draws.
void prepare_gpu_frame(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem, int crtc_x, int crtc_y,
                       int render_x, int render_y, int clip_minx, int clip_maxx, int clip_miny, int clip_maxy, float scale,
                       GpuFrame &out);

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

    const uint32_t *pixels() const { return dest_.data(); } // 512x512, 0x00RRGGBB
    uint64_t hash(int minx, int maxx, int miny, int maxy) const; // as the MAME log computes it

    struct Extra; // per-polygon shading state (MAME m2_poly_extra_data)
    // model2_3d_project and the set-up half of model2_3d_render, shared with
    // the GPU path. Returns the renderer (0-3); fills extra and clip.
    static int prepare(GeoPoly &poly, const VideoMem &mem, int crtc_x, int crtc_y, int render_x, int render_y, int clip_minx,
                       int clip_maxx, int clip_miny, int clip_maxy, Extra &extra, int *clip);

private:
    std::vector<uint32_t> dest_;
    std::vector<uint8_t> fill_;
    uint8_t gamma_[256];
    const VideoMem *mem_ = nullptr;

    void render_one(GeoPoly poly, int crtc_x, int crtc_y, int render_x, int render_y, int clip_minx, int clip_maxx,
                    int clip_miny, int clip_maxy);
    template <bool Translucent> void draw_scanline_solid(int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o);
    template <bool Translucent> void draw_scanline_tex(int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o);
    void scanline(int renderer, int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o);
    void render_triangle(const int *clip, int renderer, const Extra &o, const GeoVertex &v1, const GeoVertex &v2, const GeoVertex &v3);
    template <int NumVerts> void render_polygon(const int *clip, int renderer, const Extra &o, const GeoVertex *v);
    template <bool Translucent> uint32_t fetch_bilinear_texel(const Extra &o, int32_t miplevel, int32_t u, int32_t v) const;
};

} // namespace rt
