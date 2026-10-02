// m2run: the recompiled game running on its own on the native board: no
// trace, no MAME, no emulated CPU and no instruction clock. Headless for now:
// frames go to raw dumps (scripts/rgb2png.py converts them).
//
//   m2run IMAGES_DIR FRAMES [--inputs scripts/inputs/X.txt] [--dump DIR --every N] [--wav FILE]
//         [--aspect W:H [--hud-edges] [--stretch-backdrop]] [--draw-distance N] [--frame-skip N]
//
// --aspect widens the screen (the widescreen enhancement, e.g. 16:9); dumps
// are then wider than 496 (the width is printed).
// --wav writes the sound board's output (YM3438 + both MultiPCMs, mixed at
// 48 kHz, 16-bit stereo).
//
// Frame pacing is the game's own: vblank starts when the game has finished
// its frame and waits in its idle loop (or after a cap, for frames that never
// idle), and ends when the vblank handler has returned. A windowed build
// waits for the display's vsync at that point; the frame rate is the only
// limit.

#include "runtime/game_loop.h"
#include "../common/input_script.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// Linear resampling of interleaved stereo to `rate`, added into out.
void mix_into(std::vector<float> &out, const std::vector<float> &in, double in_rate, double rate) {
    const size_t frames_in = in.size() / 2;
    if (frames_in < 2) return;
    const size_t frames_out = size_t(double(frames_in - 1) * rate / in_rate);
    if (out.size() < frames_out * 2) out.resize(frames_out * 2, 0.0f);
    for (size_t i = 0; i < frames_out; ++i) {
        const double pos = double(i) * in_rate / rate;
        const size_t k = size_t(pos);
        const float f = float(pos - double(k));
        for (int ch = 0; ch < 2; ++ch) out[i * 2 + ch] += in[k * 2 + ch] * (1 - f) + in[(k + 1) * 2 + ch] * f;
    }
}

void write_wav(const std::string &path, const std::vector<float> &mix, uint32_t rate) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.put(char(v)); f.put(char(v >> 8)); f.put(char(v >> 16)); f.put(char(v >> 24)); };
    auto u16 = [&](uint16_t v) { f.put(char(v)); f.put(char(v >> 8)); };
    const uint32_t bytes = uint32_t(mix.size() * 2);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    f.write("data", 4); u32(bytes);
    for (float v : mix) u16(uint16_t(int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))));
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: m2run IMAGES_DIR FRAMES [--inputs FILE] [--dump DIR --every N] [--wav FILE] "
                             "[--aspect W:H]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t frames = std::strtoull(argv[2], nullptr, 10);
    std::string dump_dir, inputs_path, wav_path;
    uint64_t every = 0;
    double aspect = 0;
    int frame_skip = 0;
    bool hud_edges = false, stretch_backdrop = false;
    for (int i = 3; i < argc; i++) {
        if (!std::strcmp(argv[i], "--hud-edges")) hud_edges = true;
        if (!std::strcmp(argv[i], "--stretch-backdrop")) stretch_backdrop = true;
    }
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--hud-edges") || !std::strcmp(argv[i], "--stretch-backdrop")) { i--; continue; }
        if (!std::strcmp(argv[i], "--inputs")) inputs_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--dump")) dump_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--every")) every = std::strtoull(argv[i + 1], nullptr, 10);
        else if (!std::strcmp(argv[i], "--wav")) wav_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--draw-distance")) rt::GameLoop::set_draw_distance(std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--frame-skip")) frame_skip = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--aspect")) {
            double w = 0, h = 0;
            if (std::sscanf(argv[i + 1], "%lf:%lf", &w, &h) == 2 && h > 0) aspect = w / h;
        }
    }

    try {
        rt::GameLoop game(dir);
        game.set_frame_skip(frame_skip);
        if (aspect > 0) {
            game.set_aspect(aspect);
            game.set_hud_edges(hud_edges);
            game.set_stretch_backdrop(stretch_backdrop);
            std::printf("m2run: screen %dx%d\n", game.screen_width(), rt::GameLoop::kHeight);
        }
        tools::Script script;
        if (!inputs_path.empty()) script.load(inputs_path);
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<float> fm, pcm;
        uint64_t drive_commands = 0, drive_kinds[16] = {}; // the force feedback drive board's commands, by type
        for (uint64_t f = 0; f < frames; f++) {
            game.run_frame(script.at(game.board().frame()));
            for (uint8_t c : std::exchange(game.board().io().drive_commands, {})) ++drive_commands, ++drive_kinds[c >> 4];
            if (!wav_path.empty() && game.sound()) {
                const auto a = game.sound()->take_fm(), b = game.sound()->take_pcm();
                fm.insert(fm.end(), a.begin(), a.end());
                pcm.insert(pcm.end(), b.begin(), b.end());
            }
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
                    " TGP instructions, %d interrupts, %" PRIu64 " bytes to the sound board; %.2f s (%.0f frames/s)\n",
                    game.frames(), game.instructions(), game.board().tgp().tgp_instructions(), game.interrupts(),
                    game.board().sound_bytes_total(), s, double(game.frames()) / s);
        std::printf("  last screen hash %016" PRIx64 "\n", game.board().video().screen_hash());
        std::printf("  drive board: %" PRIu64 " commands (", drive_commands);
        for (int k = 0, first = 1; k < 16; k++)
            if (drive_kinds[k]) std::printf("%s%x-: %" PRIu64, first ? "" : ", ", k, drive_kinds[k]), first = 0;
        std::printf(")\n");
        if (const snd::SoundBoard *sb = game.sound())
            std::printf("  sound board: %" PRIu64 " 68000 instructions (all native), %zu command bytes received\n", sb->instructions(),
                        sb->bytes_received());
        if (!wav_path.empty() && game.sound()) {
            std::vector<float> mix;
            mix_into(mix, fm, game.sound()->fm_rate(), 48000);
            mix_into(mix, pcm, game.sound()->pcm_rate(), 48000);
            write_wav(wav_path, mix, 48000);
            std::printf("  wrote %s (%.1f s)\n", wav_path.c_str(), double(mix.size() / 2) / 48000.0);
        }
        return 0;
    } catch (const std::exception &e) {
        std::printf("m2run: stopped: %s\n", e.what());
        return 1;
    }
}
