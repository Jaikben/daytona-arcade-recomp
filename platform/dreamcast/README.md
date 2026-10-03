# Sega Dreamcast port (in progress)

The recompiled game on a Dreamcast (SH-4 at 200 MHz, 16 MB main RAM, 8 MB
video RAM, 2 MB sound RAM), built with KallistiOS. Like `platform/vita`, this
folder is a separate build: it compiles the main tree's runtime and the
host-generated game code. Changes the Dreamcast needs in the runtime are
behind `M2_DC_*` compile-time defines that only this build sets, so the
Windows, macOS and Linux builds are unchanged. It is built from the `daytona` ROM set only (Revision A, 1994, the set
with link play).

Status: **the attract mode runs in Flycast**, drawn by the PVR with textures
and both tile layers, in lockstep with the desktop (instruction counts and
display list identical at every checkpoint). Slow (about 4 frames/s in
Flycast), no controls or sound yet, not yet on a console. Details and the
plan: [HANDOFF.md](HANDOFF.md).

## Requirements

- The desktop build first: setup, then
  `python scripts/recompile.py --set daytona --build-dir build-daytona`.
- An SH-4 toolchain. On Windows, [DreamSDK](https://dreamsdk.org/) (R4 tested,
  at `C:/DreamSDK`); elsewhere, a KOS dc-chain toolchain (`KOS_CC_BASE`).
- KallistiOS 2.2.1 is set up by the driver in `extern/kos-dc` (git-ignored),
  from DreamSDK's offline package or cloned; DreamSDK's own installed KOS is
  not used (see [THIRD_PARTY.md](THIRD_PARTY.md)).
- Flycast to run it (2.7 tested); a console later.

## Commands

All through `platform/dreamcast/build_dreamcast.py`; output goes under the
host build directory (`build-daytona/dreamcast*`, git-ignored).

| Command | What it does |
| --- | --- |
| `kos` | Sets up and builds KallistiOS 2.2.1 in `extern/kos-dc` (the other commands do it first). |
| `selftest` | Builds the floating-point self-test as a bootable disc image and runs it in Flycast; exit 0 only if every result matches the PC's bits. |
| `videotest` | The display path: a 496x384 frame through the PVR every frame; prints the per-frame cost. |
| `compile` | Compiles the runtime and the generated game code for the SH-4 and reports their size. |
| `game` | Builds the game and its disc image (with the ROM images and the saved settings EEPROM and backup RAM, `--nvram`, by default `%APPDATA%/daytona-recomp/daytona`) and runs it in Flycast. |
| `tools` | Builds the host measuring tools in `tools/` (desktop compiler). |
| `measure` | Runs `romuse` over the input scripts: which ROM pages the game reads, and when. |
| (tools) `tracecheck` | The desktop's side of the lockstep check: the per-frame `TRACE` line the Dreamcast prints every 60 frames. |
| (tools) `dcmemcheck` | The runtime built with M2_DC_MEMORY on the desktop, ROM pages from the image files: the Dreamcast's code paths at desktop speed. |

`--flycast PATH` (or `FLYCAST`) points at `flycast.exe`. Flycast runs from a
portable copy in `build-daytona/dreamcast/flycast` with its own `emu.cfg`, so
your own Flycast settings are not changed. The program's serial output is read
from Flycast's console window (`scripts/flycast_run.py`).

## Build notes

- **`-m4-single`, not `-m4-single-only`.** The i960 code's floating-point fast
  paths need a 64-bit `double`; `-m4-single-only` makes it 32-bit. KOS is
  built that way in `extern/kos-dc`.
- **`int32_t` is `long` in newlib for SH.** The shared sources assume `int`
  (`std::max(int32_t, int)` in `raster.cpp`, `multipcm.cpp`). Both are 32 bits
  on SH-4, so the Makefile redefines `__INT32_TYPE__` as `int` for this build;
  the source is not changed.
- **The game's disc image contains your ROM set's images** (`rom/`), built
  locally from your own set. It is build output: never share or commit it.
- On Windows the KOS build runs in DreamSDK's own login shell. A plain
  `bash -c` from Git Bash mixes two MSYS runtimes and hangs.
