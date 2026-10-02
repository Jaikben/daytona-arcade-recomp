#include "app/ffb.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace app {

namespace {

// The device steering is bound to: a wheel/joystick binding first, else the gamepad.
SDL_Joystick *steering_device(const Devices &d, const Controls &c) {
    for (Action a : {SteerRight, SteerLeft}) {
        const JoyInput &j = c.bind[a].joy;
        if (j.kind != JoyInput::None)
            if (SDL_Joystick *s = d.find(j.guid)) return s;
    }
    return d.pad ? SDL_GetGamepadJoystick(d.pad) : nullptr;
}

SDL_HapticDirection steering_axis() {
    SDL_HapticDirection dir{};
    dir.type = SDL_HAPTIC_STEERING_AXIS;
    return dir;
}

} // namespace

const char *ForceFeedback::device_kind() const {
    if (haptic_) return "wheel (force feedback)";
    if (joy_) return "gamepad (rumble)";
    return "none";
}

void ForceFeedback::open(SDL_Joystick *joy) {
    joy_ = joy;
    if (!SDL_IsJoystickHaptic(joy) || !(haptic_ = SDL_OpenHapticFromJoystick(joy))) {
        // no haptics: rumble, if the device has it
        if (!SDL_GetBooleanProperty(SDL_GetJoystickProperties(joy), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false)) joy_ = nullptr;
        return;
    }
    const Uint32 features = SDL_GetHapticFeatures(haptic_);
    if (features & SDL_HAPTIC_AUTOCENTER) SDL_SetHapticAutocenter(haptic_, 0); // the game centres the wheel itself
    if (features & SDL_HAPTIC_GAIN) SDL_SetHapticGain(haptic_, 100);
    auto condition = [&](SDL_HapticEffectType type) {
        if (!(features & type)) return -1;
        SDL_HapticEffect e{};
        e.condition.type = type;
        e.condition.direction = steering_axis();
        e.condition.length = SDL_HAPTIC_INFINITY;
        e.condition.right_sat[0] = e.condition.left_sat[0] = 0xffff;
        return SDL_CreateHapticEffect(haptic_, &e);
    };
    std::fill(std::begin(sent_), std::end(sent_), -1);
    spring_ = condition(SDL_HAPTIC_SPRING);
    friction_ = condition(SDL_HAPTIC_FRICTION);
    if (features & SDL_HAPTIC_SINE) {
        SDL_HapticEffect e{};
        e.periodic.type = SDL_HAPTIC_SINE;
        e.periodic.direction = steering_axis();
        e.periodic.length = SDL_HAPTIC_INFINITY;
        e.periodic.period = 60; // ms: a rumble through the rim
        sine_ = SDL_CreateHapticEffect(haptic_, &e);
    }
    if (features & SDL_HAPTIC_CONSTANT) {
        SDL_HapticEffect e{};
        e.constant.type = SDL_HAPTIC_CONSTANT;
        e.constant.direction = steering_axis();
        e.constant.length = SDL_HAPTIC_INFINITY;
        constant_ = SDL_CreateHapticEffect(haptic_, &e);
    }
    running_ = false;
}

void ForceFeedback::close() {
    if (haptic_) {
        SDL_StopHapticEffects(haptic_);
        SDL_CloseHaptic(haptic_);
    } else if (joy_) {
        SDL_RumbleJoystick(joy_, 0, 0, 0);
    }
    haptic_ = nullptr;
    joy_ = nullptr;
    spring_ = friction_ = sine_ = constant_ = -1;
    running_ = false;
}

void ForceFeedback::update(const std::vector<uint8_t> &commands, const Devices &devices, const Controls &controls,
                           float strength, bool invert) {
    stopped_ = false;
    for (uint8_t c : commands) drive_.command(c);
    SDL_Joystick *j = strength > 0 ? steering_device(devices, controls) : nullptr;
    if (j != joy_) {
        close();
        if (j) open(j);
    }
    apply(strength, invert);
}

void ForceFeedback::stop() {
    if (stopped_) return;
    stopped_ = true;
    drive_ = rt::DriveBoard{};
    if (haptic_) {
        SDL_StopHapticEffects(haptic_);
        running_ = false;
        std::fill(std::begin(sent_), std::end(sent_), -1);
    } else if (joy_) {
        SDL_RumbleJoystick(joy_, 0, 0, 0);
    }
}

void ForceFeedback::apply(float strength, bool invert) {
    if (!joy_) return;
    strength = std::clamp(strength, 0.0f, 1.0f);
    const float force = std::clamp(drive_.force * (invert ? -1.0f : 1.0f), -1.0f, 1.0f) * strength;
    if (!haptic_) { // rumble: the vibration on the small motor, the turning force's size on the large one
        const auto lo = Uint16(std::lround(std::fabs(force) * 0xffff));
        const auto hi = Uint16(std::lround(drive_.vibration * strength * 0xffff));
        SDL_RumbleJoystick(joy_, lo, hi, 200); // renewed every frame
        return;
    }
    auto changed = [&](int slot, int level) { // only levels that changed go to the device
        if (sent_[slot] == level) return false;
        sent_[slot] = level;
        return true;
    };
    auto set_condition = [&](int id, float amount) {
        const auto level = Sint16(std::lround(amount * strength * 0x7fff));
        if (id < 0 || !changed(id == spring_ ? 0 : 1, level)) return;
        SDL_HapticEffect e{};
        e.condition.type = id == spring_ ? SDL_HAPTIC_SPRING : SDL_HAPTIC_FRICTION;
        e.condition.direction = steering_axis();
        e.condition.length = SDL_HAPTIC_INFINITY;
        e.condition.right_sat[0] = e.condition.left_sat[0] = 0xffff;
        e.condition.right_coeff[0] = e.condition.left_coeff[0] = level;
        SDL_UpdateHapticEffect(haptic_, id, &e);
    };
    set_condition(spring_, drive_.centering);
    set_condition(friction_, drive_.friction);
    const auto vibration = Sint16(std::lround(drive_.vibration * strength * 0x7fff));
    if (sine_ >= 0 && changed(2, vibration)) {
        SDL_HapticEffect e{};
        e.periodic.type = SDL_HAPTIC_SINE;
        e.periodic.direction = steering_axis();
        e.periodic.length = SDL_HAPTIC_INFINITY;
        e.periodic.period = 60;
        e.periodic.magnitude = vibration;
        SDL_UpdateHapticEffect(haptic_, sine_, &e);
    }
    const auto level = Sint16(std::lround(force * 0x7fff));
    if (constant_ >= 0 && changed(3, level)) {
        SDL_HapticEffect e{};
        e.constant.type = SDL_HAPTIC_CONSTANT;
        e.constant.direction = steering_axis();
        e.constant.length = SDL_HAPTIC_INFINITY;
        e.constant.level = level;
        SDL_UpdateHapticEffect(haptic_, constant_, &e);
    }
    if (!running_) {
        for (int id : {spring_, friction_, sine_, constant_})
            if (id >= 0) SDL_RunHapticEffect(haptic_, id, 1);
        running_ = true;
    }
}

} // namespace app
