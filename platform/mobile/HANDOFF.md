# Mobile port handoff

## 2026-10-02: Android document read/import

Reported failure: Browse returned a Downloads provider `content://` URI,
then the launcher reported `cannot open content://...`. `archive.cpp` used
`std::ifstream` on the URI. This was a missing Android document-reader path,
not evidence of a corrupt ZIP. The earlier assumption that the unmodified
desktop ROM reader would work with the SDL Android picker was wrong.

Added `app::RomFile` in `src/app/rom_file.h`: SDL opens the granted URI with
mode `rb`, streams it to a bounded app-private temporary file, and the launcher
runs its existing manifest checks on that file. Only a successful check is
committed to the persistent import and saved configuration. Failed copying,
validation and rename leave the previous import in place. Ordinary filesystem
paths still go straight to the runtime; iOS behaviour is unchanged.

Design basis: `docs/daytona-usa-recomp-design.md`, Architecture (thin host
platform layer) and Overview/goals (user-supplied ROMs). The runtime, generated
code, archive validation rules, shaders and Android SDK/Gradle versions were
not changed. No ROM bytes or generated game sources are committed.

Validation: 13 Android-path helper cases and 2 desktop-path cases pass with
both GCC and Clang using the narrow host SDL shim. The original launcher
source was checked against blob `ed26528e7bd7af206a1a478137cc170a4712e217`
before editing. No Android SDK/device or ROM set was available here; neither
an APK build nor on-device import/gameplay is claimed.

Next: rebuild/install with `adb install -r`, Browse to the archive again,
confirm all files verify, start the game, then close/reopen and reset to check
that the private copy is reused. See FILE_ACCESS.md for commands and limits.

Do not re-propose decoding the Downloads URI into a raw `/sdcard` path or
adding all-files access as the fix for `ifstream(content://...)`. The system
picker supplies document access, and SDL's Android IO is the matching reader.
