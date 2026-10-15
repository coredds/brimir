#!/bin/bash
# Lockstep validation (JIT core vs interpreter core, compared after every frame) of the BIOS
# menu and the six reference titles on a Linux host. See tools/README.md.
#
# usage: run_lockstep.sh [frames] [backend]     default: 1800 ir
# env:   BRIMIR_BENCH_DATA, BENCH, ONLY as in run_bench.sh
#        EXTRA_ARGS         extra brimir_bench options (default --no-cd-preload: lockstep runs two
#                           cores, and preloading would hold two copies of the disc image in RAM)
set -u
EXTRA_ARGS=${EXTRA_ARGS---no-cd-preload}
FRAMES=${1:-1800}; BACKEND=${2:-ir}
DATA=${BRIMIR_BENCH_DATA:-/mnt/data}
BENCH=${BENCH:-$DATA/build/bench/bin/brimir_bench}
US_BIOS="$DATA/bios/mpr-17933.bin"
JP_BIOS="$DATA/bios/Sega Saturn BIOS v1.01 (JAP).bin"
TITLES=(
  "bios|US|"
  "vf2|JP|Virtua Fighter 2 (Japan) (Rev B).chd"
  "pd2|US|Panzer Dragoon II Zwei (USA).chd"
  "rally|US|Sega Rally Championship (USA).chd"
  "br|US|Burning Rangers (USA).chd"
  "gh|US|Guardian Heroes (USA).chd"
  "sfz3|JP|Street Fighter Zero 3 (Japan).chd"
)
HOST=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || hostname)
HOST_TAG=$(echo "$HOST" | tr -cs 'A-Za-z0-9' '-' | sed 's/-$//' | cut -c1-40)
OUT=$DATA/results/$HOST_TAG/lockstep-$BACKEND-$FRAMES-$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
{
  echo "host         : $HOST, $(uname -r), $(uname -m)"
  echo "binary       : $BENCH"
  [ -r "$DATA/src/brimir/.source-commit" ] && echo "source commit: $(cat "$DATA/src/brimir/.source-commit")"
  echo "frames       : $FRAMES  backend $BACKEND  extra args: ${EXTRA_ARGS:-none}"
} | tee "$OUT/session.txt"

fail=0
for entry in "${TITLES[@]}"; do
  IFS='|' read -r key region game <<< "$entry"
  if [ -n "${ONLY:-}" ] && ! grep -qw "$key" <<< "$ONLY"; then continue; fi
  bios=$US_BIOS; [ "$region" = JP ] && bios=$JP_BIOS
  args=(--bios "$bios")
  sysdir=""
  if [ -n "$game" ]; then
    sysdir=$(mktemp -d); cp "$DATA"/bios/system/*.smpc "$sysdir"/
    args+=(--game "$DATA/games/$game" --system-dir "$sysdir")
    # shellcheck disable=SC2206
    [ -n "$EXTRA_ARGS" ] && args+=($EXTRA_ARGS)
  fi
  args+=(--lockstep "$FRAMES" --jit-backend "$BACKEND")
  start=$(date +%s)
  "$BENCH" "${args[@]}" > "$OUT/$key.log" 2>&1
  rc=$?
  wall=$(( $(date +%s) - start ))
  [ -n "$sysdir" ] && rm -rf "$sysdir"
  [ $rc -ne 0 ] && fail=1
  result=$(grep -E "^lockstep(: OK| divergence)" "$OUT/$key.log" | tail -1)
  printf "%-5s exit %s  %6ss  %s\n" "$key" "$rc" "$wall" "${result:-no result line}" | tee -a "$OUT/summary.txt"
done
echo "results: $OUT" | tee -a "$OUT/summary.txt"
touch "$OUT/DONE"
exit $fail
