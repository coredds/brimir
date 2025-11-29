# Brimir Development Roadmap

This document outlines the development roadmap for the Brimir libretro core.

**Last Updated:** 2025-11-29  
**Current Version:** 0.1.3

---

## Overview

Brimir is a performance-focused fork of Ymir for libretro, targeting modern platforms and entry-level retro handhelds. Development follows semantic versioning with phased releases:

```
Phase 1: Foundation & Performance  → v0.1.3 (Complete)
Phase 2: Extended Features         → v0.2.0 (Planned)
Phase 3: JIT & Handheld Support    → v0.3.0 (Goal)
Phase 4: Production Release        → v1.0.0 (Goal)
```

### Primary Goal for 2026

**Enable full-speed Saturn emulation on entry-level retro handhelds** through SH-2 JIT compilation and platform-specific optimizations, targeting the Trimui Smart Pro S as the reference platform.

---

## Phase 1: Foundation & Performance → v0.1.3 (COMPLETE)

**Goal:** Create a fully functional core with high-performance rendering

### Completed Features
- Ymir emulator integration with active performance enhancements
- CMake + vcpkg build system
- Complete libretro API implementation
- Video output (RGB565, dynamic resolution, deinterlacing)
- Audio output (44.1 kHz stereo, ring buffer)
- Controller support (digital pads, 2 players) with input descriptors
- Save states with full serialization
- SRAM / Backup RAM persistence
- Real-Time Clock (RTC) system-wide persistence
- Multi-format disc loading (CHD, BIN/CUE, ISO, etc.)
- 6 BIOS versions with auto-detection (JP v1.003 not supported)
- Core options (BIOS, region, video, audio, performance)
- DRAM expansion cartridge support (1MB/4MB/6MB) with auto-detection
- Game database integration (24+ games)
- 82 comprehensive test cases
- Deployment automation
- Comprehensive documentation

### Performance Achievements (v0.1.3)
- **60+ FPS @ 704×448i** high-resolution interlaced modes (2.4× faster than v0.1.2)
- **Mednafen-competitive performance** through VDP2 tile-row caching
- **SIMD optimizations** for window masking (AVX2/SSE2/NEON)
- **Pixel-perfect accuracy** maintained through all optimizations
- Cross-platform force-inlining for consistent performance
- Clean production build (profiling overhead removed)

### Tested Games
- Sega Rally Championship (USA) - Full compatibility, 60 FPS
- Saturn Bomberman (Japan) - Full compatibility, 60 FPS
- Panzer Dragoon Zwei (USA/Europe/Japan) - Full compatibility, 60+ FPS @ 704×448i menus
- King of Fighters '96 (Japan) - Full compatibility with 1MB DRAM expansion
- Street Fighter Zero 3 (Japan) - Full compatibility with 4MB DRAM expansion

**Status:** v0.1.3 released November 29, 2025

---

## Phase 2: Extended Features → v0.2.0 (PLANNED)

**Goal:** Expand compatibility and controller support

### Planned Features
- **Additional Controller Types**
  - [ ] Analog pad (Mission Stick, 3D Control Pad)
  - [ ] Racing wheel
  - [ ] Mission Stick (flight stick)
  - [ ] Mouse
  - [ ] Gun (Virtua Gun)
  - [ ] Keyboard

- **Multi-Disc Support**
  - [ ] M3U playlist handling
  - [ ] Disc swap via libretro API
  - [ ] Test multi-disc games (Panzer Dragoon Saga, etc.)

- **ROM Cartridge Support**
  - [ ] Implement ROM cartridge loading
  - [ ] Support King of Fighters '95
  - [ ] Support Ultraman

- **Cartridge RAM Persistence**
  - [ ] Implement .cart file saving/loading
  - [ ] Persist expansion RAM data between sessions

- **Backup RAM Cartridge**
  - [ ] Support for external backup RAM cartridge

- **Extended Testing**
  - [ ] Test 50+ games for compatibility
  - [ ] Create compatibility database
  - [ ] Document game-specific issues

- **Platform Support**
  - [ ] Linux testing and fixes
  - [ ] macOS testing and fixes
  - [ ] ARM builds (Windows ARM64, Linux ARM)

- **Core Options Enhancements**
  - [ ] Per-game option overrides
  - [ ] More VDP2 options
  - [ ] Save/load option presets

### Target Compatibility
- 70%+ games playable
- Major titles fully working
- Known issues documented

**Estimated Timeline:** 4-6 weeks

---

## Phase 3: JIT & Handheld Optimization → v0.3.0 (2026 GOAL)

**Goal:** Enable full-speed emulation on entry-level retro handhelds

### Target Features
- **SH-2 JIT Compiler**
  - [ ] Dynamic recompilation for master SH-2
  - [ ] Dynamic recompilation for slave SH-2
  - [ ] ARM target code generation (ARMv7, ARMv8)
  - [ ] x86-64 code generation
  - [ ] Cache management and invalidation
  - [ ] Compatibility testing (accuracy vs performance modes)

- **Handheld Platform Optimization**
  - [ ] Target: Trimui Smart Pro S (ARM Cortex-A53)
  - [ ] NEON SIMD optimizations for VDP rendering
  - [ ] Memory footprint reduction for limited RAM
  - [ ] Battery-friendly performance profiles
  - [ ] Resolution scaling for small screens

- **Performance Profiling**
  - [ ] Frame-by-frame performance analysis
  - [ ] CPU hotspot identification
  - [ ] Memory bandwidth optimization
  - [ ] Cache efficiency improvements

- **Platform Builds**
  - [ ] ARM64 Linux builds
  - [ ] ARM32 Linux builds (if needed)
  - [ ] Performance benchmarks per platform
  - [ ] Automated testing on target hardware

### Target Performance
- 60 FPS gameplay on Trimui Smart Pro S (Cortex-A53 @ 1.5 GHz)
- 90%+ games playable at full speed with JIT enabled
- Graceful degradation for accuracy mode (40-50 FPS without JIT)

**Estimated Timeline:** Q2-Q3 2026

---

## Phase 4: Production Release → v1.0.0 (GOAL)

**Goal:** Production-ready core with broad compatibility

### Target Features
- **Official Release**
  - [ ] Submit to libretro buildbot
  - [ ] Inclusion in RetroArch core downloader
  - [ ] Official core info file
  - [ ] Release on GitHub

- **Compatibility**
  - [ ] 85%+ game compatibility
  - [ ] All major titles working
  - [ ] Compatibility list published

- **Performance**
  - [ ] Maintain 60 FPS on mid-range hardware
  - [ ] Profile low-end devices
  - [ ] Optimize memory usage

- **Documentation**
  - [ ] User guide
  - [ ] Troubleshooting guide
  - [ ] Developer documentation
  - [ ] API documentation

- **Achievement Support**
  - [ ] RetroAchievements integration (if feasible)
  - [ ] Memory map for achievements

- **Additional Features**
  - [ ] Cheat code support
  - [ ] Shader compatibility testing
  - [ ] Netplay support (if feasible)

### Target Compatibility
- 85%+ games playable
- All major franchises working
- Stable, bug-free experience

**Estimated Timeline:** 8-12 weeks

---

## Future Enhancements (Post-1.0)

These are potential features for future releases:

### Advanced Features
- **CD Block LLE** (Low-Level Emulation)
  - Requires CD block ROM
  - Higher accuracy

- **Hardware Renderer**
  - OpenGL/Vulkan acceleration
  - Higher internal resolutions

- **Enhanced Graphics**
  - Texture filtering
  - Higher polygon precision
  - Anti-aliasing

- **Debugging Tools**
  - CPU debugger
  - Memory viewer
  - VDP visualization

- **Quality of Life**
  - Fast-forward (already supported by RetroArch)
  - Rewind (already supported by RetroArch)
  - Run-ahead (requires investigation)

### Platform Expansion
- Android support
- iOS support (if possible)
- Raspberry Pi optimization

---

## Relationship with Upstream Ymir

Brimir is an active fork focused on performance and platform optimization:

- **Foundation:** Built on Ymir's cycle-accurate architecture
- **License:** All changes respect Ymir's GPL-3.0 license
- **Upstream Contributions:** Performance and compatibility fixes may be contributed back where applicable
- **Independent Development:** JIT compiler and handheld optimizations developed specifically for Brimir
- **Update Strategy:** Periodically evaluate upstream changes for integration

### Development Philosophy
- Maintain architectural compatibility with Ymir
- Prioritize performance and platform reach
- Allow reasonable accuracy trade-offs for speed (user-configurable)
- Document all deviations from reference implementation

---

## Release Schedule

| Version | Target Date | Status |
|---------|-------------|--------|
| 0.1.0 | 2025-11-25 | Released |
| 0.1.1 | 2025-11-25 | Released |
| 0.1.2 | 2025-11-27 | Released |
| 0.1.3 | 2025-11-29 | Released (Current) |
| 0.2.0 | 2026-Q1 | Planned |
| 0.3.0 | 2026-Q2-Q3 | Planned (JIT) |
| 1.0.0 | 2026-Q4 | Goal |

---

## Contributing

Want to help with the roadmap? Check out:

- **[CONTRIBUTING.md](CONTRIBUTING.md)** - Contribution guidelines
- **[GitHub Issues](https://github.com/coredds/brimir/issues)** - Current tasks
- **[GitHub Projects](https://github.com/coredds/brimir/projects)** - Project board

---

## Feedback

Have suggestions for the roadmap? Open an issue or discussion on GitHub!

- **Feature Requests:** [GitHub Issues](https://github.com/coredds/brimir/issues/new?template=feature_request.md)
- **Discussions:** [GitHub Discussions](https://github.com/coredds/brimir/discussions)

---

<p align="center">
  <em>This roadmap is subject to change based on community feedback and development priorities.</em>
</p>
