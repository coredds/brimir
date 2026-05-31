# Brimir Roadmap

**Date**: 2026-05-31
**Status**: Active

Gap-analysis-driven roadmap derived from comparing Brimir against mature RetroArch cores (Flycast, Beetle PSX, DuckStation, PCSX2, Genesis Plus GX, mGBA, bsnes).

## Guiding Principle

**Ymir hardware layer stays verbatim upstream.** No patches, no namespace modifications. The Ymir source tree under `src/core/` is a direct copy of upstream. All Brimir features must be implemented in the Bridge layer (`src/bridge/`), the Libretro layer (`src/libretro/`), or proposed upstream to Ymir.

Feature categories below are tagged:
- **Bridge** — Implementable purely in Brimir's bridge/libretro code, zero Ymir changes
- **Hybrid** — Mostly Bridge, needs minor Ymir config additions (propose upstream)
- **Upstream** — Requires substantial Ymir hardware-layer changes (propose upstream or long-term fork)

---

## Tier 1 — Essential / High-Impact (Pure Bridge)

These are the biggest wins with the lowest implementation cost. All leverage Ymir's existing public API.

### 1. Cheat System
**Layer**: Bridge | **Effort**: ~500 LOC | **Target**: v0.5.0

Ymir exposes `saturn.mainBus.Poke<T>(addr, val)` for side-effect-free memory writes, plus public memory arrays (`WRAMLow`, `WRAMHigh`). Action Replay / GameShark / RAM watch / cheat search engine all possible.

- New file: `src/bridge/cheat_manager.cpp`
- Wires `retro_cheat_set` / `retro_cheat_reset` (currently stubs)
- Supports: RAM watch, hex search, cheat file import (.cht/.json)
- Flip `.info`: `cheats = "true"`

### 2. All Controller Types
**Layer**: Bridge | **Effort**: ~350 LOC | **Target**: v0.5.0

Ymir already supports all Saturn peripherals — Brimir just hardcodes `ConnectControlPad()` today. Expose everything:

| Peripheral | Ymir Method | Games |
|---|---|---|
| 3D Control Pad | `ConnectAnalogPad()` | NiGHTS, Burning Rangers, Panzer Dragoon Zwei |
| Arcade Racer | `ConnectArcadeRacer()` | Sega Rally, Daytona USA, Manx TT |
| Mission Stick | `ConnectMissionStick()` | After Burner, Space Harrier, Sky Target |
| Virtua Gun | `ConnectVirtuaGun()` | Virtua Cop 1&2, House of the Dead |
| Shuttle Mouse | `ConnectShuttleMouse()` | Shining Force III (map), Policenauts |

- Core option: "Controller Type" per port
- Input descriptors for 3D Pad analog axes, wheel, stick, gun coordinates
- Note: Virtua Gun gated by `Ymir_FF_VIRTUA_GUN` — verify build flag

### 3. System RAM Exposure + Memory Descriptors
**Layer**: Bridge | **Effort**: ~30 LOC | **Target**: v0.4.1

Ymir's `saturn.mem.WRAMLow` and `saturn.mem.WRAMHigh` are public `std::array<uint8_t, 1MiB>` fields. Currently `retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM)` returns nullptr.

- Return WRAM pointers in `retro_get_memory_data`
- Flip `.info`: `memory_descriptors = "true"`
- Expose: System RAM, Backup RAM, Cartridge RAM
- Unblocks RetroAchievements memory mapping

### 4. RetroAchievements
**Layer**: Bridge | **Effort**: ~200 LOC | **Target**: v0.5.0

Once system RAM is exposed (#3), RA integration is mostly configuration:
- Call `RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS`
- Expose memory regions for RA to peek
- Rich presence: expose game state (disc ID, region) via RA memory

### 5. Save State Compression
**Layer**: Bridge | **Effort**: ~50 LOC | **Target**: v0.4.1

Brimir controls serialization — currently raw `memcpy` of `sizeof(SaveState)`. Wrap with LZ4 (already vendored) before passing to libretro.

- 3-5x size reduction
- Enables viable rewind + runahead throughput
- Version header for forward-compatibility

### 6. Rewind & Runahead
**Layer**: Bridge + RetroArch | **Effort**: ~20 LOC | **Target**: v0.4.1

Once save states are fast and compressed (#5), these are RetroArch frontend features. Brimir just needs to ensure states serialize/deserialize quickly enough.

- Rewind: Already works if state size is reasonable
- Runahead: May need a "secondary instance" optimization (skip audio/video rendering)

### 7. Screen Rotation (TATE)
**Layer**: Bridge | **Effort**: ~100 LOC | **Target**: v0.5.0

Post-process the framebuffer in `OnFrameComplete()` before `video_cb`:
- 90° / 270° rotation for TATE shmups
- Core option: `brimir_rotation` (None, 90, 180, 270)
- Games: Battle Garegga, Dodonpachi, Radiant Silvergun, Layer Section

### 8. VDP Layer Toggling & Debug Overlay
**Layer**: Bridge | **Effort**: ~150 LOC | **Target**: v0.5.0

Ymir exposes `saturn.VDP.SetLayerEnabled(Layer, bool)` and `saturn.VDP.vdp2DebugRenderOptions` (public field with extensive debug visualization). Expose as core options:
- Per-layer toggle (VDP1 sprites, each VDP2 BG layer)
- Debug overlay modes (layer coloring, priority stacks, color calc, shadow, windows, rotation params)

### 9. Internal Cheat Database
**Layer**: Bridge | **Effort**: ~200 LOC | **Target**: v0.5.1

Ship a JSON cheat database with the core. Auto-apply per-game Action Replay codes on load. Community-contributable.

### 10. Contentless Mode (BIOS Menu)
**Layer**: Bridge | **Effort**: ~10 LOC + testing | **Target**: v0.4.1

Flip `.info`: `supports_no_game = "true"`. Ymir's `Saturn()` boots to BIOS menu when a real IPL is loaded without a disc. Gives access to the Saturn CD player, memory manager, and settings.

### 11. M3U-Less Disc Swapping
**Layer**: Bridge | **Effort**: ~50 LOC | **Target**: v0.4.1

Ymir exposes `EjectDisc()`, `LoadDisc(media::Disc&&)`, tray control. Extend disk control interface to allow direct disc path loading without M3U playlist pre-creation.

---

## Tier 2 — Significant Enhancements (Hybrid)

These need small additions to Ymir's `Configuration` struct but the heavy lifting is in Brimir. Propose these upstream to Ymir first; if accepted, implement in Brimir.

### 12. CPU Overclocking
**Layer**: Hybrid | **Effort**: ~100 LOC Brimir + Ymir config field | **Target**: v0.6.0

Add `configuration.system.cpuClockMultiplier` to Ymir (propose upstream). Core option: 100% / 125% / 150% / 200% SH-2 clock. Fixes Saturn's notorious slowdown in Panzer Dragoon Saga, Burning Rangers, Sonic R.

### 13. Frameskip
**Layer**: Hybrid | **Effort**: ~50 LOC Brimir + Ymir config field | **Target**: v0.5.0

Add `configuration.video.frameskip` to Ymir (partially exists in underlying VDP). Core option: 0–3 or Auto. Skips VDP rendering for N frames while continuing CPU emulation.

### 14. Overscan Crop
**Layer**: Bridge (no Ymir changes needed after all) | **Effort**: ~40 LOC | **Target**: v0.5.0

Post-process crop in `OnFrameComplete()` — trim N pixels from each edge before `video_cb`. Core options for horizontal and vertical overscan.

### 15. CD Read Speed Beyond 16x
**Layer**: Bridge | **Effort**: ~10 LOC | **Target**: v0.4.1

Ymir accepts values 2–200. Brimir's options only expose 2x–16x. Add 24x, 32x, Max/Instant options.

### 16. Audio Volume
**Layer**: Bridge | **Effort**: ~30 LOC | **Target**: v0.5.0

Scale samples in `OnAudioSample()` before buffering. No Ymir changes needed. Core option: 0–200% volume.

### 17. Region Patching (PAL→NTSC)
**Layer**: Hybrid | **Effort**: ~50 LOC Brimir + Ymir config | **Target**: v0.5.0

Propose `configuration.system.forceVideoStandard` upstream. Core option: "Force NTSC" for PAL games to fix 50Hz slowdown/borders.

### 18. Cartridge RAM Persistence
**Layer**: Bridge | **Effort**: ~100 LOC | **Target**: v0.5.0

Ymir's `BackupMemoryCartridge` exists and is fully wired. Brimir's `core_wrapper.cpp` has `m_cartridgePath` and TODO comments. Wire up cartridge save/load in `LoadGame()`/`UnloadGame()`.

---

## Tier 3 — Major Engineering (Propose Upstream)

These require substantial Ymir hardware-layer changes. Propose to Ymir upstream; if accepted, Ymir gains the feature and Brimir inherits it. If rejected, evaluate long-term fork viability.

### 19. Hardware Renderer + Internal Resolution Scaling
**Layer**: Upstream (new `IVDPRenderer` implementation) | **Target**: Post-JIT

Ymir had a Vulkan renderer in v0.2.x, removed in v0.3.0. Code preserved in `backup_brimir_hw/`. Needs re-implementation as `VulkanVDPRenderer` / `OpenGLVDPRenderer` / `D3DVDPRenderer` implementing `IVDPRenderer` interface.

Brimir would expose: renderer selection, 2x–8x internal resolution, texture filtering, upscaling filters.

### 20. JIT Compiler (SH-2)
**Layer**: Upstream/Bridge (depends on architecture) | **Target**: 2026–2027

1100+ test cases designed, x64/ARM64 backends stubbed, Phase 0 test infrastructure built. This is the single biggest performance feature — enables full-speed Saturn on low-end hardware, runahead, and rewind.

See `src/jit/STATUS.md` and `docs/JIT_*` for implementation details.

### 21. Widescreen Hacks
**Layer**: Upstream | **Target**: Post-JIT

Needs VDP1/VDP2 viewport manipulation or post-transform vertex adjustment. Requires understanding game-specific 3D transform matrices in VDP1 command tables.

### 22. Texture Replacement (HD Packs)
**Layer**: Upstream | **Target**: Post-HW-Renderer

Hook into `IVDPRenderer::VDP1WriteVRAM()` and `VDP1WriteFB()` to dump/intercept texture data. Dump on first write, hash, load replacement from disk.

### 23. PGXP Perspective Correction
**Layer**: Upstream | **Target**: Post-HW-Renderer

Saturn VDP1 draws affine-textured quads without perspective correction. Needs interception of VDP1 draw commands inside the renderer.

### 24. Netplay / Rollback
**Layer**: Bridge + Ymir determinism | **Target**: Post-JIT

Requires perfect determinism across dual SH-2s, SCSP timers, CD block timing. Needs Ymir deterministic mode + RetroArch netplay integration.

### 25. ST-V Arcade Support
**Layer**: Upstream (new system class) | **Target**: Post-1.0

ST-V is separate hardware with different memory map, ROM board, JAMMA I/O. Would require new `stv::Titan` class or major `Saturn` modifications. Big differentiator — no RetroArch core does ST-V well.

---

## Tier 4 — Niche / Long-Term

| Feature | Layer | Notes |
|---|---|---|
| Multitap (3–12 players) | Upstream | Ymir has `TODO: implement multi-tap`. Saturn Bomberman supports 10 players |
| MODEM / NetLink emulation | Upstream | New hardware component. Historically significant |
| MPEG / VCD card | Upstream | New cartridge hardware. Very niche |
| Gaussian audio interpolation | Upstream | 3rd interpolation mode in SCSP. Linear is "hardware accurate" per Ymir |
| Saturn Keyboard | Upstream | New `PeripheralType` + report struct |
| Link cable / Taisen cable | Upstream | New hardware bridging two Saturn instances |
| Light gun sub-frame input | Bridge | Once Virtua Gun is exposed (#2), add sub-frame polling in `RunFrame()` |

---

## Timeline (Proposed)

| Version | Features |
|---|---|
| **v0.4.1** | System RAM exposure, memory descriptors, save state compression, contentless mode, M3U-less disc swap, CD speed 24x+, rewind/runahead viability |
| **v0.5.0** | Cheat system, all controller types, screen rotation, layer toggling, overscan crop, audio volume, cartridge RAM persistence, frameskip, region patching, RetroAchievements |
| **v0.5.1** | Internal cheat database, extended compatibility testing |
| **v0.6.0** | CPU overclocking (hybrid — needs Ymir config), VDP2 debug overlay exposure |
| **v0.7.0+** | JIT compiler (Phase 2), hardware renderer review |
| **v1.0.0** | JIT stable, 85%+ compatibility, RetroArch core inclusion |

---

## Comparison Against Mature Cores

| Feature | Brimir (current) | Flycast | Beetle PSX | DuckStation | PCSX2 | Target Brimir |
|---|---|---|---|---|---|---|
| JIT/Dynarec | None | SH-4 | MIPS | MIPS | EE/VU | v0.7.0+ |
| Hardware renderer | SW only | Vulkan/OGL | Vulkan/OGL | Vulkan/OGL/D3D | Vulkan/OGL/D3D | Post-JIT |
| Cheats | Stub | Yes | Yes | Yes | Yes | v0.5.0 |
| RetroAchievements | Planned | Yes | Yes | No | Yes | v0.5.0 |
| 3D analog controller | No | Yes | DualShock | DualShock | DS2 | v0.5.0 |
| Rewind | No | Yes | Yes | Yes | Yes | v0.4.1 |
| Runahead | No | Yes | Yes | Yes | Yes | v0.4.1+ |
| Light gun | No | Yes | GunCon | No | Yes | v0.5.0 |
| Widescreen hacks | No | Yes | Yes | Yes | Yes | Post-JIT |
| HD textures | No | Yes | Mods | Yes | Yes | Post-HW |
| TATE rotation | No | Yes | Via RA | No | No | v0.5.0 |
| Netplay | No | Rollback | Yes | No | No | Post-JIT |
| ST-V arcade | No | No | N/A | N/A | N/A | Post-1.0 |

---

## Archival Notes

This roadmap replaces the previous Phase 1–4 plan (Phase 1: Ymir sync, Phase 2: JIT, Phase 3: macOS/ARM64, Phase 4: GPU acceleration). The old plan was hardware-centric and didn't account for the constraint that Ymir must stay verbatim upstream. This new roadmap:

- Respects the Ymir verbatim-upstream constraint
- Prioritizes features by user impact and implementation feasibility
- Clearly tags each feature by where it must be implemented (Bridge / Hybrid / Upstream)
- Is informed by a gap analysis against 7 mature RetroArch cores

Historical planning documents under `docs/JIT_*`, `docs/PHASE*`, `docs/SESSION_*` remain for reference but are superseded by this roadmap.
