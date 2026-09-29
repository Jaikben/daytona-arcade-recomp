# Daytona USA static recompilation

Daytona USA (Sega Model 2) rebuilt as native code: the game's i960 program
and the TGP program it uploads are statically recompiled to portable C++,
and the fixed-function hardware (geometrizer, rasterizer, tilemaps) is native
C++. No interpreter, no emulation core. MAME is used only as a test oracle.
See `docs/daytona-usa-recomp-design.md` and `HANDOFF.md`.

No game data is in this repository. You need your own `daytona93` ROM set.

## Setup

Linux or macOS:

    ./setup.sh

Windows (PowerShell):

    powershell -ExecutionPolicy Bypass -File setup.ps1

These install the toolchain (C++20 compiler, CMake, Ninja, Python 3, Git;
Visual Studio 2022 Build Tools on Windows, Homebrew packages on macOS, your
distribution's packages on Linux), fetch the pinned dependencies into
`extern/`, build, and run the tests. Put your ROM set at
`roms/daytona93.zip` first and the game code is recompiled as well (into
`build/`, never committed). Already have a toolchain? Run
`python3 scripts/setup.py` directly.

Options: `--test-extras` (optional test dependencies), `--with-mame` (MAME
source for the oracle test), `--build-mame` (the patched MAME that records
validation traces; Linux and macOS).

After changing the recompiler or the seeds: `python3 scripts/recompile.py`.

## Playing

    build/daytona [--gpu vulkan|direct3d12|metal] [--fullscreen]

(`build/Release/daytona.exe` with the Visual Studio generator.) SDL picks the
GPU backend unless `--gpu` is given. Settings and backup RAM are saved to your
user data folder on exit.

| Control | Keyboard | Gamepad |
| --- | --- | --- |
| Steer | Left / Right | Left stick |
| Accelerate / brake | Up or Z / Down or X | Right / left trigger |
| Gears 1-4 | 1 2 3 4 | Shoulders (down / up) |
| View buttons VR1-VR4 | A S D F | Face buttons |
| Coin / start | 5 / Enter | Back / Start |
| Test / service | F2 / F3 | |
| Fullscreen / quit | F11 / Esc | |

No sound yet: the sound board is next.
