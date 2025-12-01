# Brimir

<p align="center">
  <strong>Sega Saturn Emulation for libretro</strong><br>
  <em>Windows x64 Performance-Focused Core</em>
</p>

---

## Overview

**Brimir** is a high-performance libretro core for Sega Saturn emulation, forked from [Ymir](https://github.com/StrikerX3/Ymir) by [StrikerX3](https://github.com/StrikerX3). Brimir maintains cycle-accurate emulation while focusing on performance optimizations and Windows x64 support.

### Current Focus: Windows x64 Stability

> **Note**: Brimir currently supports **Windows x64 with MSVC only**. Linux and ARM platforms will be reintroduced after establishing a stable Windows build.

### Project Goals

- **Performance**: Optimize for modern Windows x64 hardware
- **Compatibility**: Achieve broad game compatibility through targeted fixes
- **Accuracy**: Maintain cycle-accurate emulation foundation
- **Stability**: Perfect Windows x64 support before platform expansion
- **Future**: JIT compilation for 3-10× performance improvement

Brimir respects the GPL-3.0 license. The codebase is a complete fork for independent development.

## Demo

[![Sega Rally on Brimir](https://img.youtube.com/vi/akkdVZk8GUY/0.jpg)](https://www.youtube.com/watch?v=akkdVZk8GUY)

Watch [Sega Rally Championship running on Brimir](https://www.youtube.com/watch?v=akkdVZk8GUY) - smooth 60 FPS gameplay on Windows x64.

## Features

### Performance & Optimizations (v0.2.1)
- **60+ FPS @ 704x448i** high-resolution interlaced modes
- **SSE2/AVX2 SIMD optimizations** for VDP rendering
- **Template-based interpreter** with aggressive inlining
- **Decode table optimization** for SH-2 execution
- **Pixel-perfect accuracy** maintained through all optimizations

### Core Emulation
- **Cycle-accurate SH-2 interpreter** with hardware optimizations
- **Multiple disc formats:** CHD, BIN/CUE, ISO, CCD, MDS
- **7 BIOS versions supported:** US, EU, JP variants with auto-detection
- **Backup RAM (SRAM)** with persistent game saves
- **Real-Time Clock (RTC)** with system-wide persistence
- **Save state support** with complete serialization
- **Expansion cartridge support:**
  - 1MB/4MB/6MB DRAM cartridges with auto-detection
  - 24+ games with automatic cartridge support
  - Auto-insertion based on game database

### libretro Integration
- **Full libretro API compliance** (v1)
- **Controller support** for Player 1 & 2 (6-button digital pads)
- **RGB565 video output** with deinterlacing support
- **44.1 kHz stereo audio**
- **Core options** for video, audio, and system configuration

### Platform Support
- ✅ **Windows 10+ (x86-64)** - Primary platform
- 🚧 **Linux** - Planned for Q2 2025
- 🚧 **macOS** - Planned for Q4 2025
- 🚧 **ARM64** - Planned for Q3 2025

## Building

### Prerequisites (Windows x64)

- **CMake 3.28+**
- **Visual Studio 2022** (Build Tools or Community Edition)
- **Git with submodules**
- **Windows 10 or later**

### Quick Build

```powershell
# Clone repository
git clone --recursive https://github.com/coredds/brimir.git
cd Brimir

# Build
.\build.ps1
```

### Manual Build

```powershell
# Configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --config Release

# Output: build\bin\Release\brimir_libretro.dll
```

### Build Options

```powershell
# Enable Link-Time Optimization (LTO)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBRIMIR_LTO=ON

# Build with tests
cmake -S . -B build -DBRIMIR_BUILD_TESTS=ON

# Build JIT test framework (for development)
cmake -S . -B build -DBUILD_JIT_TESTS=ON
```

## Installation

### RetroArch (Windows)

1. **Build or download** `brimir_libretro.dll`
2. **Copy to RetroArch cores folder:**
   ```
   C:\RetroArch\cores\brimir_libretro.dll
   ```
3. **Copy info file:**
   ```
   C:\RetroArch\info\brimir_libretro.info
   ```
4. **Restart RetroArch** and select Brimir under Sega Saturn

### Manual Install

```powershell
.\deploy-retroarch.ps1
```

This script will:
- Build the core
- Copy DLL to RetroArch cores directory
- Copy info file to RetroArch info directory

## Requirements

### BIOS Files

Place BIOS files in RetroArch's `system/` directory. Brimir supports:

| Region | Filename | MD5 |
|--------|----------|-----|
| US | `sega_101.bin` | 85ec9ca47d8f6807718151cbcca8b964 |
| JP | `saturn_bios.bin` | af5828fdff51384f99b3c4926be27762 |
| EU | `saturn_bios_eu.bin` | ... |

Auto-detection will choose the best BIOS for your game region.

## Development Status

### Completed ✅
- [x] Core emulation (SH-2, VDP1/VDP2, SCSP, CD-ROM)
- [x] Windows x64 MSVC build system
- [x] Performance optimizations (templates, SIMD, decode tables)
- [x] Simplified project structure
- [x] libretro API integration
- [x] Save states and backup RAM

### In Progress 🚧
- [ ] Project organization evaluation
- [ ] Documentation updates
- [ ] Performance profiling

### Planned 📋
- [ ] **Q1 2025**: JIT test framework API updates
- [ ] **Q2 2025**: Linux x64 support
- [ ] **Q2 2025**: JIT compiler implementation (IR & x86-64 backend)
- [ ] **Q3 2025**: ARM64 support with NEON optimizations
- [ ] **Q4 2025**: macOS support

## Performance

### Current (Optimized Interpreter)
- **Execution speed**: ~50-100 MHz SH-2 equivalent
- **Overhead**: ~10-20 host instructions per SH-2 instruction
- **SIMD**: VDP uses SSE2/AVX2 for pixel processing
- **Real-world**: 60 FPS in most games at native resolution

### Future (with JIT)
- **Expected speedup**: 3-10× over interpreter
- **Target**: Full-speed on entry-level handhelds
- **Timeline**: Q2-Q4 2025

## Documentation

- [Performance Verification](docs/PERFORMANCE_VERIFICATION.md) - Optimization analysis
- [SH-2 Interpreter Status](docs/SH2_INTERPRETER_STATUS.md) - Interpreter details
- [Project Simplification](docs/PROJECT_SIMPLIFICATION.md) - Windows-only focus
- [JIT Status](src/jit/STATUS.md) - JIT development roadmap
- [SH-2 JIT Evaluation](docs/SH2_JIT_EVALUATION.md) - JIT design
- [SH-2 JIT Roadmap](docs/SH2_JIT_ROADMAP.md) - Implementation plan

## Acknowledgments

### Foundation and Inspiration
- **[Ymir](https://github.com/StrikerX3/Ymir)** by [StrikerX3](https://github.com/StrikerX3) - Foundation, architectural reference, and cycle-accurate SH-2 interpreter
- **[Mednafen](https://mednafen.github.io/)** - Reference implementation and accuracy testing
- **[Yabause](https://yabause.org/)** - SH-2 dynarec architecture study

### Libraries
- **fmt** - Fast formatting library
- **mio** - Memory-mapped I/O
- **xxHash** - Fast hashing
- **lz4** - Compression
- **libchdr** - CHD disc format support
- **concurrentqueue** - Lock-free queue

## License

Brimir is licensed under the **GNU General Public License v3.0** (GPL-3.0).

This is a complete fork of Ymir, respecting its GPL-3.0 license and acknowledging its foundation.

See [LICENSE](LICENSE) for full text.

## Contributing

We welcome contributions! Current focus areas:
- Windows x64 optimization
- Game compatibility fixes
- Performance profiling
- Documentation improvements

For Linux/ARM contributions, please wait until Q2 2025 when platform support is reintroduced.

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## Support

- **Issues**: [GitHub Issues](https://github.com/coredds/brimir/issues)
- **Discord**: Coming soon
- **Forum**: RetroArch forums

---

**Note**: Brimir is under active development with a focus on Windows x64 stability. Cross-platform support will return in future releases.
