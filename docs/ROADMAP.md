# Brimir Development Roadmap

This document outlines the development roadmap for the Brimir libretro core.

**Last Updated:** 2025-11-27  
**Current Version:** 0.1.2

---

## Overview

Brimir development follows semantic versioning with phased releases:

```
Phase 1: Foundation          → v0.1.2 (Complete)
Phase 2: Extended Features   → v0.2.0 (Planned)
Phase 3: Production Release  → v1.0.0 (Goal)
```

---

## Phase 1: Foundation → v0.1.2 (COMPLETE)

**Goal:** Create a fully functional core with essential features

### Completed Features
- Ymir emulator integration
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
- Performance optimizations (60 FPS at all resolutions)
- 82 comprehensive test cases
- Deployment automation
- Comprehensive documentation

### Performance Achievements
- Consistent 60 FPS at 320×224 (gameplay)
- Consistent 60 FPS at 704×480 (menus)
- Zero frame drops during resolution changes
- Optimized SRAM, video, and audio pipelines
- Deinterlacing support for hi-res interlaced modes

### Tested Games
- Sega Rally Championship (USA) - Full compatibility
- Saturn Bomberman (Japan) - Full compatibility
- Panzer Dragoon Zwei (USA/Europe/Japan) - Full compatibility with hi-res interlaced menus
- King of Fighters '96 (Japan) - Full compatibility with 1MB DRAM expansion
- Street Fighter Zero 3 (Japan) - Full compatibility with 4MB DRAM expansion

**Status:** v0.1.2 released November 27, 2025

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

## Phase 3: Production Release → v1.0.0 (GOAL)

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

## Upstream Ymir Tracking

Brimir tracks the Ymir emulator project:

- **Current Ymir Version:** Commit `[hash]` (November 2025)
- **Update Cadence:** Monthly check for upstream changes
- **Integration Strategy:** Evaluate impact, test, merge

### Notable Ymir Features to Track
- SH-2 JIT compiler (performance boost)
- Hardware rendering backend
- Additional peripheral support
- Accuracy improvements

---

## Release Schedule

| Version | Target Date | Status |
|---------|-------------|--------|
| 0.1.0 | 2025-11-25 | Released |
| 0.1.1 | 2025-11-25 | Released |
| 0.1.2 | 2025-11-27 | Released (Current) |
| 0.2.0 | 2026-Q1 | Planned |
| 1.0.0 | 2026-Q2 | Goal |

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
