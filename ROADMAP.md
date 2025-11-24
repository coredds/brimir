# Brimir Development Roadmap

This document outlines the development roadmap for the Brimir libretro core. It expands on the phases defined in the PRD with more granular tasks and milestones.

**Last Updated:** 2025-11-24  
**Status:** Planning Phase

---

## Overview

The development is structured in 4 major phases spanning approximately 16-20 weeks to reach v1.0:

```
Phase 1: Foundation          (Weeks 1-4)   → v0.1.0 Alpha
Phase 2: Core Features       (Weeks 5-8)   → v0.2.0 Beta
Phase 3: Polish              (Weeks 9-12)  → v0.3.0 RC
Phase 4: Advanced Features   (Weeks 13-16) → v1.0.0 Stable
```

---

## Phase 1: Foundation (Weeks 1-4) → v0.1.0

**Goal:** Create a minimal viable core that can load and run games

### Week 1: Project Setup
- [x] ~~Create repository and initial structure~~ (DONE)
- [x] ~~Write PRD and documentation~~ (DONE)
- [ ] Set up Git submodules (Ymir, vcpkg)
- [ ] Configure CMake build system
- [ ] Create Makefile.libretro for buildbot
- [ ] Set up CI/CD pipelines
  - [ ] GitHub Actions for builds
  - [ ] Windows (x64, ARM64)
  - [ ] Linux (x64, ARM)
  - [ ] macOS (Intel, Apple Silicon)
- [ ] Configure development environment documentation
- [ ] Create issue templates
- [ ] Set up project board

**Milestone:** Build system working, CI operational

### Week 2: Libretro API Skeleton
- [ ] Implement basic libretro callbacks:
  - [ ] `retro_set_environment`
  - [ ] `retro_init` / `retro_deinit`
  - [ ] `retro_api_version`
  - [ ] `retro_get_system_info`
  - [ ] `retro_get_system_av_info`
  - [ ] `retro_set_controller_port_device`
- [ ] Set up callback storage (video_cb, audio_cb, etc.)
- [ ] Implement logging infrastructure
- [ ] Create CoreWrapper class skeleton
- [ ] Test core loads in RetroArch

**Milestone:** Core loads in RetroArch without crashing

### Week 3: Basic Game Loading & Execution
- [ ] Integrate Ymir emulator core
- [ ] Implement game loading:
  - [ ] `retro_load_game`
  - [ ] `retro_unload_game`
  - [ ] BIOS detection and loading
  - [ ] ISO/BIN+CUE format support
- [ ] Basic execution loop:
  - [ ] `retro_run` implementation
  - [ ] Frame timing
  - [ ] Main emulation loop integration
- [ ] Implement `retro_reset`
- [ ] Test with simple commercial game

**Milestone:** Can load and start a game

### Week 4: Audio/Video/Input Output
- [ ] Video output:
  - [ ] Framebuffer allocation
  - [ ] Pixel format setup (RGB565, XRGB8888)
  - [ ] Video callback delivery
  - [ ] Geometry reporting
- [ ] Audio output:
  - [ ] Audio buffer setup
  - [ ] Sample delivery via audio_batch_cb
  - [ ] Basic synchronization
- [ ] Input handling:
  - [ ] Input polling setup
  - [ ] Single controller (digital pad) support
  - [ ] Button mapping
- [ ] Integration testing with real game

**Milestone:** ✅ v0.1.0 Alpha - First Playable Build
- Core loads and runs simple games
- Audio and video output functional
- Basic controller input works
- Can complete a level in test game

---

## Phase 2: Core Features (Weeks 5-8) → v0.2.0

**Goal:** Complete essential emulation features

### Week 5: Extended Format Support
- [ ] CHD format support
- [ ] IMG+CCD format support
- [ ] MDF+MDS format support
- [ ] M3U playlist support for multi-disc games
- [ ] Disc change mechanism:
  - [ ] `retro_set_eject_state`
  - [ ] `retro_get_eject_state`
  - [ ] `retro_get_image_index`
  - [ ] `retro_set_image_index`
  - [ ] `retro_get_num_images`
- [ ] Test with various format types
- [ ] Test multi-disc games (e.g., Panzer Dragoon Saga)

**Milestone:** All disc formats supported

### Week 6: Complete Input System
- [ ] Multiple controller type support:
  - [ ] Analog pad (3D Control Pad)
  - [ ] Mission Stick
  - [ ] Racing controller
  - [ ] Twin Stick
  - [ ] Mouse
  - [ ] Keyboard
- [ ] Two-player support
- [ ] Input descriptors for proper labeling
- [ ] Configurable controller ports
- [ ] Hotplug support
- [ ] Test with games requiring specific controllers

**Milestone:** All input devices functional

### Week 7: Save States & Memory
- [ ] Save state implementation:
  - [ ] `retro_serialize_size`
  - [ ] `retro_serialize`
  - [ ] `retro_unserialize`
  - [ ] State versioning
  - [ ] Forward compatibility support
- [ ] Memory management:
  - [ ] `retro_get_memory_data` / `retro_get_memory_size`
  - [ ] Backup RAM handling
  - [ ] Memory descriptors API
  - [ ] Cartridge RAM support
- [ ] Test save states at various game points
- [ ] Verify state persistence across sessions

**Milestone:** Save states working reliably

### Week 8: Core Options & Cartridges
- [ ] Core options framework:
  - [ ] Option definitions
  - [ ] Option callback handling
  - [ ] Runtime option changes
  - [ ] Per-game overrides support
- [ ] Initial core options:
  - [ ] Region selection
  - [ ] BIOS selection
  - [ ] Cartridge type
  - [ ] CD block mode (HLE/LLE)
- [ ] Cartridge support:
  - [ ] Backup RAM (512KB)
  - [ ] DRAM (1MB/4MB)
  - [ ] ROM cartridges
  - [ ] Auto-detection
- [ ] Test with cartridge-requiring games

**Milestone:** ✅ v0.2.0 Beta - Feature Complete Core
- All formats and input types supported
- Save states functional
- Core options exposed
- 70%+ game compatibility

---

## Phase 3: Polish & Optimization (Weeks 9-12) → v0.3.0

**Goal:** Production-ready, optimized core

### Week 9: Performance Optimization
- [ ] Profile critical paths:
  - [ ] Frame execution time
  - [ ] Audio/video delivery
  - [ ] Input processing
  - [ ] Memory operations
- [ ] Optimize hot code paths
- [ ] Reduce allocations in main loop
- [ ] SIMD optimizations for pixel conversion
- [ ] Multi-threaded audio processing
- [ ] CPU usage optimization
- [ ] Benchmark on target hardware
- [ ] Frame time consistency improvements

**Milestone:** Full speed on target hardware

### Week 10: Advanced Core Options
- [ ] Additional core options:
  - [ ] Graphics options:
    - [ ] Deinterlacing mode
    - [ ] Progressive rendering
    - [ ] Mesh transparency mode
    - [ ] Internal resolution
  - [ ] Audio options:
    - [ ] Interpolation quality
    - [ ] Channel enable/disable
  - [ ] Performance options:
    - [ ] CPU mode (dynarec/interpreter)
    - [ ] Frameskip
  - [ ] Debug options:
    - [ ] Log level
    - [ ] Performance overlay
- [ ] Option validation and sanity checks
- [ ] Documentation for all options

**Milestone:** Comprehensive configuration available

### Week 11: Advanced Features
- [ ] Rewind support:
  - [ ] `retro_get_rewind_support`
  - [ ] State buffering
  - [ ] Deterministic emulation
  - [ ] Memory efficiency
- [ ] Fast-forward support
- [ ] Frame advance (step forward/backward)
- [ ] Slow motion
- [ ] Run-ahead support (if feasible)
- [ ] Screenshot support
- [ ] Test all advanced features

**Milestone:** Advanced features functional

### Week 12: Testing & Documentation
- [ ] Comprehensive testing:
  - [ ] Game compatibility testing (100+ games)
  - [ ] Platform testing (all supported platforms)
  - [ ] Performance testing
  - [ ] Stress testing (extended play sessions)
  - [ ] Regression testing
- [ ] Documentation:
  - [ ] Complete README
  - [ ] COMPILING.md with all platforms
  - [ ] User guide
  - [ ] Troubleshooting guide
  - [ ] FAQ
- [ ] Bug fixing sprint
- [ ] Code cleanup and refactoring

**Milestone:** ✅ v0.3.0 RC - Release Candidate
- Production quality code
- 80%+ game compatibility
- No critical bugs
- Complete documentation
- All platforms building successfully

---

## Phase 4: Advanced Features (Weeks 13-16) → v1.0.0

**Goal:** Feature parity with standalone Ymir, polish for release

### Week 13: Graphics Enhancements
- [ ] Hardware rendering path (optional):
  - [ ] OpenGL support
  - [ ] Vulkan support
  - [ ] Direct3D support
- [ ] Enhanced graphics options:
  - [ ] Texture filtering
  - [ ] Widescreen hack
  - [ ] SSAA/MSAA
- [ ] High-resolution mode handling
- [ ] VRR support optimization
- [ ] Test visual quality vs standalone Ymir

**Milestone:** Graphics enhancements working

### Week 14: Debug Features & Edge Cases
- [ ] Debug features:
  - [ ] CPU state inspection
  - [ ] Memory viewer integration
  - [ ] Performance counters
  - [ ] Logging levels
- [ ] Edge case handling:
  - [ ] Corrupted disc images
  - [ ] Missing BIOS
  - [ ] Unsupported formats
  - [ ] Extreme save states
- [ ] Error recovery
- [ ] Graceful degradation

**Milestone:** Robust error handling

### Week 15: Community Beta & Feedback
- [ ] Public beta announcement:
  - [ ] libretro forums
  - [ ] Reddit (r/emulation)
  - [ ] Discord
  - [ ] GitHub release
- [ ] Gather community feedback
- [ ] Create compatibility spreadsheet
- [ ] Prioritize reported issues
- [ ] Fix high-priority bugs
- [ ] Performance tuning based on feedback
- [ ] Update documentation based on questions

**Milestone:** Community validation, feedback incorporated

### Week 16: Release Preparation
- [ ] Final bug fixes
- [ ] Performance final pass
- [ ] Documentation review and polish
- [ ] Create release assets:
  - [ ] Binary builds for all platforms
  - [ ] Source archives
  - [ ] Release notes
- [ ] Update CHANGELOG.md
- [ ] Prepare core info file (final)
- [ ] libretro repository submission:
  - [ ] Submit to libretro-super
  - [ ] Coordinate with buildbot team
- [ ] Marketing materials:
  - [ ] Screenshots
  - [ ] Demo videos
  - [ ] Comparison with other cores
- [ ] Release v1.0.0!

**Milestone:** ✅ v1.0.0 - Stable Release
- Official libretro repository inclusion
- 85%+ game compatibility
- Full platform support
- Production quality
- Active community

---

## Post-1.0 Roadmap

### v1.1.0 - Enhancements (Month 7-8)
- [ ] RetroAchievements integration
- [ ] Additional graphics enhancements
- [ ] Performance improvements
- [ ] More core options
- [ ] Bug fixes from community reports

### v1.2.0 - Advanced Features (Month 9-10)
- [ ] Network multiplayer support (if feasible)
- [ ] Texture replacement system
- [ ] HD upscaling filters
- [ ] More debug features
- [ ] Upstream Ymir sync

### v2.0.0 - Major Update (Month 11-12+)
- [ ] Major architecture improvements
- [ ] AI-assisted features
- [ ] Enhanced debugger
- [ ] VR support (experimental)
- [ ] Platform expansions

---

## Ongoing Tasks (Throughout Development)

### Continuous Integration
- Maintain CI/CD pipelines
- Automated testing
- Nightly builds
- Performance benchmarking

### Community Management
- Respond to issues
- Review pull requests
- Update documentation
- Engage with users
- Blog posts / dev updates

### Upstream Synchronization
- Monitor Ymir project
- Incorporate Ymir updates monthly
- Contribute improvements back to Ymir (if applicable)
- Maintain compatibility

### Quality Assurance
- Regular game testing
- Platform testing
- Performance monitoring
- Code review
- Security audits

---

## Success Metrics

### Technical Metrics
- ✅ **v0.1.0:** First playable game
- ✅ **v0.2.0:** 70% game compatibility, all features
- ✅ **v0.3.0:** 80% game compatibility, production quality
- ✅ **v1.0.0:** 85%+ game compatibility, full speed on target hardware

### Community Metrics
- 100+ GitHub stars by v1.0
- Active issue/PR engagement
- Positive reception on emulation forums
- Inclusion in libretro recommendations

### Adoption Metrics
- Available via RetroArch core downloader
- 1000+ downloads in first month
- Featured in emulation news/blogs
- Community contributions

---

## Risk Management

### High Priority Risks
- **Ymir integration challenges:** Mitigate with early prototyping
- **Performance issues:** Continuous profiling and optimization
- **Platform-specific bugs:** Thorough testing, community beta

### Medium Priority Risks
- **Save state compatibility:** Version carefully, test thoroughly
- **Limited testing resources:** Community beta program
- **Scope creep:** Strict phase adherence

### Mitigation Strategies
- Regular checkpoints at phase boundaries
- Community engagement for testing
- Maintain close Ymir project relationship
- Realistic timeline with buffer

---

## Notes

- Timeline is approximate and may adjust based on complexity
- Community feedback may reprioritize features
- Some features may move between phases
- Backward compatibility maintained for save states
- Documentation updated continuously

---

## Resources

- **PRD:** [PRD.md](PRD.md)
- **Architecture:** [ARCHITECTURE.md](ARCHITECTURE.md)
- **Contributing:** [CONTRIBUTING.md](CONTRIBUTING.md)
- **Issue Tracker:** GitHub Issues
- **Project Board:** GitHub Projects

---

**Document Version:** 1.0  
**Last Updated:** 2025-11-24

