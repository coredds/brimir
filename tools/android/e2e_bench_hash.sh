#!/usr/bin/env bash
# Frame-hash proof: the Android brimir_bench (on the emulator or a device) against a host build, same window, same
# arguments; the per-frame video and audio hash lines must be identical.
#   e2e_bench_hash.sh <android brimir_bench> <host brimir_bench> <bios file> [<disc image> <state file> <rtc dir>]
#
# Without a disc the window is the BIOS menu (tools/perf `bios`); with one it loads the state (made by
# `brimir_bench --dump-state`) and the RTC file(s) of <rtc dir> (tools/perf records: states/<window>.state, sys/<window>).
# Both sides run `--hash --audio-hash --fixed-rtc --frames $FRAMES --warmup 0` plus $BENCH_ARGS.
#
# Environment:
#   ADB         adb command (default `adb`); from WSL with the Windows SDK set it to .../platform-tools/adb.exe
#               (local paths are converted with wslpath for adb.exe and for a host bench ending in .exe)
#   FRAMES      frames to hash (default 300)
#   BENCH_ARGS  extra brimir_bench arguments for both sides (e.g. --sh2-jit)
#   HASH_OUT    output folder (default ./e2e-bench-hash-out): android.txt, host.txt, the hash lines and the diff
# The inputs are only read: copies go to /data/local/tmp/brimir-hash on the device (removed at the end) and to a fresh
# local scratch directory for the RTC files (the core may write them back).
set -euo pipefail

if [ $# -ne 3 ] && [ $# -ne 6 ]; then
    echo "usage: $0 <android brimir_bench> <host brimir_bench> <bios file> [<disc image> <state file> <rtc dir>]" >&2
    exit 2
fi
ABENCH=$1
HBENCH=$2
BIOS=$3
GAME=${4:-}
STATE=${5:-}
RTC=${6:-}
ADB=${ADB:-adb}
FRAMES=${FRAMES:-300}
read -r -a EXTRA <<<"${BENCH_ARGS:-}"
OUT=${HASH_OUT:-./e2e-bench-hash-out}
DEV=/data/local/tmp/brimir-hash

for f in "$ABENCH" "$HBENCH" "$BIOS" ${GAME:+"$GAME"} ${STATE:+"$STATE"}; do
    [ -f "$f" ] || { echo "error: missing $f" >&2; exit 2; }
done
[ -z "$RTC" ] || [ -d "$RTC" ] || { echo "error: missing directory $RTC" >&2; exit 2; }

win_path() { # $1: tool, $2: path
    if [[ "$1" == *.exe ]] && command -v wslpath >/dev/null 2>&1; then wslpath -w "$2"; else printf '%s' "$2"; fi
}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT

# Android side.
"$ADB" wait-for-device
"$ADB" shell "rm -rf $DEV && mkdir -p $DEV/sys"
"$ADB" push "$(win_path "$ADB" "$ABENCH")" $DEV/brimir_bench >/dev/null
"$ADB" push "$(win_path "$ADB" "$BIOS")" $DEV/bios.bin >/dev/null
DARGS=(--bios $DEV/bios.bin)
if [ -n "$GAME" ]; then
    "$ADB" push "$(win_path "$ADB" "$GAME")" "$DEV/game.${GAME##*.}" >/dev/null
    "$ADB" push "$(win_path "$ADB" "$STATE")" $DEV/window.state >/dev/null
    for f in "$RTC"/*; do "$ADB" push "$(win_path "$ADB" "$f")" "$DEV/sys/$(basename "$f")" >/dev/null; done
    DARGS+=(--game "$DEV/game.${GAME##*.}" --system-dir $DEV/sys --state $DEV/window.state)
fi
DARGS+=(--hash --audio-hash --fixed-rtc --frames "$FRAMES" --warmup 0 "${EXTRA[@]}")
"$ADB" shell "chmod 755 $DEV/brimir_bench"
echo "android: brimir_bench ${DARGS[*]}"
"$ADB" shell "cd $DEV && ./brimir_bench ${DARGS[*]}" 2>&1 | tr -d '\r' >"$OUT/android.txt" || true
"$ADB" shell "rm -rf $DEV"

# Host side (BIOS and RTC files copied to the scratch directory; pass a scratch copy of the disc image too).
cp "$BIOS" "$SCRATCH/bios.bin"
HARGS=(--bios "$(win_path "$HBENCH" "$SCRATCH/bios.bin")")
if [ -n "$GAME" ]; then
    mkdir -p "$SCRATCH/sys"
    cp "$RTC"/* "$SCRATCH/sys/"
    HARGS+=(--game "$(win_path "$HBENCH" "$GAME")" --system-dir "$(win_path "$HBENCH" "$SCRATCH/sys")"
        --state "$(win_path "$HBENCH" "$STATE")")
    # (the state file is only read)
fi
HARGS+=(--hash --audio-hash --fixed-rtc --frames "$FRAMES" --warmup 0 "${EXTRA[@]}")
echo "host:    $(basename "$HBENCH") ${HARGS[*]}"
"$HBENCH" "${HARGS[@]}" 2>&1 | tr -d '\r' >"$OUT/host.txt" || true

grep -E '^(frame|audio) [0-9]+ ' "$OUT/android.txt" >"$OUT/android.hashes.txt" || true
grep -E '^(frame|audio) [0-9]+ ' "$OUT/host.txt" >"$OUT/host.hashes.txt" || true
na=$(grep -c '^frame ' "$OUT/android.hashes.txt" || true)
nh=$(grep -c '^frame ' "$OUT/host.hashes.txt" || true)
distinct=$(grep '^frame ' "$OUT/host.hashes.txt" | awk '{print $4}' | sort -u | wc -l)
echo "frame hash lines: android $na, host $nh (distinct frame hashes: $distinct)"
if [ "$na" != "$FRAMES" ] || [ "$nh" != "$FRAMES" ]; then
    echo "FAIL: expected $FRAMES frame hash lines on each side (see $OUT/android.txt, $OUT/host.txt)"
    exit 1
fi
if diff "$OUT/host.hashes.txt" "$OUT/android.hashes.txt" >"$OUT/diff.txt"; then
    echo "PASS: $FRAMES frame and $(grep -c '^audio ' "$OUT/host.hashes.txt") audio hash lines identical"
else
    echo "FAIL: hash lines differ (first: $(grep -m1 '^[<>]' "$OUT/diff.txt"); see $OUT/diff.txt)"
    exit 1
fi
