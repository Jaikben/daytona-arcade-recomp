#pragma once

#include "runtime/video.h"

#include <cstddef>
#include <cstdint>

namespace vita {

// Writes only tiles changed since the last presented source generation. The
// caller must finish any previous GPU readers before writing these textures.
// Kept independent of GXM so tests exercise the exact Vita upload conversion.
inline unsigned upload_system24_layer(const rt::Video &video, int layer, uint64_t previous,
                                      void *background, size_t background_stride,
                                      void *foreground, size_t foreground_stride) {
    constexpr size_t row_bytes = 512u * sizeof(uint32_t);
    if (!background || !foreground || background_stride < row_bytes || foreground_stride < row_bytes ||
        (background_stride % alignof(uint32_t)) || (foreground_stride % alignof(uint32_t))) return 0;
    const bool full = previous == UINT64_MAX || video.system24_palette_generation() > previous;
    const bool opaque_background = layer >= 2;
    const bool split_background = opaque_background && (video.system24_word(0x5006) & 0x6000);
    const uint16_t *pixels = video.system24_pixels(layer);
    const uint8_t *flags = video.system24_flags(layer);
    auto *back = static_cast<uint8_t *>(background);
    auto *front = static_cast<uint8_t *>(foreground);
    unsigned uploaded = 0;
    for (unsigned tile = 0; tile < 4096; ++tile) {
        if (!full && video.system24_tile_generation(layer, tile) <= previous) continue;
        const unsigned tx = (tile & 63u) * 8u, ty = (tile >> 6u) * 8u;
        for (unsigned y = ty; y < ty + 8u; ++y) {
            auto *back_row = reinterpret_cast<uint32_t *>(back + size_t(y) * background_stride);
            auto *front_row = reinterpret_cast<uint32_t *>(front + size_t(y) * foreground_stride);
            for (unsigned x = tx; x < tx + 8u; ++x) {
                const size_t i = size_t(y) * 512u + x;
                const bool opaque = (flags[i] & 0x10) != 0;
                const bool category1 = (flags[i] & 1) != 0;
                const uint32_t argb = video.system24_pen(pixels[i]);
                const uint32_t rgba = (argb & 0xff00ff00u) | ((argb & 0xffu) << 16u) |
                                      ((argb >> 16u) & 0xffu);
                // Unlike normal-mode draw_rect, split-mode tilemap_draw
                // retains the category test even with DRAW_OPAQUE.
                const bool back_visible = opaque_background ? (!split_background || !category1) :
                                                              (opaque && !category1);
                back_row[x] = back_visible ? rgba : 0u;
                front_row[x] = opaque && category1 ? rgba : 0u;
            }
        }
        ++uploaded;
    }
    return uploaded;
}

} // namespace vita
