# Vita port handoff — 29 September 2026

Baseline inspected: the root build, README, `rules.md`, the design document's
Architecture and Floating point sections, the app's frame/audio/input code,
and the runtime's GameLoop, board, ROM importer and generated-code interface.
The root HANDOFF remains the record for the shared runtime; this file records
the isolated Vita target.

## Implemented

A separate VitaSDK cross-build consumes the existing host-generated i960,
TGP and 68000 C++ and builds the shared runtime, not refcore. SDL2 displays
the existing framebuffer and mixes the two native-rate sound outputs. Native
Vita controls, a text menu, cabinet switch pulses, error reporting, bounded
frame/audio queues, EEPROM/backup persistence and VPK packaging are included.
The desktop source and build are unchanged.

Architecture follows the design's thin host layer and native board frame
loop. No per-platform game shader, replacement simulation, interpreter,
fast-math setting, ROM content or generated game code was introduced.

## Validation performed locally

- Input/timing C++ test passed with AddressSanitizer and UndefinedBehaviorSanitizer:
  all 256 steering values, all 4,096 digital combinations, pedal endpoints,
  active-low switches, gear edge/clamp behavior, menu-chord suppression, and
  100 seconds of 60 Hz host ticks yielding 5,752 board frames.
- Eight Python build-driver tests passed: missing SDK/generated files,
  conflicting directories, invalid jobs, custom paths containing spaces,
  compile-check behavior and required final package detection.
- Python syntax compilation passed.

The local environment has neither VitaSDK nor the user's ROM-generated C++.
No full Vita compilation, final ARM link, VPK installation, device run,
performance measurement or MAME race comparison is claimed by these tests.
The ROM-free CI job is a compile check, not evidence of a runnable port.

## Findings and cautions

The desktop SoftFloat `platform.h` enables intrinsic 128-bit integers, which
cannot be assumed on ARMv7. The Vita header deliberately omits that setting
and retains the existing by-value extF80 ABI and specialization. SoftFloat
state is not TLS here because board execution is exclusively main-threaded.

VitaSDK's inspected toolchain declares `CMAKE_SYSTEM_NAME=Generic` and sets
`VITA=True`; testing for the system name `Vita` was an incorrect initial
assumption and was corrected before delivery. `include()` fails on missing
files by default and does not accept a REQUIRED argument.

The renderer is a presentation backend only: the shared CPU rasterizer and
ROM importer's substantial memory use have not been optimized. A 256 MiB
newlib heap is requested; peak use, executable size and OS headroom remain
unmeasured. 7z is excluded on the handheld; ZIP retains the shared importer
and its CRC checks. The root host pipeline still accepts 7z.

## Next, in order

1. Run the SDK compile check and fix any concrete ARM/compiler errors.
2. Generate code from the user's exact daytona93 set and perform the full ARM
   link/VPK build; keep all derived output outside Git.
3. Test on Vita and record log, peak memory and frame times. Verify controls,
   sound mixing, save recovery and system suspend/resume.
4. Run the existing parity/replay validation on the target where practical.
   Optimize only measured bottlenecks without changing board semantics.

## Startup crash follow-up — 30 September 2026

An uploaded device dump from the initial frontend records main-thread stop
reason 0x30004 at text-relative PC 0x4122be, SP 0x81681000, and a 256 KiB stack
starting at that same address. Stack words repeat in 128-byte blocks. The
kernel log contains a failed ScePhyMemPartGame allocation (request 0x01000000,
reported remaining 0x00dce000); the allocation list contains no Newlib heap.
No original executable was supplied, so exact function names and the full
causal chain are not confirmed. The dump and its memory contents stay local.

The original 256 MiB up-front newlib heap request is the leading suspect for
this startup failure. The candidate correction requests 192 MiB, checks a
small allocation before stdio, and stops using native I/O/process exit on
heap or log-open failure. In particular, a failed freopen must not be followed
by writing to the closed stderr stream. No stack-size increase is used to
hide the repeating frames, and no game/ROM behavior is changed.

The patch must still be rebuilt and tested on the same Vita. A successful
startup does not establish sufficient peak memory for ROM import or a full
race. Preserve the original unstripped build/vita/daytona_vita before rebuilding
so its PC offsets can be symbolized. Do not upload dumps, ROMs or generated
executables to the public repository.


## Slow runtime follow-up — 30 September 2026

The user now reports that it runs, but at approximately 1 FPS. This is a user
observation, not a profiler measurement; the exact active build and the ratio
of simulation frames to displayed frames are not established yet. Do not
infer a measured rasterizer bottleneck solely from that report.

The initial Vita loop can compute and software-render up to four consecutive
board frames before showing only the last one. The candidate patch selects
one step per presentation, preserving fractional time and discarding overdue
wall-clock debt, not board instructions. This avoids hiding completed frames;
it does not establish improved simulation throughput or full speed.

The shared GameLoop now has optional, caller-clocked host profiling around
vblank_start (geometry), vblank_end (software video), sound and the whole
frame. The remaining core time includes i960, synchronous TGP and scheduling.
The desktop frontend does not install a profile clock. This follows the
design's Architecture separation: host timestamps only measure, never drive
board state. The original CPU/TGP/sound code and raster arithmetic are unchanged.

Vita logs separate simulation/presentation rates, board-stage averages, audio
conversion/queueing, upload, draw, present and autosave durations. Clock speeds
are read but not changed. Present timing includes driver flushing/vsync and
must not be described as a hardware GPU timestamp. Text pixels use bounded
SDL rectangle batches instead of per-pixel calls; this affects menu submission,
not Model 2 polygon rasterization. The 192 MiB startup correction is retained.

Validation of this candidate on the host: existing input/timing suite and
all eight build-driver tests pass. New profiler/pacing tests and text pixel/
layout tests pass under address/undefined-behavior sanitizers. The one-frame
clock produces the same 5,752 frames in 100 seconds of simulated 60 Hz ticks;
a stress case submits 9,216 font rectangles in 36 batches. These are unit-test
results, not Vita FPS measurements. The updated workflow includes both tests,
but has not been run remotely for this candidate.

Next: rebuild with VitaSDK, collect a MENU and a GAME perf sample from the
same Vita before relaunching (the log is overwritten), identify the dominant
stage, then optimize it and compare behavior. No new ARM build, full link,
hardware result, ROM generation or race parity is claimed for this patch.
