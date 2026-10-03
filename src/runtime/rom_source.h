// M2_DC_MEMORY builds only (platform/dreamcast): the ROM regions read through
// pages the frontend supplies from a small cache, instead of whole images
// held in memory (16 MB of RAM, 44 MB of ROM). The board, geometrizer and TGP
// board ask for a 4 KB page at a time. Desktop builds do not define
// M2_DC_MEMORY and never include this.
#pragma once

#ifdef M2_DC_MEMORY
#include <cstdint>

namespace rt {

enum class RomRegion : uint8_t { Program, MainData, Polygons, Textures, CoproData };

class RomSource {
public:
    static constexpr unsigned kPageBits = 12;
    static constexpr uint32_t kPageSize = 1u << kPageBits;
    virtual ~RomSource() = default;
    // Bytes in the region: the size of the image it replaces (a power of two
    // for Polygons, Textures and CoproData).
    virtual uint32_t size(RomRegion region) const = 0;
    // Page `index` of the region (bytes index * 4096 on). The pointer stays
    // valid until at least 8 more pages have been asked for: callers use it
    // at once.
    virtual const uint8_t *page(RomRegion region, uint32_t index) = 0;

    // A little-endian word at a byte offset (aligned), through page().
    uint32_t dword(RomRegion region, uint32_t offset) {
        const uint8_t *p = page(region, offset >> kPageBits) + (offset & (kPageSize - 1) & ~3u);
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
    uint16_t word(RomRegion region, uint32_t offset) {
        const uint8_t *p = page(region, offset >> kPageBits) + (offset & (kPageSize - 1) & ~1u);
        return uint16_t(p[0] | p[1] << 8);
    }
};

} // namespace rt
#endif
