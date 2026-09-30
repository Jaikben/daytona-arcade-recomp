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
#include "runtime/video_profile.h"

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
    // Vita GPU-fast path: keep the exact CPU tile layers, but let the host
    // draw the 3D polygons. The normal desktop/CPU path remains the default.
    void set_external_3d(bool enabled) { external_3d_ = enabled; render_done_ = false; }
    bool external_3d() const { return external_3d_; }
    const std::vector<uint32_t> &background_layer() const { return background_gpu_; }
    const std::vector<uint32_t> &foreground_layer() const { return foreground_gpu_; }
    const std::vector<GeoPoly> &gpu_polys() const;
    int gpu_windows() const { return gpu_windows_; }
    const VideoMem &gpu_mem() const { return gpu_mem_; }
    int crtc_x() const { return crtc_x_; }
    int crtc_y() const { return crtc_y_; }
    int render_x() const { return render_x_; }
    int render_y() const { return render_y_; }
    uint64_t screen_hash() const;
    const Raster &raster() const { return raster_; }
    bool rendered_now() const { return rendered_now_; } // the last update drew the 3D layer afresh
    uint64_t raster_hash() const { return raster_.hash(0, 495, 0, 383); }

    using ProfileClock = uint64_t (*)();
    void set_profile_clock(ProfileClock clock) { profile_clock_ = clock; }
    const VideoProfile &last_profile() const { return profile_; }
    static constexpr int W = 496, H = 384;

private:
    uint16_t tile(uint32_t i) const { return uint16_t(tile_ram_[i * 2] | tile_ram_[i * 2 + 1] << 8); }
    void build_layer(int layer); // pixmap_/flags_ for one tilemap
    void draw(std::vector<uint32_t> &bitmap, int layer, int flags);
    void draw_rect(std::vector<uint32_t> &dm, const uint16_t *mask, uint16_t tpri, int flags, int win, int L, int sx,
                   int sy, int xx1, int yy1, int xx2, int yy2);
    void tilemap_draw(std::vector<uint32_t> &dm, int L, int sx, int sy, int minx, int maxx, int miny, int maxy, int flags);

    uint64_t ticks() const { return profile_clock_ ? profile_clock_() : 0; }
    ProfileClock profile_clock_ = nullptr;
    VideoProfile profile_;
#ifdef M2_VITA_RENDER_OPT
    // Snapshot comparisons also see writes made through replay/raw RAM pointers.
    // No write-hook assumptions, hashes with collisions, or per-frame allocation.
    void update_tile_cache();
    std::vector<uint8_t> character_copy_, tile_ram_copy_, character_dirty_;
    std::vector<uint16_t> tile_values_;
    std::vector<uint32_t> background_;
    bool tiles_valid_ = false, layers_dirty_ = true;
#endif
    const uint8_t *tile_ram_, *char_ram_;
    uint32_t pens_[8192];
    bool palette_dirty_ = false;
    int crtc_x_ = 0, crtc_y_ = 0, render_x_ = 90, render_y_ = -8;
    uint8_t gamma_[256];
    std::vector<uint16_t> pixmap_[4];
    std::vector<uint8_t> flags_[4];
    std::vector<uint32_t> screen_, sys24_;
    std::vector<uint32_t> background_gpu_, foreground_gpu_;
    const std::vector<GeoPoly> *gpu_polys_ = nullptr;
    VideoMem gpu_mem_{};
    int gpu_windows_ = 0;
    bool external_3d_ = false;
    Raster raster_;
    bool rendered_now_ = false, render_done_ = false;
};

} // namespace rt
