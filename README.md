# Brimir

A Sega Saturn emulation core for libretro, built on the [Ymir](https://github.com/StrikerX3/ymir) emulator by StrikerX3.

## Overview

Brimir is a libretro core for Sega Saturn emulation, wrapping Ymir's cycle-accurate hardware layer. It provides accurate emulation with optimized software rendering and full VDP1/VDP2 support.

**Current Status**: Active development. Hardware layer synced verbatim from upstream Ymir (2026-06-23).

## Features

### Emulation
- **Ymir Hardware Layer**: Cycle-accurate Saturn emulation, synced verbatim from upstream Ymir (2026-06-23)
- Accurate SH-2 dual-CPU emulation with WB/EX stall timing, 32-bit instruction fetch, and inlined opcode decode
- Full VDP1 sprite engine and VDP2 scroll plane graphics with COPR register fix
- SCSP (Saturn Custom Sound Processor) audio with configurable interpolation
- M68000 sound CPU emulation
- SCU DSP and DMA emulation
- CHD and ISO disc format support via libchdr
- Save state and backup RAM persistence with Japanese character translation
- Auto-detection of console region from disc
- Configurable CD read speed (2x-16x)
- RAM expansion cartridge support (1MB, 4MB, 6MB)
- ROM cartridge support (King of Fighters '95, Ultraman)

### Rendering
- **Software Renderer**: Ymir's proven software renderer with pixel-perfect accuracy
  - Optimized with SIMD intrinsics (AVX2/SSE2) for pixel conversion
  - Threaded VDP1 and VDP2 rendering for optimal performance
  - Full resolution output (no overscan cropping by VDP)
- **Deinterlacing**: Multiple modes for interlaced video
  - Bob (smooth 60 FPS, no scanlines)
  - Weave (CRT-style scanlines)
  - Blend (field blending)
  - Current (legacy dual-field)
- **Post-Processing**:
  - Horizontal blend filter for high-res interlaced modes
  - Configurable overscan cropping (horizontal and vertical)
  - Frameskip support

### Integration
- Libretro API v2 with full core options support
- Compatible with RetroArch and other libretro frontends
- XRGB8888 pixel format output
- Performance profiling (logged every 300 frames)
- Full controller remapping support

## Build Requirements

- Windows 10/11 (x64) or Linux (x64)
- C++20 compiler: MSVC 2022+, GCC 11+, or Clang 14+
- CMake 3.28+

## Building

### Windows

```powershell
# Using the build script (recommended)
.\build.ps1 -Generator "Visual Studio 17 2022"

# With AVX2 optimizations
.\build.ps1 -Generator "Visual Studio 17 2022" -AVX2

# Or manually with CMake
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# With AVX2: add -DBrimir_AVX2=ON
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBrimir_AVX2=ON
cmake --build build --config Release

# Output: build\bin\Release\brimir_libretro.dll
```

### Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# With AVX2: add -DBrimir_AVX2=ON
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBrimir_AVX2=ON
cmake --build build -j$(nproc)

# Output: build/brimir_libretro.so
```

## Installation

### Quick Deploy

```powershell
# Automatically builds and copies to RetroArch
.\deploy-retroarch.ps1
```

### Manual Install

1. Copy `brimir_libretro.dll` to your RetroArch `cores/` directory.
2. Place a compatible Saturn BIOS file in the RetroArch `system/` directory. Supported BIOS files (any one will work):
   - `sega_101.bin` (US v1.01)
   - `mpr-17933.bin` (US v1.00)
   - `sega_100.bin` (EU v1.00)
   - `Sega Saturn BIOS v1.01 (JAP).bin`
   - `Sega Saturn BIOS v1.00 (JAP).bin`
3. Load a Saturn game (.cue, .chd, .iso) through RetroArch.

## Core Options

| Category | Option | Values |
|----------|--------|--------|
| System | BIOS Selection | Auto-detect, JP, US, EU variants |
| System | Console Region | Auto-detect, US, EU, JP |
| System | Auto-Detect Region | On/Off |
| Video | Video Standard | Auto, NTSC (60Hz), PAL (50Hz) |
| Video | Deinterlacing | On/Off |
| Video | Deinterlacing Mode | Bob, Weave, Blend, Current, None |
| Video | Horizontal Blend | On/Off (for interlaced modes) |
| Video | Horizontal Overscan | On/Off (crop 8px each side) |
| Video | Vertical Overscan | On/Off (crop 8px top/bottom) |
| Video | Frameskip | 0-3 frames |
| Audio | Interpolation | Linear, Nearest |
| Media | CD Read Speed | 2x-16x |

## Project Structure

```
brimir/
  src/
    core/
      include/ymir/   Ymir hardware layer (verbatim upstream sync)
      include/brimir/ Brimir-specific additions
      src/ymir/       Ymir source files (verbatim)
    bridge/           CoreWrapper -- interface between emulator and frontends
    libretro/         Libretro API implementation and core options
    jit/              SH-2 JIT compiler (future)
  include/         Public headers
  vendor/          Vendored dependencies
  tests/           Unit and integration tests
  tools/           Development utilities
```

## Dependencies

All dependencies are vendored in the `vendor/` directory:

- **fmt** -- Formatting library
- **mio** -- Memory-mapped I/O
- **concurrentqueue** -- Lock-free queue
- **xxHash** -- Fast hashing
- **lz4** -- Compression
- **libchdr** -- CHD disc format support (includes zlib, zstd, lzma)

## License

Licensed under the GPLv2. See LICENSE file for details.

## Credits

Brimir is built on **[Ymir](https://github.com/StrikerX3/ymir)**, a cycle-accurate Sega Saturn emulator by **StrikerX3**. The entire hardware layer under `src/core/` is synced verbatim from upstream Ymir — all Saturn CPU, VDP, audio, and peripheral emulation is Ymir's work. Brimir wraps this hardware layer in a libretro core, adding performance optimizations and frontend integration without modifying the emulation engine. Both projects are licensed under GPL.

## Contributing

Contributions are welcome. Please ensure code follows the existing style and includes appropriate testing.
