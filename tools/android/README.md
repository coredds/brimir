# Android build

The libretro core for RetroArch for Android, built with the Android NDK from the repository's CMake project. Run the
scripts here in bash on Linux (WSL or CI).

- `arm64-v8a`: the shipped ABI (`build-android-arm64-v8a/lib/brimir_libretro_android.so`).
- `x86_64`: built only for testing on the Android emulator, never released.

Build settings: Release, thin LTO (`-DBRIMIR_LTO=ON`), API 26 (`-DANDROID_PLATFORM=android-26`), static C++ runtime
(`-DANDROID_STL=c++_static`), ELF segments aligned for 16 KB pages, Vulkan renderer compiled in. The arm64 build has no
JIT (the SH-2 interpreter runs); the x86_64 emulator build has the x64 JIT.

## Install the NDK (once)

The pinned NDK version is in `ndk-version.txt` (`27.2.12479018` = NDK r27c). In WSL (Ubuntu; needs `curl`, `unzip`,
`cmake` >= 3.28 and `ninja`):

```bash
mkdir -p ~/android-ndk && cd ~/android-ndk
curl -fL -o android-ndk-r27c-linux.zip https://dl.google.com/android/repository/android-ndk-r27c-linux.zip
sha1sum android-ndk-r27c-linux.zip   # 090e8083a715fdb1a3e402d0763c388abb03fb4e
unzip -q android-ndk-r27c-linux.zip && rm android-ndk-r27c-linux.zip
grep Pkg.Revision android-ndk-r27c/source.properties   # Pkg.Revision = 27.2.12479018
export ANDROID_NDK_HOME=~/android-ndk/android-ndk-r27c   # add to ~/.bashrc to keep it
```

The unpacked NDK takes about 2 GB. An NDK installed by the Android SDK manager works too: the script then uses
`$ANDROID_SDK_ROOT/ndk/<version from ndk-version.txt>` when `ANDROID_NDK_HOME` is not set.

## Build

From the repository root:

```bash
bash tools/android/build.sh arm64-v8a           # the core
bash tools/android/build.sh x86_64 --tests      # the core plus brimir_tests and brimir_bench (emulator testing)
```

The script configures `build-android-<abi>` (Ninja), builds `brimir_libretro` (and with `--tests` also `brimir_tests` and
`brimir_bench`) and prints the core's path on its last line. It warns when the NDK's version differs from
`ndk-version.txt` and stops with a message when no NDK is found.

From Windows PowerShell, through WSL:

```powershell
wsl -e bash -lc "export ANDROID_NDK_HOME=~/android-ndk/android-ndk-r27c && cd /mnt/c/path/to/Brimir && bash tools/android/build.sh arm64-v8a"
```
