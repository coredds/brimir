#!/bin/bash
# Tests check_elf.sh (tools/android/README.md): builds tiny shared objects with the NDK clang, each breaking one check,
# and runs the checker on them and on the real cores.
#
# usage: tools/android/test_check_elf.sh
# env:   as build.sh; HOST_CORE (a Linux x64 core), ARM64_CORE (required), X86_64_CORE:
#          unset: the default path (build-wsl/lib/brimir_libretro.so, build-android-arm64-v8a/lib/...,
#                 build-android-x86_64/lib/brimir_libretro_android.so); HOST_CORE and X86_64_CORE are skipped when
#                 their default file is missing
#          a path: that file, a missing one is a failure (CI sets every one, so nothing is skipped by accident)
#          "skip:<reason>": skipped with that reason (not for ARM64_CORE)
# Exit 0 when every case gives the expected result.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
CHECK=tools/android/check_elf.sh
ARM64_CORE=${ARM64_CORE:-build-android-arm64-v8a/lib/brimir_libretro_android.so}
case "$ARM64_CORE" in skip:*) echo "error: ARM64_CORE cannot be skipped" >&2; exit 2 ;; esac

# shellcheck source=tools/android/ndk_env.sh
. tools/android/ndk_env.sh
ndk_find || exit 1
BIN=$(ndk_llvm_bin)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Sources: the libretro API (default visibility) and a hidden helper; extra.c adds one more exported function.
{
    echo '#include <stdio.h>'
    echo '__attribute__((visibility("hidden"))) int helper(int x) { return printf("%d", x); }'
    while read -r name; do
        [ -n "$name" ] && echo "__attribute__((visibility(\"default\"))) void $name(void) { helper(1); }"
    done < tools/android/libretro_exports.txt
} > "$TMP/api.c"
echo '__attribute__((visibility("default"))) void brimir_extra_symbol(void) {}' > "$TMP/extra.c"
# missing.c: the libretro API without retro_reset
grep -v ' retro_reset(void)' "$TMP/api.c" > "$TMP/missing.c"
if cmp -s "$TMP/api.c" "$TMP/missing.c"; then
    echo "error: retro_reset is not in libretro_exports.txt; pick another function for the missing-export case" >&2
    exit 1
fi
{
    echo '#include <stdio.h>'
    echo '#include <string>'
    echo 'extern "C" {'
    sed 's/void retro_api_version(void) {/void retro_api_version(void) { static std::string s; s += "x";/' "$TMP/api.c" \
        | grep -v '^#include'
    echo '}'
} > "$TMP/api.cpp"

# so <dir> <target> <compiler> <flags...>: links $TMP/<dir>/brimir_libretro_android.so
so() {
    local dir=$1 target=$2 cc=$3
    shift 3
    mkdir -p "$TMP/$dir"
    "$BIN/$cc" --target="$target" -fPIC -shared -fvisibility=hidden -O1 -Wl,--no-undefined "$@" \
        -o "$TMP/$dir/brimir_libretro_android.so"
}
A64=aarch64-linux-android26
X64=x86_64-linux-android26
so good "$A64" clang "$TMP/api.c" -Wl,-z,max-page-size=16384
so align4k "$A64" clang "$TMP/api.c" -Wl,-z,max-page-size=4096
so cxxshared "$A64" clang++ -x c++ "$TMP/api.cpp" -Wl,-z,max-page-size=16384 -nostdlib++ -lc++_shared
so extra "$A64" clang "$TMP/api.c" "$TMP/extra.c" -Wl,-z,max-page-size=16384
so missing "$A64" clang "$TMP/missing.c" -Wl,-z,max-page-size=16384
so x64 "$X64" clang "$TMP/api.c" -Wl,-z,max-page-size=16384
mkdir -p "$TMP/badname"
cp "$TMP/good/brimir_libretro_android.so" "$TMP/badname/brimir_libretro.so"

PASS=0
FAILS=0
# expect <name> <file> <abi> <ok|reason> [only]: ok = exit 0 and "check_elf: ok"; reason = exit 1 and a FAIL line
# containing it ("only": that is the only FAIL line).
expect() {
    local name=$1 file=$2 abi=$3 want=$4 only=${5:-} out status=0 fails
    out=$(bash "$CHECK" "$file" "$abi" 2>&1) || status=$?
    fails=$(grep -c '^FAIL:' <<< "$out" || true)
    local good=0
    if [ "$want" = ok ]; then
        [ "$status" = 0 ] && grep -qx 'check_elf: ok' <<< "$out" && good=1
    else
        if [ "$status" = 1 ] && grep -q "^FAIL: .*$want" <<< "$out"; then
            good=1
            [ "$only" = only ] && [ "$fails" != 1 ] && good=0
        fi
    fi
    if [ "$good" = 1 ]; then
        echo "PASS $name: expected $want${only:+ ($only)}, exit $status"
        PASS=$((PASS + 1))
    else
        echo "FAIL $name: expected $want${only:+ ($only)}, exit $status"
        FAILS=$((FAILS + 1))
    fi
    sed 's/^/    /' <<< "$out"
}

expect "control: tiny arm64 object, all checks met" "$TMP/good/brimir_libretro_android.so" arm64-v8a ok
expect "4 KB-aligned object" "$TMP/align4k/brimir_libretro_android.so" arm64-v8a alignment only
expect "object needing libc++_shared.so" "$TMP/cxxshared/brimir_libretro_android.so" arm64-v8a DT_NEEDED only
expect "object exporting an extra symbol" "$TMP/extra/brimir_libretro_android.so" arm64-v8a exports only
expect "object missing retro_reset" "$TMP/missing/brimir_libretro_android.so" arm64-v8a "exports.*missing: retro_reset" only
expect "object with the wrong file name" "$TMP/badname/brimir_libretro.so" arm64-v8a name only
expect "x86_64 object checked as arm64-v8a" "$TMP/x64/brimir_libretro_android.so" arm64-v8a machine only
expect "x86_64 object checked as x86_64" "$TMP/x64/brimir_libretro_android.so" x86_64 ok
# optional <var> <default> <name> <abi> <ok|reason>: the case for the core in $<var> (see env above)
optional() {
    local var=$1 default=$2 name=$3 abi=$4 want=$5 file
    if [ -z "${!var+set}" ]; then
        file=$default
        if [ ! -f "$file" ]; then
            echo "SKIP $name: $var not set and $file not found"
            return
        fi
    else
        file=${!var}
        case "$file" in
            skip:*) echo "SKIP $name: $var=$file"; return ;;
        esac
    fi
    expect "$name ($file)" "$file" "$abi" "$want"
}
optional HOST_CORE build-wsl/lib/brimir_libretro.so "host Linux x64 core checked as arm64-v8a" arm64-v8a machine
expect "real arm64 core ($ARM64_CORE)" "$ARM64_CORE" arm64-v8a ok
optional X86_64_CORE build-android-x86_64/lib/brimir_libretro_android.so "real x86_64 core" x86_64 ok

echo "test_check_elf: $PASS passed, $FAILS failed"
[ "$FAILS" = 0 ]
