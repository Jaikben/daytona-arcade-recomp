# Android / iOS port

The `mobile` branch keeps the desktop runtime and SDL3/SDL_GPU renderer, and packages the
same recompiled game for Android and iOS.

## Prerequisite: generate the game code on a desktop host

ROM-derived C++ is never committed. Build/recompile once on the host so these exist:

- `build/gen/daytona93/*.cpp`
- `build/gen/daytona93_tgp/tgp_gen.cpp`
- `build/gen/daytona93_snd/snd_gen.cpp`

Normally:

```sh
./setup.sh
python3 scripts/recompile.py
```

The mobile builds read that directory through `M2_GEN_ROOT`.

## Android

Requirements: JDK 17, Android SDK API 37, NDK 28.2.13676358, CMake 3.31.6, and
Gradle compatible with Android Gradle Plugin 9.4.1.

```sh
python3 scripts/setup.py --no-build
cd platform/mobile/android
gradle :app:assembleDebug
```

The APK is under `app/build/outputs/apk/debug/`.

The Android package uses SDL3's `SDLActivity`, builds SDL as `libSDL3.so`, and loads the
game CMake target as `libmain.so`. It is arm64-only for now. SDL handles Bluetooth/USB
gamepads and the SDL file dialog is used by the existing launcher for selecting a user
provided ZIP/7z ROM set.

## iOS

Requirements: macOS, Xcode, CMake, and an Apple signing identity for installation on a
physical device.

```sh
platform/mobile/ios/build.sh
```

That generates an Xcode iOS project and builds `daytona.app`. Open the generated
`daytona_recomp.xcodeproj`, select your Team under Signing, then deploy to the device.
The bundle opts into Files document sharing/open-in-place so the user's ROM archive can
be made available to the launcher.

## Current input

SDL3 gamepads work on both platforms using the same bindings as desktop. The launcher is
touch/mouse compatible through SDL/ImGui. In-race on-screen touch driving controls are
not emulated yet; a physical or paired controller is currently the intended mobile input.
