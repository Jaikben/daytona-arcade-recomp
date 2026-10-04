// soundusage: which samples the game plays (desktop). Runs input scripts
// with the native sound sequencer (no mixer, no sound board) fed the bytes
// the game sends the sound board, and lists every sample a note starts, as
// "rom bank sample" lines, for the Dreamcast's sound pack (build_dreamcast.py
// converts those samples to the AICA's ADPCM: sound RAM has 2 MB, the PCM
// ROMs 8).
//
//   soundusage IMAGES_DIR OUT.txt [--nvram DIR] SCRIPT [SCRIPT ...]

#include "runtime/game_loop.h"
#include "runtime/native_sound_sequencer.h"
#include "../../../../tools/common/input_script.h"
#include "../../../../tools/common/nvram.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {
std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}
using Key = std::tuple<unsigned, unsigned, unsigned>;
std::set<Key> g_used;
void sink(void *, const snd::NativeSoundSequencer::VoiceEvent &e) {
    if (e.kind == snd::NativeSoundSequencer::EventKind::NoteOn) g_used.insert({e.rom, e.bank, e.sample_index});
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: soundusage IMAGES_DIR OUT.txt [--nvram DIR] SCRIPT [SCRIPT ...]\n");
        return 2;
    }
    const std::string dir = argv[1], out = argv[2];
    std::string nvram;
    std::vector<std::string> scripts;
    for (int i = 3; i < argc; i++) {
        if (!std::strcmp(argv[i], "--nvram") && i + 1 < argc) nvram = argv[++i];
        else scripts.push_back(argv[i]);
    }
    try {
        const std::vector<uint8_t> program = load(dir + "/sound_program.bin");
        for (const std::string &path : scripts) {
            rt::GameLoop game(dir, false);
            if (!nvram.empty()) tools::load_nvram(game, nvram);
            tools::Script script;
            script.load(path);
            snd::NativeSoundSequencer sequencer(program, 48000);
            sequencer.set_sink(sink, nullptr);
            int frames = 6000; // the script's "frames N" line, if it has one
            {
                std::ifstream f(path);
                for (std::string line; std::getline(f, line);)
                    if (line.compare(0, 7, "frames ") == 0) frames = std::stoi(line.substr(7));
            }
            double owed = 0;
            const size_t before = g_used.size();
            for (int frame = 0; frame < frames; frame++) {
                game.run_frame(script.at(game.board().frame()));
                const std::vector<uint8_t> bytes = game.board().take_sound_bytes();
                if (!bytes.empty()) sequencer.send(bytes.data(), bytes.size());
                owed += 48000.0 / rt::GameLoop::kFrameHz;
                const size_t step = size_t(owed);
                owed -= double(step);
                sequencer.advance(step);
            }
            std::printf("soundusage: %s, %d frames: %zu samples (%zu new)\n", path.c_str(), frames, g_used.size(),
                        g_used.size() - before);
        }
        FILE *f = std::fopen(out.c_str(), "w");
        if (!f) throw std::runtime_error("cannot write " + out);
        for (const auto &[rom, bank, sample] : g_used) std::fprintf(f, "%u %u %u\n", rom, bank, sample);
        std::fclose(f);
        std::printf("soundusage: %zu samples to %s\n", g_used.size(), out.c_str());
        return 0;
    } catch (const std::exception &e) {
        std::printf("soundusage: stopped: %s\n", e.what());
        return 1;
    }
}
