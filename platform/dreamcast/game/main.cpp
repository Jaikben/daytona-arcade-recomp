// The Dreamcast frontend (in progress). The game runs with sound off and no
// inputs, its screen (the CPU renderer's) shown every 4th frame. ROM is read
// from the disc image through a page cache (rt::RomSource, the runtime's
// M2_DC_MEMORY); texture RAM, frame buffer RAM, the cache and the screen
// texture are in video RAM. Every 60 frames the screen hash goes to the
// serial console, to compare with the desktop's m2run at the same frame.
// No input, sound or speed work yet.

// The runtime before kos.h: KOS defines a BIT(n) macro, the runtime a
// BIT(x, n) function (cpu.h).
#include "runtime/game_loop.h"
#include "runtime/rom_source.h"

#include <kos.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

extern "C" void *sbrk(ptrdiff_t increment); // newlib's; not declared under strict -std=c++20
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char *kRomDir = "/cd/rom"; // the importer's images, on the disc built locally
constexpr size_t kGameStack = 512 * 1024;

// The ROM regions from the disc, a page at a time, through an LRU cache.
// A miss reads kRun pages (32 KB): the drive's cost is the seek. The cache's
// pages are in `storage` (video RAM here: main RAM is the runtime's).
class DiscRom : public rt::RomSource {
public:
    DiscRom(uint8_t *storage, size_t cache_pages) : slots_(cache_pages), data_(storage) {
        static const char *names[] = {"program", "main_data", "polygons", "textures", "copro_data"};
        for (int r = 0; r < 5; r++) {
            const std::string path = std::string(kRomDir) + "/" + names[r] + ".bin";
            files_[r] = std::fopen(path.c_str(), "rb");
            if (!files_[r]) throw std::runtime_error("cannot open " + path);
            std::fseek(files_[r], 0, SEEK_END);
            sizes_[r] = uint32_t(std::ftell(files_[r]));
            index_[r].assign(sizes_[r] >> kPageBits, -1);
        }
    }
    uint32_t size(rt::RomRegion region) const override { return sizes_[int(region)]; }
    const uint8_t *page(rt::RomRegion region, uint32_t index) override {
        const int r = int(region);
        int32_t &slot = index_[r][index];
        if (slot < 0) load(r, index);
        Slot &s = slots_[size_t(slot)];
        s.used = ++clock_;
        return &data_[size_t(slot) * kPageSize];
    }
    uint64_t misses = 0, pages_read = 0;
    volatile bool in_read = false; // a disc read in progress (the watchdog's)
    bool verbose = false;          // print each read (found the thrashing at frame 188)

private:
    static constexpr uint32_t kRun = 8;
    struct Slot { int region = -1; uint32_t index = 0; uint64_t used = 0; };

    // Read index and the pages after it that are not cached, up to kRun.
    void load(int r, uint32_t index) {
        ++misses;
        uint32_t n = 1;
        while (n < kRun && index + n < index_[r].size() && index_[r][index + n] < 0) ++n;
        std::vector<uint8_t> run(size_t(n) * kPageSize);
        if (verbose) std::printf("GAME read region %d page %u x%u ...", r, unsigned(index), unsigned(n));
        in_read = true;
        std::fseek(files_[r], long(index) << kPageBits, SEEK_SET);
        const size_t got = std::fread(run.data(), kPageSize, n, files_[r]);
        in_read = false;
        if (verbose) std::printf(" done\n");
        if (got != n) throw std::runtime_error("ROM read failed");
        for (uint32_t k = 0; k < n; k++) {
            const size_t slot = victim();
            Slot &s = slots_[slot];
            if (s.region >= 0) index_[s.region][s.index] = -1;
            s = {r, index + k, ++clock_};
            index_[r][index + k] = int32_t(slot);
            std::memcpy(&data_[slot * kPageSize], &run[size_t(k) * kPageSize], kPageSize);
            ++pages_read;
        }
    }
    size_t victim() const {
        size_t best = 0;
        for (size_t i = 1; i < slots_.size(); i++)
            if (slots_[i].used < slots_[best].used) best = i;
        return best;
    }

    FILE *files_[5] = {};
    uint32_t sizes_[5] = {};
    std::vector<int32_t> index_[5]; // page -> cache slot, or -1
    std::vector<Slot> slots_;
    uint8_t *data_;
    uint64_t clock_ = 0;
};

std::vector<uint8_t> load_file(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::fseek(f, 0, SEEK_END);
    std::vector<uint8_t> d(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(d.data(), 1, d.size(), f) != d.size()) throw std::runtime_error("cannot read " + path);
    std::fclose(f);
    return d;
}

double heap_mb() { return mallinfo().uordblks / 1048576.0; }

// Main RAM still free: the heap's free blocks and what sbrk has not given out
// (the heap grows up to the end of the 16 MB).
double free_mb() {
    const uintptr_t brk = reinterpret_cast<uintptr_t>(sbrk(0));
    return (mallinfo().fordblks + (0x8d000000u - brk)) / 1048576.0;
}

// The game's screen on the TV: Video::screen() (ARGB8888, 496 wide unless an
// enhancement widens it) converted to RGB565, uploaded to a 512x512 PVR
// texture and drawn at 620x480, centred. A first, slow path (videotest
// measured 15 ms per frame converting): only every few frames for now.
class Screen {
public:
    Screen() : texture_(pvr_mem_malloc(kTexW * kTexH * 2)) {
        if (!texture_) throw std::runtime_error("no video RAM for the screen texture");
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, kTexW, kTexH, texture_,
                         PVR_FILTER_BILINEAR);
        pvr_poly_compile(&header_, &cxt);
    }
    void show(const std::vector<uint32_t> &screen) {
        const int w = std::min<int>(kTexW, int(screen.size() / kH));
        // A row at a time (main RAM is short): converted, then uploaded.
        auto *texture = static_cast<uint8_t *>(texture_);
        for (int y = 0; y < kH; y++) {
            const uint32_t *src = &screen[size_t(y) * size_t(screen.size() / kH)];
            for (int x = 0; x < w; x++) {
                const uint32_t c = src[x];
                row_[x] = uint16_t(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
            }
            pvr_txr_load(row_, texture + size_t(y) * kTexW * 2, kTexW * 2);
        }
        const float h = 480.0f, sw = h * float(w) / kH, x0 = (640.0f - sw) / 2, x1 = x0 + sw;
        const float u1 = float(w) / kTexW, v1 = float(kH) / kTexH;
        pvr_wait_ready();
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        pvr_prim(&header_, sizeof header_);
        const float corners[4][4] = {{x0, 0, 0, 0}, {x1, 0, u1, 0}, {x0, h, 0, v1}, {x1, h, u1, v1}};
        for (int i = 0; i < 4; i++) {
            pvr_vertex_t v{};
            v.flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v.x = corners[i][0];
            v.y = corners[i][1];
            v.z = 1.0f;
            v.u = corners[i][2];
            v.v = corners[i][3];
            v.argb = 0xffffffffu;
            pvr_prim(&v, sizeof v);
        }
        pvr_list_finish();
        pvr_scene_finish();
    }

private:
    static constexpr int kTexW = 512, kTexH = 512, kH = 384;
    pvr_ptr_t texture_;
    alignas(32) uint16_t row_[kTexW] = {};
    pvr_poly_hdr_t header_;
};

// What the game thread is doing, for the watchdog on the main thread.
volatile int g_frame = 0;
volatile bool g_done = false;
DiscRom *volatile g_rom = nullptr;

void *run_game(void *) {
    try {
        std::printf("GAME start, heap %.2f MB\n", heap_mb());
        // Small PVR buffers (only the opaque list, 64 KB of vertices): video
        // RAM is for the board's texture and frame buffer RAM, the screen
        // texture and the ROM cache.
        pvr_init_params_t params = {{PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_0},
                                    64 * 1024, 0, 0, 0, 0, 0};
        pvr_init(&params);
        // Texture RAM (tex0, tex1, 4 MB) and frame buffer RAM (1 MB) in video
        // RAM: main RAM is too small for them as well.
        auto *texture_ram = static_cast<uint8_t *>(pvr_mem_malloc(0x400000));
        auto *frame_buffer_ram = static_cast<uint8_t *>(pvr_mem_malloc(0x100000));
        if (!texture_ram || !frame_buffer_ram) throw std::runtime_error("no video RAM for texture or frame buffer RAM");
        std::memset(texture_ram, 0, 0x400000);
        std::memset(frame_buffer_ram, 0, 0x100000);
        Screen screen;

        // The ROM page cache in what video RAM is left, keeping 256 KB spare.
        const size_t cache_pages = std::min<size_t>(512, (pvr_mem_available() - 0x40000) / rt::RomSource::kPageSize);
        auto *cache = static_cast<uint8_t *>(pvr_mem_malloc(cache_pages * rt::RomSource::kPageSize));
        if (!cache) throw std::runtime_error("no video RAM for the ROM cache");
        auto rom = std::make_unique<DiscRom>(cache, cache_pages);
        g_rom = rom.get();
        std::printf("GAME ROM cache: %u pages (%.2f MB) in video RAM\n", unsigned(cache_pages),
                    cache_pages * rt::RomSource::kPageSize / 1048576.0);
        rt::M2Board::Images img;
        img.copro_tables = load_file(std::string(kRomDir) + "/copro_tables.bin");
        img.rom = rom.get();
        img.texture_ram = texture_ram;
        img.frame_buffer_ram = frame_buffer_ram;
        std::printf("GAME ROM opened, heap %.2f MB\n", heap_mb());

        rt::GameLoop game(std::move(img), false);
        // The single-cabinet settings (test mode), as tools/common/nvram.h.
        const auto eeprom = load_file(std::string(kRomDir) + "/ioboard_eeprom.bin");
        const auto backup = load_file(std::string(kRomDir) + "/backup_ram.bin");
        if (eeprom.size() == game.board().io().eeprom.size())
            std::copy(eeprom.begin(), eeprom.end(), game.board().io().eeprom.begin());
        if (backup.size() == game.board().backup_ram().size())
            std::copy(backup.begin(), backup.end(), game.board().backup_ram().begin());
        std::printf("GAME constructed, heap %.2f MB\n", heap_mb());

        const uint64_t t0 = timer_ms_gettime64();
        for (int frame = 1; frame <= 600; frame++) {
            game.run_frame(rt::Inputs{});
            (void)game.board().take_sound_bytes();
            g_frame = frame;
            if (frame % 4 == 0) screen.show(game.screen());
            if (frame % 60 == 0)
                std::printf("GAME frame %d hash %016" PRIx64 " (%u px) i960 %" PRIu64 " (%.1f s, ROM misses %" PRIu64
                            ", pages read %" PRIu64 ", heap %.2f MB)\n",
                            frame, game.board().video().screen_hash(), unsigned(game.screen().size()), game.instructions(),
                            (timer_ms_gettime64() - t0) / 1000.0, rom->misses, rom->pages_read, heap_mb());
        }
        std::printf("GAME DONE\n");
    } catch (const std::exception &e) {
        std::printf("GAME FAILED: %s\n", e.what());
    }
    g_done = true;
    return nullptr;
}

} // namespace

// The game runs on a thread with a large stack: KOS's main thread has 64 KB
// (other threads 32 KB), and the runtime and generated code expect a desktop
// stack. The main thread is a watchdog: every 5 s, the frame the game has
// reached, and whether it is waiting for the disc.
int main() {
    kthread_attr_t attr = {};
    attr.stack_size = kGameStack;
    attr.prio = PRIO_DEFAULT;
    attr.label = "game";
    kthread_t *game = thd_create_ex(&attr, run_game, nullptr);
    if (!game) {
        std::printf("GAME FAILED: cannot create the game thread\n");
        return 1;
    }
    while (!g_done) {
        thd_sleep(5000);
        DiscRom *rom = g_rom;
        std::printf("GAME alive: frame %d, heap %.2f MB, %.2f MB free%s\n", g_frame, heap_mb(), free_mb(),
                    rom && rom->in_read ? ", waiting for a disc read" : "");
    }
    thd_join(game, nullptr);
    for (;;) thd_sleep(1000);
}
