# Brimir

A Sega Saturn emulation core for libretro, targeting high performance and accuracy.

## Overview

Brimir is a libretro core for Sega Saturn emulation, currently focused on Windows x64 with MSVC. The project is built on a foundation inspired by the Ymir architecture, with ongoing development to optimize performance and expand platform support.

**Current Status**: Active development - interpreter-based emulation is functional, JIT compiler in planning phase.

## Features

- Accurate SH-2 CPU emulation with hardware-accelerated optimizations
- Full VDP1 and VDP2 graphics support
- SCSP audio emulation
- CHD and ISO disc format support
- Save state functionality
- Libretro API integration for broad frontend compatibility

## Build Requirements

- Windows 10/11 (x64)
- Visual Studio 2022 Build Tools (with "Desktop development with C++" workload)
- CMake 3.28+
- Ninja (recommended, installed automatically with VS Build Tools)

### Quick Install

Run in PowerShell as Administrator:
```powershell
.\setup-env.ps1
```

## Building

**Standard build command** (use this for all builds):

```powershell
powershell -ExecutionPolicy Bypass -File .\build-all.ps1 -Clean
```

### Build Options

| Option | Description |
|--------|-------------|
| `-Clean` | Clean rebuild (removes build directory first) |
| `-BuildType Debug` | Build in Debug mode (default: Release) |
| `-WithJIT` | Include JIT test framework |
| `-WithBenchmarks` | Run benchmarks after build |

### Examples

```powershell
# Standard release build
.\build-all.ps1

# Clean release build
.\build-all.ps1 -Clean

# Debug build
.\build-all.ps1 -BuildType Debug

# Build with JIT tests
.\build-all.ps1 -WithJIT

# Full rebuild with benchmarks
.\build-all.ps1 -Clean -WithBenchmarks
```

### Output

After successful build: `build\brimir_libretro.dll`

## Installation

Copy `brimir_libretro.dll` to your libretro frontend's cores directory (e.g., RetroArch).

## Current Limitations

- Windows x64 only (Linux/macOS support planned)
- MSVC compiler required (GCC/Clang support planned)
- Interpreter-based execution (JIT compiler in development)
- Some games may have compatibility issues

## Roadmap

- **Phase 1**: Stabilize interpreter and improve game compatibility
- **Phase 2**: Implement SH-2 JIT compiler for performance
- **Phase 3**: Expand platform support (Linux, macOS, ARM devices)
- **Phase 4**: Advanced optimizations and edge case handling

## Dependencies

All dependencies are vendored in the `vendor/` directory:
- fmt - Formatting library
- mio - Memory-mapped I/O
- concurrentqueue - Lock-free queue
- xxHash - Fast hashing
- lz4 - Compression
- libchdr - CHD format support

## License

Licensed under the GPLv2. See LICENSE file for details.

## Acknowledgments

This project's architecture and licensing are inspired by the Ymir emulator core. Special thanks to the Ymir developers for their foundational work on Saturn emulation.

## Contributing

Contributions are welcome. Please ensure code follows the existing style and includes appropriate testing.

## Contact

For issues and feature requests, please use the GitHub issue tracker.
