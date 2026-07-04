# Changelog

All notable changes to the Brimir project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.4.5] - 2026-07-04

### Fixed
- **Ymir Hardware Layer Fixes (through 2026-06-26)** — Backported targeted upstream bug fixes on top of the 2026-06-23 sync
  - **SMPC** — Update peripheral PDR1/PDR2 registers when reading and when updating EXLE; fixes many cases of games not recognizing Virtua Gun inputs or missing shots
  - **Input** — 3D Control Pad digital-mode report now matches the Standard Pad (fixes Bug Too!, Black/Matrix); analog-mode report LSBs set to all ones (fixes double inputs in Deep Fear)
  - **VDP1** — Textured sprites with CMDSIZE.H=0 now fetch texels correctly (fixes glitched graphics in Policenauts' shooting range scorecard); FBRAM no longer synced on debug reads (fixes a deadlock when viewing the framebuffer in a memory viewer)
  - **VDP2** — Color calculations now correctly restricted in Hi-Res/exclusive-monitor modes with color RAM modes other than 0, preventing blending on top of palette layers (fixes Dark Savior Sound Test text); VCNT/external-latch coordinate handling overhauled — latches current VCNT on EXTEN reads, fixes the Hi-Res HCNTShift case, and corrects ExternalLatch offsets (fixes several Virtua Gun shot-offset and reload-detection errors, plus Digital Dance Mix Vol. 1 camera angles)
  - **CD Block (HLE)** — Squashed fix chain for Play/Pause/Seek/TOC/Init command handling: extended seek ticks to avoid a race between quick successive Play commands (Digital Dance Mix Vol. 1); reworked resume-from-pause and reset-position logic (fixes music looping/restart issues in Mass Destruction); Get TOC no longer clobbers playback status (fixes music stopping in Mass Destruction); read speed setting is now ignored in favor of the configured speed factor for data tracks (fixes slow FMVs); Mode 2 Form 2 sector buffers are extended when under-sized (fixes FMV/animation corruption); playback now properly resumes after a buffer-full pause (fixes freezes in Mahjong Yon Shimai, Shellshock, Sonic Jam, IRREEL); Seek Disc FAD/track/index edge cases corrected (disc-bounds/leadout clamping, track 0 = first track, out-of-range index handling)

### Technical
- 7 commits, cherry-picked from specific Ymir upstream fixes (not a full verbatim sync) rather than a wholesale hardware-layer sync
- Verified with matching test suites on MSVC (Windows), GCC 13 (Linux x64), and a real aarch64 cross-compile (confirmed NEON codegen unaffected) — all 218,082 test assertions pass on every toolchain
- **Save state compatibility note**: removes the now-unused `VCNTLatched` field from `ymir::savestate::SaveState`; existing save states will be rejected (size mismatch) after this update, same as prior hardware-layer syncs that touched save state layout

## [0.4.1] - 2026-06-04

### Changed
- **Ymir Hardware Layer Sync (2026-06-04)** — Synced hardware layer to latest Ymir upstream
  - **SH2 CPU** — Full rewrite with WB/EX stall emulation, 32-bit instruction fetch, corrected cycle counts for ~10 instructions, renamed `enableCache` → `emulateCache`, decoupled from `Scheduler`/`SystemFeatures`
  - **VDP1** — COPR register fix: separated `currCommandAddress` from `nextCommandAddress` for correct COPR reads mid-command
  - **Saturn system** — Updated SH2 construction to use `BindGlobalCycleCounter()`/`BindEmulateCacheOption()` callbacks; replaced `SystemFeatures` struct with direct bool members
  - **Save state** — Added `fetchedOpcodes`, `forceFetchOpcodes`, `wbReg`, `nextCommandAddress` fields (**not backward-compatible with old saves**)
  - **Infrastructure** — Synced `scheduler.hpp`, `configuration.hpp`, `bus.hpp`, `bit_ops.hpp`, `debug_break.hpp`, `watchpoint_defs.hpp`

### Added
- **Backup RAM utilities** — `backup_ram_utils.hpp/cpp` and `bup_char_table.inc` for Japanese character translation in backup memory
- **CMake options** — Added `Brimir_AVX2`, `Brimir_EXTRA_INLINING`, `Brimir_DISABLE_FORCE_INLINE`, `Brimir_ENABLE_IPO` options
- **Extended AVX2 flags** — Added `-mpopcnt`, `-mlzcnt`, `-mbmi2` for GCC/Clang builds

### Fixed
- **JIT wrapper** — Updated to new SH2 constructor signature (2-param) and `ymir::` namespace
- **`vdp.hpp`** — Synced to provide `GetVerticalPhase()`/`GetHorizontalPhase()` used by updated Saturn frame loop

### Technical
- 29 hardware-layer files synced verbatim from Ymir upstream
- ARM NEON intrinsics in `vdp_renderer_sw.cpp` preserved with `vreinterpretq` casts
- `system_features.hpp` is now an orphan file (no remaining consumers)

## [0.4.4] - 2026-06-23

### Changed
- **Ymir Hardware Layer Sync (2026-06-23)** — Synced hardware layer to latest Ymir upstream
  - **VDP2** — Color gradation effect implemented (fixes Saturn Bomberman Stage 4-4 background); horizontal scroll increment now updated every scanline (fixes Shellshock terrain)
  - **VDP1** — Infinite loop detection optimized with `inInfiniteLoop` flag; jumps to address 0 no longer treated as infinite loops (fixes Akumajou Dracula X); save state FBRAM synchronization fix (fixes rewind flicker / blank sprites); color bank masking moved from per-pixel to per-quad; VDP1Regs reference and double density flag cached per command instead of per pixel
  - **SCSP/M68K** — Threading synchronization redesigned: sample-counter-based sync replaced with unified event queue (Write/Sample/Quit) using `wait_dequeue_bulk` batch processing; fixes rare deadlock, ~10-15% performance improvement
  - **SH2** — Decode macros cleaned up with `#undef` after use (unity build safety)

### Technical
- 9 hardware-layer commits synced from Ymir upstream
- All targets compile cleanly

## [0.4.3] - 2026-06-17

### Changed
- **Ymir Hardware Layer Sync (2026-06-17)** — Synced hardware layer to latest Ymir upstream
  - **VDP1** — 7 accuracy fixes: full 16-bit clipping coordinates (fixes Revolution X), FBRAM read/write synchronization with event-based flush (fixes BlackFire, Burning Rangers, Scorcher), infinite loop detection no longer halts frame drawing (fixes Stellar Assault), Gouraud shading incremented in transparent pixels (fixes Cotton 2), line drawing optimization corrections
  - **CD Block HLE** — Fix "Play Disc" resume-from-pause parameters (fixes Panzer Dragoon II Zwei softlocks)
  - **Backup RAM** — Fix invalid block index infinite loop (`continue` → `break`)
  - **System** — SH-2 overclocking support with GCD clock ratio correction; configurable via `sh2OverclockFactor`
  - **SCSP/M68K** — Threaded SCSP fully implemented (was a TODO stub); SCSP and M68K now run on dedicated thread; write queue with fencing for register/RAM synchronization; thread-safe CDDA and MIDI input

### Technical
- 13 hardware-layer files synced from Ymir upstream
- ARM NEON intrinsics in `vdp_renderer_sw.cpp` preserved
- Build verified: all targets compile cleanly

## [0.4.2] - 2026-06-10

### Changed
- **SH2 decode optimization** — Removed `DecodedArgs` pre-decode table (~256KB saved); instruction handlers now decode opcode arguments directly via `DECODE_NM`/`DECODE_D_S` macros, matching Ymir upstream
- **DIV1 microoptimization** — Applied upstream performance tweaks to DIV1 instruction

### Added
- **Cartridge RAM persistence** — Save/load `.cart` files for all DRAM cartridge types (8 Mbit, 32 Mbit, 48 Mbit) using Ymir's `DumpRAM`/`LoadRAM` API

### Fixed
- **Incomplete sync** — `sh2_decode.{hpp,cpp}` now match Ymir upstream; previous 0.4.1 sync retained stale `DecodedArgs` pre-decoding that was never consumed by the updated handlers
- **Cartridge RAM load order** — Load saved cartridge RAM after hard reset to prevent immediate zeroing

### Technical
- 4 hardware-layer files synced: `sh2.{hpp,cpp}`, `sh2_decode.{hpp,cpp}`
- ARM NEON intrinsics preserved; SH1 decode unchanged
- CI: Windows build switched from VS generator to Ninja
- CI: Node.js 24 opt-in for deprecated GitHub Actions

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

The roadmap has been revised based on a gap analysis against mature RetroArch cores. The guiding constraint is that Ymir's hardware layer stays verbatim upstream — all features are implemented in Brimir's bridge/libretro layers or proposed to Ymir upstream. See [ROADMAP.md](ROADMAP.md) for the full plan.

### Completed (v0.5.0-dev)

#### Audio Volume
- New core option `brimir_audio_volume` (0%–200%, default 100%)
- Fixed-point 16.16 scaling applied per-sample in `OnAudioSample()` with zero overhead at 100%
- Validated: no overflow at 200% volume with full-scale samples

#### Screen Rotation (TATE)
- New core option `brimir_rotation` (None, 90°, 180°, 270°)
- Post-process rotation applied after pixel conversion in `OnFrameComplete()`
- Supports chaining with overscan crop (crop first, then rotate)
- Buffer swap between `m_framebuffer` and `m_displayFramebuffer` avoids third allocation

#### Overscan Crop
- New core option `brimir_overscan` (None, Small ~16px, Medium ~32px, Large ~48px)
- Crops from all four edges before rotation in `OnFrameComplete()`
- Guard prevents crop when remaining area would be < 32px

### Completed — v0.4.1 (2026-06-04)
- System RAM exposure via `RETRO_MEMORY_SYSTEM_RAM` (unblocks RetroAchievements)
- Memory descriptors (SRAM, WRAM Low, WRAM High visible in RetroArch)
- Save state compression (LZ4, enabling rewind + runahead viability)
- Contentless / BIOS menu mode (`supports_no_game = "true"`)
- M3U-less disc swapping (full disk control ext interface, 10 callbacks)
- CD read speed options expanded to 24x, 32x, Max (200x)

### Upcoming — v0.5.0
- Full cheat system (Action Replay / GameShark / RAM search)
- All controller types (3D Control Pad, Arcade Racer, Mission Stick, Virtua Gun, Shuttle Mouse)
- RetroAchievements integration
- VDP layer toggling and debug overlay exposure
- Cartridge RAM persistence
- Frameskip support (Hybrid — needs Ymir config)
- Region patching (PAL→NTSC force)

### Upcoming — v0.5.1
- Internal cheat database (shipped with core)

### Upcoming — v0.6.0 (Hybrid)
- CPU overclocking (SH-2 clock multiplier)

### Upcoming — v0.7.0+ (Upstream / Major Engineering)
- SH-2 JIT compiler (see `src/jit/STATUS.md`)
- Hardware renderer with internal resolution scaling (Vulkan/OGL/D3D)
- Widescreen hacks
- HD texture replacement
- ST-V arcade support

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

#### [0.3.0] - 2026-02-20 (Current)
- Ymir hardware layer synced verbatim upstream (v0.3.2-dev)
- Software renderer with SIMD-optimized pixel conversion
- Full VDP1/VDP2 with threaded rendering
- 6 BIOS variants + auto-detection

#### [0.1.1] - 2025-11-25
- Expansion cartridge support (RAM + ROM)
- Game database integration
- System-wide RTC configuration

#### [0.1.0] - 2025-11-25 (Initial Release)
- First feature-complete release
- Full libretro API implementation
- Production-ready core

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

[0.4.1]: https://github.com/coredds/brimir/compare/v0.3.0...v0.4.1
[0.3.0]: https://github.com/coredds/brimir/compare/v0.1.3...v0.3.0
[0.1.3]: https://github.com/coredds/brimir/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/coredds/brimir/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/coredds/brimir/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/coredds/brimir/releases/tag/v0.1.0
