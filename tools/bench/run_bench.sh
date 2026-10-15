#!/bin/bash
# Frame-time benchmark of the BIOS menu and the six reference titles on a Linux host
# (design/sh2-baseline.md method), with throttling detection. See tools/README.md.
#
# usage: run_bench.sh [rounds] [mode...]        modes: interp | ir | x64 | ...   default: 3 interp
# env:   BRIMIR_BENCH_DATA  data dir with bios/, bios/system/*.smpc, games/   (default /mnt/data)
#        BENCH              brimir_bench binary   (default $BRIMIR_BENCH_DATA/build/bench/bin/brimir_bench)
#        ONLY="pd2 sfz3"    run only these titles (bios vf2 pd2 rally br gh sfz3)
#        PIN=4-7            run brimir_bench under taskset -c PIN (big cores on big.LITTLE hosts)
#        COOL_C=55          start a run only at or below this temperature
set -u

ROUNDS=${1:-3}; shift || true
MODES=("$@"); [ ${#MODES[@]} -eq 0 ] && MODES=(interp)

DATA=${BRIMIR_BENCH_DATA:-/mnt/data}
BENCH=${BENCH:-$DATA/build/bench/bin/brimir_bench}
US_BIOS="$DATA/bios/mpr-17933.bin"
JP_BIOS="$DATA/bios/Sega Saturn BIOS v1.01 (JAP).bin"
SYSTEM_TEMPLATE=$DATA/bios/system
COOL_C=${COOL_C:-55}
MIN_FREQ_PCT=97      # a run is invalid if any CPU policy drops below this % of its hardware maximum

# key|bios region|game (empty game = BIOS menu)
TITLES=(
  "bios|US|"
  "vf2|JP|Virtua Fighter 2 (Japan) (Rev B).chd"
  "pd2|US|Panzer Dragoon II Zwei (USA).chd"
  "rally|US|Sega Rally Championship (USA).chd"
  "br|US|Burning Rangers (USA).chd"
  "gh|US|Guardian Heroes (USA).chd"
  "sfz3|JP|Street Fighter Zero 3 (Japan).chd"
)

HAVE_VCGENCMD=no; command -v vcgencmd >/dev/null 2>&1 && HAVE_VCGENCMD=yes
POLICIES=(/sys/devices/system/cpu/cpufreq/policy*)

# Highest temperature over all thermal zones, whole degrees C.
temp_c() {
  local m=0 t z
  for z in /sys/class/thermal/thermal_zone*/temp; do
    [ -r "$z" ] || continue
    t=$(( $(cat "$z") / 1000 )); [ $t -gt $m ] && m=$t
  done
  echo $m
}
# Lowest current/max frequency ratio over all CPU policies, in percent.
freq_pct() {
  local m=100 p cur max pct
  for p in "${POLICIES[@]}"; do
    [ -r "$p/scaling_cur_freq" ] || continue
    cur=$(cat "$p/scaling_cur_freq"); max=$(cat "$p/cpuinfo_max_freq")
    pct=$(( cur * 100 / max )); [ $pct -lt $m ] && m=$pct
  done
  if [ $HAVE_VCGENCMD = yes ]; then   # Raspberry Pi: firmware throttling is not visible in cpufreq
    cur=$(vcgencmd measure_clock arm | cut -d= -f2); max=$(cat "${POLICIES[0]}/cpuinfo_max_freq")
    pct=$(( cur / 10 / max )); [ $pct -lt $m ] && m=$pct
  fi
  echo $m
}
# Raspberry Pi firmware throttle flags, 0x0 elsewhere.
throttle() { if [ $HAVE_VCGENCMD = yes ]; then vcgencmd get_throttled | cut -d= -f2; else echo 0x0; fi; }

HOST=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || hostname)
HOST_TAG=$(echo "$HOST" | tr -cs 'A-Za-z0-9' '-' | sed 's/-$//' | cut -c1-40)
SESSION=$(date +%Y%m%d-%H%M%S)
OUT=$DATA/results/$HOST_TAG/bench-$SESSION
mkdir -p "$OUT"
SUMMARY=$OUT/summary.tsv
printf "title\tmode\tround\texit\tvalid\tavg\tp50\tp95\tp99\tmax\tymir\tmaster\tslave\tsh2_total\tsh2_pct\ttemp_max\tfreq_min_pct\tthrottle_seen\twall_s\n" > "$SUMMARY"

PIN_CMD=(); [ -n "${PIN:-}" ] && PIN_CMD=(taskset -c "$PIN")

{
  echo "session      : $SESSION"
  echo "host         : $HOST, $(uname -r), $(uname -m)"
  echo "cpu          : $(lscpu | awk -F: '/Model name/{gsub(/^ +/,"",$2); printf "%s; ", $2}')$(nproc) CPUs"
  for p in "${POLICIES[@]}"; do
    [ -r "$p/scaling_governor" ] && echo "cpufreq      : $(basename "$p") cpus $(cat "$p/related_cpus") governor $(cat "$p/scaling_governor") max $(( $(cat "$p/cpuinfo_max_freq") / 1000 )) MHz"
  done
  echo "binary       : $BENCH"
  [ -r "$DATA/src/brimir/.source-commit" ] && echo "source commit: $(cat "$DATA/src/brimir/.source-commit")"
  echo "pin          : ${PIN:-none}"
  echo "rounds/modes : $ROUNDS / ${MODES[*]}"
  echo "throttled    : $(throttle) at start"
} | tee "$OUT/session.txt"

for round in $(seq 1 "$ROUNDS"); do
  for entry in "${TITLES[@]}"; do
    IFS='|' read -r key region game <<< "$entry"
    if [ -n "${ONLY:-}" ] && ! grep -qw "$key" <<< "$ONLY"; then continue; fi
    bios=$US_BIOS; [ "$region" = JP ] && bios=$JP_BIOS
    for mode in "${MODES[@]}"; do
      log=$OUT/$key-$mode-r$round.log
      args=(--bios "$bios")
      sysdir=""
      if [ -n "$game" ]; then
        sysdir=$(mktemp -d); cp "$SYSTEM_TEMPLATE"/*.smpc "$sysdir"/
        args+=(--game "$DATA/games/$game" --system-dir "$sysdir" --warmup 2400 --frames 1800)
      else
        args+=(--warmup 300 --frames 1800)
      fi
      [ "$mode" != interp ] && args+=(--sh2-jit --jit-backend "$mode")

      while [ "$(temp_c)" -gt "$COOL_C" ]; do sleep 5; done
      t_before=$(throttle)

      samples=$OUT/.samples; : > "$samples"
      ( while :; do echo "$(throttle) $(temp_c) $(freq_pct)" >> "$samples"; sleep 1; done ) &
      sampler=$!

      start=$(date +%s)
      "${PIN_CMD[@]}" "$BENCH" "${args[@]}" > "$log" 2>&1
      rc=$?
      wall=$(( $(date +%s) - start ))
      kill $sampler 2>/dev/null; wait $sampler 2>/dev/null
      t_after=$(throttle)
      [ -n "$sysdir" ] && rm -rf "$sysdir"

      tmax=$(awk 'BEGIN{m=0}{if($2>m)m=$2}END{print m}' "$samples")
      fmin=$(awk 'BEGIN{m=100}{if($3<m)m=$3}END{print m}' "$samples")
      seen=no   # Pi flag bits 0-3: under-voltage, frequency capped, throttled, soft temperature limit (now)
      while read -r t _; do (( t & 0xF )) && { seen=yes; break; }; done < "$samples"
      valid=yes
      { [ $rc -ne 0 ] || [ $seen = yes ] || [ "$t_before" != "$t_after" ] || [ "$fmin" -lt $MIN_FREQ_PCT ]; } && valid=no
      { echo; echo "runner: exit $rc  wall ${wall}s  throttled $t_before -> $t_after  seen $seen  temp_max ${tmax}C  freq_min ${fmin}%  valid $valid"; } >> "$log"

      read -r avg p50 p95 p99 mx < <(awk '/^ms\/frame/{print $4,$6,$8,$10,$12}' "$log")
      ymir=$(awk '/^Ymir_RunFrame/{print $2}' "$log")
      master=$(awk '/^SH2 master/{print $4}' "$log")
      slave=$(awk '/^SH2 slave/{print $4}' "$log")
      read -r total pct < <(awk '/^SH2 total/{gsub(/[(%]/,"",$6); print $4,$6}' "$log")
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$key" "$mode" "$round" "$rc" "$valid" "${avg:-}" "${p50:-}" "${p95:-}" "${p99:-}" "${mx:-}" \
        "${ymir:-}" "${master:-}" "${slave:-}" "${total:-}" "${pct:-}" "$tmax" "$fmin" "$seen" "$wall" >> "$SUMMARY"
      printf "[r%s] %-5s %-6s exit %s valid %-3s avg %8s ms  p99 %8s  max %8s  SH2 %s%%  %ss  %sC\n" \
        "$round" "$key" "$mode" "$rc" "$valid" "${avg:-?}" "${p99:-?}" "${mx:-?}" "${pct:-?}" "$wall" "$tmax"
    done
  done
done
rm -f "$OUT/.samples"

# Run with the median avg per title and mode (the progress-table convention), valid runs only.
MEDIANS=$OUT/medians.tsv
head -1 "$SUMMARY" > "$MEDIANS"
for entry in "${TITLES[@]}"; do
  key=${entry%%|*}
  for mode in "${MODES[@]}"; do
    rows=$(awk -F'\t' -v k="$key" -v m="$mode" '$1==k && $2==m && $5=="yes"' "$SUMMARY" | sort -t$'\t' -k6,6g)
    n=$(printf "%s" "$rows" | grep -c .)
    [ "$n" -gt 0 ] && printf "%s\n" "$rows" | sed -n "$(( (n + 1) / 2 ))p" >> "$MEDIANS"
  done
done
echo
column -t -s$'\t' "$MEDIANS"
echo "results: $OUT"
touch "$OUT/DONE"
