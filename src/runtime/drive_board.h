// The force feedback drive board (838-10646 on Daytona USA), as the game
// sees it: the I/O board passes on each command byte the game writes to its
// dual-port RAM byte 0x11 (IoBoard), and the drive board's Z80 turns the
// wheel motor. Not emulated: its program (EPR-16488) is not run. Commands are
// decoded instead, by type in the high nibble, value in the low; the same
// command set Sega's later drive boards use, as Supermodel documents it
// (Src/Model3/DriveBoard/WheelBoard.cpp; GPL-3, read for the meanings only,
// no code taken; see THIRD_PARTY.md). Daytona sends 0x1-, 0x2-, 0x3-, 0x5-,
// 0x6- and 0x7- commands in a race.
//
// The result is the wheel's state for a host force feedback device:
// centring spring, friction and vibration strengths (0..1) and a constant
// force (-1 full left .. +1 full right).
#pragma once

#include <cstdint>

namespace rt {

struct DriveBoard {
    float centering = 0, friction = 0, vibration = 0, force = 0;

    void command(uint8_t cmd) {
        const int value = cmd & 0x0f;
        switch (cmd >> 4) {
        case 0x1: centering = float(value) / 15.0f; break;  // self-centring strength (0: off)
        case 0x2: friction = float(value) / 15.0f; break;   // friction strength (0: off)
        case 0x3: vibration = float(value) / 15.0f; break;  // uncentring: vibration (0: off)
        case 0x5: force = float(value + 1) / 16.0f; break;  // turn the wheel right
        case 0x6: force = -float(value + 1) / 16.0f; break; // turn the wheel left
        case 0x8:                                            // test mode: 0 stops everything
            if ((value & 7) == 0) *this = DriveBoard{};
            break;
        case 0xc: *this = DriveBoard{}; break;               // board mode set or reset: motor stops
        default: break; // 0x0- and 0x4- play built-in sequences, 0x7- set steering parameters: not modelled
        }
    }
};

} // namespace rt
