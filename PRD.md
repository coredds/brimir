# Product Requirements Document: Brimir
## Libretro Core for Sega Saturn Emulation

**Version:** 1.0  
**Date:** November 24, 2025  
**Project:** Brimir  
**Based On:** [Ymir Sega Saturn Emulator](https://github.com/StrikerX3/Ymir)  
**Target Platform:** [libretro](https://github.com/libretro)

---

## 1. Executive Summary

### 1.1 Project Overview
Brimir is a libretro core adaptation of the Ymir Sega Saturn emulator, designed to bring accurate Sega Saturn emulation to the libretro ecosystem. By leveraging Ymir's modern C++20 codebase and advanced features, Brimir will provide RetroArch and other libretro frontends with a high-quality, actively-maintained Saturn emulation option.

### 1.2 Project Goals
- Create a fully-featured libretro core from the Ymir emulator codebase
- Maintain compatibility with Ymir's core emulation accuracy and features
- Integrate seamlessly with the libretro API and existing frontends
- Provide cross-platform support (Windows, Linux, macOS, FreeBSD)
- Support multiple CPU architectures (x86-64, ARM)
- Maintain active development alongside upstream Ymir improvements

### 1.3 Target Audience
- RetroArch users seeking accurate Sega Saturn emulation
- Libretro frontend developers
- Retro gaming enthusiasts
- Emulation preservation community
- Content creators and streamers

---

## 2. Background and Context

### 2.1 Current State
**Ymir Features:**
- Load games from MAME CHD, BIN+CUE, IMG+CCD, MDF+MDS, ISO formats
- Automatic IPL (BIOS) ROM detection and region switching
- Multi-player support with customizable controllers
- Backup RAM, DRAM, and ROM cartridges
- Save states with forward compatibility
- Rewinding (up to 1 minute at 60 fps)
- Turbo speed and frame stepping
- Graphics enhancements (deinterlacing, progressive rendering)
- Optional low-level CD block emulation
- Feature-rich debugger
- GPL-3.0 licensed

**Libretro Ecosystem:**
- Standardized API for emulators and game engines
- Wide frontend support (RetroArch, BizHawk, etc.)
- Unified interface for save states, input, audio, and video
- Cross-platform deployment infrastructure
- Large existing user base

### 2.2 Market Analysis
**Existing Saturn Cores:**
- Beetle Saturn (Mednafen-based): Accurate but resource-intensive
- Yabause: Older, less actively maintained
- Kronos: Fork of Yabause with enhancements

**Brimir Differentiators:**
- Modern C++20 codebase for better performance and maintainability
- Active upstream development
- Advanced features (rewinding, debugger, graphics enhancements)
- Better accuracy through modern emulation techniques
- Forward-compatible save states

---

## 3. Technical Requirements

### 3.1 Core Requirements

#### 3.1.1 Libretro API Compliance
- **Must:** Implement libretro API v1 specification
- **Must:** Support core options interface for runtime configuration
- **Must:** Implement proper serialization/deserialization for save states
- **Must:** Provide accurate timing and frame delivery
- **Must:** Support libretro logging system
- **Must:** Implement proper resource cleanup and memory management

#### 3.1.2 Platform Support
- **Must:** Windows 10+ (x86-64, ARM64)
- **Must:** Linux (modern distributions, x86-64, ARM)
- **Must:** macOS 13+ (x86-64, Apple Silicon)
- **Should:** FreeBSD support
- **Must:** Support both 32-bit and 64-bit architectures where applicable

#### 3.1.3 Build System
- **Must:** Use CMake as primary build system
- **Must:** Integrate with libretro's build infrastructure
- **Must:** Support vcpkg for dependency management (inherited from Ymir)
- **Must:** Provide Makefile compatibility for libretro build bot
- **Should:** Generate pkg-config files
- **Should:** Support static and dynamic linking options

### 3.2 Emulation Features

#### 3.2.1 Core Emulation (Must Have)
- [ ] Full Sega Saturn hardware emulation from Ymir
- [ ] SH-2 dual CPU emulation
- [ ] VDP1 (sprite/polygon processor) emulation
- [ ] VDP2 (background/scroll processor) emulation
- [ ] SCU (System Control Unit) emulation
- [ ] SMPC (System Manager & Peripheral Control) emulation
- [ ] Sound emulation (SCSP - Saturn Custom Sound Processor)
- [ ] CD-ROM drive emulation (both HLE and LLE modes)

#### 3.2.2 Media Format Support (Must Have)
- [ ] MAME CHD files
- [ ] BIN+CUE
- [ ] ISO (ISO9660)
- [ ] IMG+CCD
- [ ] MDF+MDS
- [ ] M3U playlist support for multi-disc games

#### 3.2.3 BIOS/IPL Support (Must Have)
- [ ] Automatic IPL ROM detection
- [ ] Support for all regional BIOS variants (Japan, USA, Europe)
- [ ] Automatic region switching based on disc
- [ ] CD block ROM support for LLE mode
- [ ] BIOS selection via core options

### 3.3 Libretro-Specific Features

#### 3.3.1 Input (Must Have)
- [ ] Map Saturn controller to libretro input abstraction
- [ ] Support for multiple controller types:
  - Standard pad (digital)
  - Analog pad (3D Control Pad)
  - Mission Stick
  - Racing controller (wheel)
  - Twin Stick
  - Mouse
  - Keyboard
- [ ] Two player support on both ports
- [ ] Input descriptors for proper button labeling
- [ ] Configurable controller ports via core options

#### 3.3.2 Audio (Must Have)
- [ ] Deliver audio through libretro audio callback
- [ ] Support variable sample rates
- [ ] Proper audio/video synchronization
- [ ] Support for audio buffer status callback

#### 3.3.3 Video (Must Have)
- [ ] Output video through libretro framebuffer
- [ ] Support multiple pixel formats (RGB565, XRGB8888)
- [ ] Proper aspect ratio reporting
- [ ] Geometry changes for resolution switching
- [ ] Support for libretro hardware rendering (OpenGL, Vulkan, D3D)

#### 3.3.4 Save States (Must Have)
- [ ] Implement serialize/unserialize callbacks
- [ ] Forward-compatible save state format (from Ymir)
- [ ] Provide accurate state size reporting
- [ ] Support state thumbnails via libretro API

#### 3.3.5 Memory Access (Must Have)
- [ ] Expose system RAM for cheats
- [ ] Provide backup RAM access
- [ ] Support cartridge RAM regions
- [ ] Implement memory descriptors API

### 3.4 Advanced Features

#### 3.4.1 Core Options (Must Have)
Configuration exposed through libretro core options:

**Graphics:**
- [ ] Deinterlacing mode (auto/off)
- [ ] Progressive rendering for high-res modes
- [ ] Mesh polygon rendering mode (transparent/opaque)
- [ ] Internal resolution scaling (1x-4x)
- [ ] Frameskip options

**System:**
- [ ] Region selection (auto/Japan/USA/Europe)
- [ ] CD block emulation mode (HLE/LLE)
- [ ] IPL ROM selection
- [ ] Cartridge type selection (none/backup RAM/DRAM/ROM)

**Performance:**
- [ ] Dynarec/interpreter CPU mode
- [ ] Fast-forward speed multiplier
- [ ] Frame duplication for slow systems

**Audio:**
- [ ] Audio interpolation quality
- [ ] Enable/disable sound channels for debugging

#### 3.4.2 Enhancements (Should Have)
- [ ] Rewind support (inherited from Ymir)
- [ ] Fast-forward/turbo mode
- [ ] Frame advance (single step)
- [ ] Per-game core option overrides
- [ ] CD audio track metadata export

#### 3.4.3 Debug Features (Could Have)
- [ ] Memory viewer integration
- [ ] CPU state inspection
- [ ] Log level control via core options
- [ ] Performance counters

### 3.5 Performance Requirements

#### 3.5.1 Target Performance
- **Must:** Achieve full speed (60 FPS) on:
  - Intel Core i5-8th gen or equivalent (x86-64)
  - Apple M1 or equivalent (ARM)
  - AMD Ryzen 5 3000 series or equivalent
- **Should:** Optimize for lower-end hardware where possible
- **Must:** Support frame-time-based timing for VRR displays
- **Must:** Minimize input lag (target: < 3 frames)

#### 3.5.2 Resource Usage
- **Should:** RAM usage < 512MB for typical games
- **Should:** Startup time < 2 seconds
- **Must:** No memory leaks during extended play sessions
- **Should:** Efficient CPU usage (< 80% on target hardware)

---

## 4. Architecture and Design

### 4.1 Code Structure

```
brimir/
├── src/
│   ├── libretro/           # Libretro API implementation
│   │   ├── libretro.cpp    # Main libretro interface
│   │   ├── input.cpp       # Input mapping
│   │   ├── audio.cpp       # Audio output
│   │   ├── video.cpp       # Video output
│   │   ├── options.cpp     # Core options
│   │   └── state.cpp       # Save state handling
│   ├── ymir/               # Ymir emulator core (submodule or adapted)
│   │   └── ...             # Existing Ymir source tree
│   └── bridge/             # Bridge between libretro and Ymir
│       ├── core_wrapper.cpp
│       └── ...
├── include/
│   └── brimir/
├── resources/
│   └── info/
│       └── brimir_libretro.info  # Core info file
├── CMakeLists.txt
├── Makefile.libretro       # Libretro build system compatibility
├── LICENSE                 # GPL-3.0 (inherited from Ymir)
└── README.md
```

### 4.2 Integration Strategy

#### 4.2.1 Ymir Integration Approach
**Option A: Git Submodule (Recommended)**
- Maintain Ymir as a git submodule
- Create adapter layer in `src/bridge/`
- Easier to pull upstream changes
- Clear separation of concerns

**Option B: Fork and Modify**
- Fork Ymir codebase directly
- Integrate libretro code within Ymir structure
- More invasive but potentially cleaner integration
- Harder to maintain upstream compatibility

#### 4.2.2 Build Integration
- Use CMake to orchestrate both Ymir and libretro builds
- Provide Makefile wrapper for libretro build bots
- Handle dependency management through vcpkg
- Support standalone and bundled dependency builds

### 4.3 Key Design Decisions

#### 4.3.1 Threading Model
- **Decision:** Maintain Ymir's threading model internally
- **Rationale:** Libretro cores run on frontend's thread
- **Implementation:** Coordinate with libretro's single-threaded execution model
- **Risk Mitigation:** Careful synchronization at API boundaries

#### 4.3.2 File System Access
- **Decision:** Use libretro VFS API for all file operations
- **Rationale:** Enables frontend to intercept and redirect file access
- **Impact:** Need to adapt Ymir's file loading code
- **Alternative:** Direct file access with path translation

#### 4.3.3 Configuration Management
- **Decision:** Convert Ymir.toml settings to libretro core options
- **Rationale:** Standard libretro configuration interface
- **Implementation:** Map Ymir config to option callbacks
- **Note:** Internal profile directory for non-exposed settings

#### 4.3.4 Graphics Backend
- **Decision:** Support both software and hardware rendering
- **Primary:** Software rendering to RGB framebuffer
- **Secondary:** OpenGL/Vulkan acceleration for enhanced features
- **Rationale:** Maximum compatibility with flexible enhancement path

---

## 5. Implementation Phases

### Phase 1: Foundation (Weeks 1-4)
**Goal:** Basic core that loads and runs games

**Deliverables:**
- [ ] Project structure and build system setup
- [ ] Integrate Ymir codebase (submodule or fork)
- [ ] Implement basic libretro callbacks (retro_init, retro_load_game, etc.)
- [ ] Video output (software rendering to framebuffer)
- [ ] Audio output (basic PCM streaming)
- [ ] Input handling (single controller, basic mapping)
- [ ] Load game from file path
- [ ] Achieve playable state for simple games

**Success Criteria:**
- Core loads in RetroArch
- Can start and play a basic Saturn game (e.g., Panzer Dragoon)
- Audio and video output functional
- Basic controller input works

### Phase 2: Core Features (Weeks 5-8)
**Goal:** Complete essential emulation features

**Deliverables:**
- [ ] All media formats support (CHD, BIN/CUE, ISO, etc.)
- [ ] BIOS detection and loading
- [ ] Complete input support (all controller types, 2 players)
- [ ] Save state support (serialize/unserialize)
- [ ] Backup RAM/cartridge support
- [ ] Core options implementation (basic set)
- [ ] Memory descriptors for cheats
- [ ] Multi-disc game support (M3U)

**Success Criteria:**
- 80%+ game compatibility (compared to standalone Ymir)
- Save states work reliably
- All input devices functional
- Backup RAM persists correctly

### Phase 3: Polish and Optimization (Weeks 9-12)
**Goal:** Production-ready core

**Deliverables:**
- [ ] Performance optimization
- [ ] Complete core options set
- [ ] Rewind support
- [ ] Hardware-accelerated rendering (optional)
- [ ] Enhanced graphics options
- [ ] Comprehensive testing
- [ ] Documentation (README, compilation guide)
- [ ] Core info file with accurate metadata

**Success Criteria:**
- Full speed on target hardware specifications
- All advertised features functional
- No critical bugs in top 50 games
- Documentation complete

### Phase 4: Advanced Features (Weeks 13-16)
**Goal:** Differentiation and advanced functionality

**Deliverables:**
- [ ] Debug features integration
- [ ] Advanced graphics enhancements
- [ ] CD-ROM LLE support
- [ ] Per-game configuration
- [ ] Achievement support (if applicable)
- [ ] Network support preparation
- [ ] Performance profiling and optimization

**Success Criteria:**
- Feature parity with standalone Ymir
- Positive community feedback
- Inclusion in libretro core repository

---

## 6. Testing Requirements

### 6.1 Test Categories

#### 6.1.1 Functional Testing
**Game Compatibility:**
- [ ] Test suite of 100+ popular games
- [ ] Cover all game genres (2D, 3D, RPG, action, racing)
- [ ] Multi-disc games (Panzer Dragoon Saga, FF VII International)
- [ ] Region-specific games (JP, US, EU)
- [ ] Games using special cartridges

**Format Testing:**
- [ ] All supported disc formats (CHD, BIN/CUE, ISO, CCD, MDS)
- [ ] Multi-track audio discs
- [ ] Mixed mode CD-ROMs

#### 6.1.2 Platform Testing
- [ ] Windows 10/11 (x86-64, ARM64)
- [ ] Ubuntu 22.04+ (x86-64, ARM)
- [ ] macOS 13+ (Intel, Apple Silicon)
- [ ] FreeBSD (x86-64)
- [ ] RetroArch (primary frontend)
- [ ] Alternative libretro frontends (time permitting)

#### 6.1.3 Feature Testing
- [ ] Save state creation/loading at various game points
- [ ] Backup RAM persistence across sessions
- [ ] Rewind functionality
- [ ] Fast-forward stability
- [ ] Input device switching
- [ ] Core option changes during runtime
- [ ] Multi-session endurance testing

#### 6.1.4 Performance Testing
- [ ] Frame rate consistency (target 60 FPS)
- [ ] Frame time variance
- [ ] Memory usage over time
- [ ] CPU usage patterns
- [ ] Audio/video sync accuracy
- [ ] Input lag measurement

#### 6.1.5 Regression Testing
- [ ] Automated test ROM suite
- [ ] Game-specific test cases
- [ ] Save state forward compatibility
- [ ] Configuration migration

### 6.2 Test Infrastructure
- [ ] Automated build pipeline (CI/CD)
- [ ] Test ROM collection
- [ ] Automated smoke tests
- [ ] Performance benchmarking framework
- [ ] Community beta testing program

---

## 7. Documentation Requirements

### 7.1 User Documentation
- [ ] README.md with quick start guide
- [ ] COMPILING.md with build instructions
- [ ] Core options reference
- [ ] Supported file formats and BIOS requirements
- [ ] Troubleshooting guide
- [ ] FAQ

### 7.2 Developer Documentation
- [ ] Architecture overview
- [ ] Code structure guide
- [ ] Contribution guidelines
- [ ] API integration documentation
- [ ] Build system documentation
- [ ] Testing procedures

### 7.3 Core Info File
- [ ] Accurate `brimir_libretro.info` file
- [ ] Supported extensions
- [ ] BIOS requirements
- [ ] Feature flags (save states, rewind, etc.)
- [ ] Manufacturer and system names
- [ ] Database mapping

---

## 8. Licensing and Legal

### 8.1 License
- **Core License:** GPL-3.0 (inherited from Ymir)
- **Dependencies:** Ensure GPL-compatible licenses
- **Contribution:** CLA or DCO for contributors

### 8.2 Legal Considerations
- [ ] Clear BIOS requirements documentation
- [ ] No bundled copyrighted material
- [ ] Proper attribution to Ymir project
- [ ] Respect upstream license terms
- [ ] Contributor licensing agreements

### 8.3 Attribution
- Acknowledge Ymir project and @StrikerX3
- List all major contributors
- Credit libretro team and community

---

## 9. Release and Distribution

### 9.1 Release Strategy

#### 9.1.1 Alpha Release (Internal)
- Limited distribution to core developers
- Focus on basic functionality testing
- Gather initial feedback

#### 9.1.2 Beta Release (Public)
- Announce on libretro forums, Discord, Reddit
- Open for community testing
- Issue tracker for bug reports
- Weekly/bi-weekly updates

#### 9.1.3 Version 1.0 Release
- Full feature completion
- Stable across all platforms
- Comprehensive documentation
- Official libretro repository inclusion

### 9.2 Distribution Channels
- [ ] libretro buildbot integration
- [ ] RetroArch core downloader
- [ ] GitHub releases page
- [ ] Source code availability

### 9.3 Version Numbering
- Follow semantic versioning (MAJOR.MINOR.PATCH)
- Track upstream Ymir versions
- Clear changelog for each release

---

## 10. Success Metrics

### 10.1 Technical Metrics
- **Game Compatibility:** ≥85% of tested games playable
- **Performance:** Full speed on target hardware
- **Stability:** <1 crash per 10 hours gameplay
- **Build Success:** 100% on all supported platforms
- **Input Lag:** <3 frames end-to-end

### 10.2 Adoption Metrics
- Downloads via RetroArch core manager
- GitHub stars and forks
- Community engagement (issues, PRs, discussions)
- Positive reception in emulation community

### 10.3 Quality Metrics
- Code coverage ≥70% (where applicable)
- No critical bugs in issue tracker
- Documentation completeness
- Upstream Ymir integration lag <4 weeks

---

## 11. Risks and Mitigation

### 11.1 Technical Risks

| Risk | Severity | Probability | Mitigation |
|------|----------|-------------|------------|
| Ymir architecture incompatible with libretro | High | Low | Early prototype phase to validate integration |
| Performance regression vs. standalone Ymir | High | Medium | Continuous benchmarking, profiling |
| Save state format changes breaking compatibility | Medium | Medium | Versioned format, migration code |
| Platform-specific bugs | Medium | High | Comprehensive platform testing |
| Dependency management complexity | Medium | Medium | Use vcpkg, document thoroughly |

### 11.2 Project Risks

| Risk | Severity | Probability | Mitigation |
|------|----------|-------------|------------|
| Upstream Ymir divergence | High | Medium | Regular sync schedule, maintain contact with Ymir developer |
| Limited developer resources | High | Medium | Phased approach, community contributions |
| Lack of community adoption | Medium | Low | Early engagement, quality focus |
| GPL licensing concerns | Low | Low | Clear documentation, legal review |

### 11.3 Community Risks

| Risk | Severity | Probability | Mitigation |
|------|----------|-------------|------------|
| Competition with existing cores | Medium | High | Focus on accuracy and modern codebase advantages |
| Negative initial reception | Medium | Medium | Manage expectations, clear beta labeling |
| Insufficient testing resources | Medium | Medium | Public beta program, test automation |

---

## 12. Dependencies and Requirements

### 12.1 Build Dependencies
From Ymir project:
- CMake 3.20+
- C++20-capable compiler (GCC 11+, Clang 14+, MSVC 2022+)
- vcpkg (for dependency management)
- SDL3 (may need to stub out for libretro)

Additional for libretro:
- libretro-common headers
- Git (for submodules)

### 12.2 Runtime Dependencies
- Sega Saturn IPL/BIOS ROMs (user-provided)
- CD block ROM for LLE mode (user-provided)
- Game disc images

### 12.3 External Resources
- Ymir emulator source code
- libretro API documentation
- Sega Saturn technical documentation
- Test ROM collection

---

## 13. Team and Roles

### 13.1 Core Team
- **Project Lead:** Overall direction, coordination
- **Core Developer(s):** Libretro integration, Ymir adaptation
- **Platform Maintainers:** Windows, Linux, macOS, FreeBSD specialists
- **QA/Testing Lead:** Test coordination, compatibility tracking
- **Documentation Lead:** User and developer docs

### 13.2 Community Contributors
- Bug reporters
- Feature requesters
- Code contributors
- Testers
- Documentation writers

### 13.3 Upstream Coordination
- Maintain communication with Ymir project (@StrikerX3)
- Coordinate with libretro team for repository inclusion
- Engage with emulation community for feedback

---

## 14. Timeline

### High-Level Timeline
```
Month 1-2:  Phase 1 - Foundation
Month 2-3:  Phase 2 - Core Features
Month 3-4:  Phase 3 - Polish and Optimization
Month 4-5:  Phase 4 - Advanced Features
Month 5-6:  Beta Testing and Refinement
Month 6:    Version 1.0 Release
```

### Key Milestones
- **Week 4:** First playable build (internal alpha)
- **Week 8:** Feature-complete alpha
- **Week 12:** Public beta release
- **Week 16:** Release candidate
- **Week 20:** Version 1.0 official release
- **Week 24:** Post-release support and libretro inclusion

---

## 15. Post-Launch Plans

### 15.1 Maintenance
- Bug fix releases as needed
- Regular synchronization with upstream Ymir
- Security updates
- Platform compatibility updates

### 15.2 Future Enhancements
- Network multiplayer support
- RetroAchievements integration
- AI-assisted upscaling
- Texture replacement system
- Additional enhancement options
- Debugger integration with libretro frontends

### 15.3 Community Growth
- Maintain active presence in forums/Discord
- Regular development updates
- Respond to issues and PRs
- Foster contributor community
- Create tutorial content

---

## 16. References

### 16.1 External Resources
- **Ymir Project:** https://github.com/StrikerX3/Ymir
- **Libretro Organization:** https://github.com/libretro
- **Libretro API Docs:** https://docs.libretro.com/
- **Skeletor Template:** https://github.com/libretro/skeletor
- **RetroArch:** https://www.retroarch.com/

### 16.2 Technical Documentation
- Ymir COMPILING.md
- Libretro API specification
- Sega Saturn hardware documentation
- C++20 standard reference

---

## 17. Appendices

### Appendix A: Glossary
- **CHD:** Compressed Hunks of Data - MAME's disc image format
- **HLE:** High-Level Emulation
- **LLE:** Low-Level Emulation
- **IPL:** Initial Program Loader (BIOS)
- **SCSP:** Saturn Custom Sound Processor
- **VDP1/VDP2:** Video Display Processors
- **SH-2:** SuperH RISC CPU architecture
- **SCU:** System Control Unit
- **SMPC:** System Manager & Peripheral Control

### Appendix B: Core Option Examples
```ini
brimir_region = "auto|jp|us|eu"
brimir_cdb_mode = "hle|lle"
brimir_cart_type = "auto|none|backup_ram|dram|rom_1mb|rom_4mb"
brimir_deinterlace = "auto|off"
brimir_mesh_transparency = "on|off"
brimir_internal_resolution = "1x|2x|4x"
brimir_cpu_mode = "dynarec|interpreter"
```

### Appendix C: Supported File Extensions
```
.chd .cue .bin .iso .ccd .img .mds .mdf .m3u
```

### Appendix D: Recommended Test Games
**2D Games:**
- Radiant Silvergun
- Guardian Heroes
- Saturn Bomberman

**3D Games:**
- Panzer Dragoon series
- Nights into Dreams
- Virtua Fighter 2

**Special Cases:**
- Panzer Dragoon Saga (multi-disc)
- Street Fighter Zero 3 (4MB RAM cart)
- King of Fighters 95 (1MB RAM cart)

---

## Document History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2025-11-24 | Initial Draft | Complete PRD created |

---

## Approval

This PRD requires review and approval from:
- [ ] Project Lead
- [ ] Technical Lead
- [ ] Community Representative
- [ ] Ymir Project (for coordination)

---

**End of Document**

