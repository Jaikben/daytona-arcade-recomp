#include "runtime/rom_import.h"

#include "runtime/zip.h"

#include <cstdio>

namespace rt {

namespace {

enum Region { Program, MainData, CoproData, Polygons, Textures, CoproTables };

struct Load {
    const char *file;
    uint32_t crc;
    Region region;
    uint32_t offset, size; // ROM_LOAD32_WORD: 16-bit words into bytes 0-1 (offset 0) or 2-3 (offset 2) of each dword
};

// MAME model2.cpp ROM_START(daytona93) and MODEL2_CPU_BOARD at dddd7368.
const Load kLoads[] = {
    {"epr-16530a.12", 0x39E962B5, Program, 0x000000, 0x020000},
    {"epr-16531a.13", 0x693126EB, Program, 0x000002, 0x020000},
    {"mpr-16528.10", 0x9CE591F6, MainData, 0x000000, 0x200000},
    {"mpr-16529.11", 0xF7095EAF, MainData, 0x000002, 0x200000},
    {"mpr-16526.8", 0x5273B8B5, MainData, 0x400000, 0x200000},
    {"mpr-16527.9", 0xFC4CB0EF, MainData, 0x400002, 0x200000},
    {"epr-16534a.6", 0x1BB0D72D, MainData, 0x800000, 0x100000},
    {"epr-16535a.7", 0x459A8BFB, MainData, 0x800002, 0x100000},
    {"mpr-16537.ic28", 0x36B7C35A, CoproData, 0x000000, 0x200000},
    {"mpr-16536.ic29", 0x6D6AFED9, CoproData, 0x000002, 0x200000},
    {"mpr-16523.ic16", 0x2F484D42, Polygons, 0x000000, 0x200000},
    {"mpr-16518.ic20", 0xDF683BF7, Polygons, 0x000002, 0x200000},
    {"mpr-16524.ic17", 0x34658BD7, Polygons, 0x400000, 0x200000},
    {"mpr-16519.ic21", 0xFACD1C81, Polygons, 0x400002, 0x200000},
    {"mpr-16525.ic18", 0xFB517521, Polygons, 0x800000, 0x200000},
    {"mpr-16520.ic22", 0xD66BD9BD, Polygons, 0x800002, 0x200000},
    {"epr-16646.ic19", 0x7BA9FD6B, Polygons, 0xC00000, 0x080000},
    {"epr-16645.ic23", 0x78FE0B8A, Polygons, 0xC00002, 0x080000},
    {"mpr-16522.25", 0x55D39A57, Textures, 0x000000, 0x200000},
    {"mpr-16521.24", 0xAF1934FB, Textures, 0x000002, 0x200000},
    {"mpr-16517.27", 0x4705D3DD, Textures, 0x800000, 0x200000},
    {"mpr-16516.26", 0xA260D45D, Textures, 0x800002, 0x200000},
    {"opr-14742a.45", 0x90C6B117, CoproTables, 0x000000, 0x020000},
    {"opr-14743a.46", 0xAE7F446B, CoproTables, 0x000002, 0x020000},
};

std::string hex8(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

} // namespace

std::vector<RomCheck> check_rom_set(const std::string &zip_path) {
    std::vector<RomCheck> out;
    Zip z(zip_path);
    for (const Load &l : kLoads) {
        RomCheck c;
        c.file = l.file;
        const auto it = z.entries().find(l.file);
        if (it == z.entries().end()) c.problem = "missing";
        else if (it->second.usize != l.size) c.problem = "wrong size";
        else if (it->second.crc != l.crc) c.problem = "wrong CRC " + hex8(it->second.crc) + " (expected " + hex8(l.crc) + ")";
        else c.ok = true;
        out.push_back(c);
    }
    return out;
}

M2Board::Images import_rom_set(const std::string &zip_path) {
    Zip z(zip_path);
    M2Board::Images img;
    img.program.assign(0x200000, 0);
    img.main_data.assign(0x2000000, 0);
    img.copro_data.assign(0x800000, 0);
    img.polygons.assign(0x1000000, 0);
    img.textures.assign(0x1000000, 0);
    img.copro_tables.assign(0x40000, 0);
    for (const Load &l : kLoads) {
        const std::vector<uint8_t> data = z.read(l.file); // CRC against the zip directory
        if (data.size() != l.size || crc32(data.data(), data.size()) != l.crc)
            throw ZipError(std::string(l.file) + ": not the daytona93 ROM this build was recompiled from");
        std::vector<uint8_t> *r = nullptr;
        switch (l.region) {
        case Program: r = &img.program; break;
        case MainData: r = &img.main_data; break;
        case CoproData: r = &img.copro_data; break;
        case Polygons: r = &img.polygons; break;
        case Textures: r = &img.textures; break;
        case CoproTables: r = &img.copro_tables; break;
        }
        for (uint32_t w = 0; w < l.size / 2; w++) {
            (*r)[l.offset + w * 4] = data[w * 2];
            (*r)[l.offset + w * 4 + 1] = data[w * 2 + 1];
        }
    }
    for (uint32_t dst = 0xA00000; dst <= 0xF00000; dst += 0x100000) // ROM_COPY main_data 0x900000 mirrors
        std::copy_n(img.main_data.begin() + 0x900000, 0x100000, img.main_data.begin() + dst);
    return img;
}

} // namespace rt
