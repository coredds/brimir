#!/usr/bin/env bash
# Runs the x86_64 Android brimir_tests on a running emulator (or rooted x86_64 device):
#   emulator_tests.sh <brimir_tests> [Catch2 arguments ...]      e.g. emulator_tests.sh build-android-x86_64/bin/brimir_tests "[android-paths]"
#
# The test binary finds its fixtures through the source path it was compiled from (__FILE__, BRIMIR_SOURCE_DIR), so
# the script recreates <repo>/tests/fixtures at that same absolute path on the device and copies the local
# tests/fixtures into it (the tracked audio and cheat fixtures, plus any BIOS image placed there locally; without a BIOS
# the BIOS cases SKIP, as in CI). That needs `adb root` and a path the device can create: /mnt/... (WSL) works because
# /mnt is a tmpfs on Android; on a host whose checkout lives elsewhere (/home/runner/... in CI) the script mounts a
# tmpfs over the first missing top-level directory. The binary runs in /data/local/tmp/brimir (the shell user's
# writable scratch folder) as $RUN_AS.
#
# Environment:
#   ADB        adb command (default `adb`); from WSL with the Windows SDK set it to .../platform-tools/adb.exe
#              (local paths are converted with wslpath for adb.exe)
#   RUN_AS     shell (default; uid 2000, what `adb shell` runs as without root) or root
#   TEST_ENV   extra environment for the run, e.g. "TMPDIR=" (unset TMPDIR: the core then sees libc++'s Android
#              default /data/local/tmp) or "TMPDIR=/data/local/tmp/brimir/tmp"
#   KEEP       1 keeps /data/local/tmp/brimir and the fixtures on the device
# Exit status: brimir_tests' exit status.
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <brimir_tests> [Catch2 arguments ...]" >&2
    exit 2
fi
TESTS=$1
shift
ADB=${ADB:-adb}
RUN_AS=${RUN_AS:-shell}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
FIXTURES=$REPO/tests/fixtures
DEV=/data/local/tmp/brimir
[ -f "$TESTS" ] || { echo "error: missing $TESTS" >&2; exit 2; }

host_path() {
    if [[ "$ADB" == *.exe ]] && command -v wslpath >/dev/null 2>&1; then wslpath -w "$1"; else printf '%s' "$1"; fi
}
sh_dev() { "$ADB" shell "$*" | tr -d '\r'; }
quote() { local a out=""; for a in "$@"; do out+=" '${a//\'/\'\\\'\'}'"; done; printf '%s' "$out"; }

"$ADB" wait-for-device
if [ "$(sh_dev id -u)" != "0" ]; then
    "$ADB" root >/dev/null
    sleep 2
    "$ADB" wait-for-device
fi
[ "$(sh_dev id -u)" = "0" ] || { echo "error: adb root is not available on this device" >&2; exit 2; }

# The fixtures at the compiled-in source path.
first=/$(printf '%s' "${REPO#/}" | cut -d/ -f1)
if ! sh_dev "mkdir -p '$FIXTURES'" 2>/dev/null || ! sh_dev "[ -d '$FIXTURES' ] && echo ok" | grep -q ok; then
    sh_dev "mkdir -p '$first' 2>/dev/null || true; mount -t tmpfs tmpfs '$first'" || true
    sh_dev "mkdir -p '$FIXTURES'"
fi
sh_dev "[ -d '$FIXTURES' ] && echo ok" | grep -q ok || { echo "error: cannot create $FIXTURES on the device" >&2; exit 2; }
"$ADB" push "$(host_path "$FIXTURES")/." "$FIXTURES" >/dev/null
sh_dev "chmod -R a+rX '$first'"

sh_dev "rm -rf $DEV && mkdir -p $DEV/tmp"
"$ADB" push "$(host_path "$TESTS")" $DEV/brimir_tests >/dev/null
sh_dev "chown -R shell:shell $DEV && chmod 755 $DEV/brimir_tests"
echo "device: $(sh_dev getprop ro.build.fingerprint); page size $(sh_dev getconf PAGE_SIZE); run as $RUN_AS; env: ${TEST_ENV:-(default)}"
echo "fixtures: $FIXTURES: $(sh_dev "ls '$FIXTURES' | tr '\n' ' '")"

env_cmd=""
if [ -n "${TEST_ENV:-}" ]; then
    for kv in $TEST_ENV; do
        if [ "${kv#*=}" = "" ]; then env_cmd+="unset ${kv%%=*}; "; else env_cmd+="export $kv; "; fi
    done
fi
run="cd $DEV && ${env_cmd}echo \"TMPDIR=\${TMPDIR-(unset)}\"; ./brimir_tests$(quote "$@")"
status=0
if [ "$RUN_AS" = root ]; then
    "$ADB" shell "$run" || status=$?
else
    "$ADB" shell "su shell sh -c $(quote "$run")" || status=$?
fi
if [ "${KEEP:-0}" != 1 ]; then
    sh_dev "rm -rf $DEV '$FIXTURES'"
fi
echo "brimir_tests exit status: $status"
exit $status
