// Force feedback: the drive board's state (rt::DriveBoard, from the game's
// commands) on the host's wheel. A wheel with SDL haptics gets a centring
// spring, friction, a vibration and a constant force (the "turn the wheel"
// commands), on its steering axis; a gamepad without them gets rumble (the
// vibration and the turning force's size). The device is the one steering is
// bound to: the wheel/joystick binding first, else the gamepad.
#pragma once

#include "app/controls.h"
#include "runtime/drive_board.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

namespace app {

class ForceFeedback {
public:
    ~ForceFeedback() { close(); }
    // Each frame: the drive commands the game sent, the devices, the
    // settings (strength 0..1, 0 = off; invert the turning force).
    void update(const std::vector<uint8_t> &commands, const Devices &devices, const Controls &controls, float strength,
                bool invert);
    void stop();  // the game paused or reset: let the wheel go
    void close(); // release the device
    const rt::DriveBoard &state() const { return drive_; }
    const char *device_kind() const; // "wheel (force feedback)", "gamepad (rumble)" or "none", for the launcher

private:
    rt::DriveBoard drive_;
    SDL_Joystick *joy_ = nullptr; // the device the effects are on
    SDL_Haptic *haptic_ = nullptr;
    int spring_ = -1, friction_ = -1, sine_ = -1, constant_ = -1;
    int sent_[4] = {-1, -1, -1, -1}; // the levels last sent to each effect: only changes go to the device
    bool running_ = false, stopped_ = false;
    void open(SDL_Joystick *joy);
    void apply(float strength, bool invert);
};

} // namespace app
