// Wheel and pedal bindings (src/app/controls.cpp) against SDL virtual
// joysticks: a wheel (one axis) and separate pedals that rest at +32767 and
// press toward -32768, as many do. Checks the device list follows SDL's
// events, bindings save and load, calibrated axes reach the I/O board's ADC
// ranges, and a binding to a device that is not connected reads as nothing.
// Force feedback: the drive board's commands decode (rt::DriveBoard), and a
// steering device without haptics gets them as rumble.

#include "app/controls.h"
#include "app/ffb.h"
#include "runtime/drive_board.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

Uint16 rumble_low = 0, rumble_high = 0;
bool SDLCALL rumble(void *, Uint16 low, Uint16 high) {
    rumble_low = low, rumble_high = high;
    return true;
}

SDL_JoystickID attach(SDL_JoystickType type, int axes, int buttons, const char *name, bool can_rumble = false) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = Uint16(type);
    d.naxes = Uint16(axes);
    d.nbuttons = Uint16(buttons);
    d.name = name;
    if (can_rumble) d.Rumble = rumble;
    return SDL_AttachVirtualJoystick(&d);
}

void pump(app::Devices &devices) {
    SDL_UpdateJoysticks();
    SDL_Event e;
    while (SDL_PollEvent(&e)) devices.handle_event(e);
}

std::string guid(SDL_Joystick *j) {
    char b[64];
    SDL_GUIDToString(SDL_GetJoystickGUID(j), b, sizeof b);
    return b;
}
} // namespace

int main() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    app::Devices devices;
    const SDL_JoystickID wheel_id = attach(SDL_JOYSTICK_TYPE_WHEEL, 1, 2, "Test wheel");
    const SDL_JoystickID pedals_id = attach(SDL_JOYSTICK_TYPE_THROTTLE, 2, 0, "Test pedals");
    pump(devices);
    check(devices.joys.size() == 2, "both virtual devices opened");
    SDL_Joystick *wheel = SDL_GetJoystickFromID(wheel_id), *pedals = SDL_GetJoystickFromID(pedals_id);
    check(wheel && pedals, "devices found by id");
    if (!wheel || !pedals) return 1;
    SDL_SetJoystickVirtualAxis(pedals, 0, 32767); // pedals at rest
    SDL_SetJoystickVirtualAxis(pedals, 1, 32767);
    pump(devices);

    // Bindings as the launcher makes them: steering both ways on the wheel's
    // axis (full lock at a quarter turn each way), pedals from rest to floor.
    app::Controls c;
    c.set_defaults();
    c.joy_deadzone = 0.0f;
    auto axis = [&](SDL_Joystick *j, int index, int rest, int full) {
        app::JoyInput in;
        in.kind = app::JoyInput::Axis, in.guid = guid(j), in.index = index, in.rest = rest, in.full = full;
        return app::JoyInput::parse(in.save()); // through the config format
    };
    c.bind[app::SteerLeft].joy = axis(wheel, 0, 0, -8192);
    c.bind[app::SteerRight].joy = axis(wheel, 0, 0, 8192);
    c.bind[app::Accelerate].joy = axis(pedals, 0, 32767, -32768);
    c.bind[app::Brake].joy = axis(pedals, 1, 32767, -32768);
    app::JoyInput button;
    button.kind = app::JoyInput::Button, button.guid = guid(wheel), button.index = 1;
    c.bind[app::GearUp].joy = app::JoyInput::parse(button.save());
    check(c.bind[app::Accelerate].joy.rest == 32767 && c.bind[app::Accelerate].joy.full == -32768, "axis binding round trip");

    const bool *no_keys = nullptr;
    rt::Inputs in = c.sample(no_keys, devices);
    check(in.steer == 0x80 && in.accel == 0x20 && in.brake == 0x20, "at rest: centred, pedals up");

    SDL_SetJoystickVirtualAxis(wheel, 0, 8192); // full right lock, a quarter turn
    SDL_SetJoystickVirtualAxis(pedals, 0, -32768); // accelerator floored
    SDL_SetJoystickVirtualAxis(pedals, 1, 0);     // brake half way
    pump(devices);
    in = c.sample(no_keys, devices);
    check(in.steer == 0xe0, "full right");
    check(in.accel == 0xe0, "accelerator floored");
    check(std::abs(int(in.brake) - 0x80) <= 1, "brake half way");

    SDL_SetJoystickVirtualAxis(wheel, 0, -32768); // past the bound lock: clamps
    pump(devices);
    in = c.sample(no_keys, devices);
    check(in.steer == 0x20, "full left (clamped)");

    const int gear = c.gear;
    SDL_SetJoystickVirtualButton(wheel, 1, true);
    pump(devices);
    c.sample(no_keys, devices);
    check(c.gear == std::min(gear + 1, 4), "wheel paddle shifts up");

    app::JoyInput missing = c.bind[app::Accelerate].joy;
    missing.guid = "00000000000000000000000000000000";
    check(missing.value(devices, 0.0f) == 0.0f, "unconnected device reads 0");
    check(missing.describe(devices).find("not connected") != std::string::npos, "unconnected device described");

    // The drive board: commands by type in the high nibble.
    rt::DriveBoard drive;
    drive.command(0x1f), drive.command(0x27), drive.command(0x3f), drive.command(0x57);
    check(drive.centering == 1.0f && std::fabs(drive.friction - 7.0f / 15.0f) < 1e-6f && drive.vibration == 1.0f &&
              drive.force == 0.5f,
          "drive commands decode");
    drive.command(0x60);
    check(drive.force == -1.0f / 16.0f, "turn left");
    drive.command(0x71); // steering parameters: not modelled, nothing changes
    check(drive.force == -1.0f / 16.0f && drive.centering == 1.0f, "0x7- leaves the state");
    drive.command(0xcb);
    check(drive.centering == 0 && drive.friction == 0 && drive.vibration == 0 && drive.force == 0, "board reset stops");

    // Force feedback on a steering device without haptics: rumble. The wheel
    // here is replaced by a pad-like joystick that can rumble.
    const SDL_JoystickID rumbler_id = attach(SDL_JOYSTICK_TYPE_GAMEPAD, 2, 4, "Test rumbler", true);
    pump(devices);
    SDL_Joystick *rumbler = SDL_GetJoystickFromID(rumbler_id);
    c.bind[app::SteerLeft].joy = axis(rumbler, 0, 0, -32768);
    c.bind[app::SteerRight].joy = axis(rumbler, 0, 0, 32767);
    app::ForceFeedback ffb;
    ffb.update({0x3f, 0x57}, devices, c, 1.0f, false); // full vibration, pulling right at half
    SDL_UpdateJoysticks();
    check(std::string(ffb.device_kind()) == "gamepad (rumble)", "rumble device chosen");
    check(rumble_high == 0xffff && std::abs(int(rumble_low) - 0x8000) <= 1, "vibration and pull as rumble");
    ffb.update({}, devices, c, 0.5f, false); // the state holds; the strength scales it
    SDL_UpdateJoysticks();
    check(std::abs(int(rumble_high) - 0x8000) <= 1, "strength scales rumble");
    ffb.stop();
    SDL_UpdateJoysticks();
    check(rumble_low == 0 && rumble_high == 0, "stop: rumble off");
    ffb.update({}, devices, c, 0.0f, false); // strength 0: off, the device released
    check(std::string(ffb.device_kind()) == "none", "strength 0 releases the device");
    ffb.close();
    SDL_DetachVirtualJoystick(rumbler_id);
    pump(devices);

    SDL_DetachVirtualJoystick(pedals_id);
    pump(devices);
    check(devices.joys.size() == 1, "unplugged devices closed");
    in = c.sample(no_keys, devices);
    check(in.accel == 0x20, "pedals gone: accelerator up");

    devices.close_all();
    SDL_DetachVirtualJoystick(wheel_id);
    SDL_Quit();
    if (failures) return 1;
    std::printf("test_app_controls: ok\n");
    return 0;
}
