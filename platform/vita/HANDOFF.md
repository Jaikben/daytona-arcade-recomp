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
