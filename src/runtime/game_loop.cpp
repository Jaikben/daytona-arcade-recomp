#include "runtime/game_loop.h"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace rt {

namespace {
std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path + " (run the setup / importer first)");
    return {std::istreambuf_iterator<char>(f), {}};
}

constexpr uint64_t kProbe = 1024;       // instructions between wait-loop checks
constexpr uint64_t kFrameCap = 110000;  // a CPU-bound frame: 25 MHz / 57.52 Hz of i960 work, as MAME measures
constexpr uint64_t kVblankCap = 40000;  // a vblank handler that never returns to the wait loop
constexpr uint64_t kMinFrame = kProbe * 2;
} // namespace

GameLoop::GameLoop(const std::string &dir) : GameLoop([&] {
    M2Board::Images img;
    img.program = load(dir + "/program.bin");
    img.main_data = load(dir + "/main_data.bin");
    img.copro_tables = load(dir + "/copro_tables.bin");
    img.copro_data = load(dir + "/copro_data.bin");
    img.polygons = load(dir + "/polygons.bin");
    img.textures = load(dir + "/textures.bin");
    img.sound_program = load(dir + "/sound_program.bin");
    img.pcm1 = load(dir + "/pcm1.bin");
    img.pcm2 = load(dir + "/pcm2.bin");
    return img;
}()) {}

GameLoop::GameLoop(M2Board::Images img) {
    if (!img.sound_program.empty()) sound_ = std::make_unique<snd::SoundBoard>(img.sound_program, img.pcm1, img.pcm2);
    board_ = std::make_unique<M2Board>(std::move(img));
    cpu_ = std::make_unique<Cpu>(board_.get());
    ls_ = std::make_unique<Lockstep>(*cpu_);
    board_->attach(*cpu_, *ls_);
    cpu_->reset();
    env_ = std::make_unique<gen::Env>(gen::Env{*cpu_, *ls_});
    ls_->add_callback(kProbe, [this] { probe(); });
}

void GameLoop::probe() {
    const bool idle = board_->in_idle_loop();
    if (in_vblank_) {
        if ((idle && ls_->count - vblank_start_ >= kProbe * 2) || ls_->count - vblank_start_ >= kVblankCap) {
            {
                auto sample = profiler_.measure(profiler_.frame.video);
                board_->vblank_end();
            }
            in_vblank_ = false;
            frame_start_ = ls_->count;
            ++frames_;
            frame_done_ = true;
            ls_->end_count = ls_->count; // gen::run returns to the caller
        }
    } else {
        const uint64_t since = ls_->count - frame_start_;
        if ((idle && since >= kMinFrame) || since >= kFrameCap) {
            board_->io().inputs = inputs_;
            {
                auto sample = profiler_.measure(profiler_.frame.geometry);
                board_->vblank_start();
            }
            in_vblank_ = true;
            vblank_start_ = ls_->count;
        }
    }
    ls_->add_callback(ls_->count + kProbe, [this] { probe(); });
}

void GameLoop::run_frame(const Inputs &inputs) {
    profiler_.reset();
    auto frame_sample = profiler_.measure(profiler_.frame.total);
    inputs_ = inputs;
    frame_done_ = false;
    ls_->end_count = UINT64_MAX;
    while (!frame_done_) {
        if (!gen::has_code(cpu_->m_IP)) {
            char b[128];
            std::snprintf(b, sizeof b, "no recompiled code at %08x: add it to the seeds", cpu_->m_IP);
            throw Fatal(b);
        }
        gen::run(*env_);
    }
    // The sound board runs alongside: this frame's command bytes go down the
    // serial line, and it advances one frame of board time.
    if (sound_) {
        auto sound_sample = profiler_.measure(profiler_.frame.sound);
        const std::vector<uint8_t> bytes = board_->take_sound_bytes();
        sound_->send(bytes.data(), bytes.size());
        sound_->advance(1.0 / kFrameHz);
    }
}

} // namespace rt
