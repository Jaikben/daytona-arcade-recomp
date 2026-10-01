# Vita target dependency record

The root [THIRD_PARTY.md](../../THIRD_PARTY.md) remains authoritative for
SoftFloat, ymfm and the shared runtime. This target uses those same pinned
sources fetched by `scripts/setup.py`, without copying or modifying them.
No additional third-party source is vendored by the port.

## SDL2

Upstream: https://github.com/libsdl-org/SDL (zlib license).
The VitaSDK package recipe inspected uses SDL 2.32.8, upstream tag
`release-2.32.8`, with source archive SHA-256:
`0ca83e9c9b31e18288c7ec811108e58bac1f1bb5ec6577ad386830eac51c787e`.
Recipe: https://github.com/vitasdk/packages/blob/master/sdl2/VITABUILD
(inspected recipe blob `58e053c7c4846f691bc731c392adb061c52dfefe`).

SDL2 is linked from the user's VitaSDK installation. The port does not
fetch a moving SDL branch, vendor its implementation, use PVR/PIB, or change
SDL. Headers and the linked package must come from the same SDK install.
Reference port documentation: https://github.com/libsdl-org/SDL/blob/SDL2/docs/README-vita.md

## VitaSDK

Installation and release documentation: https://vitasdk.org/
Toolchain source: https://github.com/vitasdk/vita-toolchain
SDK headers: https://github.com/vitasdk/vita-headers

The workflow pins the documented SDK image `vitasdk/vitasdk:2026.08-20260815`,
not `latest`. The SDK is a collection of separately licensed components;
retain its corresponding license files when redistributing components.
The SDK toolchain's `vita.toolchain.cmake` was inspected at blob
`1d8f2b3f0058d2be4626165439ba8774b0d38200`; its installed version is supplied
by the SDK rather than copied into this repository. CMake SELF/VPK helpers
were inspected at blob `cd1162d0845dcd1a2c98690225369a90424149d0`.

The application calls the public homebrew SceCtrl/SceIofilemgr APIs through
SDK import stubs. No proprietary Sony SDK, firmware binary, font, or shader
is added. The small menu glyphs and portable SoftFloat configuration are
project-owned code. The specialization and floating-point policy are
unchanged from the shared build.
