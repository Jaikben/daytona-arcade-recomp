# Mobile port handoff

## 2026-10-06: unsigned iOS package

Built the mobile branch on macOS with Xcode 26.6 and the iPhoneOS 26.5 SDK,
using the existing generated daytona93, TGP and sound sources. Added
`UNSIGNED=1` packaging to the iOS script and the required executable/package
keys to the bundle template. The final script was rerun successfully with
relative `BUILD_DIR=build/ios` and `M2_GEN_ROOT=generated` paths. macOS resource
forks and extended attributes are excluded from the IPA.

Validation: Xcode Release build succeeded; plist lint passed; the executable
is arm64 Mach-O with iOS platform 2 and minimum OS 15.0. Bundle identifier is
`com.boucydesigns.daytona`, executable is `daytona`, device families are iPhone
and iPad. Linked dynamic libraries are Apple system libraries only. codesign
reports the app is not signed. ZIP integrity passed and the IPA contains only
Payload/daytona.app with its executable, Info.plist and PkgInfo, no ROM archive.
The retrieved IPA hash matched the Mac copy:
`417835b73b9f0e188f21a7115958b08c81207a387646d5e78b7d74b18e8b5f42`.
Local artifact and logs are under ignored `build/ios/`.

Next: sign/install through AltStore, supply the user's ROM archive, and test
launcher import, Metal rendering, audio and controller input on an actual iOS
device. Compilation/package validation does not establish on-device gameplay
or AltStore installation success. On-screen driving controls are not added.

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
