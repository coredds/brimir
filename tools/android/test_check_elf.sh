#!/bin/bash
# Tests check_elf.sh (tools/android/README.md): builds tiny shared objects with the NDK clang, each breaking one check,
# and runs the checker on them and on the real cores.
#
# usage: tools/android/test_check_elf.sh
# env:   as build.sh; HOST_CORE (default build-wsl/lib/brimir_libretro.so, a Linux x64 core: skipped when missing)
#        ARM64_CORE (default build-android-arm64-v8a/lib/brimir_libretro_android.so, required)
#        X86_64_CORE (default build-android-x86_64/lib/brimir_libretro_android.so, skipped when missing)
# Exit 0 when every case gives the expected result.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
CHECK=tools/android/check_elf.sh
HOST_CORE=${HOST_CORE:-build-wsl/lib/brimir_libretro.so}
ARM64_CORE=${ARM64_CORE:-build-android-arm64-v8a/lib/brimir_libretro_android.so}
X86_64_CORE=${X86_64_CORE:-build-android-x86_64/lib/brimir_libretro_android.so}

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
expect "object with the wrong file name" "$TMP/badname/brimir_libretro.so" arm64-v8a name only
expect "x86_64 object checked as arm64-v8a" "$TMP/x64/brimir_libretro_android.so" arm64-v8a machine only
expect "x86_64 object checked as x86_64" "$TMP/x64/brimir_libretro_android.so" x86_64 ok
if [ -f "$HOST_CORE" ]; then
    expect "host Linux x64 core ($HOST_CORE) checked as arm64-v8a" "$HOST_CORE" arm64-v8a machine
else
    echo "SKIP host Linux x64 core: $HOST_CORE not found"
fi
expect "real arm64 core ($ARM64_CORE)" "$ARM64_CORE" arm64-v8a ok
if [ -f "$X86_64_CORE" ]; then
    expect "real x86_64 core ($X86_64_CORE)" "$X86_64_CORE" x86_64 ok
else
    echo "SKIP real x86_64 core: $X86_64_CORE not found"
fi

echo "test_check_elf: $PASS passed, $FAILS failed"
[ "$FAILS" = 0 ]
