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
#include <cstdio>
#include <cstdlib>
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
    system24_tile_generations_.resize(4 * 4096);
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
    if (pens_[offset & 0x1fff] != pen) {
#ifdef M2_VITA_RENDER_OPT
        background_dirty_ = foreground_dirty_ = true;
#endif
        system24_source_dirty_ = true;
        system24_palette_generation_ = system24_texture_generation_ + 1;
    }
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
        if (!tiles_valid_) background_dirty_ = foreground_dirty_ = true;
        else {
            (previous & 0x8000 ? foreground_dirty_ : background_dirty_) = true;
            (val & 0x8000 ? foreground_dirty_ : background_dirty_) = true;
        }
        previous = val;
#endif
        ++profile_.tiles_rebuilt;
        system24_source_dirty_ = true;
        system24_tile_generations_[base + t] = system24_texture_generation_ + 1;
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
    const bool chars_changed = !tiles_valid_ || (write_tracking_ ? character_memory_touched_ :
        std::memcmp(char_ram_, character_copy_.data(), character_copy_.size()) != 0);
    const bool ram_changed = !tiles_valid_ || (write_tracking_ ? tile_memory_touched_ :
        std::memcmp(tile_ram_, tile_ram_copy_.data(), tile_ram_copy_.size()) != 0);
    if (!chars_changed && !ram_changed) return;
    std::fill(character_dirty_.begin(), character_dirty_.end(), uint8_t(0));
    if (chars_changed) {
        constexpr size_t page_bytes = 256;
        constexpr size_t chars_per_page = page_bytes / 32;
        for (size_t page = 0; page < character_copy_.size(); page += page_bytes) {
            if (tiles_valid_ && std::memcmp(char_ram_ + page, character_copy_.data() + page, page_bytes) == 0)
                continue;
            const size_t first = page / 32;
            for (size_t local = 0; local < chars_per_page; ++local) {
                const size_t code = first + local;
                const size_t offset = code * 32;
                if (!tiles_valid_ || std::memcmp(char_ram_ + offset, character_copy_.data() + offset, 32) != 0) {
                    std::memcpy(character_copy_.data() + offset, char_ram_ + offset, 32);
                    character_dirty_[code] = 1;
                    ++profile_.characters_changed;
                }
            }
        }
    }
    bool draw_state_changed = !tiles_valid_;
    if (tiles_valid_ && ram_changed) {
        auto changed = [&](size_t offset, size_t bytes) {
            return std::memcmp(tile_ram_ + offset, tile_ram_copy_.data() + offset, bytes) != 0;
        };
        // Line-scroll tables, layer control/scroll registers and window masks.
        draw_state_changed = changed(0x8000, 0x1000) || changed(0xa000, 0x10) || changed(0xc000, 0x2000);
    }
    for (int layer = 0; layer < 4; ++layer) build_layer(layer);
    // Normal-mode opaque backgrounds ignore category, whereas split modes
    // still filter category 0. Changing modes changes uploaded alpha even
    // when tile and character RAM are unchanged.
    if (tiles_valid_ && ram_changed &&
        bool(tile(0x5006) & 0x6000) != bool(le16(tile_ram_copy_.data(), 0x5006) & 0x6000)) {
        std::fill(system24_tile_generations_.begin() + 2 * 4096,
                  system24_tile_generations_.end(), system24_texture_generation_ + 1);
        system24_source_dirty_ = true;
    }
    // The remaining tile RAM contains scrolling, window masks and line tables.
    // Any change there invalidates composition even when no glyph was rebuilt.
    if (ram_changed) std::memcpy(tile_ram_copy_.data(), tile_ram_, tile_ram_copy_.size());
    if (draw_state_changed) background_dirty_ = foreground_dirty_ = true;
    tiles_valid_ = true;
    tile_memory_touched_ = character_memory_touched_ = false;
}
#endif

// segaic24 draw_rect, rgb32 version (model 1/2): copy a rectangle of the
// layer's pixmap to the bitmap through the 8-pixel window mask.
void Video::draw_rect(std::vector<uint32_t> &dm, const uint16_t *mask, uint16_t tpri, int flags, int win, int L, int sx,
                      int sy, int xx1, int yy1, int xx2, int yy2) {
    const uint16_t *source = &pixmap_[L][size_t(sy) * 512 + size_t(sx)];
    const uint8_t *trans = &flags_[L][size_t(sy) * 512 + size_t(sx)];
    uint32_t *dest = &dm[size_t(yy1) * size_t(dw_) + size_t(xx1)];
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
        dest += dw_;
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
        for (int x = std::max(minx, 0); x <= std::min(maxx, dw_ - 1); x++) {
            const size_t i = size_t((y + sy) & 511) * 512 + size_t((x + sx) & 511);
            if ((flags_[L][i] & mask) == value) dm[size_t(y) * size_t(dw_) + size_t(x)] = pens_[pixmap_[L][i]];
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
                    tilemap_draw(bitmap, l1, -h, sy, 0, dw_ - 1, y, y, fl);
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
                    tilemap_draw(bitmap, l1, -h, sy, 0, std::min(dw_ - 1, h - 1), y, y, fl);
                    tilemap_draw(bitmap, l1 ^ 1, -h, sy, std::max(0, h), dw_ - 1, y, y, fl);
                }
                break;
            }
        } else {
            const int sx = -(hscr & 0x1ff);
            switch ((ctrl & 0x6000) >> 13) {
            case 1: {
                const int v = (-vscr) & 0x1ff;
                if (!((-vscr) & 0x200)) layer ^= 1;
                tilemap_draw(bitmap, layer, sx, sy, 0, dw_ - 1, 0, std::min(H - 1, v - 1), fl);
                tilemap_draw(bitmap, layer ^ 1, sx, sy, 0, dw_ - 1, std::max(0, v), H - 1, fl);
                break;
            }
            case 2:
            case 3: {
                const int h = hscr & 0x1ff;
                if (!(hscr & 0x200)) layer ^= 1;
                tilemap_draw(bitmap, layer, sx, sy, 0, std::min(dw_ - 1, h - 1), 0, H - 1, fl);
                tilemap_draw(bitmap, layer ^ 1, sx, sy, std::max(0, h), dw_ - 1, 0, H - 1, fl);
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
            if (hscr + dw_ <= 512) {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, y, dw_, y + 1);
            } else {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, y, 512 - hscr, y + 1);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, vscr, 512 - hscr, y, dw_, y + 1);
            }
            vscr = (vscr + 1) & 0x1ff;
        }
    } else {
        hscr = uint16_t((-hscr) & 0x1ff);
        vscr = uint16_t((+vscr) & 0x1ff);
        if (hscr + dw_ <= 512) {
            if (vscr + 384 <= 512) {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, dw_, 384);
            } else {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, dw_, 512 - vscr);
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, 0, 0, 512 - vscr, dw_, 384);
            }
        } else {
            if (vscr + 384 <= 512) {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, 512 - hscr, 384);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, vscr, 512 - hscr, 0, dw_, 384);
            } else {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, 512 - hscr, 512 - vscr);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, vscr, 512 - hscr, 0, dw_, 512 - vscr);
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, 0, 0, 512 - vscr, 512 - hscr, 384);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, 0, 512 - hscr, 512 - vscr, dw_, 384);
            }
        }
    }
}

// draw() for screen columns x0..x0+width-1, any of them off the 496-pixel
// screen (widescreen's margins): the same scroll registers, layer splits,
// priority and window masks, pixel by pixel, as if the screen were wider. A
// column left of the screen comes from the layer and place the hardware's
// rules give it, so a sky spread over two side-by-side layers continues
// instead of repeating one layer. Window masks are screen columns; off the
// screen the nearest column's applies.
void Video::draw_ext(std::vector<uint32_t> &bm, int layer, int flags, int x0, int width) {
    uint16_t hscr = tile(0x5000 + uint32_t(layer >> 1));
    uint16_t vscr = tile(0x5004 + uint32_t(layer >> 1));
    const uint16_t ctrl = tile(0x5004 + uint32_t((layer >> 1) & 2));
    const uint16_t tpri = uint16_t(layer & 1);
    const int L = layer >> 1;
    const int fl = tpri | flags;
    if (vscr & 0x8000) return; // layer disable
    const int xend = x0 + width;

    // tilemap_draw for one row y, screen columns xa..xb
    auto row_tm = [&](int Lr, int sx, int sy, int y, int xa, int xb) {
        const uint8_t cat = uint8_t(fl & CATEGORY_MASK);
        const uint8_t m = (fl & DRAW_OPAQUE) ? CATEGORY_MASK : uint8_t(CATEGORY_MASK | PIXEL_LAYER0);
        const uint8_t v = (fl & DRAW_OPAQUE) ? cat : uint8_t(cat | PIXEL_LAYER0);
        const size_t srow = size_t((y + sy) & 511) * 512;
        for (int x = std::max(xa, x0); x <= std::min(xb, xend - 1); x++) {
            const size_t i = srow + size_t((x + sx) & 511);
            if ((flags_[Lr][i] & m) == v) bm[size_t(y) * size_t(width) + size_t(x - x0)] = pens_[pixmap_[Lr][i]];
        }
    };
    constexpr int kAll0 = -(1 << 20), kAll1 = 1 << 20;

    if (ctrl & 0x6000) { // special window/scroll modes: layers L and L^1 side by side or stacked
        if (L & 1) return;
        const int sy = vscr & 0x1ff;
        const int mode = (ctrl & 0x6000) >> 13;
        if (hscr & 0x8000) {
            const uint32_t hscrtb = 0x4000 + 0x200 * uint32_t(L);
            if (mode == 1) {
                const int v = (-vscr) & 0x1ff;
                int Lb = L;
                if (!((-vscr) & 0x200)) Lb ^= 1;
                for (int y = 0; y < H; y++)
                    row_tm(y >= v ? Lb ^ 1 : Lb, -(tile(hscrtb + uint32_t(y)) & 0x1ff), sy, y, kAll0, kAll1);
            } else {
                for (int y = 0; y < H; y++) {
                    const uint16_t hl = tile(hscrtb + uint32_t(y));
                    const int h = hl & 0x1ff;
                    int l1 = L;
                    if (!(hl & 0x200)) l1 ^= 1;
                    row_tm(l1, -h, sy, y, kAll0, h - 1);
                    row_tm(l1 ^ 1, -h, sy, y, h, kAll1);
                }
            }
        } else {
            const int sx = -(hscr & 0x1ff);
            if (mode == 1) {
                const int v = (-vscr) & 0x1ff;
                int Lb = L;
                if (!((-vscr) & 0x200)) Lb ^= 1;
                for (int y = 0; y < H; y++) row_tm(y < v ? Lb : Lb ^ 1, sx, sy, y, kAll0, kAll1);
            } else {
                const int h = hscr & 0x1ff;
                int Lb = L;
                if (!(hscr & 0x200)) Lb ^= 1;
                for (int y = 0; y < H; y++) {
                    row_tm(Lb, sx, sy, y, kAll0, h - 1);
                    row_tm(Lb ^ 1, sx, sy, y, h, kAll1);
                }
            }
        }
        return;
    }

    // draw_rect: one layer, whole-screen or per-line scroll, window masks
    const int win = L & 1;
    const uint8_t want = uint8_t(tpri | PIXEL_LAYER0);
    const bool per_line = hscr & 0x8000;
    const uint32_t hscrtb = 0x4000 + 0x200 * uint32_t(L);
    int vs = vscr & 0x1ff;
    const int hs_all = (-hscr) & 0x1ff;
    for (int y = 0; y < H; y++) {
        const int hs = per_line ? ((-tile(hscrtb + uint32_t(y))) & 0x1ff) : hs_all;
        const int sy = per_line ? vs : ((y + vs) & 511);
        const size_t srow = size_t(sy) * 512;
        for (int x = x0; x < xend; x++) {
            const int mx = std::clamp(x, 0, W - 1);
            uint16_t mk = tile((layer & 4 ? 0x6800 : 0x6000) + uint32_t(y * 4 + (mx >> 7)));
            if (win) mk = uint16_t(~mk);
            if (mk & (0x8000 >> ((mx >> 3) & 15))) continue;
            const size_t i = srow + size_t((x + hs) & 511);
            if (flags_[L][i] == want || (flags & DRAW_OPAQUE)) bm[size_t(y) * size_t(width) + size_t(x - x0)] = pens_[pixmap_[L][i]];
        }
        if (per_line) vs = (vs + 1) & 0x1ff;
    }
}

bool Video::system24_gpu_compatible() const {
    // The Vita GXM compositor supports normal windowing plus all three
    // System24 split-layer modes. Keep this query for the CPU fallback API.
    return true;
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
    if (palette_dirty_) {
        for (uint32_t i = 0; i < 0x1000; i++) palette_w(i, mem.palram, mem.colorxlat);
        palette_dirty_ = false;
    }
#ifdef M2_VITA_RENDER_OPT
    update_tile_cache();
#else
    for (int l = 0; l < 4; l++) build_layer(l);
#endif
    if (system24_source_dirty_) {
        ++system24_texture_generation_;
        system24_source_dirty_ = false;
    }
    profile_.tile_cache = ticks() - before;
    if (external_3d_ && system24_gpu_compatible()) {
        // GXM composes the cached System-24 tile textures around the 3D
        // layer. Do not spend ~35 ms rebuilding CPU bitmaps for scrolling.
        rendered_now_ = false;
        return;
    }
    // Non-zero pixels of a `width`-wide source onto the screen at column `at`.
    const size_t out_w = size_t(width());
    auto copy_trans = [&](const uint32_t *source, size_t stride, int width = W, int at = 0) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < width; ++x)
                if (const uint32_t pixel = source[size_t(y) * stride + size_t(x)])
                    screen_[size_t(y) * out_w + size_t(at + x)] = pixel;
    };
#ifdef M2_VITA_RENDER_OPT
    before = ticks();
    if (background_dirty_) {
        // All tile writes are replacements, not blends. Drawing the back
        // layers over pen 0 is identical to zero + transparent copy over pen 0.
        std::fill(background_.begin(), background_.end(), pens_[0]);
        for (int layer = 3; layer >= 2; --layer) draw(background_, layer << 1, DRAW_OPAQUE);
        for (int layer = 1; layer >= 0; --layer) draw(background_, layer << 1, 0);
        background_dirty_ = false;
        ++background_generation_;
        profile_.layers_rebuilt = true;
    }
    if (foreground_dirty_) {
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
        foreground_dirty_ = false;
        ++foreground_generation_;
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
    copy_trans(sys24_.data(), W, W, margin_);
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
    // Widescreen, HUD at the edges: the front tilemaps are drawn first, to
    // decide which HUD groups move; 3D windows inside a moved group go with it.
    const bool hud_edges = margin_ && hud_edges_;
    if (hud_edges) {
        before = ticks();
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
        profile_.tile_draw += ticks() - before;
        // Only while the race HUD is on screen (its condition panel's box).
        const bool race_hud = raster_.find_race_hud(polys, crtc_x_ + margin_, crtc_y_);
        if (race_hud != hud_on_) { hud_on_ = race_hud; set_raster_hud_moves(); render_done_ = false; }
    }
    if (!render_done_ && !polys.empty()) {
        before = ticks();
        raster_.render(polys, windows, mem, crtc_x_ + margin_, crtc_y_, render_x_ + margin_, render_y_, 0,
                       width() - 1, 0, H - 1);
        profile_.raster = ticks() - before;
        if (margin_) { // widescreen: how much of the original screen the 3D layer covers
            size_t covered = 0;
            for (int y = 0; y < H; ++y) {
                const uint32_t *row = raster_.pixels() + size_t(y) * size_t(raster_.stride()) + size_t(margin_);
                for (int x = 0; x < W; ++x) covered += row[x] != 0;
            }
            coverage_ = int(covered * 100 / (size_t(W) * H));
        }
        render_done_ = true;
        rendered_now_ = true;
    }
    before = ticks();
    if (margin_) {
        if (!render_done_) coverage_ = 0; // no 3D this frame: a 2D screen
        fill_margins();
    }
    if (render_done_) copy_trans(raster_.pixels(), size_t(raster_.stride()), width());
    profile_.composite += ticks() - before;
#ifndef M2_VITA_RENDER_OPT
    if (!hud_edges) {
        before = ticks();
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
        profile_.tile_draw += ticks() - before;
    }
#endif
    before = ticks();
    if (hud_edges && hud_on_) {
        copy_front_hud_to_edges();
    } else {
        copy_trans(sys24_.data(), W, W, margin_);
    }
    profile_.composite += ticks() - before;
}

// Widescreen, HUD at the edges: the race HUD's side groups (lap and lap
// times at the top left; position, condition panel and course map on the
// right) move out by the margin; the centre (speed, speedometer) stays.
// The front tilemaps also carry banners that scroll through those areas
// ("ROLLING START"), so what moves is decided per item, not per area: the
// front layers' pixels are grouped into blobs (pixels within kHudJoin of
// each other join, so a banner's letters and backing are one blob), and a
// blob moves only if it lies wholly inside a group. A banner crossing a
// group's edge stays put and is never torn, and nothing flips frame to
// frame as its letters pass. Rectangles in 496-wide coordinates.
namespace {
struct HudGroup { int x0, x1, y0, y1, side; };
constexpr HudGroup kHudGroups[2] = {{0, 125, 0, 130, -1},   // lap, lap times
                                    {352, 496, 0, 300, 1}}; // position ("40TH" reaches x 367), condition, course map
constexpr int kHudJoin = 4;
} // namespace

void Video::set_raster_hud_moves() {
    // The condition panel's overlay quads go with the right-hand group.
    raster_.set_hud_shift(hud_on_ ? kHudGroups[1].side * margin_ : 0);
}

void Video::copy_front_hud_to_edges() {
    const size_t n = size_t(W) * H;
    // Pixels present, widened by kHudJoin in x then y (a square neighbourhood).
    hud_mask_.assign(n, 0);
    hud_tmp_.assign(n, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (sys24_[size_t(y) * W + size_t(x)])
                for (int d = std::max(0, x - kHudJoin); d <= std::min(W - 1, x + kHudJoin); ++d)
                    hud_tmp_[size_t(y) * W + size_t(d)] = 1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (hud_tmp_[size_t(y) * W + size_t(x)])
                for (int d = std::max(0, y - kHudJoin); d <= std::min(H - 1, y + kHudJoin); ++d)
                    hud_mask_[size_t(d) * W + size_t(x)] = 1;
    // Label the widened blobs; bound each by its real pixels.
    hud_label_.assign(n, -1);
    hud_box_.clear();
    std::vector<uint32_t> &stack = hud_stack_;
    for (size_t start = 0; start < n; ++start) {
        if (!hud_mask_[start] || hud_label_[start] >= 0) continue;
        const int label = int(hud_box_.size());
        hud_box_.push_back({W, 0, H, 0});
        stack.assign(1, uint32_t(start));
        hud_label_[start] = label;
        while (!stack.empty()) {
            const uint32_t i = stack.back();
            stack.pop_back();
            const int x = int(i % W), y = int(i / W);
            if (sys24_[i]) {
                auto &b = hud_box_[size_t(label)];
                b[0] = std::min(b[0], x), b[1] = std::max(b[1], x + 1);
                b[2] = std::min(b[2], y), b[3] = std::max(b[3], y + 1);
            }
            const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
            for (int k = 0; k < 4; ++k) {
                if (nx[k] < 0 || nx[k] >= W || ny[k] < 0 || ny[k] >= H) continue;
                const uint32_t j = uint32_t(ny[k]) * W + uint32_t(nx[k]);
                if (hud_mask_[j] && hud_label_[j] < 0) hud_label_[j] = label, stack.push_back(j);
            }
        }
    }
    // Each blob's move: its group's, if it lies wholly inside one.
    hud_move_.assign(hud_box_.size(), 0);
    for (size_t l = 0; l < hud_box_.size(); ++l)
        for (const HudGroup &G : kHudGroups) {
            const auto &b = hud_box_[l];
            if (b[0] >= G.x0 && b[1] <= G.x1 && b[2] >= G.y0 && b[3] <= G.y1) hud_move_[l] = G.side * margin_;
        }
    const size_t out_w = size_t(width());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = size_t(y) * W + size_t(x);
            if (const uint32_t pixel = sys24_[i])
                screen_[size_t(y) * out_w + size_t(margin_ + x + hud_move_[size_t(hud_label_[i])])] = pixel;
        }
}

// Widescreen side margins, under the 3D layer. The back tilemaps scroll and
// wrap every 512 pixels. Behind a 3D scene they are the sky (clouds,
// mountains), drawn to wrap: the margins continue it, the back layers drawn
// once more 512 wide and margin column x taken from (x mod 512). On a 2D screen
// (car and circuit select, titles) the picture covers only the 496 visible
// columns: its 16 hidden ones hold leftovers, and wrapping showed them as
// stripes and put each side's colour on the other, so each row carries its
// own edge colours outwards instead. Which: whether the 3D layer covers half
// the original screen (measured: races 69-100%, select screens about 23%).
// A test of whether the picture joins up across its hidden columns failed on
// the race sky (median 79% of rows), stretching the clouds.
void Video::fill_margins() {
    const bool scene = coverage_ >= 50;
    const int out = width();
    if (scene) {
        backwide_.assign(size_t(out) * H, 0u);
        for (int layer = 3; layer >= 2; --layer) draw_ext(backwide_, layer << 1, DRAW_OPAQUE, -margin_, out);
        for (int layer = 1; layer >= 0; --layer) draw_ext(backwide_, layer << 1, 0, -margin_, out);
        if (std::getenv("M2_CHECK_DRAW_EXT")) { // the visible columns must match draw() exactly
            size_t bad = 0;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const uint32_t a = backwide_[size_t(y) * size_t(out) + size_t(margin_ + x)];
                    const uint32_t b = screen_[size_t(y) * size_t(out) + size_t(margin_ + x)];
                    bad += (a ? a : pens_[0]) != b;
                }
            if (bad) std::fprintf(stderr, "draw_ext: %zu visible pixels differ from draw()\n", bad);
        }
    }
    for (int y = 0; y < H; ++y) {
        uint32_t *row = &screen_[size_t(y) * size_t(out)];
        const uint32_t *ext = scene ? &backwide_[size_t(y) * size_t(out)] : nullptr;
        const uint32_t left = row[margin_], right = row[margin_ + W - 1];
        for (int x = 0; x < margin_; ++x) {
            const uint32_t p = ext ? ext[x] : left;
            row[x] = p ? p : pens_[0];
        }
        for (int x = margin_ + W; x < out; ++x) {
            const uint32_t p = ext ? ext[x] : right;
            row[x] = p ? p : pens_[0];
        }
    }
}

void Video::set_wide_margin(int margin) {
#ifdef M2_VITA_RENDER_OPT
    margin = 0; // the Vita compositor draws the 496-wide layers itself
#endif
    if (external_3d_) margin = 0;
    margin = std::max(margin, 0);
    if (margin == margin_) return;
    margin_ = margin;
    set_raster_hud_moves();
    screen_.assign(size_t(width()) * H, 0u);
    raster_.set_wide_margin(margin_);
    render_done_ = false; // redraw the 3D layer at the new width
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
