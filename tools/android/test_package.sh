#!/bin/bash
# Tests package.sh (tools/android/README.md) with stand-in files: a tiny arm64 shared object built with the NDK clang
# (with a symbol table, debug info and a build-id) as the core, and small text files as the .info, install text, LICENSE and
# third-party notices.
#
# usage: tools/android/test_package.sh
# env:   as build.sh
# Exit 0 when every case gives the expected result.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
PACKAGE=tools/android/package.sh

# shellcheck source=tools/android/ndk_env.sh
. tools/android/ndk_env.sh
ndk_find || exit 1
BIN=$(ndk_llvm_bin)
command -v unzip >/dev/null || { echo "error: unzip not found (sudo apt install unzip)" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/in" "$TMP/out"
cat > "$TMP/in/core.c" << 'EOF'
#include <stdio.h>
__attribute__((visibility("hidden"))) int brimir_hidden_helper(int x) { return printf("%d", x); }
__attribute__((visibility("default"))) void retro_init(void) { brimir_hidden_helper(1); }
__attribute__((visibility("default"))) unsigned retro_api_version(void) { return 1; }
EOF
"$BIN/clang" --target=aarch64-linux-android26 -fPIC -shared -fvisibility=hidden -O1 -g -Wl,--build-id=sha1 \
    -Wl,-z,max-page-size=16384 "$TMP/in/core.c" -o "$TMP/in/brimir_libretro_android.so"
CORE="$TMP/in/brimir_libretro_android.so"
printf 'display_name = "stand-in info"\n' > "$TMP/in/stand-in.info"
printf 'stand-in install text\n' > "$TMP/in/stand-in.txt"
printf 'stand-in licence\n' > "$TMP/in/stand-in-license"
printf 'stand-in notices\n' > "$TMP/in/stand-in-notices"
printf 'not an ELF file\n' > "$TMP/in/not-elf.so"
CORE_SHA=$(sha256sum "$CORE" | cut -d' ' -f1)

PASS=0
FAILS=0
check() {
    local name=$1 result=$2
    if [ "$result" = 1 ]; then
        echo "PASS $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL $name"
        FAILS=$((FAILS + 1))
    fi
}
# run <out-var> <status-var> <command...>: captures output and exit status
run() {
    local -n _out=$1 _status=$2
    shift 2
    _status=0
    _out=$("$@" 2>&1) || _status=$?
    sed 's/^/    /' <<< "$_out"
}
build_id() { "$BIN/llvm-readelf" -n "$1" | sed -n 's/^ *Build ID: *//p'; }
dynsyms() { "$BIN/llvm-nm" -D --defined-only -P "$1" | awk '{ print $1, $2 }' | LC_ALL=C sort; }
names() { unzip -Z1 "$1" | LC_ALL=C sort | tr '\n' ' '; }
WANT="ANDROID-INSTALL.txt LICENSE THIRD_PARTY_NOTICES.txt brimir_libretro.info brimir_libretro_android.so "

# 1. Stand-in inputs. A junk zip at the output path is replaced, not appended to.
(cd "$TMP/out" && echo junk > junk.txt && zip -q stale.zip junk.txt && mv stale.zip pkg.zip)
run out status env INFO_FILE="$TMP/in/stand-in.info" INSTALL_TEXT="$TMP/in/stand-in.txt" \
    LICENSE_FILE="$TMP/in/stand-in-license" NOTICES_FILE="$TMP/in/stand-in-notices" \
    bash "$PACKAGE" "$CORE" "$TMP/out/pkg.zip"
echo "    unzip -l:"
unzip -l "$TMP/out/pkg.zip" | sed 's/^/      /'
check "stand-ins: exit 0" "$([ "$status" = 0 ] && echo 1)"
check "stand-ins: zip lists exactly the five files, no folder ($(names "$TMP/out/pkg.zip"))" \
    "$([ "$(names "$TMP/out/pkg.zip")" = "$WANT" ] && echo 1)"
check "stand-ins: unzip -t (a plain, intact zip)" "$(unzip -tq "$TMP/out/pkg.zip" >/dev/null && echo 1)"
mkdir -p "$TMP/x1"
unzip -q "$TMP/out/pkg.zip" -d "$TMP/x1"
check "stand-ins: .info, install text, LICENSE and notices copied unchanged" \
    "$(cmp -s "$TMP/x1/brimir_libretro.info" "$TMP/in/stand-in.info" &&
       cmp -s "$TMP/x1/ANDROID-INSTALL.txt" "$TMP/in/stand-in.txt" &&
       cmp -s "$TMP/x1/LICENSE" "$TMP/in/stand-in-license" &&
       cmp -s "$TMP/x1/THIRD_PARTY_NOTICES.txt" "$TMP/in/stand-in-notices" && echo 1)"
SO="$TMP/x1/brimir_libretro_android.so"
check "stripped: still an AArch64 ELF" "$("$BIN/llvm-readelf" -h "$SO" | grep -q 'Machine: *AArch64' && echo 1)"
check "stripped: input had .symtab and .debug_info, output has neither" \
    "$("$BIN/llvm-readelf" -S -W "$CORE" | grep -q ' \.symtab ' &&
       "$BIN/llvm-readelf" -S -W "$CORE" | grep -q ' \.debug_info ' &&
       ! "$BIN/llvm-readelf" -S -W "$SO" | grep -qE ' \.(symtab|debug_info) ' && echo 1)"
check "stripped: same dynamic symbols ($(dynsyms "$SO" | awk '{ print $1 }' | tr '\n' ' '))" \
    "$([ -n "$(dynsyms "$SO")" ] && [ "$(dynsyms "$SO")" = "$(dynsyms "$CORE")" ] && echo 1)"
check "stripped: same build-id ($(build_id "$SO"))" \
    "$([ -n "$(build_id "$SO")" ] && [ "$(build_id "$SO")" = "$(build_id "$CORE")" ] && echo 1)"
check "stripped: smaller than the input ($(stat -c %s "$CORE") -> $(stat -c %s "$SO") bytes)" \
    "$([ "$(stat -c %s "$SO")" -lt "$(stat -c %s "$CORE")" ] && echo 1)"
check "input core not changed" "$([ "$(sha256sum "$CORE" | cut -d' ' -f1)" = "$CORE_SHA" ] && echo 1)"

# 2. Default .info, install text, LICENSE and notices (the repository's).
run out status bash "$PACKAGE" "$CORE" "$TMP/out/defaults.zip"
mkdir -p "$TMP/x2"
unzip -q "$TMP/out/defaults.zip" -d "$TMP/x2"
check "defaults: exit 0, the repository's .info, ANDROID-INSTALL.txt, LICENSE and THIRD_PARTY_NOTICES.txt" \
    "$([ "$status" = 0 ] && [ "$(names "$TMP/out/defaults.zip")" = "$WANT" ] &&
       cmp -s "$TMP/x2/brimir_libretro.info" resources/info/brimir_libretro.info &&
       cmp -s "$TMP/x2/ANDROID-INSTALL.txt" tools/android/ANDROID-INSTALL.txt &&
       cmp -s "$TMP/x2/LICENSE" LICENSE &&
       cmp -s "$TMP/x2/THIRD_PARTY_NOTICES.txt" THIRD_PARTY_NOTICES.txt && echo 1)"

# 3. Failures: exit 1 with a message naming the problem, and no zip written.
expect_fail() {
    local name=$1 want=$2 zip=$3 out status
    shift 3
    run out status "$@"
    check "$name: exit 1, message '$want', no zip" \
        "$([ "$status" = 1 ] && grep -q "error: .*$want" <<< "$out" && [ ! -e "$zip" ] && echo 1)"
}
expect_fail "missing core" "missing input $TMP/in/none.so" "$TMP/out/f1.zip" \
    bash "$PACKAGE" "$TMP/in/none.so" "$TMP/out/f1.zip"
expect_fail "missing .info" "missing input $TMP/in/none.info" "$TMP/out/f2.zip" \
    env INFO_FILE="$TMP/in/none.info" bash "$PACKAGE" "$CORE" "$TMP/out/f2.zip"
expect_fail "missing install text" "missing input $TMP/in/none.txt" "$TMP/out/f3.zip" \
    env INSTALL_TEXT="$TMP/in/none.txt" bash "$PACKAGE" "$CORE" "$TMP/out/f3.zip"
expect_fail "missing LICENSE" "missing input $TMP/in/none-license" "$TMP/out/f6.zip" \
    env LICENSE_FILE="$TMP/in/none-license" bash "$PACKAGE" "$CORE" "$TMP/out/f6.zip"
expect_fail "missing third-party notices" "missing input $TMP/in/none-notices" "$TMP/out/f7.zip" \
    env NOTICES_FILE="$TMP/in/none-notices" bash "$PACKAGE" "$CORE" "$TMP/out/f7.zip"
expect_fail "core that is not an ELF file" "llvm-strip failed" "$TMP/out/f4.zip" \
    bash "$PACKAGE" "$TMP/in/not-elf.so" "$TMP/out/f4.zip"
expect_fail "missing output folder" "missing output directory" "$TMP/none/f5.zip" \
    bash "$PACKAGE" "$CORE" "$TMP/none/f5.zip"
run out status bash "$PACKAGE" "$CORE"
check "one argument: usage, exit 2" "$([ "$status" = 2 ] && grep -q '^usage:' <<< "$out" && echo 1)"

echo "test_package: $PASS passed, $FAILS failed"
[ "$FAILS" = 0 ]
