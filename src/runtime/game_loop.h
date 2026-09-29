// The game running on the native board, one frame at a time: the frame
// pacing m2run and the windowed game share. No clock: vblank starts when the
// game waits in its wait-for-vblank loop (or, for a CPU-bound frame, after
// one frame's worth of i960 work), and the frame is done when the vblank
// handler has returned. The caller shows the frame and calls again; with a
// window that is the display's vsync, so the frame rate is the only limit.
#pragma once

#include "runtime/gen_support.h"
#include "runtime/lockstep.h"
#include "runtime/m2_board.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rt {

class GameLoop {
public:
    // images_dir: the importer's output (build/rom_cache/daytona93).
    explicit GameLoop(const std::string &images_dir);
    // Images already loaded (rt::import_rom_set: straight from the ROM zip).
    explicit GameLoop(M2Board::Images images);

    // Run until the next screen is composed. `inputs` are latched by the I/O
    // board at the start of this frame's vblank.
    void run_frame(const Inputs &inputs);

    const std::vector<uint32_t> &screen() const { return board_->video().screen(); } // 496x384, 0xAARRGGBB
    static constexpr int kWidth = Video::W, kHeight = Video::H;
    M2Board &board() { return *board_; }
    uint64_t frames() const { return frames_; }
    uint64_t instructions() const { return ls_->count; }
    int interrupts() const { return ls_->interrupts(); }

private:
    void probe();
    std::unique_ptr<M2Board> board_;
    std::unique_ptr<Cpu> cpu_;
    std::unique_ptr<Lockstep> ls_;
    std::unique_ptr<gen::Env> env_;
    Inputs inputs_;
    bool in_vblank_ = false, frame_done_ = false;
    uint64_t frame_start_ = 0, vblank_start_ = 0, frames_ = 0;
};

} // namespace rt
