# Brimir

<p align="center">
  <strong>Sega Saturn Emulation for libretro</strong><br>
  <em>A high-performance libretro core based on the Ymir emulator</em>
</p>

---

## Overview

**Brimir** is a libretro core wrapper for the [Ymir Sega Saturn emulator](https://github.com/StrikerX3/Ymir) by [StrikerX3](https://github.com/StrikerX3), bringing Ymir's accurate and feature-rich Sega Saturn emulation to RetroArch and other libretro frontends.

Named after the primordial giant in Norse mythology (complementing "Ymir"), Brimir serves as a bridge between Ymir's modern C++20 emulation core and the libretro API ecosystem.

**All emulation work is done by Ymir.** Brimir only provides the libretro API integration layer.

## Status

**Project Status:** Active Development  
**Current Version:** 0.1.2  
**Last Updated:** November 27, 2025

Core features are implemented and tested with 82 comprehensive test cases. The core achieves consistent 60 FPS emulation on modern hardware. Expansion cartridge support is functional but has some limitations (see Known Issues below).

## Demo

**See Brimir in action:**

[![Sega Rally on Brimir](https://img.youtube.com/vi/akkdVZk8GUY/0.jpg)](https://www.youtube.com/watch?v=akkdVZk8GUY)

Watch [Sega Rally Championship running on Brimir](https://www.youtube.com/watch?v=akkdVZk8GUY) - demonstrating smooth 60 FPS gameplay with the libretro core.

## Features

### Core Emulation
- **Accurate hardware emulation** via Ymir's cycle-accurate core
- **Multiple disc formats:** CHD, BIN/CUE, ISO, CCD, MDS, BIN
- **7 BIOS versions supported:** US, EU, JP variants with auto-detection
- **High-level CD block emulation** with configurable read speeds (2x-200x)
- **Backup RAM (SRAM)** with persistent game saves (per-game .bup files)
- **Real-Time Clock (RTC)** with system-wide persistence
- **Expansion cartridge support (Beta):**
  - Auto-detection and insertion based on game database
  - 1MB (8 Mbit) DRAM cartridges for SNK fighters (working)
  - 4MB (32 Mbit) DRAM cartridges for Capcom fighters (limited - see Known Issues)
  - 6MB (48 Mbit) DRAM dev cartridges for rare prototypes
  - ROM cartridge support for King of Fighters '95 and Ultraman (not yet implemented)
  - Cartridge RAM persistence not yet implemented (data lost between sessions)
- **Game database integration:**
  - 24+ games with automatic expansion cartridge support
  - Automatic SH-2 cache emulation for compatibility
  - Fast bus timing adjustments for stability

### libretro Integration
- **Full libretro API compliance** (v1)
- **Save state support** with complete serialization including cartridge data
- **Controller support** for Player 1 & 2 (digital pads with 6-button layout)
- **RGB565 video output** with automatic resolution handling
- **44.1 kHz stereo audio** with ring buffer optimization
- **Core options** for extensive configuration
- **SRAM interface** with dual format support (.bup + .srm)

### Performance Optimizations
- **Threaded VDP rendering** for consistent 60 FPS
- **Optimized pixel conversion** (XBGR8888 to RGB565)
- **SRAM caching** with 300-frame refresh cycle
- **Audio ring buffer** for low-latency, high-throughput audio (4096 samples)
- **Profiling support** for performance monitoring and diagnostics

### Core Options
- **Video Settings:** Deinterlacing (for hi-res interlaced menus), threaded VDP, transparent meshes
- **Audio Settings:** Interpolation mode (linear/nearest)
- **System Settings:** BIOS selection, region, video standard (NTSC/PAL)
- **Media Settings:** CD read speed (2x-200x), autodetect region
- **Performance Settings:** SH-2 cache emulation

### Platform Support
- Windows 10+ (x86-64) - Fully tested
- Linux (x86-64) - Builds successfully
- macOS (Intel, Apple Silicon) - Builds successfully

## Building

### Prerequisites
- **CMake 3.20+**
- **C++20 compiler:** MSVC 2022 17.0+, GCC 11+, or Clang 14+
- **Visual Studio Build Tools 2022** (Windows)
- **Git** with submodules support
- **vcpkg** (included as submodule)

### Quick Start (Windows)

```powershell
# 1. Clone with submodules
git clone --recursive https://github.com/coredds/brimir.git
cd Brimir

# 2. Run automated setup (installs Visual Studio Build Tools if needed)
PowerShell -ExecutionPolicy Bypass -File .\setup-env.ps1

# 3. Build the core
.\build.ps1
```

The compiled core will be in `build/src/libretro/brimir_libretro.dll`.

### Quick Start (Linux)

```bash
# 1. Clone with submodules
git clone --recursive https://github.com/coredds/brimir.git
cd Brimir

# 2. Install dependencies
sudo apt install build-essential cmake git  # Ubuntu/Debian
# OR
sudo dnf install gcc-c++ cmake git          # Fedora
# OR
sudo pacman -S base-devel cmake git         # Arch

# 3. Build the core
chmod +x build.sh
./build.sh
```

The compiled core will be in `build/src/libretro/libbrimir_libretro.so`.

### Manual Build (Windows)

```powershell
# Bootstrap vcpkg
.\vcpkg\bootstrap-vcpkg.bat

# Install dependencies
.\vcpkg\vcpkg.exe install fmt:x64-windows catch2:x64-windows

# Configure with CMake
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --config Release
```

### Manual Build (Linux)

```bash
# Bootstrap vcpkg
./vcpkg/bootstrap-vcpkg.sh

# Install dependencies
./vcpkg/vcpkg install fmt:x64-linux catch2:x64-linux

# Configure with CMake
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --config Release -j$(nproc)
```

### Running Tests

The project includes 82 comprehensive test cases covering all major features.

**Windows:**
```powershell
cd build
ctest -C Release --output-on-failure
```

**Linux:**
```bash
cd build
ctest --output-on-failure
```

**Test Coverage:**
- Core initialization and lifecycle (8 tests)
- BIOS loading and integration (11 tests)
- Game loading and validation (10 tests)
- Save state functionality (8 tests)
- Video output (7 tests)
- Audio output (7 tests)
- Input handling (5 tests)
- Expansion cartridge support (15 tests)
- SRAM persistence (10 tests)
- Error handling and edge cases

All tests focus on critical functionality without artificial coverage inflation.

## Usage

### Installation

**Windows:**

1. Copy the core to RetroArch's `cores/` directory:
   ```
   C:\RetroArch-Win64\cores\brimir_libretro.dll
   ```

2. Copy core info to RetroArch's `info/` directory:
   ```
   C:\RetroArch-Win64\info\brimir_libretro.info
   ```

Or use the deployment script:
```powershell
.\deploy-retroarch.ps1
```

**Linux:**

```bash
# Copy core library
sudo cp build/src/libretro/libbrimir_libretro.so /usr/lib/libretro/
# OR for local install
mkdir -p ~/.config/retroarch/cores
cp build/src/libretro/libbrimir_libretro.so ~/.config/retroarch/cores/

# Copy core info
sudo cp brimir_libretro.info /usr/share/libretro/info/
# OR for local install
mkdir -p ~/.config/retroarch/info
cp brimir_libretro.info ~/.config/retroarch/info/
```

**Both Platforms:**

3. Add BIOS files to RetroArch's `system/` directory (see below)

### BIOS Requirements

Brimir requires Sega Saturn BIOS ROMs. Place them in RetroArch's `system/` directory:

| File | Region | Version | Size |
|------|--------|---------|------|
| `Sega Saturn BIOS v1.01 (JAP).bin` | Japan | v1.01 | 512 KB |
| `Sega Saturn BIOS v1.00 (JAP).bin` | Japan | v1.00 | 512 KB |
| `sega_101.bin` | USA | v1.01a | 512 KB |
| `mpr-17933.bin` | USA | v1.00 | 512 KB |
| `sega_100.bin` | Europe | v1.00a | 512 KB |
| `mpr-17951.bin` | Europe | v1.00 | 512 KB |
| `Sega Saturn BIOS (EUR).bin` | Europe | - | 512 KB |

The core auto-detects available BIOS files and allows selection via **Quick Menu → Options → System Settings → BIOS**.

**⚠️ IMPORTANT:** Japanese BIOS v1.003 (`sega1003.bin`) is **NOT supported** due to compatibility issues. Do not use this version. Use Japanese v1.01 or v1.00 instead.

**Note:** You must obtain BIOS files legally from your own Saturn console or official sources.

### Persistent Data

Brimir creates persistent files in multiple locations:

**Per-Game Data** (`saves/Brimir/`):
```
saves/Brimir/
├── {Game Name}.bup   - Backup RAM (game saves) ✅ Working
├── {Game Name}.srm   - RetroArch backup copy ✅ Working
└── {Game Name}.cart  - Cartridge RAM ❌ Not yet implemented (progress lost)
```

**System-Wide Data** (`system/`):
```
system/
└── brimir_saturn_rtc.smpc  - Real-Time Clock configuration (shared across all games)
```

**ROM Cartridges** (alongside game disc image):
```
{Game Folder}/
├── game.cue          - Disc image
├── game.bin          - Disc data
└── game.rom          - ROM cartridge (4MB, required for KOF'95/Ultraman)
```

**Notes:** 
- The RTC clock configuration is system-wide (not per-game) to match the behavior of the actual Sega Saturn hardware
- **Cartridge RAM persistence is not yet working** - progress in expansion RAM games is lost between sessions
- ROM cartridges are not yet implemented

## Documentation

- **[CHANGELOG.md](CHANGELOG.md)** - Version history
- **[CONTRIBUTING.md](CONTRIBUTING.md)** - How to contribute

## Tested Games

| Game | Region | Status | Cartridge | Notes |
|------|--------|--------|-----------|-------|
| Sega Rally Championship | USA | ✅ Working | None | 60 FPS, saves working, clock persists |
| Saturn Bomberman | Japan | ✅ Working | None | Full gameplay tested |
| Panzer Dragoon Zwei | USA/Europe/Japan | ✅ Working | None | Gameplay excellent, hi-res interlaced menus supported |
| King of Fighters '96 | Japan | ✅ Working | 1MB DRAM | Auto-detected, fully playable with 1MB RAM expansion |
| Street Fighter Zero 3 | Japan | ✅ Working | 4MB DRAM | Auto-detected, fully playable with 4MB RAM expansion |
| X-Men vs. Street Fighter | Japan | ⚠️ Partial | 1MB DRAM | Detects cartridge but has upstream emulation issues |
| Marvel Super Heroes vs. SF | Japan | ⚠️ Partial | 4MB DRAM | Detects cartridge, freezes at Capcom logo (upstream) |
| King of Fighters '95 | Japan/Europe | ❌ Not Implemented | 4MB ROM | ROM cartridge support pending |
| Ultraman | Japan | ❌ Not Implemented | 4MB ROM | ROM cartridge support pending |

**Compatibility Notes:**
- ✅ **Working**: Game boots, plays, and saves correctly
- ⚠️ **Partial**: Game detects cartridge but has upstream Ymir emulation issues
- ❌ **Not Tested/Implemented**: Feature not yet available or not tested
- Games with expansion cartridge requirements are automatically detected via game database
- 24+ games are in the database with automatic cartridge configuration
- Cartridge RAM persistence (`.cart` files) not yet implemented - progress lost between sessions
- Some Capcom fighters have known upstream emulation issues (freeze at logo, etc.)

## Known Issues & Limitations

### Expansion Cartridge Issues
- **Cartridge RAM persistence not implemented:** Progress in games requiring expansion RAM (SNK/Capcom fighters) is lost between sessions. The `.cart` save files are not yet functional due to MSVC compilation issues.
- **Some Capcom fighters freeze:** Games like Marvel Super Heroes vs. Street Fighter freeze at the Capcom logo. This is an **upstream Ymir emulation issue** - the standalone Ymir emulator has the same behavior.
- **ROM cartridges not implemented:** King of Fighters '95 and Ultraman require ROM cartridge files which are not yet supported.

### BIOS Compatibility
- **Japanese BIOS v1.003 (sega1003.bin) is NOT supported** - This early Japanese BIOS version has compatibility issues with the emulator core. If you have this version, replace it with Japanese v1.01 (`Sega Saturn BIOS v1.01 (JAP).bin`) or v1.00 (`Sega Saturn BIOS v1.00 (JAP).bin`) instead.

### Other Limitations
- **Controller support:** Digital pads only (analog, mouse, light gun, etc. not yet implemented)
- **Multi-disc games:** Not yet tested
- **External backup RAM cartridge:** Not implemented (for save creation tools like Dezaemon 2)
- **CD Block LLE:** Low-level CD block emulation removed due to instability

### Upstream Issues
Some game compatibility issues are inherited from the upstream Ymir emulator and will be resolved as Ymir improves:
- Marvel Super Heroes vs. Street Fighter freezes at Capcom logo
- X-Men vs. Street Fighter has detection issues
- Other Capcom fighters may have similar issues

**Note:** We track these issues and will sync with upstream Ymir updates as they become available.

## ROM Cartridge Games

Two games require special ROM cartridge files to run:

**King of Fighters '95** (Europe: MK-81088, Japan: T-3101G)
- Requires a 4MB ROM cartridge file
- Place `{game}.rom` alongside your disc image
- ROM must be a verified dump matching the expected hash

**Ultraman: Hikari no Kyojin Densetsu** (Japan: T-13308G)
- Requires a 4MB ROM cartridge file
- Place `{game}.rom` alongside your disc image
- ROM must be a verified dump matching the expected hash

The core will automatically detect these games and load the ROM cartridge if found. Supported naming patterns:
- `{disc_name}.rom` (e.g., `kof95.rom` for `kof95.cue`)
- `{product_code}.rom` (e.g., `T-3101G.rom`)
- `rom.rom` or `cartridge.rom` (generic fallback)

**Note:** You must legally obtain ROM cartridge dumps from your own hardware.

## Contributing

Contributions are welcome! Here's how you can help:

- **Bug Reports:** Open an issue with detailed steps to reproduce
- **Game Testing:** Test games and report compatibility
- **Code Contributions:** Submit PRs for improvements
- **Documentation:** Improve guides and add translations

See [CONTRIBUTING.md](CONTRIBUTING.md) for detailed guidelines.

**Note:** Brimir wraps the Ymir emulator. If you encounter emulation issues, we'll investigate and potentially sync with upstream Ymir updates as they become available. Please report all issues here - we'll handle coordination with upstream if needed.


## License

Brimir is licensed under the **GPL-3.0** license, inherited from the Ymir project.

See [LICENSE](LICENSE) for details.

## About Ymir

**[Ymir](https://github.com/StrikerX3/Ymir)** is a cycle-accurate Sega Saturn emulator written in modern C++20 by [StrikerX3](https://github.com/StrikerX3). It aims for high accuracy and compatibility while maintaining good performance.

### Why Ymir?

- **Accuracy-focused**: Cycle-accurate emulation of Saturn hardware
- **Modern codebase**: Clean C++20 implementation
- **Active development**: Regular updates and improvements
- **Well-documented**: Extensive documentation and comments
- **Standalone app**: Works great without libretro

### Ymir's Design

Ymir is designed as a standalone, cycle-accurate emulator. Brimir adapts it to work with RetroArch and other libretro frontends.

If you're interested in Saturn emulation, check out the **[Ymir repository](https://github.com/StrikerX3/Ymir)** to see how it works!

## Acknowledgments

- **[Ymir](https://github.com/StrikerX3/Ymir)** by [StrikerX3](https://github.com/StrikerX3) - This project would not exist without Ymir. All emulation credit goes to StrikerX3 and the Ymir project.
- **[libretro](https://github.com/libretro)** - The platform that makes this integration possible
- **Sega Saturn community** - For preservation efforts and technical documentation

## Support

**The best way to support this project:**

- **Support StrikerX3** (Ymir's creator): [Patreon](https://www.patreon.com/StrikerX3) - Ymir makes this possible!
- **Star Ymir**: [https://github.com/StrikerX3/Ymir](https://github.com/StrikerX3/Ymir) - Show appreciation for the emulator
- **Star this repository** - For the libretro integration
- **Test games** and report issues here
- **Contribute** improvements
- **Share** with the community

## Links

### Ymir (Upstream Emulator)
- **Ymir Repository:** [https://github.com/StrikerX3/Ymir](https://github.com/StrikerX3/Ymir)
- **Support StrikerX3:** [https://www.patreon.com/StrikerX3](https://www.patreon.com/StrikerX3)

### libretro / RetroArch
- **libretro:** [https://github.com/libretro](https://github.com/libretro)
- **RetroArch:** [https://www.retroarch.com](https://www.retroarch.com)
- **Libretro Docs:** [https://docs.libretro.com](https://docs.libretro.com)

---

<p align="center">
  <strong>Powered by <a href="https://github.com/StrikerX3/Ymir">Ymir</a></strong><br>
  Made for the Sega Saturn and emulation community
</p>
