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

#include "renderer.h"

#include <kos.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

extern "C" void *sbrk(ptrdiff_t increment); // newlib's; not declared under strict -std=c++20

// Large C++ allocations (256 KB and up) with the caller, for addr2line: main
// RAM is nearly all spoken for, and the one that does not fit is the one to
// find.
void *operator new(size_t size) {
    if (size >= 256 * 1024)
        std::printf("ALLOC %u bytes from %08lx\n", unsigned(size), (unsigned long)__builtin_return_address(0));
    if (void *p = std::malloc(size ? size : 1)) return p;
    std::printf("ALLOC FAILED %u bytes from %08lx\n", unsigned(size), (unsigned long)__builtin_return_address(0));
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char *kRomDir = "/cd/rom"; // the importer's images, on the disc built locally
constexpr size_t kGameStack = 512 * 1024;
constexpr size_t kCachePages = 320; // 1.25 MB of ROM pages
constexpr size_t kVertexBuffer = 448 * 1024; // the busiest frame: 2,182 polygons, ~340 KB with a header each
constexpr int kFrames = 3600;        // a minute of attract mode, then GAME DONE

// The ROM regions from the disc, a page at a time, through an LRU cache whose
// pages are in `storage`. A miss reads one page, or kRun pages (32 KB) when
// it carries on from the last read in that region (a sequential reader): the
// game's reads are mostly scattered 4 KB pages, and reading ahead on every
// miss filled the cache with pages nobody asked for (the attract mode's 3D
// thrashed a 1.5 MB cache that its ~600 KB per 16 frames fits in).
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
        if (index == next_[r])
            while (n < kRun && index + n < index_[r].size() && index_[r][index + n] < 0) ++n;
        next_[r] = index + n;
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
    uint32_t next_[5] = {}; // per region: the page after the last read
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

// What the game thread is doing, for the watchdog on the main thread.
volatile int g_frame = 0;
volatile bool g_done = false;
DiscRom *volatile g_rom = nullptr;
dc::Renderer *volatile g_renderer = nullptr;
volatile int g_running = 0; // 1 while GameLoop::run_frame runs

void *run_game(void *) {
    try {
        std::printf("GAME start, heap %.2f MB\n", heap_mb());
        // The PVR with only the lists the renderer uses: opaque (the
        // background layer) and translucent with autosort off (the polygons
        // and the front layer, in the game's order); bins of 32 and one spare
        // set (many polygons can fall in one 32x32 tile); two kVertexBuffer
        // vertex buffers (with vbuf_doublebuf_disabled set, Flycast stopped
        // with "SH4 exception when blocked").
        pvr_init_params_t params = {{PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_0},
                                    kVertexBuffer, 0, 0, 1, 1, 0};
        pvr_init(&params);
        std::printf("GAME PVR ready, video RAM free %.2f MB\n", pvr_mem_available() / 1048576.0);
        // Texture RAM (tex0, tex1, 4 MB) in video RAM: main RAM is too small
        // for it as well. The game's writes reach only the first 1 MB of each
        // sheet; the other halves are the renderer's texture cache (nothing
        // reads them: dcmemcheck --fill-upper matches the desktop for a whole
        // race). No frame buffer RAM: the game never writes it.
        auto *texture_ram = static_cast<uint8_t *>(pvr_mem_malloc(0x400000));
        if (!texture_ram) throw std::runtime_error("no video RAM for texture RAM");
        std::memset(texture_ram, 0, 0x400000);
        dc::Renderer renderer(kVertexBuffer, {{texture_ram + 0x100000, 0x100000}, {texture_ram + 0x300000, 0x100000}});
        g_renderer = &renderer;
        std::printf("GAME video RAM free: %.2f MB\n", pvr_mem_available() / 1048576.0);

        // The ROM page cache in main RAM: with the PVR drawing the 3D, the
        // CPU rasterizer's 1.25 MB is not allocated.
        auto *cache = static_cast<uint8_t *>(std::malloc(kCachePages * rt::RomSource::kPageSize));
        if (!cache) throw std::runtime_error("no main RAM for the ROM cache");
        auto rom = std::make_unique<DiscRom>(cache, kCachePages);
        g_rom = rom.get();
        std::printf("GAME ROM cache: %u pages (%.2f MB) in main RAM\n", unsigned(kCachePages),
                    kCachePages * rt::RomSource::kPageSize / 1048576.0);
        rt::M2Board::Images img;
        img.copro_tables = load_file(std::string(kRomDir) + "/copro_tables.bin");
        img.rom = rom.get();
        img.texture_ram = texture_ram;
        std::printf("GAME ROM opened, heap %.2f MB\n", heap_mb());

        rt::GameLoop game(std::move(img), false);
        game.board().video().set_external_3d(true); // the PVR draws the 3D
        // The single-cabinet settings (test mode), as tools/common/nvram.h.
        const auto eeprom = load_file(std::string(kRomDir) + "/ioboard_eeprom.bin");
        const auto backup = load_file(std::string(kRomDir) + "/backup_ram.bin");
        if (eeprom.size() == game.board().io().eeprom.size())
            std::copy(eeprom.begin(), eeprom.end(), game.board().io().eeprom.begin());
        if (backup.size() == game.board().backup_ram().size())
            std::copy(backup.begin(), backup.end(), game.board().backup_ram().begin());
        std::printf("GAME constructed, heap %.2f MB, %.2f MB free\n", heap_mb(), free_mb());

        const uint64_t t0 = timer_ms_gettime64();
        for (int frame = 1; frame <= kFrames; frame++) {
            g_running = 1;
            game.run_frame(rt::Inputs{});
            g_running = 0;
            (void)game.board().take_sound_bytes();
            g_frame = frame;
            if (frame % 60 == 0) { // the lockstep check: the same line as tools/tracecheck on the desktop
                uint64_t h = 0xcbf29ce484222325ULL;
                const uint32_t *buffer = game.board().tgp().buffer_data();
                for (int i = 0; i < 0x8000; i++) h = (h ^ buffer[i]) * 0x100000001b3ULL;
                std::printf("TRACE %d i960 %llu tgp %llu buffer %016llx\n", frame, (unsigned long long)game.instructions(),
                            (unsigned long long)game.board().tgp().tgp_instructions(), (unsigned long long)h);
            }
            if (frame % 4 == 0) renderer.draw(game.board().video());
            if (frame % 60 == 0)
                std::printf("GAME frame %d: %u polygons drawn (%u textured, flat for want of a palette %u / a build %u, %u skipped, %u KB; %u textures, %u flushes), i960 %" PRIu64 " (%.1f s, ROM misses %" PRIu64
                            ", pages read %" PRIu64 ", %.2f MB free)\n",
                            frame, renderer.drawn, renderer.textured, renderer.no_bank, renderer.no_build, renderer.skipped, unsigned(renderer.vertex_bytes / 1024),
                            unsigned(renderer.sources()), renderer.flushes,
                            game.instructions(), (timer_ms_gettime64() - t0) / 1000.0,
                            rom->misses, rom->pages_read, free_mb());
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
    std::setvbuf(stdout, nullptr, _IONBF, 0); // every line out at once: a crash must not swallow the last ones
    std::printf("GAME main\n");
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
        dc::Renderer *renderer = g_renderer;
        std::printf("GAME alive: frame %d, heap %.2f MB, %.2f MB free, ROM misses %llu, %s%s\n", g_frame, heap_mb(),
                    free_mb(), rom ? (unsigned long long)rom->misses : 0ull,
                    g_running ? "running the frame" : renderer && renderer->stage ? "drawing" : "between frames",
                    rom && rom->in_read ? ", waiting for a disc read" : "");
        if (renderer && renderer->stage) std::printf("GAME alive: draw stage %d\n", renderer->stage);
        // Where the game thread is (its saved registers: it is pre-empted),
        // for addr2line on game.elf.
        std::printf("GAME alive: game thread pc %08lx pr %08lx\n", (unsigned long)game->context.pc,
                    (unsigned long)game->context.pr);
    }
    thd_join(game, nullptr);
    for (;;) thd_sleep(1000);
}
