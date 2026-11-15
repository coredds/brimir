#!/bin/bash
# Checks an Android libretro core before it is shipped (tools/android/README.md).
#
# usage: tools/android/check_elf.sh <core.so> <arm64-v8a|x86_64>
# env:   as build.sh (the pinned NDK's llvm-readelf and llvm-nm are used)
#
# Checks: the ELF machine matches the ABI; DT_NEEDED only names libc.so libm.so libdl.so liblog.so; every PT_LOAD segment
# is aligned for 16 KB pages (>= 0x4000); the exported symbols (defined, global, dynamic) are exactly the lines of
# libretro_exports.txt; the file is named brimir_libretro_android.so.
# Exit 0 and "check_elf: ok" when all hold, else exit 1 with one "FAIL: <reason>" line per failed check.
set -euo pipefail

usage() {
    echo "usage: $0 <core.so> <arm64-v8a|x86_64>" >&2
    exit 2
}

[ $# -eq 2 ] || usage
CORE=$1
ABI=$2
case "$ABI" in
    arm64-v8a) MACHINE="AArch64" ;;
    x86_64) MACHINE="Advanced Micro Devices X86-64" ;;
    *) usage ;;
esac

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=tools/android/ndk_env.sh
. "$HERE/ndk_env.sh"
ndk_find || exit 1
BIN=$(ndk_llvm_bin)
READELF="$BIN/llvm-readelf"
NM="$BIN/llvm-nm"
for tool in "$READELF" "$NM"; do
    if [ ! -x "$tool" ]; then
        echo "error: $tool not found (NDK $NDK)" >&2
        exit 1
    fi
done

if [ ! -f "$CORE" ]; then
    echo "FAIL: $CORE: no such file"
    exit 1
fi
if ! HEADER=$("$READELF" -h "$CORE" 2>&1); then
    echo "FAIL: $CORE is not an ELF file: $HEADER"
    exit 1
fi

FAILED=0
fail() {
    echo "FAIL: $*"
    FAILED=1
}

# (1) machine
machine=$(sed -n 's/^ *Machine: *//p' <<< "$HEADER")
class=$(sed -n 's/^ *Class: *//p' <<< "$HEADER")
if [ "$machine" != "$MACHINE" ] || [ "$class" != "ELF64" ]; then
    fail "machine is '$machine' ($class), $ABI needs '$MACHINE' (ELF64)"
fi

# (2) DT_NEEDED
allowed=" libc.so libm.so libdl.so liblog.so "
needed=$("$READELF" -d "$CORE" | sed -n 's/.*(NEEDED) *Shared library: *\[\(.*\)\].*/\1/p')
for lib in $needed; do
    case "$allowed" in
        *" $lib "*) ;;
        *) fail "DT_NEEDED $lib is not one of:$allowed" ;;
    esac
done

# (3) PT_LOAD alignment (last column of each LOAD line)
loads=$("$READELF" -l -W "$CORE" | awk '$1 == "LOAD" { print $NF }')
if [ -z "$loads" ]; then
    fail "alignment: no PT_LOAD segment"
fi
small=""
for align in $loads; do
    if [ $((align)) -lt $((0x4000)) ]; then
        small+=" $align"
    fi
done
if [ -n "$small" ]; then
    fail "alignment: PT_LOAD segments aligned to$small, all need >= 0x4000 (16 KB pages)"
fi

# (4) exports: defined global (upper-case type, weak included) dynamic symbols
exports=$("$NM" -D --defined-only -P "$CORE" | awk '$2 ~ /^[A-Z]$/ { print $1 }' | LC_ALL=C sort -u)
expected=$(grep -v '^[[:space:]]*$' "$HERE/libretro_exports.txt" | LC_ALL=C sort -u)
if [ "$exports" != "$expected" ]; then
    extra=$(LC_ALL=C comm -23 <(echo "$exports") <(echo "$expected") | sed '/^$/d')
    missing=$(LC_ALL=C comm -13 <(echo "$exports") <(echo "$expected") | sed '/^$/d')
    n_extra=$(printf '%s' "$extra" | grep -c . || true)
    n_missing=$(printf '%s' "$missing" | grep -c . || true)
    more=""
    [ "$n_extra" -gt 5 ] && more="..."
    detail=""
    [ "$n_extra" -gt 0 ] && detail+=" extra: $(head -n 5 <<< "$extra" | tr '\n' ' ')$more"
    [ "$n_missing" -gt 0 ] && detail+=" missing: $(tr '\n' ' ' <<< "$missing")"
    fail "exports differ from libretro_exports.txt: $n_extra extra, $n_missing missing;$detail"
fi

# (5) file name
if [ "$(basename "$CORE")" != "brimir_libretro_android.so" ]; then
    fail "name is '$(basename "$CORE")', RetroArch for Android needs brimir_libretro_android.so"
fi

if [ "$FAILED" = 1 ]; then
    exit 1
fi
echo "check_elf: ok"
