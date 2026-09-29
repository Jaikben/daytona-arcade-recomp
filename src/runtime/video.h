// license:BSD-3-Clause
// copyright-holders:Olivier Galibert, R. Belmont, ElSemi, Angelo Salese
//
// Model 2 screen output: the Sega System 24 tilemap chip (segaic24: four
// 64x64-tile layers, per-line scroll, 8-pixel window masks), the palette the
// tilemaps use, the CRTC offsets, and the composition of the 2D layers with
// the 3D layer (Raster), as MAME's model2_state::screen_update does.
// Transplanted from MAME at dddd73680656e355bb2b5beecab1167c9f07bf81
// (src/mame/sega/segaic24.cpp, model2_v.cpp, model2.cpp; BSD-3-Clause,
// notices above kept as the licence requires). MAME's generic tilemap engine
// is replaced by a direct renderer with the same pixel rules. See
// THIRD_PARTY.md.
#pragma once

#include "runtime/raster.h"

#include <cstdint>
#include <vector>

namespace rt {

class Video {
public:
    // tile_ram: 0x01000000 (0x10000 bytes, u16 entries); char_ram: 0x01080000
    // (0x80000 bytes, u16 entries); both as the i960 wrote them.
    Video(const uint8_t *tile_ram, const uint8_t *char_ram);

    // Register writes (MAME handlers), fed by the bus as they happen.
    // palette_w after the bus has stored the write (palram holds the new value).
    void palette_w(uint32_t offset, const uint8_t *palram, const uint8_t *colorxlat);
    void colorxlat_w(uint32_t offset) { if ((offset & 0xff) == 0x80 / 2) palette_dirty_ = true; }
    void xhout_w(uint16_t data) { crtc_x_ = 84 + int16_t(data); render_x_ = crtc_x_; }
    void xvout_w(uint16_t data) { crtc_y_ = 130 + int16_t(data); render_y_ = crtc_y_; }

    // The geometrizer started a new frame (MAME render_frame_start).
    void frame_start() { render_done_ = false; }
    // MAME screen_update at the end of vblank: 2D back layers, the 3D layer
    // (drawn from `polys` once per geometrizer frame, then reused), 2D front
    // layers. Output: 496x384, 0xAARRGGBB.
    void screen_update(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem);
    const std::vector<uint32_t> &screen() const { return screen_; }
    uint64_t screen_hash() const;
    const Raster &raster() const { return raster_; }
    bool rendered_now() const { return rendered_now_; } // the last update drew the 3D layer afresh
    uint64_t raster_hash() const { return raster_.hash(0, 495, 0, 383); }

    static constexpr int W = 496, H = 384;

    // GPU mode (the windowed game): screen_update leaves the 3D layer to the
    // GPU renderer. screen() then holds the back 2D layers, front() the front
    // layers (0 where transparent), and gpu_frame() the 3D layer's triangles
    // when the update prepared a new one (gpu_frame_new()); layer3d_on() says
    // whether the 3D layer shows. `reference` also draws it on the CPU
    // (raster()) so the two can be compared.
    void set_gpu(bool on, float scale = 1.0f, bool reference = false) { gpu_ = on, gpu_scale_ = scale, gpu_ref_ = reference; }
    bool gpu() const { return gpu_; }
    const std::vector<uint32_t> &front() const { return front_; }
    const GpuFrame &gpu_frame() const { return gpu_frame_; }
    bool gpu_frame_new() const { return rendered_now_; }
    bool layer3d_on() const { return render_done_; }

private:
    uint16_t tile(uint32_t i) const { return uint16_t(tile_ram_[i * 2] | tile_ram_[i * 2 + 1] << 8); }
    void build_layer(int layer); // pixmap_/flags_ for one tilemap
    void draw(std::vector<uint32_t> &bitmap, int layer, int flags);
    void draw_rect(std::vector<uint32_t> &dm, const uint16_t *mask, uint16_t tpri, int flags, int win, int L, int sx,
                   int sy, int xx1, int yy1, int xx2, int yy2);
    void tilemap_draw(std::vector<uint32_t> &dm, int L, int sx, int sy, int minx, int maxx, int miny, int maxy, int flags);

    const uint8_t *tile_ram_, *char_ram_;
    uint32_t pens_[8192];
    bool palette_dirty_ = false;
    int crtc_x_ = 0, crtc_y_ = 0, render_x_ = 90, render_y_ = -8;
    uint8_t gamma_[256];
    std::vector<uint16_t> pixmap_[4];
    std::vector<uint8_t> flags_[4];
    std::vector<uint32_t> screen_, sys24_;
    Raster raster_;
    bool gpu_ = false, gpu_ref_ = false;
    float gpu_scale_ = 1.0f;
    std::vector<uint32_t> front_;
    GpuFrame gpu_frame_;
    bool rendered_now_ = false, render_done_ = false;
};

} // namespace rt
