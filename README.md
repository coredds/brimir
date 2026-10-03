# Brimir

A Sega Saturn emulation core for libretro, built on the [Ymir](https://github.com/StrikerX3/ymir) emulator by StrikerX3.

## Overview

Brimir is a libretro core for Sega Saturn emulation, wrapping Ymir's cycle-accurate hardware layer. It provides accurate emulation with optimized software rendering and full VDP1/VDP2 support.

**Current Status**: Active development. Based on the upstream Ymir hardware-layer sync (2026-06-23), with selected upstream fixes through 2026-09-24 and Brimir-specific optimizations.

## What's New in v0.5.5

- **Crash fix** - RetroArch on Windows no longer exits on Quick Menu > Close Content ([#7](https://github.com/coredds/brimir/issues/7)). Hotfix on top of v0.5.4 with no other changes; save states and saves are unaffected.

## Previous Highlights

### v0.5.4

- **Compatibility fixes** - SMPC now ignores SSHON while the slave SH-2 is running, fixing Guardian Heroes level transitions and letting Gekitotsu Koushien, Madden NFL 97 (Europe), Ten Pin Alley, UEFA Euro 96 - England, and No-appointment Gals Olympos go in-game. VDP1 no longer clears COPR at frame start (fixes Alone in the Dark - One-Eyed Jack's Revenge lockups). SH-2 cache workarounds added for Hissatsu! and No-appointment Gals Olympos.
- **Threaded VDP1 rendering** - CPU framebuffer writes are no longer lost when the render thread publishes its framebuffer, fixing the Waialae no Kiseki - Extra 36 Holes title screen with the default threaded VDP1 option.
- **VDP2 correctness** - mid-frame back screen/line color and display-disable changes take effect, EXTEN reads latch HCNT, and rotation backgrounds no longer overrun line buffers when the resolution changes mid-frame.
- **Save states** - VCNT is now restored correctly, which matters for rewind and run-ahead. Save-state layout is unchanged.
- **Regression coverage** - 83 active tests with 647,796 assertions pass on Windows x64 and Linux x64.

### v0.5.3

- **Compatibility fixes** - Dragon Force II SH-2 cache workaround; corrected HLE CD seek track numbering, playback-reset ordering, and pregap-relative sub-Q timing.
- **Rendering correctness** - fixed ARM64 per-pixel blend ratios and SSE2/NEON color-gradation selection.
- **Host-side optimizations** - enabled baseline SSE2 on Windows x64, stopped profiling collection when disabled, corrected LTO compilation/link targeting, and resample mono MP3/OGG audio before stereo expansion.
- **Regression coverage** - 71 active tests with 647,107 assertions pass on Windows x64, Linux x64, and ARM64 under QEMU, with LTO enabled and disabled. Save-state layout and CPU requirements are unchanged.

Synthetic Windows sprite-rendering tests measured approximately 5-8% lower median render time with SSE2 and identical frame hashes. LTO showed no clear additional gain in those scenes. These are not game-FPS claims; native macOS ARM64 and real-game performance validation remain outstanding.

### v0.5.2

- **Ymir hardware-layer backports** - VDP2 color-gradation fix (Astal fog effect), BIN/CUE loader sanity checks for malformed sheets, and a devlog crash fix when switching VDP renderers.

### v0.5.1

- **Multi-file CUE / compressed audio support** — the BIN/CUE loader now handles multiple `FILE` entries and decodes `MP3`/`OGG` audio tracks to 44.1 kHz stereo PCM, with hardening against empty/invalid audio files.

### v0.5.0

- **Ymir hardware-layer backports** — VDP2 EXTEN save-state fix, SCU 8-bit DSP writes, VDP1 MSB/force-align fixes, Virtua Gun graduation, SMPC callback cleanup, CDBlock HLE unimplemented-command status, and MSVC `FORCE_INLINE_EX` hints.
- **Interlaced-mode performance** — vectorized the final VDP2 alpha-opaquing pass and skip resetting the unused alt-field fetcher bank when deinterlacing is disabled.
- **Deployment fix** — `deploy-retroarch.ps1` now correctly finds `brimir_libretro.info` under `resources\info\`.

## Features

### Emulation
- **Ymir Hardware Layer**: Cycle-accurate Saturn emulation, based on the 2026-06-23 sync with selected upstream fixes through 2026-09-24
- Accurate SH-2 dual-CPU emulation with WB/EX stall timing, 32-bit instruction fetch, and inlined opcode decode
- Full VDP1 sprite engine and VDP2 scroll plane graphics with COPR register fix
- SCSP (Saturn Custom Sound Processor) audio with configurable interpolation and volume control
- M68000 sound CPU emulation
- SCU DSP and DMA emulation
- CHD and ISO disc format support via libchdr
- Save states (LZ4-compressed) and backup RAM persistence
- Versioned save states with backward compatibility
- SRAM managed as a single source of truth for libretro `.srm` files
- Per-region persistent SMPC/RTC data with automatic migration from the legacy filename
- Auto-detection of console region from disc; real NTSC/PAL timing reported to the frontend
- Configurable CD read speed (2x-32x/Max)
- SH-2 CPU overclocking (100%-300%)
- RAM expansion cartridge support (1MB, 4MB, 6MB), inserted automatically from the game database

### Rendering
- **Software Renderer**: Ymir's proven software renderer with pixel-perfect accuracy
  - SIMD software rendering: baseline SSE2 on x64, NEON on ARM64, optional AVX2 on compatible x64 CPUs
  - Threaded VDP1 and VDP2 rendering for optimal performance
  - Full resolution output (no overscan cropping by VDP)
- **Deinterlacing**: Multiple modes for interlaced video
  - Bob (smooth 60 FPS, no scanlines)
  - Weave (CRT-style scanlines)
  - Blend (field blending)
  - Current (legacy dual-field)
- **Post-Processing**:
  - Configurable overscan cropping (horizontal and vertical)
  - Screen rotation (TATE) for vertical games

### Integration
- Libretro API v2 with live-updatable core options
- Compatible with RetroArch and other libretro frontends
- XRGB8888 pixel format output
- Memory descriptors for RetroArch cheat search / memory viewer
- Real NTSC/PAL region reporting with dynamic AV timing updates
- Save state versioning for forward compatibility
- Performance profiling option (gated via core option)
- Full controller remapping support

## Known Limitations

- **Interlaced / high-resolution performance** — The software VDP2 renderer can be CPU-bound in interlaced titles such as *Virtua Fighter 2*. When the core cannot complete a frame within the NTSC/PAL frame budget, RetroArch compensates by stretching or dropping audio, causing music to cut out or slow down while video remains smooth. Deinterlacing Mode = `None` provides the best performance in these titles.
- **Cheats** — `retro_cheat_set` / `retro_cheat_reset` are stubs; cheat codes are not applied yet.
- **Controllers** — Only the standard Saturn Control Pad is exposed (ports 1 and 2). ROM cartridges other than DRAM expansion carts are not inserted automatically.
- **SH-2 JIT** — Experimental and off by default (core option "SH-2 JIT (Experimental)", key `brimir_sh2_jit`). It compiles every SH-2 instruction except `TRAPA`, `RTE`, `SLEEP` and illegal opcodes, which stay on Ymir's interpreter, and is validated against the interpreter in lockstep (BIOS plus six games, up to 36000 frames each, identical; see [design/sh2-validation.md](design/sh2-validation.md)). On x86-64 builds it generates native x86-64 code (via asmjit); other builds run the compiled blocks through an IR interpreter. Both are validated against the interpreter. The x64 backend runs at about the interpreter's speed (its SH-2 time is between 20% below and 10% above the interpreter's across six games) and the IR interpreter backend about 3x slower (see [design/sh2-x64-performance.md](design/sh2-x64-performance.md)), and it stutters while compiling new code, badly in games that keep reloading their code: Burning Rangers runs at about half the interpreter's speed over a long session. It uses up to 128 MB of native code per SH-2 CPU, allocated on demand. It is not used while SH-2 cache emulation is active. Optimizations are planned (see [design/sh2-jit.md](design/sh2-jit.md) and [ROADMAP.md](ROADMAP.md)).

## Build Requirements

- Release targets: Windows x64, Linux x64/ARM64, and macOS ARM64
- C++20 compiler: release CI uses MSVC 2022+, GCC 14, or Apple Clang on macOS 14
- CMake 3.28+

## Building

### Windows

```powershell
# Use a fresh directory to avoid stale compiler flags in an existing CMake cache.
cmake -S . -B build-release -G "Visual Studio 17 2022" -A x64 -DBRIMIR_LTO=ON -DBrimir_ENABLE_IPO=ON
cmake --build build-release --config Release --target brimir_libretro

# Output: build-release\bin\Release\brimir_libretro.dll
```

### Linux

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_C_COMPILER=gcc-14 -DBRIMIR_LTO=ON -DBrimir_ENABLE_IPO=ON
cmake --build build-release --target brimir_libretro -j$(nproc)

# Output: build-release/lib/brimir_libretro.so
```

LTO applies to optimized configurations, not Debug. To disable it completely, set both `BRIMIR_LTO=OFF` and `Brimir_ENABLE_IPO=OFF`. Optional `Brimir_AVX2=ON` raises the x64 CPU requirements and is not enabled in the portable release builds.

On Windows, run the commands from a *Developer PowerShell for VS 2022* (or any shell with the MSVC environment loaded) when using the Ninja generator.

## Testing

Unit and regression tests use Catch2 (amalgamated, in `tests/`) and are registered with CTest:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBRIMIR_BUILD_TESTS=ON
cmake --build build --target brimir_tests
ctest --test-dir build --output-on-failure

# Or run the Catch2 binary directly, optionally filtered by tag:
./build/bin/brimir_tests "[core]"
```

The suite covers core options, the `CoreWrapper` bridge (save states, SRAM persistence, rotation, overscan, audio), the media loader (CUE/CCD, multi-file, MP3/OGG tracks), Ymir hardware-layer backports, VDP color calculation, CD block HLE and the game database.

A few integration tests boot a real Saturn BIOS. They run only when a BIOS image is placed in `tests/fixtures/` (e.g. `sega_101.bin`) and are skipped otherwise. BIOS images are copyrighted and ignored by git — never commit them.

Continuous integration (`.github/workflows/ci.yml`) builds the core and runs the test suite on Windows x64, Linux x64 and macOS ARM64 for every push to `master` and every pull request. Tagged releases are built by `.github/workflows/release.yml`.

## Installation

### Quick Deploy

```powershell
# Automatically builds and copies to RetroArch
.\deploy-retroarch.ps1
```

The deployment helper uses its own `build` directory, not `build-release`. For the builds above, use the manual installation steps with the DLL or library from the documented output path.

### Manual Install

1. Copy `brimir_libretro.dll` to your RetroArch `cores/` directory.
2. Place a compatible Saturn BIOS file in the RetroArch `system/` directory. Supported BIOS files (any one will work):
   - `sega_101.bin` (US v1.01)
   - `mpr-17933.bin` (US v1.00)
   - `sega_100.bin` (EU v1.00)
   - `Sega Saturn BIOS v1.01 (JAP).bin`
   - `Sega Saturn BIOS v1.00 (JAP).bin`
3. Load a Saturn game (.cue, .chd, .ccd, .mds, .iso or .m3u playlist) through RetroArch. Raw `.bin`/`.img`/`.mdf` track files must be opened through their `.cue`/`.ccd`/`.mds` sheet.

## Core Options

| Category | Option | Values |
|----------|--------|--------|
| System | BIOS Selection | Auto, JP v1.01, JP v1.00, US v1.01, US v1.00, EU v1.00, EU alt |
| System | Auto-Detect Region from Disc | On/Off |
| System | SH-2 CPU Overclock | 100% (Stock), 125%, 150%, 175%, 200%, 250%, 300% |
| System | SH-2 JIT (Experimental) | On/Off (default Off; not faster yet, for testing) |
| System | Performance Profiling | On/Off |
| Video | Deinterlacing | On/Off |
| Video | Deinterlacing Mode | Bob, Weave, Blend, Current, None |
| Video | Threaded VDP1 Rendering | On/Off |
| Video | Threaded VDP2 Rendering | On/Off |
| Video | Screen Rotation (TATE) | None, 90°, 180°, 270° |
| Video | Overscan Crop | None, Small (~16px), Medium (~32px), Large (~48px) |
| Audio | Audio Interpolation | Linear, Nearest |
| Audio | Audio Volume | 0%, 25%, 50%, 75%, 100%, 125%, 150%, 175%, 200% |
| Media | CD Read Speed | 2x, 4x, 6x, 8x, 12x, 16x, 24x, 32x, Max (200x) |
| Media | Preload Disc to RAM | On/Off |

## Project Structure

```
brimir/
  src/
    core/
      include/ymir/   Ymir hardware layer (verbatim upstream sync, except the SH-2 fork)
      include/brimir/ Brimir-specific additions
      src/ymir/       Ymir source files (verbatim, except the SH-2 fork)
    bridge/           CoreWrapper -- interface between emulator and frontends
    libretro/         Libretro API implementation and core options
    jit/              SH-2 JIT library (experimental)
  include/         Public headers (libretro.h, CoreWrapper)
  resources/info/  Libretro core info file
  vendor/          Vendored dependencies
  tests/           Catch2 unit and regression tests
  tools/           Development utilities (brimir_bench headless frame benchmark)
  cmake/           CMake helper modules
```

## Dependencies

All dependencies are vendored in the `vendor/` directory:

- **fmt** -- Formatting library
- **mio** -- Memory-mapped I/O
- **concurrentqueue** -- Lock-free queue
- **xxHash** -- Fast hashing
- **lz4** -- Compression
- **libchdr** -- CHD disc format support (includes zlib, zstd, lzma)
- **dr_libs** -- MP3 decoding (dr_mp3) for CUE audio tracks
- **stb** -- Ogg Vorbis decoding (stb_vorbis) for CUE audio tracks
- **asmjit** -- x86-64 code generation for the SH-2 JIT (zlib license; x86-64 builds only)

Upstream test suites, benchmarks and examples are not vendored.

## License

Licensed under the GNU General Public License v3.0 (GPL-3.0), the same license as Ymir. See [LICENSE](LICENSE).

## Credits

Brimir is built on **[Ymir](https://github.com/StrikerX3/ymir)**, a cycle-accurate Sega Saturn emulator by **StrikerX3**. The hardware layer under `src/core/` is synced from upstream Ymir — all Saturn CPU, VDP, audio, and peripheral emulation is Ymir's work. The SH-2 files are a Brimir-maintained fork of Ymir's SH-2 with hooks for the SH-2 JIT, plus a read-only page-table accessor for the JIT in `ymir/sys/bus.hpp`; see `src/core/BRIMIR_FORK.md`. Apart from these, Brimir uses the hardware layer unmodified and wraps it in a libretro core, adding performance optimizations and frontend integration. Both projects are licensed under GPL.

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for coding standards and the pull request process. Please make sure `ctest` passes and add tests for new behavior.
