# Brimir Quick Start Guide

This guide will help you get started with Brimir development quickly.

---

## For Users

> **Note:** Brimir is currently in the planning phase. This section will be updated once releases are available.

### Installing via RetroArch (Future)

1. Open RetroArch
2. Go to **Online Updater** → **Core Downloader**
3. Find and download **Sega - Saturn (Brimir)**
4. Done!

### Manual Installation (Future)

1. Download the latest release for your platform from [Releases](https://github.com/YOUR_USERNAME/brimir/releases)
2. Extract `brimir_libretro.dll` (Windows) / `brimir_libretro.so` (Linux) / `brimir_libretro.dylib` (macOS)
3. Copy to RetroArch's `cores` directory
4. Restart RetroArch

### BIOS Setup

Brimir requires Sega Saturn BIOS files. Place them in RetroArch's `system` directory:

```
system/
├── saturn_bios.bin       # Any region BIOS
├── mpr-17933.bin         # Japan BIOS
├── mpr-18811.bin         # USA BIOS  
└── mpr-17951.bin         # Europe BIOS
```

**Note:** You must obtain these files legally from your own Saturn console.

### Loading Games

Supported formats:
- **CHD** (recommended) - `game.chd`
- **BIN/CUE** - Select the `.cue` file
- **ISO** - `game.iso`
- **IMG/CCD** - Select the `.cue` file
- **MDF/MDS** - Select the `.mds` file

For multi-disc games, create an M3U playlist:
```m3u
game_disc1.chd
game_disc2.chd
game_disc3.chd
```

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
git clone --recursive https://github.com/YOUR_USERNAME/brimir.git
cd brimir
```

If you forgot `--recursive`:
```bash
git submodule update --init --recursive
```

#### 2. Build

**Linux/macOS:**
```bash
# Configure
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc)

# Output: build/brimir_libretro.so (or .dylib on macOS)
```

**Windows (Visual Studio):**
```bash
# Configure
cmake -B build -S . -G "Visual Studio 17 2022" -A x64

# Build
cmake --build build --config Release

# Output: build\Release\brimir_libretro.dll
```

**Windows (Ninja):**
```bash
# Configure (in x64 Native Tools Command Prompt)
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build

# Output: build\brimir_libretro.dll
```

#### 3. Install to RetroArch

**Linux:**
```bash
cp build/brimir_libretro.so ~/.config/retroarch/cores/
```

**macOS:**
```bash
cp build/brimir_libretro.dylib ~/Library/Application\ Support/RetroArch/cores/
```

**Windows:**
```bash
copy build\Release\brimir_libretro.dll C:\RetroArch\cores\
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
5. **`PRD.md`** - Requirements and features
6. **`ARCHITECTURE.md`** - System design

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

- 📖 Check [ARCHITECTURE.md](ARCHITECTURE.md)
- 🐛 Search [Issues](https://github.com/YOUR_USERNAME/brimir/issues)
- 💬 Ask in [Discussions](https://github.com/YOUR_USERNAME/brimir/discussions)
- 💭 Join Discord (link TBD)

---

## Useful Resources

### Documentation
- [PRD.md](PRD.md) - Product requirements
- [ARCHITECTURE.md](ARCHITECTURE.md) - System design
- [CONTRIBUTING.md](CONTRIBUTING.md) - Contribution guide
- [ROADMAP.md](ROADMAP.md) - Development roadmap

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

1. Read [ARCHITECTURE.md](ARCHITECTURE.md) to understand the system
2. Look at [ROADMAP.md](ROADMAP.md) for current tasks
3. Check [Issues](https://github.com/YOUR_USERNAME/brimir/issues) for work to do
4. Join the community and introduce yourself
5. Start coding!

---

**Happy coding! 🎮**

