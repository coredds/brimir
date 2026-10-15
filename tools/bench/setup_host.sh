#!/bin/bash
# Prepare a dedicated Debian/Ubuntu-based Linux device as a Brimir benchmark host.
# Run once as a user with sudo. See tools/README.md.
#
# - installs the build and measurement tools,
# - pins the CPU frequency governor to "performance" now and at every boot,
# - creates the data dir layout used by run_bench.sh and run_lockstep.sh.
#
# usage: setup_host.sh [data dir]               default: /mnt/data (mount any extra storage there first)
set -eu
DATA=${1:-${BRIMIR_BENCH_DATA:-/mnt/data}}

sudo DEBIAN_FRONTEND=noninteractive apt-get update -q
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
  build-essential gcc g++ cmake ninja-build clang lld git linux-cpupower hyperfine time stress-ng
# perf: linux-perf on Debian, linux-tools-* on Ubuntu; not fatal if neither matches this kernel.
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends linux-perf 2>/dev/null \
  || sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends "linux-tools-$(uname -r)" 2>/dev/null \
  || echo "warning: could not install perf for kernel $(uname -r)"

sudo tee /etc/systemd/system/cpu-performance.service >/dev/null <<'EOF'
[Unit]
Description=Set CPU governor to performance
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/usr/bin/cpupower frequency-set -g performance

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable --now cpu-performance.service

mkdir -p "$DATA"/{bios/system,games,src,build,results}
echo
echo "governor : $(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | sort | uniq -c | xargs)"
echo "data dir : $DATA"
echo "next     : copy the BIOS images to $DATA/bios, the configured brimir_saturn_rtc_<jp|us_eu>.smpc"
echo "           files to $DATA/bios/system and the six reference CHDs to $DATA/games (tools/README.md)."
