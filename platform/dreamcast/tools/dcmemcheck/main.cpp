// dcmemcheck: the Dreamcast's memory layout (M2_DC_MEMORY) on the desktop.
// The runtime built with M2_DC_MEMORY, ROM through a RomSource serving pages
// from the importer's images, texture and frame buffer RAM supplied: the
// same code paths as the Dreamcast frontend, at desktop speed, so a
// difference from the normal runtime (tracecheck) shows here first. Prints
// tracecheck's per-frame line plus the ROM pages read that frame, by region.
//
//   dcmemcheck IMAGES_DIR FIRST LAST [--inputs FILE] [--nvram DIR] [--external-3d]
//              [--fill-upper] [--no-frame-buffer]

#include "runtime/game_loop.h"
#include "runtime/rom_source.h"
#include "../../../../tools/common/input_script.h"
#include "../../../../tools/common/nvram.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#ifndef M2_DC_MEMORY
#error "dcmemcheck needs the runtime built with M2_DC_MEMORY"
#endif

namespace {

std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

class FileRom : public rt::RomSource {
public:
    explicit FileRom(const std::string &dir) {
        static const char *names[] = {"program", "main_data", "polygons", "textures", "copro_data"};
        for (int r = 0; r < 5; r++) images_[r] = load(dir + "/" + names[r] + ".bin");
    }
    uint32_t size(rt::RomRegion region) const override { return uint32_t(images_[int(region)].size()); }
    const uint8_t *page(rt::RomRegion region, uint32_t index) override {
        const auto &image = images_[int(region)];
        if ((size_t(index) << kPageBits) >= image.size()) throw std::runtime_error("page past the end of a ROM region");
        read[int(region)].insert(index);
        last[calls % 64] = {int(region), index};
        if (++calls > kLimit) {
            std::printf("dcmemcheck: over %llu page reads in one frame; the last 64 (region page):\n",
                        (unsigned long long)kLimit);
            for (int k = 0; k < 64; k++) {
                const auto &e = last[(calls + k) % 64];
                std::printf(" %d:%u", e.first, e.second);
            }
            std::printf("\n");
            throw std::runtime_error("runaway frame");
        }
        return image.data() + (size_t(index) << kPageBits);
    }
    std::set<uint32_t> read[5];
    uint64_t calls = 0; // this frame
    static constexpr uint64_t kLimit = 2000000;
    std::pair<int, uint32_t> last[64];

private:
    std::vector<uint8_t> images_[5];
};

} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: dcmemcheck IMAGES_DIR FIRST LAST [--inputs FILE] [--nvram DIR] [--external-3d]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const int first = std::atoi(argv[2]), last = std::atoi(argv[3]);
    std::string inputs, nvram;
    bool external = false, fill_upper = false, no_frame_buffer = false;
    for (int i = 4; i < argc; i++) {
        if (!std::strcmp(argv[i], "--external-3d")) external = true;
        else if (!std::strcmp(argv[i], "--fill-upper")) fill_upper = true;
        else if (!std::strcmp(argv[i], "--no-frame-buffer")) no_frame_buffer = true;
        else if (i + 1 < argc && !std::strcmp(argv[i], "--inputs")) inputs = argv[++i];
        else if (i + 1 < argc && !std::strcmp(argv[i], "--nvram")) nvram = argv[++i];
    }
    try {
        FileRom rom(dir);
        std::vector<uint8_t> texture_ram(0x400000), frame_buffer_ram(0x100000);
        rt::M2Board::Images img;
        img.copro_tables = load(dir + "/copro_tables.bin");
        img.rom = &rom;
        img.texture_ram = texture_ram.data();
        img.frame_buffer_ram = no_frame_buffer ? nullptr : frame_buffer_ram.data();
        // --fill-upper: a pattern in the half of each texture sheet the game's
        // writes do not reach (where the Dreamcast keeps its PVR texture
        // cache): a trace that still matches the desktop shows nothing reads it.
        if (fill_upper)
            for (size_t sheet : {size_t(0), size_t(0x200000)})
                std::fill(texture_ram.begin() + long(sheet + 0x100000), texture_ram.begin() + long(sheet + 0x200000),
                          uint8_t(0xa5));
        rt::GameLoop game(std::move(img), false);
        if (external) game.board().video().set_external_3d(true);
        if (!nvram.empty()) tools::load_nvram(game, nvram);
        tools::Script script;
        if (!inputs.empty()) script.load(inputs);
        for (int frame = 1; frame <= last; frame++) {
            for (auto &r : rom.read) r.clear();
            rom.calls = 0;
            game.run_frame(script.at(game.board().frame()));
            (void)game.board().take_sound_bytes();
            if (frame < first) continue;
            uint64_t h = 0xcbf29ce484222325ULL;
            const uint32_t *buffer = game.board().tgp().buffer_data();
            for (int i = 0; i < 0x8000; i++) h = (h ^ buffer[i]) * 0x100000001b3ULL;
            // External 3D: the polygons handed to the host renderer, and the
            // PVR vertex data the Dreamcast's renderer makes of them (a 32-byte
            // header and a 32-byte vertex each).
            size_t polys = 0, pvr_bytes = 0, kept = 0;
            if (external) {
                kept = game.board().video().gpu_polys().size();
                for (const rt::GeoPoly &p : game.board().video().gpu_polys())
                    if (p.num_vertices >= 3 && p.num_vertices <= 8) ++polys, pvr_bytes += 32 + 32u * p.num_vertices;
            }
            std::printf("TRACE %d i960 %" PRIu64 " tgp %" PRIu64 " buffer %016" PRIx64
                        "  pages: program %zu main %zu polygons %zu textures %zu copro %zu  3d: %zu polys, %zu KB, kept %zu\n",
                        frame, game.instructions(), game.board().tgp().tgp_instructions(), h, rom.read[0].size(),
                        rom.read[1].size(), rom.read[2].size(), rom.read[3].size(), rom.read[4].size(), polys,
                        pvr_bytes / 1024, kept);
        }
        // How much of the RAM the frontend supplies the game ever wrote: 4 KB
        // pages that are not all zero (it starts zeroed).
        auto used_pages = [](const std::vector<uint8_t> &ram, size_t from, size_t bytes) {
            size_t n = 0;
            for (size_t page = from; page < from + bytes; page += 4096)
                for (size_t i = page; i < page + 4096; i++)
                    if (ram[i]) { ++n; break; }
            return n;
        };
        std::printf("RAM written: texture RAM tex0 %zu of 512 pages, tex1 %zu of 512; frame buffer RAM %zu of 256\n",
                    used_pages(texture_ram, 0, 0x200000), used_pages(texture_ram, 0x200000, 0x200000),
                    used_pages(frame_buffer_ram, 0, 0x100000));
        return 0;
    } catch (const std::exception &e) {
        std::printf("dcmemcheck: stopped: %s\n", e.what());
        return 1;
    }
}
