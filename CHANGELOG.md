# Changelog

All notable changes to the Brimir project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.3.0] - 2026-02-20

### Changed
- **Ymir Hardware Layer Integration** - Complete adoption of Ymir's hardware emulation layer
  - Replaced custom hardware implementation with Ymir's proven, cycle-accurate emulation
  - Software renderer now uses Ymir's optimized VDP implementation
  - Simplified codebase by removing custom VDP1/VDP2 split architecture
  - Easier upstream synchronization with Ymir project
  
- **API Compatibility Fixes**
  - Fixed `LoadInternalBackupMemoryImage` to include `copyOnWrite` parameter
  - Updated threading configuration to use separate `threadedVDP1` and `threadedVDP2`
  - Changed cartridge access from `GetCartridgeSlot()` to `SCU.GetCartridge()`
  - Removed `SetSH2SyncStep()` (Ymir uses fixed synchronization)

### Removed
- **GPU Rendering** - Removed experimental Vulkan renderer (will be re-architected in future)
  - Removed internal resolution scaling options
  - Removed upscale filter options (Nearest, Bilinear, Sharp Bilinear, FSR)
  - Removed post-processing options (FXAA, RCAS sharpening)
  - Removed color adjustment options (debanding, brightness, gamma)
  - All GPU code preserved in `backup_brimir_hw/` for future re-integration

- **Core Options** - Removed obsolete options
  - Renderer selection (software is now the only mode)
  - All GPU-only video enhancement options
  - SH-2 sync step option (not applicable with Ymir)

### Technical
- Software rendering provides pixel-perfect output matching Ymir
- Full VDP1 and VDP2 support with threaded rendering
- Deinterlacing modes preserved (Bob, Weave, Blend, Current)
- SIMD optimizations maintained for pixel format conversion
- Save states and backup RAM fully compatible

---

## [0.1.3] - 2025-11-29

### Added
- **Mednafen-Style Tile-Row Caching** - Major VDP2 rendering performance optimization
  - Implements tile-row caching to reduce redundant VRAM reads
  - Renders each 8-pixel tile row once and reuses cached data for consecutive pixels
  - Up to 7 out of 8 pixels rendered with zero VRAM access
  - **+90% FPS improvement** in high-resolution modes (31.6 → 60+ FPS @ 704x448i)
  
- **SIMD Window Masking Optimization** - Cross-platform vectorized transparency processing
  - AVX2/SSE2/NEON implementations for window mask application
  - Processes entire scanlines at once instead of per-pixel
  - **+11% FPS improvement** (28.6 → 31.6 FPS)

### Performance
- **High-Resolution Mode Performance** - Now runs at full 60 FPS @ 704x448i
  - **2.4× overall speedup** from v0.1.2 (25 FPS → 60+ FPS)
  - Matches Mednafen Saturn core performance
  - Zero rendering artifacts or accuracy loss
  - Pixel-perfect output verified via regression testing

### Fixed
- **Tile Rendering Byte-Order Issues** - Corrected Palette16/256 pixel extraction
  - Fixed distorted text/graphics in tile-batch renderer
  - Proper individual byte reads instead of bulk reads with bit manipulation
  - Maintains tile-row caching performance while ensuring accuracy

---

## [0.1.2] - 2025-11-27

### Added
- **Input Descriptors** - Full RetroArch controller remapping support
  - Added `RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS` implementation
  - Proper button labels for Saturn controller (A, B, C, X, Y, Z, L, R, Start)
  - Descriptors for both Player 1 and Player 2
  - Fixes `input_descriptors = "true"` claim in `.info` file
  - Enables proper button remapping in RetroArch's controller configuration UI

- **Deinterlacing Support** - New core option for interlaced video modes
  - Added "Deinterlacing" option in Video Settings (enabled by default)
  - Fixes high-resolution interlaced menu rendering (e.g., Panzer Dragoon Zwei)
  - Users can disable for minor performance gain in progressive-only games
  - Added `SetDeinterlacing()` method to CoreWrapper

### Changed
- **Deinterlacing Default** - Now enabled by default (was disabled for performance)
  - Necessary for games with hi-res interlaced menus (704x480 interlaced)
  - Panzer Dragoon Zwei menus now render correctly

### Fixed
- **Interlaced Menu Rendering** - Games using hi-res interlaced modes for menus now display correctly
  - Panzer Dragoon Zwei: 640x224 progressive gameplay + 704x480 interlaced menus both work
  - Other games with similar resolution switching patterns benefit

### Tested Games
- ✅ **Panzer Dragoon Zwei** - Gameplay excellent, hi-res interlaced menus now supported
- ✅ **King of Fighters '96** - Confirmed fully playable with 1MB DRAM expansion
- ✅ **Street Fighter Zero 3** - Confirmed fully playable with 4MB DRAM expansion

### Documentation
- **BIOS Compatibility** - Clarified that Japanese BIOS v1.003 (`sega1003.bin`) is NOT supported
  - Removed from supported BIOS table
  - Added prominent warning in BIOS Requirements section
  - Updated all documentation to recommend JP v1.01 or v1.00 instead
  - Removed from deployment scripts
- **Core Options** - Updated to document deinterlacing setting
- **Game Compatibility** - Updated tested games list with latest test results

---

## [0.1.0] - 2025-11-25

### Added
- **Core Emulation**
  - Full Ymir Sega Saturn emulator integration
  - Support for multiple disc formats: CHD, BIN/CUE, ISO, CCD, MDS, BIN
  - 7 BIOS versions supported with auto-detection (US, EU, JP variants)
  - Backup RAM (SRAM) with persistent storage (`.bup` files)
  - Real-Time Clock (RTC) persistence (`.smpc` files)

- **libretro API**
  - Complete libretro v1 API implementation
  - Video output: RGB565 format with dynamic resolution handling
  - Audio output: 44.1 kHz stereo with ring buffer
  - Controller support: Digital pad for Player 1 & 2
  - Save state support with serialization/deserialization
  - SRAM interface for backup RAM

- **Core Options**
  - BIOS selection (auto-detected from system directory)
  - Console region (Japan, North America, Europe, etc.)
  - Video standard (NTSC, PAL)
  - Threaded VDP rendering (Recommended/Experimental)
  - Audio interpolation (Linear, Nearest)
  - CD read speed multiplier (2x-200x)
  - Autodetect region from disc
  - SH-2 cache emulation
  - VDP2 deinterlacing modes
  - Transparent mesh rendering

- **Performance Optimizations**
  - Threaded VDP rendering for consistent 60 FPS
  - Optimized XBGR8888 → RGB565 pixel conversion
  - SRAM caching (refresh every 300 frames)
  - Audio ring buffer for low-latency streaming
  - Performance profiling support

- **Infrastructure**
  - CMake build system with vcpkg integration
  - Automated build scripts (Windows: PowerShell, Linux: Bash)
  - Automated environment setup (Windows)
  - Unit tests with Catch2
  - Integration tests with optional BIOS support
  - Deployment script for RetroArch (Windows)
  - Comprehensive documentation

- **Documentation**
  - Quick start guide
  - BIOS support guide
  - Architecture documentation
  - Performance optimization details
  - Testing guides and strategy
  - Contribution guidelines
  - Roadmap

### Performance
- Achieved consistent **60 FPS** at all resolutions on modern hardware
- Optimized for both low-resolution (320×224) and high-resolution (704×480) modes
- Zero frame drops during resolution changes

### Tested Games
- **Sega Rally Championship (USA)** - Full compatibility
  - Gameplay: 60 FPS
  - Menus: 60 FPS (704×480 interlaced)
  - Saves: Working
  - Clock: Persists

### Known Limitations
- Only digital pad controllers supported (no analog, mouse, wheel, etc.)
- Multi-disc games not tested
- Backup RAM cartridge not implemented
- CD Block LLE requires external ROM (not included)
- Primary testing on Windows x86-64 (Linux builds successfully but needs testing)

### Upstream
- **Ymir version:** Commit `[insert hash]` from November 2025
- Using Ymir's threaded VDP and deinterlacer
- Memory-mapped backup RAM implementation
- SMPC persistent data support

---

## [Unreleased]

### Planned for 0.2.0
- Additional controller types (analog pad, mission stick, mouse, wheel)
- Multi-disc game support
- Backup RAM cartridge support
- Extended compatibility testing
- Linux and macOS testing
- Achievement support (RetroAchievements)

---

## [0.1.1] - 2025-11-25

### Added
- **RAM Expansion Cartridge Support**
  - Automatic detection and insertion of expansion cartridges based on game database
  - 1MB (8 Mbit) DRAM cartridge for SNK fighters (King of Fighters, Real Bout, Metal Slug, etc.)
  - 4MB (32 Mbit) DRAM cartridge for Capcom fighters (X-Men vs. SF, Marvel Super Heroes vs. SF, Street Fighter Zero 3, etc.)
  - 6MB (48 Mbit) DRAM dev cartridge for rare prototypes (Heart of Darkness)
  - Cartridge RAM persistence in `.cart` files (per-game)
  - Cartridge state included in save states
  - Covers 24+ games from the official game database

- **ROM Cartridge Support**
  - Support for King of Fighters '95 and Ultraman ROM cartridges
  - ROM files placed alongside disc images for user convenience
  - Automatic hash validation against known ROM dumps
  - Flexible naming patterns: `{disc}.rom`, `{product_code}.rom`, `rom.rom`, `cartridge.rom`
  - Clear error messages when ROM is missing or invalid

- **Game Database Integration**
  - Automatic application of SH-2 cache emulation for games that require it (Astal, Dark Savior, Soviet Strike)
  - Automatic application of fast bus timings for stability (Marvel Super Heroes vs. SF, X-Men vs. SF)
  - Product code and disc hash matching for game-specific settings

### Changed
- **RTC (Real-Time Clock) storage is now system-wide instead of per-game**
  - The `.smpc` file is now stored as `system/brimir_saturn_rtc.smpc` (shared across all games)
  - Previously stored as `saves/Brimir/{Game Name}.smpc` (per-game)
  - This matches the behavior of the actual Sega Saturn console where the clock is a system setting
  - **Breaking change:** Users will need to reconfigure the clock once after updating

---

## [0.1.0] - 2025-11-25

### Initial Release

**Core Emulation**
- Full Ymir Sega Saturn emulator integration
- Support for multiple disc formats: CHD, BIN/CUE, ISO, CCD, MDS, BIN
- 7 BIOS versions supported with auto-detection (US, EU, JP variants)
- Backup RAM (SRAM) with persistent storage (`.bup` files)

**libretro API**
- Complete libretro v1 API implementation
- Video output: RGB565 format with dynamic resolution handling
- Audio output: 44.1 kHz stereo with ring buffer
- Controller support: Digital pad for Player 1 & 2
- Save state support with serialization/deserialization

**Performance Optimizations**
- Threaded VDP rendering for consistent 60 FPS
- Optimized XBGR8888 → RGB565 pixel conversion
- SRAM caching (refresh every 300 frames)
- Audio ring buffer for low-latency streaming

---

## Version History

### Version Numbering
- **Major**: Breaking changes, major feature additions
- **Minor**: New features, non-breaking changes
- **Patch**: Bug fixes, minor improvements

### Milestones

#### [0.1.1] - 2025-11-25 (Current)
- Expansion cartridge support (RAM + ROM)
- Game database integration
- System-wide RTC configuration

#### [0.1.0] - 2025-11-25 (Initial Release)
- First feature-complete release
- Full libretro API implementation
- Production-ready core

#### [0.2.0] - TBD (Planned)
- Extended controller support
- Multi-disc games
- Broader platform testing

#### [1.0.0] - TBD (Goal)
- Official libretro repository inclusion
- 85%+ game compatibility
- Full platform support
- Stable API

---

## Development Notes

### Sync with Upstream Ymir
Brimir tracks the Ymir emulator project:
- Check for Ymir updates monthly
- Evaluate impact on integration
- Update submodule and adapt as needed
- Document Ymir version in releases

### Breaking Changes Policy
- Breaking changes increment major version
- Deprecation warnings one version in advance
- Save state migrations provided
- Core option changes documented

### Performance Philosophy
- Target: Maintain 60 FPS on mid-range hardware
- Optimize hot paths (pixel conversion, audio, SRAM)
- Match or exceed standalone Ymir performance
- Profile before optimizing

---

## Template for Future Releases

```markdown
## [X.Y.Z] - YYYY-MM-DD

### Added
- New features

### Changed
- Changes to existing functionality

### Deprecated
- Features to be removed

### Removed
- Removed features

### Fixed
- Bug fixes

### Performance
- Performance improvements

### Security
- Security fixes

### Upstream
- Ymir version: commit hash or tag
- Notable upstream changes
```

---

[Unreleased]: https://github.com/coredds/brimir/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/coredds/brimir/compare/v0.1.3...v0.3.0
[0.1.3]: https://github.com/coredds/brimir/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/coredds/brimir/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/coredds/brimir/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/coredds/brimir/releases/tag/v0.1.0
