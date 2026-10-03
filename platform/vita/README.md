# Native PS Vita target (experimental)

This is a VitaSDK/SDL2 frontend for the existing native runtime. It is a
native Vita application, **not a PSP/Adrenaline build**. The i960, TGP and
68000 programs still come from the host recompilation pipeline. No
interpreter, replacement game logic, ROM bytes or generated code is added.

The desktop SDL3/SDL_GPU application and root CMake build are unchanged.
The Vita frontend uploads the existing 496x384 software-composited screen
through SDL2's Vita renderer. This does **not** move the Model 2 rasterizer
onto the Vita GPU. Tested on a real Vita (2026-10-06): it plays with no
issues reported. Performance, memory headroom and full-race parity figures
are not recorded yet.

## Build

### Optional GXM CPU enhancements

The GXM frontend options offer CPU 500 MHz and fourth-core scheduling.
Both require compatible firmware/plugin support; no plugins are installed
by the game. Defaults remain CPU 333 MHz and fourth core off.
The CPU option shows actual frequency, and unsuccessful 500 MHz requests
fall back to requesting 444 MHz. Check your overclock plugin's per-game
profile if the actual frequency differs from the selection.
Fourth-core access is verified through thread affinity readback. Rejected
requests retain ordinary three-core scheduling and show unavailable.
Game and audio threads may use the extra core; this does not split sequential
game logic into additional workers or guarantee higher FPS.

Use a homebrew-enabled Vita, a host C++20 toolchain and VitaSDK with its
SDL2 development package. Reference SDK release: 2026.08. Set `VITASDK`
and install the package with that release's package manager:

```sh
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"
vdpm install sdl2
```

Follow the SDK's installation documentation at https://vitasdk.org/ for a
new SDK install. No proprietary SDK or runtime module is required by this
frontend; it uses SDL2's normal Vita renderer, not PVR/PIB.

From the repository root, prepare the host build using your own complete
`daytona93` ROM set in `roms/daytona93.zip` (or `.7z`):

```sh
python3 scripts/setup.py
# After changing the seeds/recompilers, regenerate with the HOST compiler:
python3 scripts/recompile.py
# Build a separate ARM executable and installable package:
python3 scripts/build_vita.py
```

`setup.py` already recompiles when the ROM set is present. The explicit
`recompile.py` command is only necessary after changes or when adding the
ROM set later. The default output is:

```text
build/vita/daytona_vita.vpk
```

A different host build directory is supported:

```sh
python3 scripts/build_vita.py --host-build-dir build-host --build-dir build/vita --jobs 4
```

The equivalent CMake configuration is:

```sh
cmake -S platform/vita -B build/vita \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DDAYTONA_GEN_ROOT="$PWD/build/gen"
cmake --build build/vita --parallel 4
```

Do not configure the host build directory with the Vita toolchain. The
importer and three recompilers run on your computer, not on the Vita. The
Vita configuration fails rather than creating an empty game if any required
generated source group is missing. Keep VPKs, ROM caches and generated C++
local, under the ignored `build/` directory.

## Install and play

### Dual-ROM GXM package and LAN link play

The dual package launches daytona93 by default. In Options, change ROM to
DAYTONA (1994 REVISION A), then choose Start/Reset; switch back the same way.
Switching replaces the native executable, not just the ROM data.
Supply both complete ZIPs yourself:

- `ux0:data/daytona93/daytona93.zip`
- `ux0:data/daytona/daytona.zip`

Settings, EEPROM and backup RAM stay in each set's own directory. Revision A
has its own defaults; CPU/core and other options may need setting again.
An older single-game package reports when the other executable is missing.

Link play requires Revision A on all cabinets and a connected Wi-Fi LAN.
Enable Link Play in its Vita options, set the four next-cabinet IPv4 octets,
listen port and next-cabinet port (default 15112). Reset to apply.
With two cabinets, each one's next address is the other. With more, form a
ring. Desktop uses main's Link Play settings and the same TCP port/protocol.
The pause menu shows the Vita IP, receive/transmit connection and cabinet ID.
No discovery, hostname resolution or internet port forwarding is provided.

Enter test mode with Select+Triangle; Select+Square is service, Cross advances
the menu and Start confirms. Under GAME SYSTEM set one MASTER, the others
SLAVE, and unique CAR NUMBERs. Cabinet type/region and game settings must
match (for example TWIN/JPN); otherwise the game can cancel the link.
Frame sync is optional, off by default. For solo Revision A play, disable
link and configure a single cabinet in test mode.

Build each set's generated sources separately using `scripts/recompile.py
--set daytona --build-dir build/revision-a-host` and the existing daytona93
host build. Then:

```sh
python3 scripts/build_vita.py --set daytona --gpu-fast --host-build-dir build/revision-a-host --build-dir build/vita-revision-a
python3 scripts/build_vita.py --set daytona93 --gpu-fast --host-build-dir build --build-dir build/vita-dual --revision-a-self build/vita-revision-a/eboot.bin
```

The second VPK contains both executables. Actual Vita-to-Vita/Vita-to-desktop
Wi-Fi operation and executable switching require hardware validation.

Install your locally built VPK with VitaShell, then put your **complete
ZIP ROM set** at:

```text
ux0:data/daytona93/daytona93.zip
```

Launch **Daytona Recomp** and choose **START GAME**. ROM CRC and size
validation uses the existing importer. Renaming a different Daytona set
will not make it compatible. The Vita target deliberately omits the 7z
SDK to avoid solid-archive decoding memory spikes. A host `.7z` source can
still be used for recompilation; repack the complete set as `.zip` with
unchanged ROM filenames before copying it to the Vita.

| Action | Vita control |
| --- | --- |
| Steering | Left stick, or D-pad left/right |
| Accelerate / brake | R / L |
| Analogue accelerate / brake | Right stick up / down |
| Shift up / down | D-pad up / down (one shift per press) |
| View 1 / 2 / 3 / 4 | Cross / Circle / Square / Triangle |
| Coin / start | Select / Start |
| Pause menu | Start + Select together |
| Navigate / select / resume | D-pad / Cross / Circle |
| Test switch / service coin | Pause-menu entries |

The menu chord suppresses coin/start while both buttons are held. Pressing
Select significantly before Start can still insert a coin before the chord
exists. Buttons must be released after loading, pausing or resuming so menu
presses do not leak into the game.

The frontend retains the desktop's square-pixel framebuffer aspect ratio,
with side bars on the Vita display. Simulation steps use
`rt::GameLoop::kFrameHz`, not a hard-coded 60 Hz. The Vita frontend now runs
at most one complete simulation frame before presenting it. Fractional host
time is retained at normal speed; overdue whole steps are discarded under
load rather than rendering four complete frames and displaying only the last.
This slows wall-clock progress when the device cannot keep up; it does not
skip guest instructions, increase the guest timestep, or make the simulation
itself four times faster. The generic FrameClock default remains four steps.

See [PERFORMANCE.md](PERFORMANCE.md) for the stage timings now written to
`vita.log`. This is a diagnostic build, not a confirmed full-speed fix.

## Sound and saved data

FM and PCM are resampled independently to the output device rate, mixed,
clamped and played as stereo signed 16-bit audio. The callback only consumes
samples; board execution stays on the main thread. Queues are bounded and
cleared on pause/reset. Mute is saved independently of board state.

EEPROM and backup RAM are saved when changed, every five seconds and on
pause/reset/quit. Writes use a temporary file and retain a `.bak` generation;
loads reject incorrect sizes and try the backup. A sudden power loss can
still lose changes since the last successful save. Use SAVE AND QUIT for a
clean exit. All files are under `ux0:data/daytona93/`:

```text
ioboard_eeprom.bin
backup_ram.bin
mute.bin
vita.log
```

`vita.log` is replaced on each launch. Copy it before reopening the app when
reporting a crash. Missing/incorrect ROM errors and runtime faults are also
shown in the menu. A decoder/runtime fault requires a reset rather than
resuming a possibly inconsistent board state.

## Validation and limitations

Host tests (no ROM or SDK required):

```sh
mkdir -p build
c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/test_vita_controls.cpp -o build/test_vita_controls
build/test_vita_controls
python3 -m unittest discover -s tests -p test_build_vita.py -v
```

ROM-free ARM compile check (requires VitaSDK, SDL2, and fetched SoftFloat/ymfm):

```sh
python3 scripts/setup.py --no-build
python3 scripts/build_vita.py --compile-check
```

The included workflow performs the host tests and this ARM compile check.
**Compile-check mode makes objects only: it does not link generated game
code, produce a VPK, prove floating-point parity on ARM, or test gameplay.**
It intentionally does not upload artifacts or use ROM secrets.

This target still needs a full ROM-generated ARM link and real-device
verification: boot, all courses, manual/automatic gears, sound, sustained
frame time, peak memory, saved settings, and suspend/resume. The Vita
frontend handles SDL background/foreground events and limits post-stall
catch-up, but whether the installed SDL build emits those events during
system suspend must be verified on hardware. No overclock is forced.

SoftFloat keeps the existing 8086-SSE *semantic specialization* (not x86
machine instructions) but uses a project-owned portable platform header
without GCC `__int128`. Its state is main-thread-only in this target. Do
not move board execution onto multiple threads without restoring TLS or
introducing explicitly separate SoftFloat state.

See [HANDOFF.md](HANDOFF.md) for the port's status and
[THIRD_PARTY.md](THIRD_PARTY.md) for SDK dependency references.

## Widescreen, draw distance and cabinet controls

The GXM Options menu now saves Aspect (Original, 16:10, 16:9, 21:9),
HUD (Centred or Screen Edges), and scenery Draw Distance (Shortest through
Furthest). Changes apply on resume; original aspect, centred HUD and default
distance remain the defaults. Widescreen shows additional scenery with the
same focal length, not stretched pixels. On the 960x544 display, 21:9 is
letterboxed vertically. The road window is unchanged by scenery distance.

HUD relocation shares the desktop per-item rules and only activates when
the race HUD is visible. Scenery stays put and crossing banners stay whole.
Recovery 1 restores the earlier rendering paths after the perspective build
lost textured geometry on hardware. Original mode uses GXM tile composition;
wide mode uses CPU tile/HUD composition. Road wobble and widescreen performance
are not fixed by this recovery. Steering curves remain available. The layer arena is12MiB instead of10MiB,
making the three GPU arenas32MiB total.

Hold Select and press Triangle for cabinet Test (enter/confirm).
Hold Select and press Square for cabinet Service (advance/select).
Release between presses. Cross supplies VR1 (menu next) and Start supplies
cabinet Start (menu select), as used by the game's test screens.
Start+Select still opens the frontend pause menu.
Plain Select inserts a coin on release; Test/Service chords do not insert
coins or operate view buttons. The bindings are listed in Options.

Draw distance requires generated code with seeds/daytona93_hooks.txt:
regenerate with scripts/recompile.py before building. Merely linking the
enhancement runtime cannot add a missing hook to old generated code.

## Steering curves

Options → Steering Curve selects Linear (default), Soft (signed square) or
Extra Soft (cubic). Curves apply after the stick deadzone and before inversion;
full lock and D-pad steering remain unchanged. Soft settings give finer control
around centre. The choice is saved as steer_curve=0/1/2 in vita.cfg.


## Wide 2 update

Includes GitHub main through c081a2d, including the stricter condition-panel
overlay detection. Options adds Stretch Tile Background and Skip Launcher,
both off by default and saved in vita.cfg. Background stretching in the Vita
GPU frontend scales only the backdrop to the wide viewport, not the 3D scene
or HUD. Original aspect is unaffected. Unlike desktop's coverage-gated setting,
the Vita option stretches the backdrop whenever widescreen is selected.
Skip Launcher auto-loads the installed ROM on next launch; a load failure
returns to the menu with its error. Start+Select always opens the menu in-game.

The restored polygon/tessellation path is unchanged. Widescreen keeps a native
496x384 CPU tile backdrop and scales it with the existing 2D draw API; no custom
matrix or GPU tile compositor change. This reduces backdrop upload bytes by27%
at16:9. Unchanged foreground pixels reuse HUD grouping and uploads. CPU tile
drawing and wider scene geometry still cost time; real Vita FPS is unverified.

## GPU tiles after main 3044f3b

Current recovery build disables wide GPU tile composition after a hardware
slowdown report, using the CPU wide layers instead. Original-aspect GPU tiles
and GPU 3D stay enabled. The implementation below is retained for profiling.

The Vita branch includes the latest desktop GPU renderer but still uses GXM,
not SDL_GPU's desktop shaders. Background and centred foreground tile layers
are composed on GXM, including widescreen. Like desktop main, moving individual
HUD items to the edges retains a CPU foreground-composition fallback. Tile
decoding/cache updates remain on the CPU. Physical GPU buffering is separate:
double by default, with single and triple available in Options.

Road subdivision additionally checks perspective texture error against the
same reciprocal-depth interpolation used by main. The existing eight-way cap
and pool limits remain; this is not per-pixel perspective-shader parity and
can increase geometry work. Hardware appearance/performance needs testing.
