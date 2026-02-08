# Brimir

A Sega Saturn emulation core for libretro, targeting high performance and accuracy.

## Overview

Brimir is a libretro core for Sega Saturn emulation, built on a foundation inspired by the Ymir emulator architecture. It provides both a software renderer and an experimental Vulkan GPU renderer with upscaling and post-processing capabilities.

**Current Status**: Active development -- interpreter-based emulation is functional with GPU-accelerated upscaling via Vulkan. JIT compiler is in the planning phase.

## Features

### Emulation
- Accurate SH-2 dual-CPU emulation (interpreter)
- Full VDP1 sprite engine and VDP2 scroll plane graphics
- SCSP (Saturn Custom Sound Processor) audio with configurable interpolation
- M68000 sound CPU emulation
- SCU DSP and DMA emulation
- CHD and ISO disc format support via libchdr
- Save state and backup RAM persistence
- Auto-detection of console region from disc
- Configurable CD read speed (2x-16x)

### Rendering
- **Software Renderer**: Accurate, fully featured, optimized with SSE4.2/AVX2 intrinsics
- **Vulkan Renderer** (Experimental): Headless GPU rendering for upscaling and post-processing
  - Internal resolution scaling: 1x (native), 2x, 4x, 8x
  - Upscale filters: Nearest, Bilinear, Sharp Bilinear, FSR 1.0 EASU (edge-adaptive)
  - Post-processing: FXAA, RCAS sharpening (FSR 1.0)
  - Color debanding for Saturn's RGB555 palette
  - Brightness and gamma correction
- Deinterlacing modes: Bob, Weave, Blend, Current field
- Horizontal blend filter for high-res interlaced modes
- Configurable overscan cropping (horizontal and vertical)
- Frameskip support

### Integration
- Libretro API v2 with full core options support
- Compatible with RetroArch and other libretro frontends
- XRGB8888 pixel format output
- Performance profiling (logged every 300 frames)

## Build Requirements

- Windows 10/11 (x64) or Linux (x64)
- C++20 compiler: MSVC 2022+, GCC 11+, or Clang 14+
- CMake 3.28+
- Vulkan SDK (optional, for shader compilation)

## Building

### Windows

```powershell
# Using the build script (recommended)
.\build.ps1 -Generator "Visual Studio 17 2022"

# Or manually with CMake
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Output: build\bin\Release\brimir_libretro.dll
```

### Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Output: build/brimir_libretro.so
```

### Shader Compilation

The Vulkan shaders are embedded as SPIR-V bytecode. To recompile after modifying GLSL sources:

```powershell
cd src\core\src\hw\vdp\gpu\shaders
.\compile_shaders.ps1
```

This requires the Vulkan SDK with `glslc`. The build system will pick up the updated `embedded_shaders.hpp` automatically.

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
| Video | Renderer | Software, Vulkan (Experimental) |
| Video | Internal Resolution | 1x, 2x, 4x, 8x (GPU only) |
| Video | Upscale Filter | Nearest, Bilinear, Sharp Bilinear, FSR 1.0 (GPU only) |
| Video | Sharpening | Off, FXAA, RCAS (GPU only) |
| Video | Debanding | Off, On (GPU only) |
| Video | Brightness / Gamma | Adjustable (GPU only) |
| Video | Deinterlacing | On/Off, modes: Bob, Weave, Blend, Current, None |
| Video | Overscan | Horizontal and vertical cropping |
| Video | Frameskip | 0-3 frames |
| Audio | Interpolation | Linear, Nearest |
| Media | CD Read Speed | 2x-16x |

## Project Structure

```
brimir/
  src/
    core/          Core emulation (SH-2, VDP1/2, SCSP, SCU, CD block, etc.)
      src/hw/vdp/gpu/     Vulkan renderer and GLSL shaders
    bridge/        CoreWrapper -- interface between emulator and frontends
    libretro/      Libretro API implementation and core options
    jit/           SH-2 JIT compiler (Phase 0 -- planning/infrastructure)
  include/         Public headers
  vendor/          Vendored dependencies
  tests/           Unit and integration tests
  tools/           Development utilities
```

## Current Limitations

- x64 only (ARM64 support planned)
- macOS not yet tested
- Interpreter-based SH-2 execution (JIT compiler in planning)
- VDP1 sprite rendering and VDP2 NBG layer rendering are software-only (GPU stubs exist)
- Some games may have compatibility issues

## Roadmap

- **Phase 1** (current): Stabilize interpreter, improve game compatibility, refine GPU upscaling pipeline
- **Phase 2**: Implement SH-2 JIT compiler for x86-64
- **Phase 3**: Expand platform support (macOS, ARM64)
- **Phase 4**: GPU-native VDP1/VDP2 rendering, ARM64 JIT backend

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

## Acknowledgments

This project's architecture is inspired by the Ymir emulator by StrikerX3. Special thanks to the Ymir developers for their foundational work on Saturn emulation.

## Contributing

Contributions are welcome. Please ensure code follows the existing style and includes appropriate testing.
