// Vita input mapping, independent of SDL and VitaSDK so it can be host-tested.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vita {
enum Button : uint32_t {
    Cross = 1u << 0, Circle = 1u << 1, Square = 1u << 2, Triangle = 1u << 3,
    Up = 1u << 4, Down = 1u << 5, Left = 1u << 6, Right = 1u << 7,
    L = 1u << 8, R = 1u << 9, Start = 1u << 10, Select = 1u << 11
};
struct Pad { uint32_t buttons = 0; uint8_t lx = 128, ry = 128; };
struct Input {
    uint8_t steer = 0x80, accel = 0x20, brake = 0x20;
    uint8_t in0 = 0xff, in1 = 0x8f, in2 = 0xff;
};
inline bool menu_chord(uint32_t buttons) {
    return (buttons & (Start | Select)) == (Start | Select);
}
inline float axis(uint8_t value, float deadzone = 0.12f) {
    deadzone = std::clamp(deadzone, 0.0f, 0.4f);
    const float x = value < 128 ? (int(value) - 128) / 128.f : (int(value) - 128) / 127.f;
    const float magnitude = std::abs(x);
    if (magnitude <= deadzone) return 0.f;
    return std::copysign((magnitude - deadzone) / (1.f - deadzone), x);
}
class Controls {
public:
    Input sample(Pad pad) {
        Input in;
        const uint32_t pressed = pad.buttons & ~held_;
        held_ = pad.buttons;
        // The menu chord must not inject a coin or start a race.
        if (menu_chord(pad.buttons)) { set_gear(in); return in; }
        float steer = axis(pad.lx, deadzone_);
        if (pad.buttons & (Left | Right))
            steer = float(bool(pad.buttons & Right)) - float(bool(pad.buttons & Left));
        if (steer_invert_) steer = -steer;
        const float pedal = axis(pad.ry, deadzone_);
        const float accel = pad.buttons & R ? 1.f : std::max(-pedal, 0.f);
        const float brake = pad.buttons & L ? 1.f : std::max(pedal, 0.f);
        in.steer = uint8_t(std::lround(128.f + 96.f * steer));
        in.accel = uint8_t(std::lround(32.f + 192.f * accel));
        in.brake = uint8_t(std::lround(32.f + 192.f * brake));
        // Shift only on edges, including when several board frames are run.
        const int shift = int(bool(pressed & Up)) - int(bool(pressed & Down));
        gear_ = std::clamp(gear_ + shift, 1, 4);
        set_gear(in);
        if (pad.buttons & Select) in.in0 &= uint8_t(~0x01);
        if (pad.buttons & Start) in.in0 &= uint8_t(~0x10);
        if (pad.buttons & Cross) in.in0 &= uint8_t(~0x20);
        if (pad.buttons & Circle) in.in0 &= uint8_t(~0x40);
        if (pad.buttons & Square) in.in0 &= uint8_t(~0x80);
        if (pad.buttons & Triangle) in.in1 &= uint8_t(~0x01);
        return in;
    }
    void latch(uint32_t buttons) { held_ = buttons; }
    int gear() const { return gear_; }
    void set_deadzone(float value) { deadzone_ = std::clamp(value, 0.0f, 0.4f); }
    void set_steer_invert(bool value) { steer_invert_ = value; }
private:
    void set_gear(Input &in) const {
        // Same gearbox encoding as src/app/controls.cpp.
        constexpr uint8_t codes[] = {0, 2, 1, 6, 5};
        in.in1 = uint8_t((in.in1 & ~0x70) | (codes[gear_] << 4));
    }
    uint32_t held_ = 0;
    int gear_ = 1;
    float deadzone_ = 0.12f;
    bool steer_invert_ = false;
};

// Host time only; each simulation step still advances at GameLoop::kFrameHz.
class FrameClock {
public:
    explicit FrameClock(double hz, int max_steps = 4)
        : step_(1.0 / hz), max_steps_(std::clamp(max_steps, 1, 4)) {}
    int advance(double seconds) {
        if (!std::isfinite(seconds) || seconds < 0) { reset(); return 0; }
        pending_ = std::min(pending_ + seconds, step_ * 4);
        int count = 0;
        while (pending_ >= step_ && count < max_steps_) { pending_ -= step_; ++count; }
        // Under load, present each completed frame instead of computing up to
        // four complete rasters and displaying only the last. Discard overdue
        // host-time debt, not guest instructions; preserve fractional time so
        // 57.524 Hz on a 60 Hz display does not accidentally become 30 Hz.
        if (pending_ >= step_) pending_ = std::fmod(pending_, step_);
        return count;
    }
    void reset() { pending_ = 0; }
private:
    double step_, pending_ = 0;
    int max_steps_;
};
} // namespace vita
