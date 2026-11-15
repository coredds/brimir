#!/bin/bash
# Builds the libretro core for Android with the NDK (tools/android/README.md).
#
# usage: tools/android/build.sh <arm64-v8a|x86_64> [--tests]
# env:   ANDROID_NDK_HOME   the NDK, used when it is the version in tools/android/ndk-version.txt
#        ANDROID_SDK_ROOT or ANDROID_HOME   else the SDK's ndk/<pinned version> is used
#        BRIMIR_ALLOW_NDK_MISMATCH=1   accept another NDK version in ANDROID_NDK_HOME (never for releases)
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
SDK=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}
PIN_NDK=${SDK:+$SDK/ndk/$NDK_VERSION}

# Pkg.Revision of the NDK at $1, empty when it has none.
ndk_version() {
    sed -n 's/^Pkg.Revision *= *//p' "$1/source.properties" 2>/dev/null | tr -d '[:space:]' || true
}

# The NDK must be the pinned version: ANDROID_NDK_HOME when it is, else the SDK's ndk/<pin>. BRIMIR_ALLOW_NDK_MISMATCH=1
# accepts another version in ANDROID_NDK_HOME (local experiments only, never for a release build).
NDK=""
NDK_SOURCE=""
if [ -n "${ANDROID_NDK_HOME:-}" ] && [ "$(ndk_version "$ANDROID_NDK_HOME")" = "$NDK_VERSION" ]; then
    NDK=$ANDROID_NDK_HOME
    NDK_SOURCE="ANDROID_NDK_HOME"
elif [ -n "$PIN_NDK" ] && [ "$(ndk_version "$PIN_NDK")" = "$NDK_VERSION" ]; then
    NDK=$PIN_NDK
    NDK_SOURCE="derived from the pin: \$ANDROID_SDK_ROOT (or \$ANDROID_HOME)/ndk/$NDK_VERSION"
    if [ -n "${ANDROID_NDK_HOME:-}" ]; then
        echo "note: ANDROID_NDK_HOME ($ANDROID_NDK_HOME) is version '$(ndk_version "$ANDROID_NDK_HOME")'," \
             "not the pinned $NDK_VERSION; using $NDK" >&2
    fi
elif [ -n "${ANDROID_NDK_HOME:-}" ] && [ "${BRIMIR_ALLOW_NDK_MISMATCH:-0}" = 1 ]; then
    NDK=$ANDROID_NDK_HOME
    NDK_SOURCE="ANDROID_NDK_HOME"
    found=$(ndk_version "$NDK")
    echo "warning: BRIMIR_ALLOW_NDK_MISMATCH=1: using NDK $NDK, version '${found:-unknown}'," \
         "pinned is $NDK_VERSION" >&2
else
    echo "error: no Android NDK $NDK_VERSION (tools/android/ndk-version.txt); see tools/android/README.md" >&2
    if [ -n "${ANDROID_NDK_HOME:-}" ]; then
        found=$(ndk_version "$ANDROID_NDK_HOME")
        echo "  ANDROID_NDK_HOME=$ANDROID_NDK_HOME is version '${found:-unknown}'" \
             "(set BRIMIR_ALLOW_NDK_MISMATCH=1 to use it anyway)" >&2
    else
        echo "  ANDROID_NDK_HOME is not set" >&2
    fi
    if [ -n "$PIN_NDK" ]; then
        found=$(ndk_version "$PIN_NDK")
        [ -n "$found" ] && found="version $found" || found="missing or not an NDK"
        echo "  $PIN_NDK (derived from the pin under \$ANDROID_SDK_ROOT or \$ANDROID_HOME): $found" >&2
    else
        echo "  neither ANDROID_SDK_ROOT nor ANDROID_HOME is set" >&2
    fi
    exit 1
fi
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
if [ ! -f "$TOOLCHAIN" ]; then
    echo "error: $NDK ($NDK_SOURCE) is not an Android NDK (no build/cmake/android.toolchain.cmake);" \
         "see tools/android/README.md" >&2
    exit 1
fi
echo "Android NDK: $NDK ($NDK_SOURCE), version $(ndk_version "$NDK")"

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
else
    CONFIG_ARGS+=(-DBRIMIR_BUILD_TESTS=OFF)
fi

cmake "${CONFIG_ARGS[@]}"
cmake --build "$BUILD_DIR" --target "${TARGETS[@]}"

CORE="$BUILD_DIR/lib/brimir_libretro_android.so"
if [ ! -f "$CORE" ]; then
    echo "error: $CORE was not built" >&2
    exit 1
fi
echo "$CORE"
