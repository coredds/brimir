# Brimir Roadmap

**Status**: Active — version 0.11.0. Every upstream Ymir core change up to `ff935379` (2026-11-07) is reviewed (selective sync; see `src/core/UPSTREAM_SYNC_2026-11.md`)
**Last Updated**: 2026-11-25 - v0.11.0 added five more ST-V games (thirteen in all), Screen Rotation Auto and the SMEM save; v0.10.0 added ST-V arcade support

Gap-analysis-driven roadmap derived from comparing Brimir against mature RetroArch cores (Flycast, Beetle PSX, DuckStation, PCSX2, Genesis Plus GX, mGBA, bsnes). The release history is in [CHANGELOG.md](CHANGELOG.md).

## Guiding Principle

**Ymir hardware layer stays verbatim upstream, except the SH-2.** The Ymir source tree under `src/core/` is a direct copy of upstream with one exception: the SH-2 CPU (`src/core/include/ymir/hw/sh2/*`, `src/core/src/ymir/hw/sh2/*`) is Brimir-owned so the SH-2 JIT can hook into it. Changes and hand-ported upstream fixes are logged in [`src/core/BRIMIR_FORK.md`](src/core/BRIMIR_FORK.md). All other Brimir features must be implemented in the Bridge layer (`src/bridge/`), the Libretro layer (`src/libretro/`), or proposed upstream to Ymir.

Open items below are tagged:
- **Bridge** — Implementable purely in Brimir's bridge/libretro code, zero Ymir changes
- **Hybrid** — Mostly Bridge, needs minor Ymir config additions (propose upstream)
- **Upstream** — Requires substantial Ymir hardware-layer changes (propose upstream or long-term fork)

---

## Done

- **Cheats** — Action Replay codes from RetroArch's Saturn `.cht` database, applied once per frame (deterministic under run-ahead and netplay); RetroArch's cheat search works through the Beetle-layout RAM view
- **All controller types** — Control Pad, 3D Control Pad, Arcade Racer, Mission Stick, Shuttle Mouse and Virtua Gun with crosshair, plus deadzone and mouse sensitivity options (not done: six-axis/dual Mission Stick, Twin-Stick, Stunner)
- **System RAM exposure and memory descriptors** — work RAM in Beetle Saturn's layout
- **RetroAchievements** — Saturn sets made with Beetle read the same bytes (unlocks still to be confirmed with an RA account)
- **Save state compression** — LZ4-compressed, versioned states
- **Rewind and run-ahead** — deterministic save states (`savestate_features = "deterministic"`), virtual RTC by default
- **Netplay** — RetroArch netplay on the deterministic states (peers need the same BIOS, core options, cheats and CPU architecture)
- **Screen rotation (TATE)** and **overscan crop**
- **Audio volume** and **CD read speed up to 32x/Max**
- **CPU overclocking** — 100%–300% SH-2 clock
- **Contentless mode** (BIOS menu) and **M3U-less disc swapping**
- **Cartridges** — RAM cartridge saves, the 4 Mbit backup memory cartridge (`<game>.bcr`, Beetle's layout), the King of Fighters '95 and Ultraman ROM cartridges
- **Vulkan renderer** — Brimir's own compute renderer, identical to software at 1x, internal resolution up to 4x with VDP1 upscaling
- **SH-2 JIT, experimental** — opt-in x86-64 native backend and IR-interpreter backend (see item 20 below for what is open)
- **Accuracy options** — SH-2 cache emulation, CD Block low-level emulation, SCSP step granularity
- **Android (arm64) core** — tested on a Galaxy A56 (Vulkan at 1x)
- **ST-V arcade support** — thirteen games from MAME ROM sets: Baku Baku Animal, Columns '97, Puyo Puyo Sun, Die Hard Arcade, Golden Axe: The Duel, Cotton 2, Guardian Force, Radiant Silvergun, Shienryu (upright under Screen Rotation Auto), Final Fight Revenge, Steep Slope Sliders (the 315-5881 protection chip), DaeJeon! SanJeon SuJeon and Decathlete (the 315-5838 chip, checked against libretro-stv), the others against MAME 0.289 (board emulation in `src/bridge/stv/`)

---

## Open: Tier 1 — Bridge

### 8. VDP Layer Toggling & Debug Overlay
**Layer**: Bridge | **Effort**: ~150 LOC

Ymir exposes `saturn.VDP.SetLayerEnabled(Layer, bool)` and `saturn.VDP.vdp2DebugRenderOptions` (public field with extensive debug visualization). Expose as core options:
- Per-layer toggle (VDP1 sprites, each VDP2 BG layer)
- Debug overlay modes (layer coloring, priority stacks, color calc, shadow, windows, rotation params)

### 9. Internal Cheat Database
**Layer**: Bridge | **Effort**: ~200 LOC

Ship a JSON cheat database with the core. Auto-apply per-game Action Replay codes on load. Community-contributable. (Today the cheat files of RetroArch's database are used.)

### Cheat search of our own
**Layer**: Bridge | RetroArch's cheat search is used today.

---

## Open: Tier 2 — Hybrid

These need small additions to Ymir's `Configuration` struct but the heavy lifting is in Brimir. Propose these upstream to Ymir first; if accepted, implement in Brimir.

### 13. Frameskip
**Layer**: Hybrid | **Effort**: ~50 LOC Brimir + Ymir config field

Add `configuration.video.frameskip` to Ymir (partially exists in underlying VDP). Core option: 0–3 or Auto. Skips VDP rendering for N frames while continuing CPU emulation.

### 17. Region Patching (PAL→NTSC)
**Layer**: Hybrid | **Effort**: ~50 LOC Brimir + Ymir config

Propose `configuration.system.forceVideoStandard` upstream. Core option: "Force NTSC" for PAL games to fix 50Hz slowdown/borders.

---

## Open: Tier 3 — Major Engineering (Propose Upstream)

These require substantial Ymir hardware-layer changes. Propose to Ymir upstream; if accepted, Ymir gains the feature and Brimir inherits it. If rejected, evaluate long-term fork viability.

### 20. JIT Compiler (SH-2)
**Layer**: Upstream/Bridge (depends on architecture) | **Target**: 2026–2027

This is the single biggest performance feature — enables full-speed Saturn on low-end hardware, runahead, and rewind.

**Status**: Experimental, opt-in core option: "SH-2 JIT (Experimental)", `brimir_sh2_jit`, off by default. That is the intended state for now. The design is [design/sh2-jit.md](design/sh2-jit.md): portable IR, an x86-64 native backend (asmjit) and an IR-interpreter backend elsewhere, and interpreter-identical results at every instruction boundary. The SH-2 is forked in place, the one exception to the verbatim-upstream rule, logged in `src/core/BRIMIR_FORK.md`.

Working today:
- full instruction coverage except `TRAPA`, `RTE`, `SLEEP` and illegal opcodes;
- an x64 backend with block chaining, a register cache and tiered compiles;
- long-session code reuse and per-block release, and no flush on state loads (run-ahead and rewind keep their code);
- native compiles on a background thread (`brimir_sh2_jit_compile_thread`, x86-64 with 4+ threads);
- cheaper dispatch between scheduler slices.

Since the v0.8.0 performance round made the interpreter about 25% faster, the x86-64 JIT is slower than the interpreter: its SH-2 time is 18-73% higher on 7 of 9 test windows (faster only on the BIOS menu, -27%), and the whole frame with threaded VDP is 14-54% longer on most of them (evaluation of 2026-11-15, `docs/superpowers/records/2026-11-15-jit-evaluation/report.md`, local). The main costs are re-entering generated code every 32-cycle CPU slice, per-instruction exactness bookkeeping in the generated code, short blocks, and `RTE`/interrupt entry and polling loops falling back to the interpreter. Lockstep against the interpreter stays identical (BIOS menu and six games, 36,000 frames each). See [design/sh2-x64-performance.md](design/sh2-x64-performance.md) and [design/sh2-validation.md](design/sh2-validation.md).

Decision (2026-11-26): the JIT stays as it is, experimental and off by default, with no further speed work on the x86-64 backend. Longer CPU slices would change emulated timing (Saturn accuracy comes first), and leaner exact code generation is a large rewrite with an uncertain gain. Performance work goes where it matters, ARM devices, starting with a measurement of where their time goes; an ARM64 JIT (exact, or a faster less exact mode next to the exact interpreter) is only one possible answer to it. **Postponed** until ARM handhelds are available for testing.

Still open on the JIT itself:
- a long Burning Rangers session stutters while it loads a lot of new code (358 of 33,600 frames over 16.7 ms; the interpreter has none).

Next steps, including moving block decoding off the emulation thread and milestone 3 (ARM64 backend), are listed in [design/sh2-jit.md](design/sh2-jit.md), "Where to pick up".

### 21. Widescreen Hacks
**Layer**: Upstream | **Target**: Post-JIT

Needs VDP1/VDP2 viewport manipulation or post-transform vertex adjustment. Requires understanding game-specific 3D transform matrices in VDP1 command tables.

### 22. Texture Replacement (HD Packs)
**Layer**: Upstream | **Target**: Post-HW-Renderer

Hook into `IVDPRenderer::VDP1WriteVRAM()` and `VDP1WriteFB()` to dump/intercept texture data. Dump on first write, hash, load replacement from disk.

### 23. PGXP Perspective Correction
**Layer**: Upstream | **Target**: Post-HW-Renderer

Saturn VDP1 draws affine-textured quads without perspective correction. Needs interception of VDP1 draw commands inside the renderer.

### More Controller Types
**Layer**: Upstream (Ymir)

Six-axis and dual Mission Stick, Twin-Stick and Stunner are not offered.

### 24. Host-Independent RTC Time Zone
**Layer**: Bridge + Ymir determinism

Netplay peers must use the same time zone: the save state holds the clock, but the date the game shows goes through each computer's local time. Open: make that independent of the host.

### 25. More ST-V Arcade Games
**Layer**: Bridge + small hooks in the core | **Target**: Post-1.0

Thirteen games are done (see "Done"; the 315-5881 and 315-5838 chips and SanJeon's scramble are in). Open: more games from MAME's ST-V database (each needs its ROM list and a check against MAME), rewind and netplay checks with ST-V games, save states and run-ahead on the games other than Baku Baku Animal (the only game tried with both in RetroArch; on Decathlete only save and load state were tried, not run-ahead), a physical Android device, and the known differences from MAME (attract-mode pacing, Steep Slope Sliders' haze, Final Fight Revenge's Chinatown health bars, the 315-5881's block-to-block switch checked on synthetic data only; see `src/bridge/stv/STATUS.md`). Decided, kept as a known difference: the CD block. Real ST-V hardware has none; Brimir keeps the Saturn's mapped in ST-V mode, so the BIOS's check for it passes at once and games boot slightly earlier than on real hardware (Die Hard Arcade 23 frames ahead of MAME, measured; Steep Slope Sliders 28 at its chip's first access; the other games not measured one by one). Unmapping it was considered and not done: with Brimir's timing every boot would end 44-111 frames later than now, after MAME's, and the real wait length is unknown. The old branch `feature/stv-support` stays as reference. Big differentiator - no RetroArch core does ST-V well.

---

## Open: Tier 4 — Niche / Long-Term

| Feature | Layer | Notes |
|---|---|---|
| Multitap (3–12 players) | Upstream | Ymir has `TODO: implement multi-tap`. Saturn Bomberman supports 10 players |
| MODEM / NetLink emulation | Upstream | New hardware component. Historically significant |
| MPEG / VCD card | Upstream | New cartridge hardware. Very niche |
| Gaussian audio interpolation | Upstream | 3rd interpolation mode in SCSP. Linear is "hardware accurate" per Ymir |
| Saturn Keyboard | Upstream | New `PeripheralType` + report struct |
| Link cable / Taisen cable | Upstream | New hardware bridging two Saturn instances |
| Light gun sub-frame input | Bridge | The Virtua Gun is exposed; sub-frame polling in `RunFrame()` is open |

---

## Toward v1.0.0

Full speed on the target devices (desktop and the tested Android phones; a JIT is one possible way, not a goal in itself), 85%+ compatibility, RetroArch core inclusion (which also removes the Android Core Info workaround: Brimir's `.info` in libretro's core info files).

---

## Comparison Against Mature Cores

| Feature | Brimir | Flycast | Beetle PSX | DuckStation | PCSX2 | Status |
|---|---|---|---|---|---|---|
| JIT/Dynarec | SH-2, experimental (x86-64 native, opt-in) | SH-4 | MIPS | MIPS | EE/VU | Open |
| Hardware renderer | Vulkan (compute, 1x-4x) | Vulkan/OGL | Vulkan/OGL | Vulkan/OGL/D3D | Vulkan/OGL/D3D | Done |
| Cheats | Action Replay, RetroArch database | Yes | Yes | Yes | Yes | Done |
| RetroAchievements | Yes, Beetle layout | Yes | Yes | No | Yes | Done |
| 3D analog controller | 3D Control Pad, Arcade Racer, Mission Stick | Yes | DualShock | DualShock | DS2 | Done |
| Rewind | Yes, deterministic | Yes | Yes | Yes | Yes | Done |
| Runahead | Yes, deterministic | Yes | Yes | Yes | Yes | Done |
| Light gun | Virtua Gun with crosshair | Yes | GunCon | No | Yes | Done |
| Widescreen hacks | No | Yes | Yes | Yes | Yes | Open |
| HD textures | No | Yes | Mods | Yes | Yes | Open |
| TATE rotation | Yes | Yes | Via RA | No | No | Done |
| Netplay | RetroArch netplay | Rollback | Yes | No | No | Done |
| ST-V arcade | Yes, 13 games | No | N/A | N/A | N/A | Done (more games open) |
