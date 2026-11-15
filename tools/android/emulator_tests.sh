#!/usr/bin/env bash
# Runs the x86_64 Android brimir_tests on a running emulator (or an x86_64 device):
#   emulator_tests.sh <brimir_tests> [Catch2 arguments ...]
#   e.g. emulator_tests.sh build-android-x86_64/bin/brimir_tests "[android-paths]"
#
# Copies the binary and the repository's tests/fixtures (audio, cheat files; never a *.bin BIOS image from there) to
# /data/local/tmp/brimir and runs the binary there with BRIMIR_TEST_FIXTURES_DIR pointing at the copy, so neither the
# checkout path nor the place the binary was built matters. Without BIOS_DIR the BIOS cases SKIP (as in CI).
# Everything it created on the device is removed on exit (also after a failure).
#
# Environment:
#   ADB        adb command (default `adb`); from WSL with the Windows SDK set it to .../platform-tools/adb.exe
#              (local paths are converted with wslpath for adb.exe)
#   BIOS_DIR   local folder whose BIOS images (mpr-17933.bin, "Sega Saturn BIOS v1.01 (JAP).bin", ...) are copied
#              into the fixtures copy (only read; use a scratch copy)
#   RUN_AS     shell (default: the user `adb shell` runs as, uid 2000), root, a numeric uid, or an installed app
#              package (its uid, e.g. com.retroarch: what an app may do). Anything but shell needs `adb root`.
#   TEST_ENV   extra environment, space separated: VAR=value sets, VAR= unsets. TMPDIR defaults to
#              /data/local/tmp/brimir/tmp (removed with the rest); "TMPDIR=" unsets it to see libc++'s Android default
#              temp folder /data/local/tmp (then leftovers of the tests stay there)
#   KEEP       1 keeps /data/local/tmp/brimir on the device
# Exit status: brimir_tests' exit status (2 for a usage or setup error).
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
DEV=/data/local/tmp/brimir
[ -f "$TESTS" ] || { echo "error: missing $TESTS" >&2; exit 2; }
[ -z "${BIOS_DIR:-}" ] || [ -d "$BIOS_DIR" ] || { echo "error: missing directory $BIOS_DIR" >&2; exit 2; }

host_path() {
    if [[ "$ADB" == *.exe ]] && command -v wslpath >/dev/null 2>&1; then wslpath -w "$1"; else printf '%s' "$1"; fi
}
sh_dev() { "$ADB" shell "$*" | tr -d '\r'; }
quote() { local a out=""; for a in "$@"; do out+=" '${a//\'/\'\\\'\'}'"; done; printf '%s' "$out"; }

SCRATCH=$(mktemp -d)
created=0
cleanup() {
    rm -rf "$SCRATCH"
    if [ $created = 1 ] && [ "${KEEP:-0}" != 1 ]; then
        "$ADB" shell "rm -rf $DEV" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

"$ADB" wait-for-device
case "$RUN_AS" in
    shell) ;;
    *)
        if [ "$(sh_dev id -u)" != "0" ]; then
            "$ADB" root >/dev/null
            sleep 2
            "$ADB" wait-for-device
        fi
        [ "$(sh_dev id -u)" = "0" ] || { echo "error: RUN_AS=$RUN_AS needs adb root" >&2; exit 2; }
        ;;
esac
case "$RUN_AS" in
    shell | root) RUN_UID= ;;
    *[!0-9]*)
        RUN_UID=$(sh_dev "pm list packages -U $RUN_AS" | sed -n "s/^package:$RUN_AS uid:\([0-9]*\).*/\1/p" | head -n1)
        [ -n "$RUN_UID" ] || { echo "error: no installed package $RUN_AS" >&2; exit 2; }
        ;;
    *) RUN_UID=$RUN_AS ;;
esac

# Fixtures: the tracked ones from the repository, BIOS images only from BIOS_DIR.
mkdir -p "$SCRATCH/fixtures"
(cd "$REPO/tests/fixtures" && find . -type f ! -name '*.bin' ! -name '*.ic1' -print0 |
    xargs -0 -I{} cp --parents {} "$SCRATCH/fixtures/")
if [ -n "${BIOS_DIR:-}" ]; then
    find "$BIOS_DIR" -maxdepth 1 -type f \( -name '*.bin' -o -name '*.ic1' -o -name '*.smpc' \) \
        -exec cp {} "$SCRATCH/fixtures/" \;
fi

sh_dev "rm -rf $DEV && mkdir -p $DEV/fixtures $DEV/tmp"
created=1
"$ADB" push "$(host_path "$SCRATCH/fixtures")/." $DEV/fixtures >/dev/null
"$ADB" push "$(host_path "$TESTS")" $DEV/brimir_tests >/dev/null
sh_dev "chmod 755 $DEV/brimir_tests"
if [ "$(sh_dev id -u)" = "0" ]; then
    sh_dev "chown -R shell:shell $DEV"
fi
if [ -n "$RUN_UID" ]; then
    # The binary and fixtures must be readable by that uid; /data/local/tmp itself stays as it is (not writable for it).
    sh_dev "chmod -R a+rX $DEV"
fi
echo "device: $(sh_dev getprop ro.build.fingerprint); page size $(sh_dev getconf PAGE_SIZE); run as $RUN_AS${RUN_UID:+ (uid $RUN_UID)}; env: ${TEST_ENV:-(default)}"
echo "fixtures: $(sh_dev "cd $DEV/fixtures && ls | tr '\n' ' '")"

# TMPDIR defaults to a folder under $DEV (some tests leave files in the temp folder; they go with $DEV)
env_cmd="export BRIMIR_TEST_FIXTURES_DIR=$DEV/fixtures; export TMPDIR=$DEV/tmp; "
for kv in ${TEST_ENV:-}; do
    if [ -z "${kv#*=}" ]; then env_cmd+="unset ${kv%%=*}; "; else env_cmd+="export $kv; "; fi
done
run="cd $DEV && ${env_cmd}echo \"id: \$(id -u); TMPDIR=\${TMPDIR-(unset)}\"; ./brimir_tests$(quote "$@")"
status=0
case "$RUN_AS" in
    root) "$ADB" shell "$run" || status=$? ;;
    shell) if [ "$(sh_dev id -u)" = "0" ]; then "$ADB" shell "su shell sh -c $(quote "$run")" || status=$?
           else "$ADB" shell "$run" || status=$?; fi ;;
    *) "$ADB" shell "su $RUN_UID sh -c $(quote "$run")" || status=$? ;;
esac
echo "brimir_tests exit status: $status"
exit $status
