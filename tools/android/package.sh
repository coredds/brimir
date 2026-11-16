#!/bin/bash
# Packages the Android core for a download (tools/android/README.md).
#
# usage: tools/android/package.sh <core.so> <out.zip>
# env:   as build.sh (the pinned NDK's llvm-strip is used)
#        INFO_FILE      default resources/info/brimir_libretro.info
#        INSTALL_TEXT   default tools/android/ANDROID-INSTALL.txt
#
# Writes a plain zip (no folders) holding brimir_libretro_android.so (the core after llvm-strip --strip-unneeded: the
# dynamic symbols and the build-id stay, the unstripped input is kept for symbolication and not changed),
# brimir_libretro.info and ANDROID-INSTALL.txt. An existing <out.zip> is replaced.
set -euo pipefail

usage() {
    echo "usage: $0 <core.so> <out.zip>" >&2
    exit 2
}

[ $# -eq 2 ] || usage
CORE=$1
OUT=$2
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
INFO_FILE=${INFO_FILE:-$ROOT/resources/info/brimir_libretro.info}
INSTALL_TEXT=${INSTALL_TEXT:-$ROOT/tools/android/ANDROID-INSTALL.txt}

for input in "$CORE" "$INFO_FILE" "$INSTALL_TEXT"; do
    if [ ! -f "$input" ]; then
        echo "error: missing input $input" >&2
        exit 1
    fi
done
case "$OUT" in
    *.zip) ;;
    *) echo "error: $OUT: the output must end in .zip" >&2; exit 1 ;;
esac
OUT_DIR=$(dirname "$OUT")
if [ ! -d "$OUT_DIR" ]; then
    echo "error: missing output directory $OUT_DIR" >&2
    exit 1
fi
OUT_ABS="$(cd "$OUT_DIR" && pwd)/$(basename "$OUT")"
command -v zip >/dev/null || { echo "error: zip not found (sudo apt install zip)" >&2; exit 1; }

# shellcheck source=tools/android/ndk_env.sh
. "$ROOT/tools/android/ndk_env.sh"
ndk_find || exit 1
STRIP="$(ndk_llvm_bin)/llvm-strip"
[ -x "$STRIP" ] || { echo "error: $STRIP not found (NDK $NDK)" >&2; exit 1; }

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
if ! "$STRIP" --strip-unneeded -o "$STAGE/brimir_libretro_android.so" "$CORE"; then
    echo "error: llvm-strip failed on $CORE" >&2
    exit 1
fi
cp "$INFO_FILE" "$STAGE/brimir_libretro.info"
cp "$INSTALL_TEXT" "$STAGE/ANDROID-INSTALL.txt"

rm -f "$OUT_ABS"
(cd "$STAGE" && zip -q -X "$OUT_ABS" brimir_libretro_android.so brimir_libretro.info ANDROID-INSTALL.txt)
echo "core: $(stat -c %s "$CORE") bytes -> stripped $(stat -c %s "$STAGE/brimir_libretro_android.so") bytes"
echo "$OUT_ABS"
