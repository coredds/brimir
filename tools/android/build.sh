#!/bin/bash
# Builds the libretro core for Android with the NDK (tools/android/README.md).
#
# usage: tools/android/build.sh <arm64-v8a|x86_64> [--tests]
# env:   ANDROID_NDK_HOME   the NDK (default: $ANDROID_SDK_ROOT/ndk/<tools/android/ndk-version.txt>)
#
# Configures build-android-<abi> (Ninja, Release, LTO, API 26, static C++ runtime), builds brimir_libretro (with --tests
# also brimir_tests and brimir_bench) and prints the core's path on the last line.
set -euo pipefail

usage() {
    echo "usage: $0 <arm64-v8a|x86_64> [--tests]" >&2
    exit 2
}

ABI=${1:-}
case "$ABI" in
    arm64-v8a|x86_64) ;;
    *) usage ;;
esac
TESTS=0
if [ $# -ge 2 ]; then
    [ "$2" = "--tests" ] || usage
    TESTS=1
fi
[ $# -le 2 ] || usage

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

NDK_VERSION=$(tr -d '[:space:]' < tools/android/ndk-version.txt)
NDK=${ANDROID_NDK_HOME:-}
if [ -z "$NDK" ] && [ -n "${ANDROID_SDK_ROOT:-}" ]; then
    NDK="$ANDROID_SDK_ROOT/ndk/$NDK_VERSION"
fi
if [ -z "$NDK" ]; then
    echo "error: no Android NDK: set ANDROID_NDK_HOME (or ANDROID_SDK_ROOT with ndk/$NDK_VERSION installed);" \
         "see tools/android/README.md" >&2
    exit 1
fi
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
if [ ! -f "$TOOLCHAIN" ]; then
    echo "error: $NDK is not an Android NDK (no build/cmake/android.toolchain.cmake); see tools/android/README.md" >&2
    exit 1
fi
FOUND_VERSION=$(sed -n 's/^Pkg.Revision *= *//p' "$NDK/source.properties" 2>/dev/null | tr -d '[:space:]')
if [ "$FOUND_VERSION" != "$NDK_VERSION" ]; then
    echo "warning: NDK at $NDK is version '${FOUND_VERSION:-unknown}', pinned is $NDK_VERSION" >&2
fi

BUILD_DIR="build-android-$ABI"
CONFIG_ARGS=(
    -S . -B "$BUILD_DIR" -G Ninja
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
    -DANDROID_ABI="$ABI"
    -DANDROID_PLATFORM=android-26
    -DANDROID_STL=c++_static
    -DCMAKE_BUILD_TYPE=Release
    -DBRIMIR_LTO=ON
)
TARGETS=(brimir_libretro)
if [ "$TESTS" = 1 ]; then
    CONFIG_ARGS+=(-DBRIMIR_BUILD_TESTS=ON)
    TARGETS+=(brimir_tests brimir_bench)
fi

cmake "${CONFIG_ARGS[@]}"
cmake --build "$BUILD_DIR" --target "${TARGETS[@]}"

CORE="$BUILD_DIR/lib/brimir_libretro_android.so"
if [ ! -f "$CORE" ]; then
    echo "error: $CORE was not built" >&2
    exit 1
fi
echo "$CORE"
