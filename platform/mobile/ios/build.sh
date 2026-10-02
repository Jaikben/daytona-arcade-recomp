#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build/ios}"
GEN="${M2_GEN_ROOT:-$ROOT/build/gen}"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "iOS builds require macOS with Xcode." >&2
    exit 1
fi
if [[ ! -f "$GEN/daytona93/gen_table.cpp" ]]; then
    echo "Missing generated game sources in $GEN." >&2
    echo "Run the normal host recompile first, then rerun this script." >&2
    exit 1
fi

python3 "$ROOT/scripts/setup.py" --no-build

cmake -S "$ROOT" -B "$BUILD" -G Xcode     -DCMAKE_SYSTEM_NAME=iOS     -DCMAKE_OSX_ARCHITECTURES=arm64     -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0     -DM2_GEN_ROOT="$GEN"

cmake --build "$BUILD" --config Release --target daytona
echo
echo "Built: $BUILD/Release-iphoneos/daytona.app"
echo "Open $BUILD/daytona_recomp.xcodeproj to select your signing team and deploy."
