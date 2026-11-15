#!/usr/bin/env bash
# RetroArch for Android end-to-end check on a running emulator (or rooted device):
#   e2e_retroarch.sh <core.so> <bios-dir> [scenario ...]
#
# Installs the core in RetroArch's cores directory, starts RetroArch through an intent with the core and no content
# (the BIOS menu), lets it run, collects its log and checks it. Scenarios (default: all three):
#   software            software renderer, no run-ahead
#   runahead-software   run-ahead 2 frames with "Use Second Instance", software renderer
#   runahead-vulkan     the same with brimir_renderer=vulkan (the second copy must decline, RetroArch falls back to
#                       run-ahead on the main instance; when the emulator's Vulkan misses the core's baseline the
#                       Device report shows the software fallback instead)
# Exit 0 only when every assertion of every scenario holds.
#
# Needs: RetroArch installed, `adb root` allowed (a google_apis userdebug image), the US BIOS mpr-17933.bin in
# <bios-dir> (only read; pushed into a scratch directory in the app's data folder, removed on exit, also after a
# failure).
#
# Local only, never in CI: starting the core needs a tap on RetroArch's "Start Core" menu entry (see below).
#
# Lasting changes on the device (left in place for re-runs): the core in <app data>/cores/brimir_libretro_android.so
# and, with E2E_INFO=1, brimir_libretro.info in <app data>/info/ (both owned by the app); RetroArch granted
# READ/WRITE_EXTERNAL_STORAGE (pm grant); `settings put secure immersive_mode_confirmations confirmed`; adbd running
# as root. RetroArch's own retroarch.cfg is not touched (the run uses its own configuration file).
#
# Environment:
#   ADB        adb command (default `adb`). From WSL with the Windows SDK:
#              ADB=/mnt/c/Users/<you>/AppData/Local/Android/Sdk/platform-tools/adb.exe (local paths are converted
#              with wslpath when ADB ends in .exe)
#   PKG        RetroArch's package (default com.retroarch)
#   E2E_WAIT   seconds RetroArch runs per scenario (default 20)
#   E2E_OUT    local output folder for logs and screenshots (default: a new mktemp -d folder, printed)
#   E2E_INFO   1 (default) installs resources/info/brimir_libretro.info; 0 removes it from the info directory
#   E2E_TAP    "x y" of the "Start Core" entry (default computed for a portrait phone)
#   E2E_KEEP   1 keeps the scratch folder on the device
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <core.so> <bios-dir> [software|runahead-software|runahead-vulkan ...]" >&2
    exit 2
fi
CORE=$1
BIOS_DIR=$2
shift 2
SCENARIOS=("$@")
[ ${#SCENARIOS[@]} -eq 0 ] && SCENARIOS=(software runahead-software runahead-vulkan)
for s in "${SCENARIOS[@]}"; do
    case "$s" in
        software | runahead-software | runahead-vulkan) ;;
        *) echo "error: unknown scenario $s (software, runahead-software, runahead-vulkan)" >&2; exit 2 ;;
    esac
done

ADB=${ADB:-adb}
WAIT=${E2E_WAIT:-20}
OUT=${E2E_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/brimir-e2e-retroarch.XXXXXX")}
INSTALL_INFO=${E2E_INFO:-1}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
BIOS=$BIOS_DIR/mpr-17933.bin
VERSION=$(sed -n 's/^ *VERSION \([0-9][0-9.]*\).*/\1/p' "$REPO/CMakeLists.txt" | head -n1)
VERSION_RE=${VERSION//./\\.}

PKG=${PKG:-com.retroarch}
# The activity class keeps RetroArch's Java package whatever the application id
ACTIVITY=$PKG/com.retroarch.browser.retroactivity.RetroActivityFuture
DATA=/data/user/0/$PKG
CORES=$DATA/cores
INFO_DIR=$DATA/info
E2E=$DATA/brimir-e2e
EXT=/storage/emulated/0/Android/data/$PKG/files
CORE_NAME=brimir_libretro_android.so

for f in "$CORE" "$BIOS"; do
    [ -f "$f" ] || { echo "error: missing $f" >&2; exit 2; }
done

# Local path as adb sees it: adb.exe (Windows) run from WSL needs Windows paths.
host_path() {
    if [[ "$ADB" == *.exe ]] && command -v wslpath >/dev/null 2>&1; then wslpath -w "$1"; else printf '%s' "$1"; fi
}
adbq() { "$ADB" "$@"; }
sh_root() { "$ADB" shell "$*" | tr -d '\r'; }

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
echo "output: $OUT"

device_ready=0
cleanup() {
    if [ $device_ready = 1 ]; then
        "$ADB" shell "am force-stop $PKG; rm -f /data/local/tmp/brimir-e2e.png" >/dev/null 2>&1 || true
        [ "${E2E_KEEP:-0}" = 1 ] || "$ADB" shell "rm -rf $E2E" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

adbq wait-for-device
if [ "$(sh_root id -u)" != "0" ]; then
    adbq root >/dev/null
    sleep 2
    adbq wait-for-device
fi
[ "$(sh_root id -u)" = "0" ] || { echo "error: adb root is not available on this device" >&2; exit 2; }
device_ready=1
sh_root pm path $PKG | grep -q package: || { echo "error: $PKG is not installed" >&2; exit 2; }
APK=$(sh_root pm path $PKG | head -n1 | sed 's/^package://')
APP_UID=$(sh_root stat -c %u $DATA)
APP_GID=$(sh_root stat -c %g $DATA)
# "Start Core" in RGUI: line ~31 of the 240-line menu picture, which is 3/4 of the screen width tall and starts below
# the display cutout inset.
if [ -n "${E2E_TAP:-}" ]; then
    read -r TAP_X TAP_Y <<<"$E2E_TAP"
else
    SCREEN_W=$(sh_root wm size | sed -n 's/.*size: \([0-9]*\)x.*/\1/p' | tail -n1)
    TOP=$(sh_root dumpsys window displays | sed -n 's/.*mDisplayCutout=DisplayCutout{insets=Rect(0, \([0-9]*\) - .*/\1/p' | head -n1)
    TOP=${TOP:-0}
    TAP_X=$((SCREEN_W / 6))
    TAP_Y=$((TOP + SCREEN_W * 3 / 4 * 31 / 240))
fi
echo "device: $(sh_root getprop ro.build.fingerprint); page size $(sh_root getconf PAGE_SIZE); RetroArch $(sh_root dumpsys package $PKG | sed -n 's/.*versionName=//p' | head -n1); uid $APP_UID"

# Without the storage permission RetroArch 1.22.2 derives its default folders from an uninitialised buffer (garbage
# paths in its log); the launcher activity would ask for it, the intent used here does not.
sh_root am force-stop $PKG
sh_root pm grant $PKG android.permission.READ_EXTERNAL_STORAGE
sh_root pm grant $PKG android.permission.WRITE_EXTERNAL_STORAGE
# No "Viewing full screen" hint over the first fullscreen start.
sh_root settings put secure immersive_mode_confirmations confirmed

# Core (and info file) into RetroArch's own directories, owned by the app.
sh_root mkdir -p $CORES $INFO_DIR
adbq push "$(host_path "$CORE")" $CORES/$CORE_NAME >/dev/null
if [ "$INSTALL_INFO" = 1 ]; then
    adbq push "$(host_path "$REPO/resources/info/brimir_libretro.info")" $INFO_DIR/brimir_libretro.info >/dev/null
else
    sh_root rm -f $INFO_DIR/brimir_libretro.info
fi
sh_root rm -rf $CORES/retroarch_temp
sh_root chown -R $APP_UID:$APP_GID $CORES $INFO_DIR
sh_root chmod 0771 $CORES $INFO_DIR
sh_root chmod 0644 $CORES/$CORE_NAME
# The SELinux label of the app's own data folder (restorecon right after a reinstall can apply the previous install's
# MLS categories, which the app may then not open).
APP_CTX=$(sh_root ls -Zd $DATA | awk '{print $1}')
sh_root chcon -R "$APP_CTX" $CORES $INFO_DIR

# One scenario: fresh scratch tree (config, core options, system dir with the BIOS, logs), run, collect, check.
FAILED=0
run_scenario() {
    local name=$1 renderer=software runahead=false
    case "$name" in
        software) ;;
        runahead-software) runahead=true ;;
        runahead-vulkan) runahead=true renderer=vulkan ;;
    esac
    local video=gl
    [ "$renderer" = vulkan ] && video=vulkan

    local tmp
    tmp=$(mktemp -d)
    cat >"$tmp/retroarch.cfg" <<EOF
libretro_directory = "$CORES"
libretro_info_path = "$INFO_DIR"
system_directory = "$E2E/system"
savefile_directory = "$E2E/saves"
savestate_directory = "$E2E/states"
rgui_config_directory = "$E2E/config"
cache_directory = "$E2E/cache"
log_dir = "$E2E/logs"
log_to_file = "true"
log_to_file_timestamp = "false"
log_verbosity = "true"
frontend_log_level = "0"
libretro_log_level = "0"
core_info_cache_enable = "false"
config_save_on_exit = "false"
global_core_options = "true"
core_options_path = "$E2E/core-options.cfg"
video_driver = "$video"
menu_driver = "rgui"
pause_nonactive = "false"
audio_driver = "null"
run_ahead_enabled = "$runahead"
run_ahead_frames = "2"
run_ahead_secondary_instance = "true"
run_ahead_hide_warnings = "false"
EOF
    cat >"$tmp/core-options.cfg" <<EOF
brimir_profiling = "enabled"
brimir_renderer = "$renderer"
EOF
    sh_root rm -rf $E2E
    sh_root mkdir -p $E2E/system $E2E/saves $E2E/states $E2E/config $E2E/cache $E2E/logs
    adbq push "$(host_path "$tmp/retroarch.cfg")" $E2E/retroarch.cfg >/dev/null
    adbq push "$(host_path "$tmp/core-options.cfg")" $E2E/core-options.cfg >/dev/null
    adbq push "$(host_path "$BIOS")" $E2E/system/mpr-17933.bin >/dev/null
    rm -rf "$tmp"
    sh_root chown -R $APP_UID:$APP_GID $E2E
    sh_root chcon -R "$APP_CTX" $E2E

    echo "== $name (renderer $renderer, video_driver $video, run-ahead $runahead)"
    sh_root am force-stop $PKG
    adbq logcat -c
    sh_root am start -W -n $ACTIVITY \
        -e LIBRETRO $CORES/$CORE_NAME -e CONFIGFILE $E2E/retroarch.cfg \
        -e DATADIR $DATA -e APK "$APK" -e SDCARD /storage/emulated/0 -e EXTERNAL $EXT \
        -e IME com.android.inputmethod.latin/.LatinIME >"$OUT/$name.am.txt"
    # An intent without content opens RetroArch's menu with the core loaded and "Start Core" as the first entry
    # (RetroArch passes --menu when there is no content). Neither injected keyevents (`input keyevent ENTER`,
    # DPAD_CENTER, X) nor the network command interface (not compiled into the Android build) reach the menu; a tap on
    # the entry does (RGUI, portrait: the 4:3 menu picture spans the screen width below the top inset).
    local t started=0
    for t in 1 2 3 4 5; do
        sleep 4
        sh_root input tap $TAP_X $TAP_Y
        sleep 2
        if sh_root "grep -q 'Loading game' $E2E/logs/retroarch.log"; then started=1; break; fi
    done
    if [ $started = 1 ]; then
        echo "  core started after tap $t at ($TAP_X,$TAP_Y); running ${WAIT}s"
    else
        echo "  core did not start (tap at $TAP_X,$TAP_Y; set E2E_TAP=\"x y\" to the 'Start Core' entry)"
    fi
    sleep "$WAIT"
    local pid
    pid=$(sh_root pidof $PKG || true)
    adbq logcat -d >"$OUT/$name.logcat.txt"
    sh_root screencap -p /data/local/tmp/brimir-e2e.png
    adbq pull /data/local/tmp/brimir-e2e.png "$(host_path "$OUT/$name.png")" >/dev/null || true
    sh_root rm -f /data/local/tmp/brimir-e2e.png
    sh_root ls -la $CORES $CORES/retroarch_temp >"$OUT/$name.cores-ls.txt" 2>&1 || true
    [ -n "$pid" ] && sh_root cat /proc/$pid/maps | grep -i brimir >"$OUT/$name.maps.txt" || true
    sh_root am force-stop $PKG
    sleep 1
    adbq exec-out cat $E2E/logs/retroarch.log >"$OUT/$name.retroarch.log" 2>/dev/null || true

    local log="$OUT/$name.all.txt"
    cat "$OUT/$name.retroarch.log" "$OUT/$name.logcat.txt" >"$log"
    local ok=1
    check() { # description, grep -E pattern, expect (1 present / 0 absent)
        if grep -qE "$2" "$log"; then
            if [ "$3" = 1 ]; then echo "  ok   $1: $(grep -m1 -oE "$2.{0,160}" "$log")"; else echo "  FAIL $1 (found): $(grep -m1 -E "$2" "$log")"; ok=0; fi
        else
            if [ "$3" = 1 ]; then echo "  FAIL $1 (missing)"; ok=0; else echo "  ok   $1 (absent)"; fi
        fi
    }
    [ -n "$pid" ] || { echo "  FAIL RetroArch was not running after ${WAIT}s"; ok=0; }
    check "core version" "Brimir $VERSION_RE initializing" 1
    check "device report" "Device report: " 1
    check "300 frames ran" "Performance Profile \(300 frames\)" 1
    if [ "$runahead" = true ]; then
        # RetroArch copies the core to <cores>/retroarch_temp/ and dlopens the copy as the second instance.
        local inits
        inits=$(grep -cE "Brimir $VERSION_RE initializing" "$OUT/$name.retroarch.log" || true)
        if [ "$inits" -ge 2 ]; then echo "  ok   second copy initialised ($inits retro_init calls)"; else
            echo "  FAIL second copy initialised ($inits retro_init calls)"; ok=0; fi
        if [ "$renderer" = software ]; then
            check "second instance created" "Failed to create second(ary)? instance" 0
            # (the maps name the folder /data/user/0/<pkg> or its alias /data/data/<pkg>, depending on the release)
            if grep -q "$PKG/cores/$CORE_NAME" "$OUT/$name.maps.txt" && grep -q "$PKG/cores/retroarch_temp/$CORE_NAME" "$OUT/$name.maps.txt"; then
                echo "  ok   both copies mapped in process $pid: $(awk '{print $NF}' "$OUT/$name.maps.txt" | sort -u | tr '\n' ' ')"
            else
                echo "  FAIL both copies mapped in process $pid (see $OUT/$name.maps.txt)"; ok=0
            fi
        else
            if grep -qE "Device report: renderer vulkan" "$log"; then
                check "second copy declines Vulkan" "Another instance of Brimir in this process renders through the hardware context" 1
                check "RetroArch single-instance fallback" "Failed to create second(ary)? instance" 1
            else
                echo "  note Vulkan not used (emulator's Vulkan misses the baseline): $(grep -m1 -oE 'Device report: .*' "$log")"
                check "software fallback reported" 'Device report: renderer software; .*fallback "[^n]' 1
            fi
        fi
    fi
    if [ $ok = 1 ]; then echo "  PASS $name"; else echo "  FAILED $name"; FAILED=1; fi
}

for s in "${SCENARIOS[@]}"; do
    run_scenario "$s"
done
echo "output: $OUT"
exit $FAILED
