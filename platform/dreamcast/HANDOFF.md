# Dreamcast port: handoff

## Rules for this port

- Dreamcast frontend, build and tools are in `platform/dreamcast/`.
- Changes the port needs in the shared runtime (`src/runtime`) go there,
  like the Vita's `M2_VITA_RENDER_OPT`, behind compile-time defines
  (`M2_DC_*`) that only the Dreamcast build sets. Windows, macOS and Linux
  compile exactly what they did before; each change is checked with desktop
  replays (screen hashes and instruction counts identical before and after).
- The generated game code is used as the desktop recompile writes it.
- **ROM set: `daytona` (Revision A) only**, for link play. Not `daytona93`,
  for builds or for measurements.
- It runs after the desktop build (`build_dreamcast.py`), from its generated
  code and ROM images, like `platform/vita` and `scripts/build_vita.py`.
- Proof of concept in Flycast first; the console after that. Floating-point
  and speed results from Flycast are stated as Flycast results.

## Current state (2026-10-03, overnight)

**A whole recorded race runs in Flycast, in lockstep with the desktop**:
`build_dreamcast.py game --inputs scripts/inputs/race_basic.txt` (coin-up,
course and transmission select, rolling start, the Beginner race) matched
`tools/tracecheck` with the same inputs at all 100 checkpoints of its 6,000
frames (i960 and TGP instruction counts, the display list's hash). Drawn by
the PVR with textures and both tile layers (`game/renderer.h`); the busiest
frame 1,826 polygons, 1,626 textured, none dropped; 292 textures cached; main
RAM steady at 1.35 MB free; 1,133 ROM misses in the race.

**Slow, but four times as fast as at the start of the night**: 6,000 frames
in 350 s, about 17 frames/s in Flycast (not a console figure; the arcade
runs 57.52). Every 4th frame is drawn. Every 60 frames the frontend prints
`PROFILE` (ms per frame, `timer_us_gettime64`, Flycast), late in the race:

| Step | core (i960 code) | geometrizer | video | drawing | race |
| --- | --- | --- | --- | --- | --- |
| Start of the night | 64 | 52 | 80 | 15 | 1,414 s |
| Runtime frame skip (`set_frame_skip(3)`: tile layers composed only for drawn frames) | 85 | 61 | 33 | 16 | 1,045 s |
| ROM's last page remembered (`RomSource::page_fast`) | | | | | 965 s (with `--sample`) |
| Frame-wait skip (M2_DC_SPIN_SKIP) and tile runs (M2_DC_SPEED) | 39 | 41 | 22 | 16 | 626 s |
| Tile rows with nothing to draw skipped; back layers straight onto the screen; clear front-layer rows not uploaded again | 39 | 41 | 11 | 13 | 540 s |
| Geometrizer object data parsed only for frames that are shown; frame skip 3 really set (it was clamped to 2) | 39 | 11 | 8 | 13 | 374 s |
| Renderer: polygons through KOS's direct rendering, no `std::isfinite` (soft-float calls at -fno-fast-math); layers written through the store queues; TGP status helpers inline | 38 | 11 | 8 | 9 | 350 s |

(Drawing is about 64 ms for each drawn frame. The first row is from earlier
in the race, so its core and geometry figures are lower.) Every step matched
the desktop at all 100 checkpoints.

Drawing, per drawn frame (`PROFILE draw` lines): the layers' conversion to
16 bits and upload 27 ms, sorting and waiting for the PVR 1, materials 2,
polygons 6 (21 before direct rendering).

Screenshots of the Flycast window (`scripts/flycast_shot.ps1 OUT.png`, no
input sent) at the attract mode's opening scene and mid-race (lap 1, HUD,
cars, track, grandstand) show the picture right.

Where the time went before that round (`build_dreamcast.py game --sample`: the game
thread's PC at each KOS timer tick, about every 10 ms; `scripts/pc_profile.py`
names the functions from `game.elf`), frames 2700-6000: `Renderer::draw` 11%
(the layers' conversion to 16 bits), `Lockstep::boundary` 8% (once per i960
instruction, out of line at -Os), `tilemap_draw` 5%, `draw_rect` 3%, the
geometrizer 9%, board memory access about 8%.

**Controls**: the controller in port A (`game/controls.h`, tested on the PC
by `tools/test_controls`), or a recorded input script compiled in. With the
controller (`build_dreamcast.py game` without `--inputs`) the game runs until
Flycast is closed: tested past frame 10,000 in the attract mode, controller
A0 found, free RAM steady at 1.33-1.37 MB.

**Lockstep tools**: every 60th frame the frontend prints `TRACE frame i960
tgp buffer`; `tools/tracecheck` prints the same on the desktop.
`tools/dcmemcheck` runs the runtime built with M2_DC_MEMORY on the desktop
(ROM pages from the image files): it matches the desktop at all 60
checkpoints of 3,600 attract frames and all 333 of a whole race
(race_to_end), also with the texture cache's half of texture RAM filled with
a pattern and no frame buffer RAM.

Not done yet: sound, speed, the Medium and Long courses' ROM figures, link
play. Not run on a console.

## Plan

0. **Measure (desktop, no Dreamcast code).** Done: ROM read per run
   (`romuse`), sound commands, SH-4 code size, denormals (`ftzcheck`).
   Still open: the Medium and Long courses as races.
1. **Skeleton.** Done: KOS build, bootable CDI, runs in Flycast.
2. **Floating point on the SH-4.** Self-test done (Flycast); the game's own
   results match the desktop so far. Not yet on a console.
3. **The game runs** (no speed): the attract mode runs, in lockstep with the
   desktop (above).
4. **PowerVR2 renderer.** 3D through the PVR (fixed function, no shaders);
   tilemaps drawn on the CPU, uploaded as textures, as before the desktop's
   tile shaders. Done: polygons (textured: luma as grey palettes, colour and
   light per vertex, an approximation), both tile layers.
5. **Controls (pad, racing controller), VMU saves, 57.52 Hz on 60 Hz.**
   Controls done (`game/controls.h`); saves and pacing not yet.
6. **Sound.** The native sequencer; samples in the 2 MB sound RAM (AICA
   ADPCM) or music from CD audio, decided by the Phase 0 numbers.
7. **Speed.**

Memory plan to test: music on CD audio tracks (no RAM), effects in sound
RAM as AICA ADPCM, textures in video RAM, and in main RAM only the code and
the ROM a course reads, loaded at course select. The drive cannot play CD
audio and read data at once, so nothing can stream from disc during a race.

## Runtime changes: M2_DC_MEMORY

All in `src/runtime`, all inside `#ifdef M2_DC_MEMORY` (only the Dreamcast
Makefile and `tools/dcmemcheck` define it):

- `rom_source.h` (new): `RomSource`, the ROM regions a 4 KB page at a time
  from the frontend's cache; `page_fast` remembers each region's last page
  (the source calls `forget()` when it loads one, which may evict).
- `m2_board`: `Images` gains `rom`, `texture_ram`, `frame_buffer_ram`; ROM
  pages map to the source (`map_rom`); texture RAM (4 MB) and frame buffer
  RAM (1 MB) are the frontend's (video RAM); a two-level page table (1 MB
  chunks of 256 pages, allocated as mapped) instead of 1M entries (12 MB
  with the extra fields); the duplicate `copro_tables` image is released.
- `geo`: a constructor taking the source (it also reserves room for 2,400
  kept polygons: the desktop's maximum in attract and in a whole race is
  2,182; growing past 2,048 needed 800 KB while the old 400 KB existed);
  `GeoPtr`/`GeoPtr16` read polygon and texture ROM through it; `geo_test`
  (the ROM self-test) only moves the cursor on (below).
- `m2_tgp_board`: a constructor taking the source; copro data read through it.
- `video`: no GPU-layer copies (1.5 MB): in external 3D the layers are the
  screen and sys24 buffers themselves (`background_layer`/`foreground_layer`),
  and they are composed (the Vita's early return, for its own tile textures,
  is not taken).
- `m2_board`: frame buffer RAM optional (never written by the game in 9,000
  attract frames or a whole race; without it the range is unmapped).
- `raster.cpp`: its buffers (1.25 MB) only on the first CPU render.
- `lockstep`: called callbacks' slots are reused (below).

## Runtime changes for speed: M2_DC_SPIN_SKIP, M2_DC_SPEED

Also only in the Dreamcast Makefile and `tools/dcmemcheck`:

- **M2_DC_SPIN_SKIP** (`m2_board.cpp`, `read_byte`): the game's wait for the
  next frame, `0x1394: ldob 0x500000,r3` / `0x139c: cmpibe r3,g0,0x1394`,
  is 76% of the i960's instructions in a race (`tools/ipprof`, frames
  2700-6000). Every frame runs to the probe's instruction caps (110,592 +
  40,960 = 151,552 a frame) because `in_idle_loop` lists only 0x12b0 and
  0x12f0; that timing is the game's on the desktop too, so it stays. When the
  read at 0x1394 will compare equal, the passes up to just before the next
  lockstep event are skipped: the count moves on by whole passes, nothing
  else changes (the byte is RAM, only an interrupt or callback changes it).
  race_basic: 715,083,926 of 909,312,001 instructions skipped; dcmemcheck
  matches the desktop at all 100 checkpoints.
- **M2_DC_SPEED**, the same output, faster:
  - `video.cpp`: `tilemap_draw` a row at a time in runs that do not wrap;
    `build_layer` counts, per layer and row of tiles, the tiles of each
    category and those with an opaque pixel, and `tilemap_draw`/`draw_rect`
    skip rows where nothing can match; the back layers are drawn straight
    over pen 0 (as the Vita's path) instead of into a cleared `sys24_`
    copied over it.
  - `geo.cpp`, `m2_board.cpp`: with a frame skip, object data is not parsed
    for frames whose polygons are never shown (neither that frame nor, in
    30 Hz mode, the next is drawn): the rasterizer gets the command's
    opening and closing words, every other command runs, so the
    geometrizer's state is unchanged.
  - `m2_board.h`: `set_frame_skip` allows 3. The desktop's range is 0-2, and
    the frontend's `set_frame_skip(3)` had been clamped to 2: the layers
    were composed every 3rd frame and drawn every 4th, up to two frames old.

  Checked with dcmemcheck (`-DDC_SPEED=OFF` builds the reference): with and
  without M2_DC_SPEED at frame skip 2, race_basic (6,000 frames) and
  attract_long (9,000) give identical lines every frame (instruction counts,
  display list, tile-layer hash, and the drawn frames' polygon hash); at frame
  skip 3 the 1,500 + 2,250 drawn frames are identical to a frame skip 0 run.

Desktop check after the changes (build-daytona m2run, race_basic, 6,000
frames, single-cabinet settings): screen hash `9427a612c5cb7511`,
909,312,001 i960 and 195,261,176 TGP instructions, the same as before them.

## For the desktop too (not changed there: the user's decision)

- **`Lockstep::calls_` never shrinks.** `GameLoop::probe` adds a callback
  every 1,024 i960 instructions (about 150 a frame) and every callback stays
  in `calls_` for good. On the Dreamcast (16 bytes per `std::function`) that
  was 1 MB by frame 220; on the desktop (32 bytes) it is about 270 KB a
  second, roughly 1 GB an hour (estimated from those numbers, not measured).
  The M2_DC_MEMORY fix (reuse a slot once its callback has run) is
  behaviour-neutral (dcmemcheck matches the desktop at 60 checkpoints) and
  could be made unconditional.

## Found on the way (and what was wrong)

- **iostreams stop KOS before `main`.** Reading the recorded input script
  with `tools::Script::load` (an `std::ifstream`) linked libstdc++'s
  iostreams (+263 KB of code) and their start-up, and the program stopped
  during KOS's start-up ("SH4 exception when blocked" in Flycast), at
  different points as the layout changed. Two theories were wrong and were
  tested and dropped: the disc layout (an extra file on the disc in `rom/` or
  the root) and the binary's size (padding 1ST_READ to whole sectors). With an
  empty script the compiler had dropped the `load` call (`kInputs[0]` is a
  constant), which is why that build booted. The frontend now parses the
  script itself (`parse_script`) and uses `Script::at` as it is. Do not use
  iostreams in the Dreamcast build.

- **The geometrizer's ROM self-test (`geo_test`) and the compiler.** Its sums
  have no effect (the LEDs are not emulated), so the desktop's compiler
  removes the loop: it costs nothing there and romuse never saw it. Through
  `RomSource` the reads cannot be removed. In the attract mode (frame 189) the
  display list asks for a huge number of blocks: first the Dreamcast read
  megabytes of polygon ROM from the disc (it looked like cache thrashing),
  then, with only the inner loop skipped, it stepped through billions of
  words. M2_DC_MEMORY now moves the cursor on 3 * blocks words, the result
  the desktop's compiled code has. Found with `dcmemcheck` (the same hang on
  the PC) and cdb's stack at a runaway-read throw; the Dreamcast's watchdog
  reads the game thread's saved PC for addr2line.
- **Wrong guesses on the way there**, tested and dropped: read-ahead filling
  the cache (it reads ahead only on sequential misses now, a fine change but
  not the cause), the TGP's floating point (its counts and display list match
  the desktop to frame 188 and beyond), vertex buffer and tile bin sizes.
- **`vbuf_doublebuf_disabled = 1`** stopped Flycast ("SH4 exception when
  blocked"); double-buffered vertex buffers work.
- **"SH4 exception when blocked" was usually KOS aborting** (out of memory)
  with the message lost when Flycast stops; the runner now keeps every
  console text it reads (`dreamcast/flycast/serial.txt`), stdout is
  unbuffered, and large `operator new` calls are logged with their caller.
- **A stale object, not a runtime bug.** After `Images` gained a member,
  the screen vector came out empty (`0 px`) and the run stopped. The cause:
  `game_loop.o` was not rebuilt (still the old `Images` layout), so the
  frame buffer pointer was garbage and the game's frame buffer writes went
  into the heap. DreamSDK's compiler writes `C:/...` paths in `-MMD` files,
  which MSYS make cannot use (the colon), so header changes rebuilt nothing.
  The Makefile now rewrites them to `/c/...` (`FIXDEP`). Two guesses before
  that were wrong and were tested and dropped: the custom `pvr_init`
  parameters, and frame buffer RAM in video RAM (neither mattered).
- **The stack**, also suspected then, was not the cause either, but the game
  runs on a 512 KB-stack thread anyway (KOS's main thread has 64 KB); the
  main thread is a watchdog printing the frame, free RAM, ROM misses, what
  the game thread is doing and its PC every 5 s.
- **Blurred picture in Flycast, the BIOS too**: Flycast was started
  minimised; Direct3D then renders at the minimised window's size. It is
  started normally now, without taking the focus.
- **Leftover Flycast windows**: after a crash the windows outlived the
  process the runner started; it now closes every Flycast running from the
  build directory's copy (by path) before and after each run.
- KOS's `kos.h` defines `BIT(n)`; the runtime has `BIT(x, n)`: the frontend
  includes the runtime's headers first.
- **Serial lines cut in two**: the game thread and the watchdog (and the
  `--sample` thread) printing at once mixed their characters, once inside a
  TRACE line (the checkpoint was right; the comparison failed). The
  frontend's prints go through `say()`, one line at a time under a mutex
  (timed, so a stuck game thread cannot silence the watchdog); the sampler's
  reports are printed by the game thread between frames.
- **Inlining `Lockstep::boundary` is out**: forced inline, one generated
  chunk grew from 97 KB to 191 KB (measured): the generated code would not
  fit. It stays a call per i960 instruction.
- Out of memory before the game ran: the ROM cache moved to video RAM and
  back to main RAM (1.25 MB) once the CPU rasterizer's buffers went; frame
  buffer RAM is in video RAM; the GPU layers are not allocated.

## Measured so far

### ROM read by the game (Revision A, `romuse`, desktop)

MB of each ROM region read (4 KB pages; a page counts once), from the
single-cabinet settings saved by `daytona.exe` (`--nvram`). `romuse` leaves
the game unchanged: race_basic's last screen hash is m2run's
(`9427a612c5cb7511`).

| Run | main_data | polygons | textures | copro | program | pcm1+2 | total |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Power-on, frames 0-1200 | 2.82 | 0.65 | 0.42 | 0.07 | 0.16 | 0.83 | 4.95 |
| Full Beginner race, frames 2400-20000 (race_to_end) | 1.67 | 1.08 | 0.68 | 0.09 | 0.11 | 2.70 | 6.34 |
| Attract demo, frames 1200-9000 (attract_long) | 2.92 | 3.41 | 2.00 | 0.53 | 0.10 | 0.91 | 9.86 |
| Union of all six runs | 5.79 | 3.88 | 2.25 | 0.55 | 0.18 | 3.42 | 16.08 |
| Union from frame 1200 (no power-on) | 4.25 | 3.88 | 2.25 | 0.55 | 0.12 | 3.42 | 14.48 |

- Power-on reads 2.8 MB of main_data in its first 10 s (most likely the
  ROM check); after it, a Beginner race reads 1.7 MB of it.
- **Only the Beginner course is measured as a race.** On Revision A's
  Circuit Select the cursor follows the wheel's position, so
  `scripts/inputs/course_advanced.txt` and `course_expert.txt` (written for
  daytona93) both race Beginner (screens show LAP 1/8).
  `scripts/inputs/reva_long.txt` here selects Long (LAP 1/2), but the fixed
  steering stops the car against the side, so it reads only the start area;
  the Medium and Long courses need scripts that drive them. The attract demo
  shows other courses, so its 3.4 MB of polygons is the best figure so far
  for all courses together.
- Samples: a full race reads 2.7 MB of PCM (music included); every run
  together 3.4 MB.

### Denormals (`ftzcheck`, desktop)

Identical with SSE flush-to-zero and denormals-are-zero (the SH-4's DN
treatment), every frame: race_basic 6,000 frames, course_expert 6,000,
race_to_end 20,000 (3,031,040,001 i960 and 647,890,832 TGP instructions,
screen hashes all equal). The game does not depend on a denormal in these.

### Sound commands (race_basic, `--sound-log`)

All on MIDI channel 14: 37 "start sequence" (0xAn), 284 note-ons, 882
controller changes. Bank 16 is started three times (frames 185, 1212, 2579:
attract, coin-up, race start), which looks like the music; bank 33 is started
often (index 20 about every 2 s in the race), which looks like effects and
voices. Not yet confirmed which bank is which (next: run each sequence through
the native sequencer and measure how long it plays).

### SH-4 code size, Revision A (`build_dreamcast.py compile`)

| | size |
| --- | --- |
| Generated code: i960, TGP, 68000 (`-Os`) | 2.96 MB |
| Runtime, i960 lib, trace, ymfm, SoftFloat (`-O2`) | 0.41 MB |
| Total, before KOS and libstdc++ | 3.37 MB |

Compiled with no errors (9 warnings, all in the shared `geo.cpp`), 2 min 17 s
with 6 jobs. Not linked yet.

### SH-4 code size, daytona93 (compile only, GCC 13.2, KOS flags, `-m4-single`)

| | -O2 | -Os |
| --- | --- | --- |
| Generated i960 code (23 chunks + table) | 6.69 MB | 2.47 MB |
| TGP generated | 0.15 MB | |
| Sound 68000 generated | 0.42 MB | |
| Runtime + i960 lib + ymfm | 0.29 MB | |
| Total | 7.54 MB | about 3.3 MB (generated i960 at -Os) |

No large static arrays (largest `.bss` 17 KB). The generated code compiled
in 517 s at -O2 and 71 s at -Os (8 jobs). **Measured on `daytona93`'s
generated code, before the port settled on Revision A: to be measured again
on `daytona`.**

Only two shared files failed, both for `int32_t` being `long` (see the
README); the `__INT32_TYPE__` override compiles them with no errors or
warnings.

### Floating point (Flycast 2.7, self-test, booted from a CDI)

`double` is 8 bytes with `-m4-single`; FPSCR is `00040000` at start (DN
set). Divide, multiply, add and square root in `double` and `float` match
the PC's bits; `snprintf` of a `double` gives the PC's text. Denormal
results are flushed to zero (`f denormal`, `d denormal`), as the SH-4 does
with DN set; the self-test reports that and does not fail on it. Whether the
game ever depends on a denormal is what `ftzcheck` measures. Not yet run on
a console.

### Display path (Flycast 2.7, videotest)

A 496x384 ARGB8888 frame (as `Video::screen()`) converted to RGB565 and
uploaded to a 512x512 non-twiddled PVR texture each frame, drawn at 620x480
centred (10-pixel bars), bilinear. Per frame, averaged over 120 frames:
conversion 15.3 ms, upload (`pvr_txr_load`) 1.0 ms. Flycast's SH-4 timing is
not the console's, but at that cost the frontend cannot convert the whole
screen each frame (a frame is 17.4 ms): tile layers should be drawn into the
PVR's 16-bit format, or only changed areas converted, as the Vita's tile
upload does. The controller in port A0 is read.

### KallistiOS: 2.2.1 in extern/kos-dc, not DreamSDK's installed KOS

DreamSDK R4 had installed KOS git master (`d458073c`, 2026-10-01) next to
its prebuilt toolchain (GCC 13.2.0, libraries dated 2025-07-05). With that
KOS, every C++ program that pulls in libstdc++'s exception support stops
before `main` prints anything: `Assertion "m->holder == thd && m->count > 0"
failed at mutex.c:184 in mutex_unlock`, from newlib's stdout buffer flush.
Bisected in Flycast: `printf` alone works, `malloc` and `new int[]` work,
one `std::vector` fails (it links libstdc++'s exception-support static
constructors: eh globals, emergency pool, terminate handler). KOS's own
`examples/dreamcast/cpp/concurrency` fails the same way, so it is not this
project's code. Same with `-m4-single-only`, so not the float ABI. Not
proven why; KOS master has since dropped newlib's `_pthreads.h` and changed
its mutexes, which libstdc++'s thread layer was built against.

Fix: KOS 2.2.1 (`857e4e69`, 2025-08-30), the version DreamSDK R4's
toolchains were packaged with, unpacked from DreamSDK's own offline package
into `extern/kos-dc` (git-ignored) and built with `-m4-single`
(`build_dreamcast.py kos`, about a minute). With it the same `std::vector`
program, `videotest` and the self-test all run. DreamSDK's own install is
not changed.

### Toolchain findings

- KOS's `environ_dreamcast.sh` tests for `-m4-single` by compiling to
  `/dev/null` with the Windows-native compiler, which cannot write there; the
  test fails and it falls back to `-m4-single-only` (a 32-bit `double`) with
  a "toolchain does not support m4-single" warning. The toolchain does
  support it (multilibs `m4-single` and `m4-single-only`). DreamSDK's shell
  shows that fallback; the driver's own environment sets `-m4-single`.
- DreamSDK's `makeip` is a MinGW program; its libpng and zlib DLLs are in
  `/mingw64/bin`, which is not on the shell's PATH (exit 127 otherwise).
- KOS's top-level `make` also builds its PC-side helpers with a PC compiler
  the DreamSDK shell does not have; the driver copies DreamSDK's built ones
  and builds only the kernel and addons.
- Flycast prints a program's serial output in a console window of its own,
  which redirecting stdout does not capture; `flycast_run.py` reads the
  console buffer.

## What not to re-propose

- Measurement hooks in `src/runtime`: not needed. `romuse` watches the ROM
  buffers from outside with guard pages, `ftzcheck` sets MXCSR.
- Building the KOS side through `bash -c` from Git Bash (hangs).
- iostreams (`<fstream>`, `<sstream>`, `<iostream>`) in anything the
  Dreamcast build links: their start-up stops KOS before `main`.
- DreamSDK's installed KOS master with its R4 toolchain (C++ programs stop at
  startup; see above). Use `extern/kos-dc`.
