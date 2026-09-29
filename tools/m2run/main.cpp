// m2run: the recompiled game running on its own on the native board: no
// trace, no MAME, no emulated CPU and no instruction clock. Headless for now:
// frames go to raw dumps (scripts/rgb2png.py converts them).
//
//   m2run IMAGES_DIR FRAMES [--inputs scripts/inputs/X.txt] [--dump DIR --every N]
//
// Frame pacing is the game's own: vblank starts when the game has finished
// its frame and waits in its idle loop (or after a cap, for frames that never
// idle), and ends when the vblank handler has returned. A windowed build
// waits for the display's vsync at that point; the frame rate is the only
// limit.

#include "runtime/game_loop.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

// scripts/inputs format: "frames N", "<from>-<to> name=value" or "<at> name=value".
struct Script {
    struct Line {
        uint64_t from, to;
        std::string name;
        unsigned value;
    };
    std::vector<Line> lines;
    void load(const std::string &path) {
        std::ifstream f(path);
        if (!f) throw std::runtime_error("cannot open " + path);
        std::string s;
        while (std::getline(f, s)) {
            if (s.empty() || s[0] == '#' || s.compare(0, 6, "frames") == 0) continue;
            std::istringstream in(s);
            std::string range, kv;
            in >> range >> kv;
            const auto dash = range.find('-'), eq = kv.find('=');
            if (eq == std::string::npos) continue;
            Line l;
            l.from = std::stoull(range.substr(0, dash));
            l.to = dash == std::string::npos ? l.from : std::stoull(range.substr(dash + 1));
            l.name = kv.substr(0, eq);
            l.value = unsigned(std::stoul(kv.substr(eq + 1), nullptr, 0));
            lines.push_back(l);
        }
    }
    rt::Inputs at(uint64_t frame) const {
        rt::Inputs in;
        static const struct {
            const char *name;
            int port; // 0: IN0, 1: IN1
            uint8_t bit;
        } buttons[] = {{"coin", 0, 0x01}, {"test", 0, 0x04}, {"service", 0, 0x08}, {"start", 0, 0x10}, {"vr1", 0, 0x20},
                       {"vr2", 0, 0x40},  {"vr3", 0, 0x80},  {"vr4", 1, 0x01}};
        static const uint8_t gearvalue[5] = {0, 2, 1, 6, 5}; // MAME daytona_gearbox_r
        for (const Line &l : lines) {
            if (frame < l.from || frame > l.to) continue;
            if (l.name == "steer") in.steer = uint8_t(l.value);
            else if (l.name == "accel") in.accel = uint8_t(l.value);
            else if (l.name == "brake") in.brake = uint8_t(l.value);
            else if (l.name.compare(0, 4, "gear") == 0 && l.value) {
                const int g = l.name[4] - '0';
                if (g >= 0 && g < 5) in.in1 = uint8_t((in.in1 & ~0x70) | (gearvalue[g] << 4));
            } else
                for (const auto &b : buttons)
                    if (l.name == b.name && l.value) (b.port ? in.in1 : in.in0) &= uint8_t(~b.bit); // active low
        }
        return in;
    }
};

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: m2run IMAGES_DIR FRAMES [--inputs FILE] [--dump DIR --every N]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t frames = std::strtoull(argv[2], nullptr, 10);
    std::string dump_dir, inputs_path;
    uint64_t every = 0;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--inputs")) inputs_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--dump")) dump_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--every")) every = std::strtoull(argv[i + 1], nullptr, 10);
    }

    try {
        rt::GameLoop game(dir);
        Script script;
        if (!inputs_path.empty()) script.load(inputs_path);
        const auto t0 = std::chrono::steady_clock::now();
        for (uint64_t f = 0; f < frames; f++) {
            game.run_frame(script.at(game.board().frame()));
            if (!dump_dir.empty() && every && game.board().frame() % every == 0) {
                char path[512];
                std::snprintf(path, sizeof path, "%s/run_%05" PRIu64 ".rgb", dump_dir.c_str(), game.board().frame());
                if (FILE *d = std::fopen(path, "wb")) {
                    std::fwrite(game.screen().data(), 4, game.screen().size(), d);
                    std::fclose(d);
                }
            }
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("m2run: %" PRIu64 " frames, %" PRIu64 " i960 instructions (all native), %" PRIu64
                    " TGP instructions, %d interrupts, %zu bytes to the sound board; %.2f s (%.0f frames/s)\n",
                    game.frames(), game.instructions(), game.board().tgp().tgp_instructions(), game.interrupts(),
                    game.board().sound_bytes().size(), s, double(game.frames()) / s);
        std::printf("  last screen hash %016" PRIx64 "\n", game.board().video().screen_hash());
        return 0;
    } catch (const std::exception &e) {
        std::printf("m2run: stopped: %s\n", e.what());
        return 1;
    }
}
