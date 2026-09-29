# Daytona USA static recompilation

Daytona USA (Sega Model 2) rebuilt as native code: the game's i960 program
and the TGP program it uploads are statically recompiled to portable C++,
and the fixed-function hardware (geometrizer, rasterizer, tilemaps) is native
C++. No interpreter, no emulation core. MAME is used only as a test oracle.
See `docs/daytona-usa-recomp-design.md` and `HANDOFF.md`.

No game data is in this repository. You need your own `daytona93` ROM set
(a MAME-format `.zip` or `.7z`).

## Setup

Linux or macOS:

    ./setup.sh

Windows (PowerShell):

    powershell -ExecutionPolicy Bypass -File setup.ps1

These install the toolchain (C++20 compiler, CMake, Ninja, Python 3, Git;
Visual Studio 2022 Build Tools on Windows, Homebrew packages on macOS, your
distribution's packages on Linux), fetch the pinned dependencies into
`extern/`, build, and run the tests. Put your ROM set at
`roms/daytona93.zip` (or `.7z`) first and the game code is recompiled as well (into
`build/`, never committed). Already have a toolchain? Run
`python3 scripts/setup.py` directly.

Options: `--test-extras` (optional test dependencies), `--with-mame` (MAME
source for the oracle test), `--build-mame` (the patched MAME that records
validation traces; Linux and macOS).

After changing the recompiler or the seeds: `python3 scripts/recompile.py`.

## Playing

    build/daytona

(`build/Release/daytona.exe` with the Visual Studio generator.) The launcher
opens first:

- **Game**: choose your `daytona93` ROM set, `.zip` or `.7z` (Browse, or type the path); every file
  is checked against the ROM set this build was recompiled from. Graphics API
  (automatic, Vulkan, Direct3D 12, Metal) and fullscreen. Start.
- **Controls**: bind every arcade control to a key and a gamepad button or
  axis (click, then press). Triggers and sticks are analogue: the accelerator
  and brake follow trigger travel, steering follows the stick. Live meters,
  dead zone, invert steering.

In the game, Esc brings the launcher back (Resume, Reset, Quit). Settings
are saved as they change, with the settings EEPROM and backup RAM, in your
user data folder (`launcher.ini`). Options: `--rom FILE.zip --autostart
--gpu vulkan|direct3d12|metal --fullscreen`. Resolution and upscaling
options are to come.

Default controls:

| Control | Keyboard | Gamepad |
| --- | --- | --- |
| Steer | Left / Right | Left stick (analogue) |
| Accelerate / brake | Up / Down | Right / left trigger (analogue) |
| Gears 1-4 | 1 2 3 4 | |
| View buttons VR1-VR4 | A S D F | Face buttons |
| Shift up / down | W / Q | Right / left shoulder |
| Coin / start | 5 / Enter | Back / Start |
| Test / service | F2 / F3 | |
| Fullscreen / launcher | F11 / Esc | |

No sound yet: the sound board is next.
