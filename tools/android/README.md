# Android build

The libretro core for RetroArch for Android, built with the Android NDK from the repository's CMake project. Run the
scripts here in bash on Linux (WSL or CI). They run on **Linux x86-64 hosts only**: `ndk_env.sh` (`ndk_llvm_bin`) uses
the NDK's `toolchains/llvm/prebuilt/linux-x86_64`, and `check_elf.sh` / `test_check_elf.sh` use the LLVM tools from
there. macOS and Linux arm64 hosts are not supported.

- `arm64-v8a`: the shipped ABI (`build-android-arm64-v8a/lib/brimir_libretro_android.so`).
- `x86_64`: built only for testing on the Android emulator, never released.

Two text files in this folder are for people, not for the scripts:

- `ANDROID-INSTALL.txt`: the short install text for users of the download (packaged with the core).
- `TEST-GUIDE.md`: the step-by-step test of a build on the developer's Galaxy A56, ending in the report template.

Build settings: Release, thin LTO (`-DBRIMIR_LTO=ON`), API 26 (`-DANDROID_PLATFORM=android-26`), static C++ runtime
(`-DANDROID_STL=c++_static`), ELF segments aligned for 16 KB pages, Vulkan renderer compiled in. The arm64 build has no
JIT (the SH-2 interpreter runs); the x86_64 emulator build has the x64 JIT.

## Install the NDK (once)

Prerequisites (Ubuntu 24.04 or WSL with it): `sudo apt install curl unzip cmake ninja-build`. CMake must be 3.28 or newer
(Ubuntu 22.04's 3.22 is too old).

The pinned NDK version is in `ndk-version.txt`: `30.0.16248370` = NDK r30, the 2026 LTS release. The release name, its
download file and SHA-1 below belong to that version: change them together with `ndk-version.txt` (the
[NDK downloads wiki](https://github.com/android/ndk/wiki) lists name, file, SHA-1 and version of each release). In WSL:

```bash
NDK_RELEASE=r30                                          # matches ndk-version.txt
NDK_SHA1=5107f898313790e449e87eee2183d9a20602dee9        # SHA-1 of android-ndk-$NDK_RELEASE-linux.zip
mkdir -p ~/android-ndk && cd ~/android-ndk
curl -fL -o android-ndk-$NDK_RELEASE-linux.zip https://dl.google.com/android/repository/android-ndk-$NDK_RELEASE-linux.zip
echo "$NDK_SHA1  android-ndk-$NDK_RELEASE-linux.zip" | sha1sum -c -
unzip -q android-ndk-$NDK_RELEASE-linux.zip && rm android-ndk-$NDK_RELEASE-linux.zip
grep Pkg.Revision android-ndk-$NDK_RELEASE/source.properties   # must equal ndk-version.txt
export ANDROID_NDK_HOME=~/android-ndk/android-ndk-$NDK_RELEASE   # add to ~/.bashrc to keep it
```

The unpacked NDK takes about 2 GB. An NDK installed by the Android SDK manager works too.

The script uses only the pinned version: `ANDROID_NDK_HOME` when its `Pkg.Revision` matches `ndk-version.txt`, otherwise
`$ANDROID_SDK_ROOT/ndk/<pinned version>` (or `$ANDROID_HOME/...`); with neither it stops with an error naming both. (The
GitHub ubuntu-24.04 runner's `ANDROID_NDK_HOME` points at another version; its SDK has `ndk/30.0.16248370`, which the
script then picks.) `BRIMIR_ALLOW_NDK_MISMATCH=1` accepts another version in `ANDROID_NDK_HOME` for local experiments; never
for a release build.

## Build

From the repository root:

```bash
bash tools/android/build.sh arm64-v8a           # the core
bash tools/android/build.sh x86_64 --tests      # the core plus brimir_tests and brimir_bench (emulator testing)
```

The script configures `build-android-<abi>` (Ninja), builds `brimir_libretro` (and with `--tests` also `brimir_tests` and
`brimir_bench`) and prints the core's path on its last line. Without `--tests` it configures with
`-DBRIMIR_BUILD_TESTS=OFF`.

From Windows PowerShell, through WSL:

```powershell
wsl -e bash -lc "export ANDROID_NDK_HOME=~/android-ndk/android-ndk-r30 && cd /mnt/c/path/to/Brimir && bash tools/android/build.sh arm64-v8a"
```

## Check the core

```bash
bash tools/android/check_elf.sh build-android-arm64-v8a/lib/brimir_libretro_android.so arm64-v8a
bash tools/android/test_check_elf.sh   # tests the checker itself
```

`check_elf.sh <core.so> <arm64-v8a|x86_64>` prints `check_elf: ok` (exit 0) when the core is fit to ship, otherwise one
`FAIL: <reason>` line per failed check (exit 1):

- `machine`: the ELF machine matches the ABI (64-bit AArch64 or x86-64).
- `DT_NEEDED`: the core needs only `libc.so`, `libm.so`, `libdl.so` and `liblog.so` (the C++ runtime is static).
- `alignment`: every `PT_LOAD` segment is aligned to at least 0x4000, so the core loads on 16 KB page devices.
- `exports`: the exported dynamic symbols are exactly the libretro API in `libretro_exports.txt` (the `retro_*`
  functions the core defines, as in the Linux build's `nm -D`); update the list when the core implements another
  libretro function.
- `name`: the file is `brimir_libretro_android.so`.

It uses `llvm-readelf` and `llvm-nm` of the pinned NDK, found as `build.sh` finds it (`ndk_env.sh`).
`test_check_elf.sh` builds tiny shared objects with the NDK clang that each break one check (4 KB alignment, a
`libc++_shared.so` dependency, an extra exported symbol, a missing one (`retro_reset`), a wrong file name, the wrong
machine) and expects each to fail for that reason only; it also checks a Linux x64 core as `arm64-v8a` (`HOST_CORE`,
default `build-wsl/lib/brimir_libretro.so`) and expects the built Android cores to pass (`ARM64_CORE`, required;
`X86_64_CORE`). Unset, `HOST_CORE` and `X86_64_CORE` are skipped when their default file is missing; set to a path, a
missing file fails; `skip:<reason>` skips with that reason (CI sets them all).

## Package

```bash
bash tools/android/package.sh build-android-arm64-v8a/lib/brimir_libretro_android.so brimir-android-arm64.zip
bash tools/android/test_package.sh   # tests the packager with stand-in files
```

`package.sh <core.so> <out.zip>` writes a plain zip (no folder) with `brimir_libretro_android.so`,
`brimir_libretro.info` (from `resources/info`) and `ANDROID-INSTALL.txt`. The core in the zip is the input after the
NDK's `llvm-strip --strip-unneeded` (about 57 MB -> 6 MB): the dynamic symbols and the build-id stay, the symbol table
and debug info go. The unstripped input is not changed; keep it for symbolicating crash reports (same build-id). Run
`check_elf.sh` on the stripped core too (unzip it; CI does).

`check_release_text.sh <install text> <git ref>` is the release guard: it fails (exit 1) when the ref is a tag
(`refs/tags/...`) and the text still says "in testing" (the A56 line; see "Before the first Android release" in
`TEST-GUIDE.md`); any other ref passes, so dry runs work. `test_check_release_text.sh` tests it with the two texts in
`testdata/`.

## CI and release

- `.github/workflows/ci.yml`, job `build-android` (every push): the release guard, `build.sh arm64-v8a`, a Linux x64
  core (host compiler, for the `HOST_CORE` case), `check_elf.sh` on the unstripped and the stripped core,
  `test_check_elf.sh`, `test_package.sh`, `test_check_release_text.sh`; uploads `brimir-android-arm64` (the zip) and
  `brimir-android-arm64-unstripped` (the unstripped core, for crash symbolication).
- Job `android-emulator-tests`: `build.sh x86_64 --tests`, `check_elf.sh` on the x86_64 core, then
  `emulator_tests.sh build-android-x86_64/bin/brimir_tests "~[gpu]"` on an API 34 `google_apis` x86_64 emulator
  (reactivecircus/android-emulator-runner, KVM). No BIOS, so the BIOS cases skip. No case is excluded. It does not
  gate releases (yet).
- `.github/workflows/release.yml`, job `build-android-arm64`: the same steps as `build-android` but without the host core
  (`HOST_CORE=skip:...`); the release job (tags only) runs the release guard again and publishes the zip as
  `brimir-<tag>-android-arm64.zip`. A run started by hand on a branch (dry run) builds and uploads the same artifacts;
  its `brimir-android-arm64` artifact is the test build for the phone.
- The NDK comes from the runner's SDK (`$ANDROID_HOME/ndk/<pin>`, installed with `sdkmanager` when missing); the build
  logs print the NDK path and version (`Android NDK: ...`).

The Android build compiles every target with hidden visibility and links the core with `-Wl,--exclude-libs,ALL` and the
version script `src/libretro/android_exports.map` (`retro_*` only; libc++ marks some of its typeinfo default, which hidden
visibility alone does not hide). This is safe for RetroArch's run-ahead second instance, which loads the core twice into
one process: each copy keeps its own static C++ runtime and its own globals (no symbol of one copy can bind to the other,
which the hidden exports make more certain, not less), and the hardware-context claim that makes the second copy decline
Vulkan (`src/libretro/hw_context_owner.*`) uses process memory found through an environment variable, not exported
symbols, and is unchanged.

## Test on the Android emulator

The x86_64 build runs in the SDK's Android emulator (x86_64 images, hardware acceleration: WHPX on Windows, KVM on
Linux). The scripts are bash; adb is taken from `ADB` (default `adb`). From WSL with the Windows SDK:
`export ADB=/mnt/c/Users/<you>/AppData/Local/Android/Sdk/platform-tools/adb.exe` (the scripts convert local paths with
`wslpath` when `ADB` ends in `.exe`).

### Emulator setup (once)

```powershell
# SDK command-line tools in <SDK>\cmdline-tools\latest; Java 17
sdkmanager "system-images;android-34;google_apis;x86_64"            # google_apis (not Play): adb root works
avdmanager create avd -n brimir-x86_64 -k "system-images;android-34;google_apis;x86_64" -d pixel_6
sdkmanager "system-images;android-35;google_apis_ps16k;x86_64"      # optional: 16 KB pages
avdmanager create avd -n brimir-x86_64-16k -k "system-images;android-35;google_apis_ps16k;x86_64" -d pixel_6
emulator -avd brimir-x86_64 -no-window -no-audio -no-snapshot -no-boot-anim -gpu host
adb emu kill                                                        # when done
```

Use `-gpu host`: the guest then gets the host GPU's Vulkan through gfxstream (an RX 9060 XT reported Vulkan 1.3).
With `-gpu swiftshader_indirect` the guest reports SwiftShader Vulkan 1.2, and RetroArch's Vulkan video driver killed
the emulator (host log `Failed to unbox VkDevice`), so the Vulkan scenario needs `-gpu host`.

RetroArch: the universal APK from the libretro buildbot (`https://buildbot.libretro.com/stable/1.22.2/android/RetroArch.apk`),
`adb install RetroArch.apk`. RetroArch 1.22.2 does not start on a 16 KB-page system: its own
`libretroarch-activity.so` has 4 KB-aligned segments and the linker crashes loading it. The nightly
(`https://buildbot.libretro.com/nightly/android/RetroArch.apk`, November 2026) is 16 KB aligned and runs there.

### Installing the core in RetroArch

The user's route, checked without root on the Android 14 image with RetroArch 1.22.2 (screenshots and logs in the
task 5 evidence, `fix1/install-route/`): copy `brimir_libretro_android.so` to the phone's `Download` folder; in
RetroArch, Load Core > Install or Restore a Core (it opens `/storage/emulated/0/RetroArch/downloads`; go up twice to
`/storage/emulated/0/`, open `Download`) and tap the file. RetroArch copies it unchanged (same SHA-256) to its cores
folder under the same name, `brimir_libretro_android.so`, and returns to the main menu. Load Core then lists it:

- without `brimir_libretro.info` as `brimir_libretro_android.so` ("License: N/A"); the core loads, the title bar shows
  `Brimir (0.8.1)` (from the core itself), Start Core runs the BIOS menu (`Performance Profile (300 frames)` in the log
  with the BIOS in `/storage/emulated/0/RetroArch/system/` and `brimir_profiling` enabled in
  `RetroArch/config/Brimir/Brimir.opt`), and run-ahead with the second instance behaves as with the root install (both
  copies loaded, no `Failed to create second instance`);
- with a user-writable Core Info folder (`libretro_info_path` set to `/storage/emulated/0/RetroArch/info`, holding
  `brimir_libretro.info`) as "Sega - Saturn (Brimir)", "License: GPLv3"; Information > Core Information shows the
  info file's metadata and the firmware list with present/missing status. That folder replaces the bundled one (other
  cores' info files are then not found unless copied there too). The app's own info folder
  `/data/user/0/<package>/info/` needs root.

After run-ahead with the second instance, a `retroarch_temp` folder (the copy RetroArch loads as the second instance)
can appear in the Load Core list when RetroArch was closed without cleaning up.

Package names: the universal APK is `com.retroarch`; the arm64 APK (`RetroArch_aarch64.apk`, label "RetroArch
(AArch64)", native code arm64-v8a and x86_64) is `com.retroarch.aarch64`, so its folders are
`/data/user/0/com.retroarch.aarch64/...` and `/storage/emulated/0/Android/data/com.retroarch.aarch64/files/retroarch.cfg`;
the activity classes are the same (`com.retroarch.browser.mainmenu.MainMenuActivity`,
`com.retroarch.browser.retroactivity.RetroActivityFuture`). Both 1.22.2 APKs target SDK 28.

RetroArch loads cores from `/data/user/0/com.retroarch/cores/` (`libretro_directory`); the folder exists once RetroArch
has run. The scripts install as root: copy `brimir_libretro_android.so` there, owned by the app's user, with the SELinux label of the app's
data folder (`chcon`; `restorecon` right after a reinstall can apply the previous install's categories, and the app is
then denied). `brimir_libretro.info` goes to `/data/user/0/com.retroarch/info/`: RetroArch matches it to
`brimir_libretro_android.so` (it drops the `_android` suffix). Without it the core still loads, starts without content
and runs run-ahead with a second instance (RetroArch assumes deterministic save states and learns "no content needed"
from the core); the info file supplies the display name, the firmware list and the database.

### Scripts

```bash
bash tools/android/build.sh x86_64 --tests
# RetroArch end to end: BIOS menu, device report, 300 frames, run-ahead second instance (software and Vulkan)
bash tools/android/e2e_retroarch.sh build-android-x86_64/lib/brimir_libretro_android.so <bios-dir>
# Frame hashes: Android bench against a host bench, BIOS window (add <disc> <state> <rtc-dir> for a game window)
bash tools/android/e2e_bench_hash.sh build-android-x86_64/bin/brimir_bench build/bin/brimir_bench.exe <bios-dir>/mpr-17933.bin
# Unit tests on the device (Catch2 arguments pass through)
bash tools/android/emulator_tests.sh build-android-x86_64/bin/brimir_tests "~[gpu]"
```

- `e2e_retroarch.sh <core.so> <bios-dir> [software|runahead-software|runahead-vulkan ...]` needs `adb root`, RetroArch
  installed and `mpr-17933.bin` in `<bios-dir>`. It installs the core and info file, writes a scratch configuration
  (logging to a file, `brimir_profiling=enabled`, system folder with the BIOS copy) in the app's data folder, starts
  RetroArch through an intent (`-e LIBRETRO <core> -e CONFIGFILE <cfg>`; no `ROM`), and taps "Start Core": without
  content RetroArch opens its menu with the core loaded, injected key events do not reach the menu and its network
  command interface is not built for Android (`E2E_TAP="x y"` overrides the tap position). After `E2E_WAIT` seconds
  (20) it collects RetroArch's log, logcat, the process maps and a screenshot into `E2E_OUT` and checks: `Brimir <version>
  initializing`, the `Device report:` line, `Performance Profile (300 frames)`; with run-ahead also two `retro_init`
  calls (the second copy, which RetroArch copies to `cores/retroarch_temp/` and loads into the same process); software:
  no `Failed to create second instance` and both copies mapped; Vulkan: the core's decline message and RetroArch's
  single-instance fallback (or, when the GPU misses the baseline, the software fallback in the device report). Exit 0
  only when every check holds. Scenario names are checked before anything runs; the scratch folder (with the BIOS
  copy) is removed on exit, also after a failure. `PKG` selects the RetroArch package (e.g. `com.retroarch.aarch64`).
  It leaves on the device: the core (and info file) in RetroArch's folders, the storage permission granted (`pm grant`),
  `immersive_mode_confirmations` set, adbd as root. Local only: the "Start Core" tap makes it unsuitable for CI.
- `e2e_bench_hash.sh` runs `--hash --audio-hash --fixed-rtc --frames $FRAMES --warmup 0 $BENCH_ARGS` on both sides,
  requires `$FRAMES` frame and `$FRAMES` audio hash lines on each side and compares them. The device folder with the
  BIOS/disc copies is removed on exit, also after a failure.
- Both write their logs to `E2E_OUT` / `HASH_OUT`, by default a new `mktemp -d` folder (printed), never inside the
  repository.
- `emulator_tests.sh` copies `brimir_tests` and `tests/fixtures` (without `*.bin` / `*.ic1` images) to
  `/data/local/tmp/brimir` and runs the binary there with `BRIMIR_TEST_FIXTURES_DIR=/data/local/tmp/brimir/fixtures`
  (the tests' fixtures folder, by default the source tree's) and `TMPDIR=/data/local/tmp/brimir/tmp`; it needs no root
  and works whatever the checkout path or the place the binary was built (CI). `BIOS_DIR=<folder>` adds BIOS images
  (otherwise the BIOS cases skip). `RUN_AS=shell` (default), `root`, a uid or an app package (e.g. `com.retroarch`:
  what an app may do; needs `adb root`); `TEST_ENV="TMPDIR="` unsets TMPDIR (libc++'s Android default is
  `/data/local/tmp`, writable for the shell user, not for apps). Everything it created is removed on exit.

### What was proven (2026-11-15, emulator 36.1.9, x86_64 core from this branch)

On Android 14 (`google_apis`, 4 KB pages) with RetroArch 1.22.2, and on Android 15 `google_apis_ps16k` (16 KB pages)
with the RetroArch nightly:

- RetroArch loads the core and runs the BIOS menu (first-boot language screen) for well over 300 frames, software and
  Vulkan renderer (`Device report: renderer vulkan; GPU "AMD Radeon RX 9060 XT"; Vulkan API 1.3.0`).
- Run-ahead with the second instance: with the software renderer both copies of the core are loaded in one process and
  run; with the Vulkan renderer the second copy declines and RetroArch falls back to run-ahead on one instance while
  frames keep advancing.
- `brimir_bench` on the emulator and the Windows x64 build give identical frame and audio hashes for 300 frames of the
  BIOS menu and of a game window from a save state, with the interpreter and with the SH-2 JIT (the x86_64 build
  has the x64 JIT).
- `brimir_tests "~[gpu]"` passes on both images as the shell user (one case skips on every platform by design). One
  intermittent failure was seen on the Android 14 image: "Sprite layer and compose fast paths match with the threaded
  VDP2 and deinterlace renderers" (2 of 40 repeats; 0 of 40 on Windows). It was a race in the threaded VDP renderer
  (TVMD read from the VDP2 thread's copy), fixed since (`src/core/BRIMIR_FORK.md`; 0 of 200 on a 2-vCPU emulator).

Not covered by the emulator: API 26 devices, arm64 code (the A56 test covers it), real-device GPU drivers.
