// license:BSD-3-Clause
// copyright-holders:Olivier Galibert, R. Belmont, ElSemi, Angelo Salese
//
// Model 2 screen output, transplanted from MAME (see video.h): segaic24's
// tile_info, draw_rect (rgb32) and draw_common; model2_state::palette_w,
// colorxlat_w, horizontal/vertical_sync_w, render_polygons' frame logic and
// screen_update. MAME's tilemap engine is replaced by build_layer (the same
// pixmap and flags MAME's tilemap caches hold) and tilemap_draw (the same
// pixel rule as tilemap_t::draw with one scroll value).

#include "runtime/video.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rt {

namespace {
constexpr uint8_t PIXEL_LAYER0 = 0x10;  // TILEMAP_PIXEL_LAYER0
constexpr uint8_t CATEGORY_MASK = 0x0f; // TILEMAP_PIXEL_CATEGORY_MASK
constexpr int DRAW_OPAQUE = 0x80;       // TILEMAP_DRAW_OPAQUE
inline uint16_t le16(const uint8_t *b, uint32_t i) { return uint16_t(b[i * 2] | b[i * 2 + 1] << 8); }
inline uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return 0xff000000u | (r << 16) | (g << 8) | b; }
} // namespace

Video::Video(const uint8_t *tile_ram, const uint8_t *char_ram)
    : tile_ram_(tile_ram), char_ram_(char_ram), screen_(size_t(W) * H), sys24_(size_t(W) * (H + 4)),
      background_gpu_(size_t(W) * H), foreground_gpu_(size_t(W) * H) {
    for (auto &p : pens_) p = rgb(0, 0, 0); // palette_device starts black
    for (int i = 0; i < 256; i++) gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));
    for (int l = 0; l < 4; l++) pixmap_[l].assign(512 * 512, 0), flags_[l].assign(512 * 512, 0);
#ifdef M2_VITA_RENDER_OPT
    character_copy_.resize(0x80000);
    character_dirty_.resize(0x4000);
    tile_ram_copy_.resize(0x10000);
    tile_values_.resize(4 * 4096);
    background_.resize(size_t(W) * (H + 4));
#endif
}

void Video::palette_w(uint32_t offset, const uint8_t *palram, const uint8_t *colorxlat) {
    const uint16_t palcolor = le16(palram, offset);
    const uint8_t r = uint8_t(le16(colorxlat, (0x0080 >> 1) + (((palcolor >> 0) & 0x1f) << 8)));
    const uint8_t g = uint8_t(le16(colorxlat, (0x4080 >> 1) + (((palcolor >> 5) & 0x1f) << 8)));
    const uint8_t b = uint8_t(le16(colorxlat, (0x8080 >> 1) + (((palcolor >> 10) & 0x1f) << 8)));
    const uint32_t pen = rgb(gamma_[r], gamma_[g], gamma_[b]);
#ifdef M2_VITA_RENDER_OPT
    if (pens_[offset & 0x1fff] != pen) layers_dirty_ = true;
#endif
    pens_[offset & 0x1fff] = pen;
}

// segaic24 tile_info + MAME tilemap pixmap: 64x64 tiles (TILEMAP_SCAN_ROWS)
// of 8x8, 4bpp chars (char_layout, bit order swapped within 16-bit words),
// pen = color * 16 + pixel, pen 0 transparent, category = tile bit 15.
void Video::build_layer(int layer) {
    const uint32_t base = uint32_t(layer) * 0x1000; // tile_info_0s/0w/1s/1w
    uint16_t *pm = pixmap_[layer].data();
    uint8_t *fm = flags_[layer].data();
    for (uint32_t t = 0; t < 64 * 64; t++) {
        const uint16_t val = tile(t | base);
        const uint32_t code = val & 0x3fff;
#ifdef M2_VITA_RENDER_OPT
        uint16_t &previous = tile_values_[base + t];
        if (tiles_valid_ && previous == val && !character_dirty_[code]) continue;
        previous = val;
#endif
        ++profile_.tiles_rebuilt;
        const uint32_t color = (val >> 7) & 0xff;
        const uint8_t category = (val & 0x8000) ? 1 : 0;
        const uint32_t tx = (t & 63) * 8, ty = (t >> 6) * 8;
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 8; x++) {
                const uint32_t b = code * 32 + y * 4 + (x >> 1);
                const uint8_t byte = char_ram_[b ^ 1];
                const uint8_t pix = (x & 1) ? (byte & 0x0f) : (byte >> 4);
                const size_t i = size_t(ty + y) * 512 + (tx + x);
                pm[i] = uint16_t(color * 16 + pix);
                fm[i] = uint8_t(category | (pix ? PIXEL_LAYER0 : 0));
            }
    }
}

#ifdef M2_VITA_RENDER_OPT
void Video::update_tile_cache() {
    const bool chars_changed = !tiles_valid_ ||
        std::memcmp(char_ram_, character_copy_.data(), character_copy_.size()) != 0;
    const bool ram_changed = !tiles_valid_ ||
        std::memcmp(tile_ram_, tile_ram_copy_.data(), tile_ram_copy_.size()) != 0;
    if (!chars_changed && !ram_changed) return;
    std::fill(character_dirty_.begin(), character_dirty_.end(), uint8_t(0));
    if (chars_changed) {
        for (size_t code = 0; code < character_dirty_.size(); ++code) {
            const size_t offset = code * 32;
            if (!tiles_valid_ || std::memcmp(char_ram_ + offset, character_copy_.data() + offset, 32) != 0) {
                std::memcpy(character_copy_.data() + offset, char_ram_ + offset, 32);
                character_dirty_[code] = 1;
                ++profile_.characters_changed;
            }
        }
    }
    for (int layer = 0; layer < 4; ++layer) build_layer(layer);
    // The remaining tile RAM contains scrolling, window masks and line tables.
    // Any change there invalidates composition even when no glyph was rebuilt.
    if (ram_changed) std::memcpy(tile_ram_copy_.data(), tile_ram_, tile_ram_copy_.size());
    if (!tiles_valid_ || ram_changed || profile_.tiles_rebuilt) layers_dirty_ = true;
    tiles_valid_ = true;
}
#endif

// segaic24 draw_rect, rgb32 version (model 1/2): copy a rectangle of the
// layer's pixmap to the bitmap through the 8-pixel window mask.
void Video::draw_rect(std::vector<uint32_t> &dm, const uint16_t *mask, uint16_t tpri, int flags, int win, int L, int sx,
                      int sy, int xx1, int yy1, int xx2, int yy2) {
    const uint16_t *source = &pixmap_[L][size_t(sy) * 512 + size_t(sx)];
    const uint8_t *trans = &flags_[L][size_t(sy) * 512 + size_t(sx)];
    uint32_t *dest = &dm[size_t(yy1) * W + size_t(xx1)];
    tpri |= PIXEL_LAYER0;
    mask += yy1 * 4;
    yy2 -= yy1;
    while (xx1 >= 128) {
        xx1 -= 128;
        xx2 -= 128;
        mask++;
    }
    for (int y = 0; y < yy2; y++) {
        const uint16_t *src = source;
        const uint8_t *srct = trans;
        uint32_t *dst = dest;
        const uint16_t *mask1 = mask;
        int llx = xx2;
        int cur_x = xx1;
        while (llx > 0) {
            uint16_t m = *mask1++;
            if (win) m = uint16_t(~m);
            if (!cur_x && llx >= 128) {
                if (!m) {
                    for (int x = 0; x < 128; x++) {
                        if (*srct++ == tpri || (flags & DRAW_OPAQUE)) *dst = pens_[*src];
                        src++;
                        dst++;
                    }
                } else if (m == 0xffff) {
                    src += 128;
                    srct += 128;
                    dst += 128;
                } else {
                    for (int x = 0; x < 128; x += 8) {
                        if (!(m & 0x8000))
                            for (int xx = 0; xx < 8; xx++)
                                if (srct[xx] == tpri || (flags & DRAW_OPAQUE)) dst[xx] = pens_[src[xx]];
                        src += 8;
                        srct += 8;
                        dst += 8;
                        m = uint16_t(m << 1);
                    }
                }
            } else {
                const int llx1 = llx >= 128 ? 128 : llx;
                if (!m) {
                    for (int x = cur_x; x < llx1; x++) {
                        if (*srct++ == tpri || (flags & DRAW_OPAQUE)) *dst = pens_[*src];
                        src++;
                        dst++;
                    }
                } else if (m == 0xffff) {
                    src += 128 - cur_x;
                    srct += 128 - cur_x;
                    dst += 128 - cur_x;
                } else {
                    for (int x = cur_x; x < llx1; x++) {
                        if ((*srct++ == tpri || (flags & DRAW_OPAQUE)) && !(m & (0x8000 >> (x >> 3)))) *dst = pens_[*src];
                        src++;
                        dst++;
                    }
                }
            }
            llx -= 128;
            cur_x = 0;
        }
        source += 512;
        trans += 512;
        dest += W;
        mask += 4;
    }
}

// tilemap_t::draw with one scroll value: dest (x, y) takes pixmap
// ((x + sx) & 511, (y + sy) & 511) where (flags & mask) == value; mask is the
// category, plus layer 0 (opacity) unless drawing opaque.
void Video::tilemap_draw(std::vector<uint32_t> &dm, int L, int sx, int sy, int minx, int maxx, int miny, int maxy, int flags) {
    const uint8_t cat = uint8_t(flags & CATEGORY_MASK);
    const uint8_t mask = (flags & DRAW_OPAQUE) ? CATEGORY_MASK : uint8_t(CATEGORY_MASK | PIXEL_LAYER0);
    const uint8_t value = (flags & DRAW_OPAQUE) ? cat : uint8_t(cat | PIXEL_LAYER0);
    for (int y = std::max(miny, 0); y <= std::min(maxy, H - 1); y++)
        for (int x = std::max(minx, 0); x <= std::min(maxx, W - 1); x++) {
            const size_t i = size_t((y + sy) & 511) * 512 + size_t((x + sx) & 511);
            if ((flags_[L][i] & mask) == value) dm[size_t(y) * W + size_t(x)] = pens_[pixmap_[L][i]];
        }
}

// segaic24 draw_common for the rgb32 bitmap, cliprect = the whole screen.
void Video::draw(std::vector<uint32_t> &bitmap, int layer, int flags) {
    uint16_t hscr = tile(0x5000 + uint32_t(layer >> 1));
    uint16_t vscr = tile(0x5004 + uint32_t(layer >> 1));
    const uint16_t ctrl = tile(0x5004 + uint32_t((layer >> 1) & 2));
    uint16_t mask[0x800];
    for (uint32_t i = 0; i < 0x800; i++) mask[i] = tile((layer & 4 ? 0x6800 : 0x6000) + i);
    const uint16_t tpri = uint16_t(layer & 1);
    layer >>= 1;
    const int fl = tpri | flags;

    if (vscr & 0x8000) return; // layer disable

    if (ctrl & 0x6000) { // special window/scroll modes
        if (layer & 1) return;
        const int sy = vscr & 0x1ff;
        if (hscr & 0x8000) {
            const uint32_t hscrtb = 0x4000 + 0x200 * uint32_t(layer);
            switch ((ctrl & 0x6000) >> 13) {
            case 1: {
                const uint16_t v = uint16_t((-vscr) & 0x1ff);
                if (!((-vscr) & 0x200)) layer ^= 1;
                for (int y = 0; y < H; y++) {
                    const int l1 = y >= v ? layer ^ 1 : layer;
                    const uint16_t h = tile(hscrtb + uint32_t(y)) & 0x1ff;
                    tilemap_draw(bitmap, l1, -h, sy, 0, W - 1, y, y, fl);
                }
                break;
            }
            case 2:
            case 3:
                for (int y = 0; y < H; y++) {
                    hscr = tile(hscrtb + uint32_t(y));
                    const int h = hscr & 0x1ff;
                    int l1 = layer;
                    if (!(hscr & 0x200)) l1 ^= 1;
                    tilemap_draw(bitmap, l1, -h, sy, 0, std::min(W - 1, h - 1), y, y, fl);
                    tilemap_draw(bitmap, l1 ^ 1, -h, sy, std::max(0, h), W - 1, y, y, fl);
                }
                break;
            }
        } else {
            const int sx = -(hscr & 0x1ff);
            switch ((ctrl & 0x6000) >> 13) {
            case 1: {
                const int v = (-vscr) & 0x1ff;
                if (!((-vscr) & 0x200)) layer ^= 1;
                tilemap_draw(bitmap, layer, sx, sy, 0, W - 1, 0, std::min(H - 1, v - 1), fl);
                tilemap_draw(bitmap, layer ^ 1, sx, sy, 0, W - 1, std::max(0, v), H - 1, fl);
                break;
            }
            case 2:
            case 3: {
                const int h = hscr & 0x1ff;
                if (!(hscr & 0x200)) layer ^= 1;
                tilemap_draw(bitmap, layer, sx, sy, 0, std::min(W - 1, h - 1), 0, H - 1, fl);
                tilemap_draw(bitmap, layer ^ 1, sx, sy, std::max(0, h), W - 1, 0, H - 1, fl);
                break;
            }
            }
        }
        return;
    }

    const int win = layer & 1;
    if (hscr & 0x8000) {
        const uint32_t hscrtb = 0x4000 + 0x200 * uint32_t(layer);
        vscr &= 0x1ff;
        for (int y = 0; y < 384; y++) {
            hscr = uint16_t((-tile(hscrtb + uint32_t(y))) & 0x1ff);
            if (hscr + 496 <= 512) {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, y, 496, y + 1);
            } else {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, y, 512 - hscr, y + 1);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, vscr, 512 - hscr, y, 496, y + 1);
            }
            vscr = (vscr + 1) & 0x1ff;
        }
    } else {
        hscr = uint16_t((-hscr) & 0x1ff);
        vscr = uint16_t((+vscr) & 0x1ff);
        if (hscr + 496 <= 512) {
            if (vscr + 384 <= 512) {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, 496, 384);
            } else {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, 496, 512 - vscr);
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, 0, 0, 512 - vscr, 496, 384);
            }
        } else {
            if (vscr + 384 <= 512) {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, 512 - hscr, 384);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, vscr, 512 - hscr, 0, 496, 384);
            } else {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, 512 - hscr, 512 - vscr);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, vscr, 512 - hscr, 0, 496, 512 - vscr);
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, 0, 0, 512 - vscr, 512 - hscr, 384);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, 0, 512 - hscr, 512 - vscr, 496, 384);
            }
        }
    }
}

const std::vector<GeoPoly> &Video::gpu_polys() const {
    static const std::vector<GeoPoly> empty;
    return gpu_polys_ ? *gpu_polys_ : empty;
}

void Video::screen_update(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem) {
    gpu_polys_ = &polys;
    gpu_windows_ = windows;
    gpu_mem_ = mem;
    profile_ = {};
    uint64_t before = ticks();
    // Retain the reference's sticky palette-dirty behavior. palette_w marks
    // cached composition dirty only if the resulting RGB value really changed.
    if (palette_dirty_)
        for (uint32_t i = 0; i < 0x1000; i++) palette_w(i, mem.palram, mem.colorxlat);
#ifdef M2_VITA_RENDER_OPT
    update_tile_cache();
#else
    for (int l = 0; l < 4; l++) build_layer(l);
#endif
    profile_.tile_cache = ticks() - before;
    auto copy_trans = [&](const uint32_t *source, size_t stride) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (const uint32_t pixel = source[size_t(y) * stride + size_t(x)])
                    screen_[size_t(y) * W + size_t(x)] = pixel;
    };
#ifdef M2_VITA_RENDER_OPT
    before = ticks();
    if (layers_dirty_) {
        // All tile writes are replacements, not blends. Drawing the back
        // layers over pen 0 is identical to zero + transparent copy over pen 0.
        std::fill(background_.begin(), background_.end(), pens_[0]);
        for (int layer = 3; layer >= 2; --layer) draw(background_, layer << 1, DRAW_OPAQUE);
        for (int layer = 1; layer >= 0; --layer) draw(background_, layer << 1, 0);
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
        layers_dirty_ = false;
        profile_.layers_rebuilt = true;
    }
    profile_.tile_draw = ticks() - before;
    before = ticks();
    std::copy_n(background_.data(), screen_.size(), screen_.data());
    profile_.composite += ticks() - before;
#else
    before = ticks();
    std::fill(screen_.begin(), screen_.end(), pens_[0]);
    std::fill(sys24_.begin(), sys24_.end(), 0u);
    for (int layer = 3; layer >= 2; --layer) draw(sys24_, layer << 1, DRAW_OPAQUE);
    for (int layer = 1; layer >= 0; --layer) draw(sys24_, layer << 1, 0);
    profile_.tile_draw += ticks() - before;
    profile_.layers_rebuilt = true;
    before = ticks();
    copy_trans(sys24_.data(), W);
    profile_.composite += ticks() - before;
#endif
    rendered_now_ = false;
    if (external_3d_) {
        // Save the exact two System-24 layers separately. The Vita frontend
        // draws background -> GPU 3D -> foreground. No CPU polygon pixels are
        // produced in this mode, so raster_ms should remain zero.
        std::copy_n(screen_.data(), screen_.size(), background_gpu_.data());
#ifndef M2_VITA_RENDER_OPT
        // Reference path has not drawn the post-3D tile pass yet.
        before = ticks();
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
        profile_.tile_draw += ticks() - before;
#endif
        std::fill(foreground_gpu_.begin(), foreground_gpu_.end(), 0u);
        std::copy_n(sys24_.data(), std::min(sys24_.size(), foreground_gpu_.size()), foreground_gpu_.data());
        return;
    }
    if (!render_done_ && !polys.empty()) {
        before = ticks();
        raster_.render(polys, windows, mem, crtc_x_, crtc_y_, render_x_, render_y_, 0, W - 1, 0, H - 1);
        profile_.raster = ticks() - before;
        render_done_ = true;
        rendered_now_ = true;
    }
    before = ticks();
    if (render_done_) copy_trans(raster_.pixels(), 512);
    profile_.composite += ticks() - before;
#ifndef M2_VITA_RENDER_OPT
    before = ticks();
    std::fill(sys24_.begin(), sys24_.end(), 0u);
    for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
    profile_.tile_draw += ticks() - before;
#endif
    before = ticks();
    copy_trans(sys24_.data(), W);
    profile_.composite += ticks() - before;
}

uint64_t Video::screen_hash() const {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t px : screen_)
        for (int b = 0; b < 4; b++) {
            h ^= (px >> (8 * b)) & 0xff;
            h *= 0x100000001b3ULL;
        }
    return h;
}

} // namespace rt
