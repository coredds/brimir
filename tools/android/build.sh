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

# shellcheck source=tools/android/ndk_env.sh
. tools/android/ndk_env.sh
ndk_find || exit 1
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
if [ ! -f "$TOOLCHAIN" ]; then
    echo "error: $NDK ($NDK_SOURCE) is not an Android NDK (no build/cmake/android.toolchain.cmake);" \
         "see tools/android/README.md" >&2
    exit 1
fi
NDK_FOUND=$(ndk_version "$NDK")
echo "Android NDK: $NDK ($NDK_SOURCE), version ${NDK_FOUND:-unknown}"

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
