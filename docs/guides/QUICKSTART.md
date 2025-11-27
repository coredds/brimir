# Brimir Quick Start Guide

This guide will help you get started with Brimir quickly, whether you're a user or developer.

---

## For Users

Brimir is a functional Sega Saturn emulator core for RetroArch. Follow these steps to get started:

### Installing via RetroArch (Coming Soon)

> **Note:** Brimir is not yet in the official libretro buildbot. Use manual installation for now.

1. Open RetroArch
2. Go to **Online Updater** → **Core Downloader**
3. Find and download **Sega - Saturn (Brimir)**
4. Done!

### Manual Installation

1. Download `brimir_libretro.dll` from [Releases](https://github.com/coredds/brimir/releases)
2. Copy to RetroArch's `cores\` directory:
   ```
   C:\RetroArch-Win64\cores\brimir_libretro.dll
   ```
3. Copy `brimir_libretro.info` to RetroArch's `info\` directory:
   ```
   C:\RetroArch-Win64\info\brimir_libretro.info
   ```
4. Restart RetroArch

### BIOS Setup

Brimir requires Sega Saturn BIOS files. Place them in RetroArch's `system\` directory:

```
C:\RetroArch-Win64\system\
├── sega_101.bin                       # USA v1.01 (recommended)
├── mpr-17933.bin                      # USA v1.00
├── sega_100.bin                       # Europe v1.00
├── Sega Saturn BIOS (EUR).bin         # Europe
├── Sega Saturn BIOS v1.01 (JAP).bin  # Japan v1.01 (recommended)
└── Sega Saturn BIOS v1.00 (JAP).bin  # Japan v1.00
```

**At least one BIOS is required.** The core auto-detects available files and lets you choose via **Quick Menu → Options → BIOS**.

**⚠️ IMPORTANT:** Do NOT use `sega1003.bin` (Japanese BIOS v1.003) - it is not supported.

**Note:** You must obtain these files legally. See [BIOS.md](BIOS.md) for details.

### Loading Games

Supported formats:
- **CHD** (recommended) - `game.chd`
- **BIN/CUE** - Select the `.cue` file
- **ISO** - `game.iso`
- **CCD** - Select the `.ccd` file
- **MDS** - Select the `.mds` file
- **BIN** (raw) - `game.bin`

**Multi-disc games:** Not yet tested. Use M3U playlists (untested):
```m3u
game_disc1.chd
game_disc2.chd
```

### Saves & Clock

Brimir creates per-game save files in `saves\Brimir\`:
```
C:\RetroArch-Win64\saves\Brimir\
├── Sega Rally Championship (USA).bup   # Backup RAM (game saves)
└── Sega Rally Championship (USA).srm   # RetroArch copy
```

The Real-Time Clock is stored system-wide in `system\`:
```
C:\RetroArch-Win64\system\
└── brimir_saturn_rtc.smpc  # Clock data (shared across all games)
```

**Don't delete these!** Save files and clock data are needed for proper game saves and time persistence.

---

## For Developers

### Prerequisites

**Required:**
- Git
- CMake 3.20+
- C++20 compiler:
  - GCC 11+
  - Clang 14+
  - MSVC 2022+ (Visual Studio 2022)
- vcpkg (will be set up automatically via submodule)

**Optional:**
- Ninja build system (recommended)
- ccache (for faster rebuilds)
- Clang-tidy / clang-format

### Quick Setup

#### 1. Clone Repository

```bash
git clone --recursive https://github.com/coredds/brimir.git
cd brimir
```

If you forgot `--recursive`:
```bash
git submodule update --init --recursive
```

#### 2. Install Dependencies

**Linux (Ubuntu/Debian):**
```bash
sudo apt install build-essential cmake git
```

**Linux (Fedora):**
```bash
sudo dnf install gcc-c++ cmake git
```

**Linux (Arch):**
```bash
sudo pacman -S base-devel cmake git
```

**Windows:**
- Visual Studio 2022 Build Tools (installed via `setup-env.ps1`)

#### 3. Build

**Linux (Automated):**
```bash
chmod +x build.sh
./build.sh

# Output: build/src/libretro/libbrimir_libretro.so
```

**Windows (Automated):**
```powershell
.\build.ps1

# Output: build\src\libretro\brimir_libretro.dll
```

**Manual Build (Any Platform):**
```bash
# Bootstrap vcpkg
./vcpkg/bootstrap-vcpkg.sh  # Linux/macOS
.\vcpkg\bootstrap-vcpkg.bat  # Windows

# Install dependencies
./vcpkg/vcpkg install fmt:x64-linux catch2:x64-linux  # Linux
.\vcpkg\vcpkg install fmt:x64-windows catch2:x64-windows  # Windows

# Configure
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --config Release -j$(nproc)  # Linux
cmake --build build --config Release  # Windows
```

#### 4. Install to RetroArch

**Linux:**
```bash
# System-wide
sudo cp build/src/libretro/libbrimir_libretro.so /usr/lib/libretro/
sudo cp brimir_libretro.info /usr/share/libretro/info/

# OR User-local
mkdir -p ~/.config/retroarch/cores ~/.config/retroarch/info
cp build/src/libretro/libbrimir_libretro.so ~/.config/retroarch/cores/
cp brimir_libretro.info ~/.config/retroarch/info/
```

**macOS:**
```bash
mkdir -p ~/Library/Application\ Support/RetroArch/cores
cp build/src/libretro/libbrimir_libretro.dylib ~/Library/Application\ Support/RetroArch/cores/
```

**Windows:**
```powershell
# Automated
.\deploy-retroarch.ps1

# OR Manual
copy build\src\libretro\brimir_libretro.dll C:\RetroArch-Win64\cores\
copy brimir_libretro.info C:\RetroArch-Win64\info\
```

### Development Build

For development with debug symbols and assertions:

```bash
cmake -B build-debug -S . -DCMAKE_BUILD_TYPE=Debug -DBRIMIR_BUILD_TESTS=ON
cmake --build build-debug
```

### Project Structure Overview

```
brimir/
├── src/
│   ├── libretro/      # Libretro API implementation (start here)
│   ├── bridge/        # Ymir ↔ libretro bridge
│   └── ymir/          # Ymir emulator (submodule)
├── include/           # Public headers
├── tests/             # Unit and integration tests
├── docs/              # Documentation
└── resources/         # Core info, icons
```

### Key Files to Understand

1. **`src/libretro/libretro.cpp`** - Main entry point, implements `retro_*` functions
2. **`src/bridge/core_wrapper.cpp`** - Wraps Ymir emulator
3. **`include/brimir/core_wrapper.hpp`** - Main interface
4. **`CMakeLists.txt`** - Build configuration
5. **`docs/development/ARCHITECTURE.md`** - System design and architecture
6. **`README.md`** - Project overview and features

### Common Tasks

#### Run Tests
```bash
cd build
ctest --output-on-failure
```

#### Format Code
```bash
clang-format -i src/**/*.cpp include/**/*.hpp
```

#### Build Specific Target
```bash
cmake --build build --target brimir_libretro
cmake --build build --target tests
```

#### Clean Build
```bash
rm -rf build
cmake -B build -S .
cmake --build build
```

#### Update Submodules (Ymir)
```bash
git submodule update --remote --merge
```

### Debugging

#### GDB (Linux/macOS)
```bash
# Build debug version
cmake -B build-debug -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug

# Run RetroArch with core under GDB
gdb --args retroarch -L build-debug/brimir_libretro.so game.chd
```

#### Visual Studio (Windows)
```bash
# Open solution
cmake -B build -S . -G "Visual Studio 17 2022"
start build\brimir.sln

# Set retroarch.exe as debug target with arguments:
#   Command: C:\RetroArch\retroarch.exe
#   Arguments: -L $(OutDir)brimir_libretro.dll game.chd
#   Working Dir: C:\RetroArch
```

#### LLDB (macOS)
```bash
lldb -- /Applications/RetroArch.app/Contents/MacOS/RetroArch \
  -L build-debug/brimir_libretro.dylib game.chd
```

### Logging

Brimir logs through libretro's logging interface. View logs in:
- **RetroArch:** Enable logging in Settings → Logging
- **Console:** Run RetroArch from terminal
- **File:** Set log file in RetroArch config

Logging levels:
- `RETRO_LOG_DEBUG` - Verbose debugging
- `RETRO_LOG_INFO` - General information
- `RETRO_LOG_WARN` - Warnings
- `RETRO_LOG_ERROR` - Errors

### Testing with RetroArch

```bash
# Load game directly
retroarch -L build/brimir_libretro.so game.chd

# Verbose logging
retroarch -v -L build/brimir_libretro.so game.chd

# Start with specific config
retroarch -L build/brimir_libretro.so game.chd --config custom.cfg

# Windowed mode
retroarch -L build/brimir_libretro.so game.chd --windowed
```

### Performance Profiling

#### Linux (perf)
```bash
perf record -g retroarch -L build/brimir_libretro.so game.chd
perf report
```

#### macOS (Instruments)
```bash
# Run with Instruments
instruments -t "Time Profiler" -D profile.trace \
  /Applications/RetroArch.app/Contents/MacOS/RetroArch \
  -L build/brimir_libretro.dylib game.chd
```

#### Windows (Visual Studio Profiler)
Use Visual Studio's built-in profiler:
1. Debug → Performance Profiler
2. Select CPU Usage
3. Start profiling

---

## Development Workflow

### Making Changes

1. **Create a branch:**
   ```bash
   git checkout -b feature/your-feature
   ```

2. **Make changes and test:**
   ```bash
   # Edit code
   cmake --build build
   # Test in RetroArch
   retroarch -L build/brimir_libretro.so test.chd
   ```

3. **Commit:**
   ```bash
   git add .
   git commit -m "feat: add your feature"
   ```

4. **Push and create PR:**
   ```bash
   git push origin feature/your-feature
   # Create PR on GitHub
   ```

### Working with Ymir Upstream

```bash
# Update Ymir submodule to latest
cd src/ymir
git checkout main
git pull
cd ../..
git add src/ymir
git commit -m "chore: update Ymir to latest"

# Test integration still works
cmake --build build
```

### Code Style

- Follow existing code style
- Use `clang-format` for formatting
- Write clear commit messages
- Add tests for new features
- Update documentation

---

## Troubleshooting

### Build Issues

**Problem:** CMake can't find C++20 compiler
```bash
# Solution: Specify compiler explicitly
export CXX=g++-11  # or clang++-14, cl.exe
cmake -B build -S .
```

**Problem:** Submodules not initialized
```bash
# Solution:
git submodule update --init --recursive
```

**Problem:** vcpkg dependencies fail
```bash
# Solution: Clean and rebuild vcpkg
rm -rf vcpkg/buildtrees vcpkg/packages
cmake -B build -S . --fresh
```

### Runtime Issues

**Problem:** Core not loading in RetroArch
- Check core info file is in place
- Verify BIOS files are present
- Check RetroArch logs

**Problem:** Game not loading
- Verify game format is supported
- Check BIOS matches game region
- Try different game/format

**Problem:** Performance issues
- Use Release build (not Debug)
- Check CPU usage in task manager
- Try different core options

### Getting Help

- Check [ARCHITECTURE.md](../development/ARCHITECTURE.md)
- Search [Issues](https://github.com/coredds/brimir/issues)
- Ask in [Discussions](https://github.com/coredds/brimir/discussions)

---

## Useful Resources

### Documentation
- [README.md](../../README.md) - Project overview and features
- [ARCHITECTURE.md](../development/ARCHITECTURE.md) - System design
- [CONTRIBUTING.md](../../CONTRIBUTING.md) - Contribution guide
- [ROADMAP.md](../ROADMAP.md) - Development roadmap

### External Resources
- [Libretro API Docs](https://docs.libretro.com/)
- [Ymir Project](https://github.com/StrikerX3/Ymir)
- [Sega Saturn Tech Docs](http://wiki.yabause.org/)
- [RetroArch Docs](https://docs.libretro.com/guides/retroarch-documentation/)

### Tools
- [RetroArch](https://www.retroarch.com/)
- [CMake Docs](https://cmake.org/documentation/)
- [vcpkg](https://vcpkg.io/)

---

## Next Steps

1. Read [ARCHITECTURE.md](../development/ARCHITECTURE.md) to understand the system
2. Look at [ROADMAP.md](../ROADMAP.md) for current tasks
3. Check [Issues](https://github.com/coredds/brimir/issues) for work to do
4. Join the community and introduce yourself
5. Start coding!

---

**Happy coding! **



