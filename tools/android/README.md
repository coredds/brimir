# Android build

The libretro core for RetroArch for Android, built with the Android NDK from the repository's CMake project. Run the
scripts here in bash on Linux (WSL or CI).

- `arm64-v8a`: the shipped ABI (`build-android-arm64-v8a/lib/brimir_libretro_android.so`).
- `x86_64`: built only for testing on the Android emulator, never released.

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
`libc++_shared.so` dependency, an extra exported symbol, a wrong file name, the wrong machine) and expects each to fail
for that reason only; it also checks the Linux x64 core in `build-wsl` as `arm64-v8a` (skipped when missing) and expects
the built Android cores to pass.

The Android build compiles every target with hidden visibility and links the core with `-Wl,--exclude-libs,ALL` and the
version script `src/libretro/android_exports.map` (`retro_*` only; libc++ marks some of its typeinfo default, which hidden
visibility alone does not hide). This is safe for RetroArch's run-ahead second instance, which loads the core twice into
one process: each copy keeps its own static C++ runtime and its own globals (no symbol of one copy can bind to the other,
which the hidden exports make more certain, not less), and the hardware-context claim that makes the second copy decline
Vulkan (`src/libretro/hw_context_owner.*`) uses process memory found through an environment variable, not exported
symbols, and is unchanged.
