# Brimir

A Sega Saturn emulation core for libretro, built on the [Ymir](https://github.com/StrikerX3/ymir) emulator by StrikerX3.

## Overview

Brimir is a libretro core for Sega Saturn emulation, wrapping Ymir's cycle-accurate hardware layer. It provides accurate emulation with optimized software rendering and full VDP1/VDP2 support.

**Current Status**: Active development. Every upstream Ymir core change up to `ff935379` (2026-11-07) is reviewed and the relevant fixes are taken, with Brimir-specific fixes and optimizations on top.

## What's New in v0.7.2

- **Upstream Ymir sync** - every upstream Ymir core change up to `ff935379` (2026-11-07) reviewed: 533 commits, 4 fixes taken, 92 already in Brimir (including every game fix Ymir names in that period), Brimir's own version kept for 2, the rest skipped (mostly Ymir's GPU renderers and host CD drive support). Details in `src/core/UPSTREAM_SYNC_2026-11.md`.
- **CD Block LLE** - resuming a paused CD audio track now continues where it was paused instead of restarting from the track's pregap.
- No visible change otherwise: frames, save states and run-ahead behave exactly as in 0.7.1.

## Previous Highlights

### v0.7.1

- **BIOS Auto picks the disc's region** - a Japan/Asia-only disc boots with a Japanese BIOS, any other disc with a US/EU one (falling back to any BIOS present), and region autodetection runs a multi-region disc in the BIOS's region. See [Installation](#installation).
- **BIOS settings in save states** - a state carries the BIOS language and settings and applies them for the session; your saved settings never change from a loaded state. See [Save states](#save-states-run-ahead-rewind-and-netplay).
- **Controller options** - Analog Stick Deadzone and Trigger Deadzone (15 % by default; 0 % = 0.7.0's mapping) and Mouse Sensitivity (100 % by default); the Shuttle Mouse moves as in Beetle Saturn. See [Controllers](#controllers).
- **CD Block LLE validated with the real ROMs** - `cdb106.bin` and `cdb105.bin` boot the tested games, and run-ahead, rewind and netplay are exact with them in the tested games (a save-state fix). See [Accuracy options](#accuracy-options).
- **Upgrading from 0.7.0** - states saved by 0.7.0 for non-Japanese discs were made with the Japanese BIOS (0.7.0's Auto); they load with the US/EU BIOS now, but if one misbehaves, set BIOS Selection to the Japanese BIOS to continue it. In-game saves (`.srm`) are unaffected. In Host RTC mode the new `_us_eu` settings file means the BIOS setup screen appears once.

### v0.7.0

- **Run-ahead and netplay** - save states now continue exactly like the uninterrupted run, so RetroArch allows run-ahead and netplay, and rewind is exact. The Saturn clock runs on emulated time by default (new RTC Mode option), and a first boot no longer stops at the BIOS clock setup screen. See [Save states, run-ahead, rewind and netplay](#save-states-run-ahead-rewind-and-netplay).
- **Cheats and RetroAchievements** - the Saturn cheat files of RetroArch's database work (Action Replay codes), and work RAM is laid out as in Beetle Saturn, so achievement sets and cheat searches made with Beetle work too. See [Cheats](#cheats) and [RetroAchievements](#retroachievements).
- **Every Saturn controller** - 3D Control Pad, Arcade Racer, Mission Stick, Shuttle Mouse and Virtua Gun (with an on-screen crosshair) on both ports. See [Controllers](#controllers).
- **Cartridges** - The King of Fighters '95 and Ultraman with their ROM cartridge dumps, a backup memory cartridge saved in Beetle Saturn's `.bcr` layout, and a Cartridge option. See [Cartridges](#cartridges).
- **Accuracy options** - SH-2 cache emulation, low-level CD Block emulation (needs the CD Block ROM) and SCSP step granularity. See [Accuracy options](#accuracy-options).
- **Save-state fixes** - the state format is now version 11; states from v0.6.x and earlier versions down to version 4 still load (except Dezaemon 2 and Galaxy Force II states, see [Save states](#save-states-run-ahead-rewind-and-netplay)). Full list in [CHANGELOG.md](CHANGELOG.md).

### v0.6.1

- **Crash fix** - RetroArch no longer crashes on Quick Menu > Close Content when the core uses the Vulkan renderer. Hotfix on top of v0.6.0 with no other changes; save states and saves are unaffected.

### v0.6.0

- **Vulkan renderer (optional)** - set Renderer to Vulkan (applies on content load; needs RetroArch's Vulkan video driver and a Vulkan 1.2 GPU) to draw VDP1 and VDP2 with compute shaders on the GPU. At native resolution every frame and save state is identical to the software renderer, and states load in either renderer. See [Vulkan renderer](#vulkan-renderer).
- **Internal resolution up to 4x** - Internal Resolution (live) renders backgrounds, and with VDP1 Upscaling (on by default, live) polygons and sprites, at 2x-4x: sharper 3D edges, rotated floors and distant textures. Framebuffer memory, save states, run-ahead, rewind and netplay behave exactly as at 1x.
- **Software renderer fixes** - checking the GPU renderer against software frame by frame found and fixed several software-renderer bugs: threaded VDP2 save states and resets, VDP1 line freezes and lost lines, uninitialised VDP state, 16-colour background save states (save-state format version 5; older states convert on load), transparent meshes, mosaic line colour, colour gradation and rotated hi-res sprites.
- **Also** - the Deinterlacing option works on its own, CHD memory use is capped, backup RAM no longer depends on a temp file, upstream Ymir CD block, SMPC, SCU, VDP and SCSP fixes, and an experimental SH-2 JIT (off by default). Full list in [CHANGELOG.md](CHANGELOG.md).

### v0.5.5

- **Crash fix** - RetroArch on Windows no longer exits on Quick Menu > Close Content ([#7](https://github.com/coredds/brimir/issues/7)). Hotfix on top of v0.5.4 with no other changes; save states and saves are unaffected.

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
- **Ymir Hardware Layer**: Cycle-accurate Saturn emulation, based on the upstream Ymir hardware-layer sync of 2026-11-07 (`ff935379`)
- Accurate SH-2 dual-CPU emulation with WB/EX stall timing, 32-bit instruction fetch, and inlined opcode decode
- Full VDP1 sprite engine and VDP2 scroll plane graphics with COPR register fix
- SCSP (Saturn Custom Sound Processor) audio with configurable interpolation and volume control
- M68000 sound CPU emulation
- SCU DSP and DMA emulation
- CHD and ISO disc format support via libchdr
- Save states (LZ4-compressed) and backup RAM persistence
- Versioned save states (states from incompatible builds are rejected)
- SRAM managed as a single source of truth for libretro `.srm` files
- Per-region persistent SMPC/RTC data with automatic migration from the legacy filename
- Auto-detection of console region from disc; real NTSC/PAL timing reported to the frontend
- Configurable CD read speed (2x-32x/Max)
- SH-2 CPU overclocking (100%-300%)
- Cartridges inserted automatically from the game database, or chosen with the "Cartridge" core option: RAM
  expansion (1MB, 4MB, 6MB), the ROM cartridges of The King of Fighters '95 and Ultraman (dumps `mpr-18811-mx.ic1`
  and `mpr-19367-mx.ic1` in the system folder), and a 4 Mbit backup memory cartridge saved to `<game>.bcr` (Beetle
  Saturn's file layout)
- Ymir's accuracy options as core options: SH-2 cache emulation (always on for the games Ymir's database marks),
  low-level CD Block emulation (needs the CD Block ROM, `cdb106.bin`, `cdb105.bin` or `cdb104.bin`, in the system
  folder; without it the CD Block stays high-level) and SCSP step granularity

### Rendering
- **Software Renderer**: Ymir's proven software renderer with pixel-perfect accuracy
  - SIMD software rendering: baseline SSE2 on x64, NEON on ARM64, optional AVX2 on compatible x64 CPUs
  - Threaded VDP1 and VDP2 rendering for optimal performance
  - Full resolution output (no overscan cropping by VDP)
- **Vulkan Renderer** (optional, 64-bit desktop builds): VDP1 and VDP2 on the GPU, identical to the software
  renderer at native resolution, with an internal resolution of 2x-4x for sharper 3D (see
  [Vulkan renderer](#vulkan-renderer))
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
- Work RAM exposed in Beetle Saturn's layout (`RETRO_MEMORY_SYSTEM_RAM` and memory map) for RetroAchievements, RetroArch cheat search and memory viewer
- Action Replay cheats (`retro_cheat_set`) for the Saturn cheat files in RetroArch's database
- Deterministic save states (`savestate_features = "deterministic"`): run-ahead, rewind and netplay
- Control Pad, 3D Control Pad, Arcade Racer, Mission Stick, Shuttle Mouse and Virtua Gun on ports 1 and 2
- Real NTSC/PAL region reporting with dynamic AV timing updates
- Save state versioning for forward compatibility
- Performance profiling option (gated via core option)
- Full controller remapping support

## Performance

The 2026-11 performance round (in the next release) made the software renderer and the SH-2 interpreter faster without changing any output. Frames, audio and save states are byte-identical to 0.7.2. On the development PC (Ryzen 7 5700G, Release + LTO, MSVC on Windows; GCC and Clang builds moved in the same direction when measured during the round but were not re-measured at the end), the median frame time over 9 test windows is 20-64 % shorter with threaded VDP and 29-61 % shorter without it. With threaded VDP, Virtua Fighter 2 dropped from 18.4 to 6.7 ms and Fighters Megamix from 17.6 to 6.9 ms. Single-instance run-ahead frames are 23-39 % shorter. Details per area are in `CHANGELOG.md`. The harness used to prove and measure each change is in `tools/perf/`.

ARM devices have not been measured. Scaled from the PC profile (a Cortex-A77-class prime core at about 0.6x the PC's single-thread speed), the software renderer would now keep every test window under the 16.7 ms frame budget on a Snapdragon 865-class device, where 0.7.2 was estimated at 11-35 ms. The SH-2 interpreter (aarch64 has no native JIT) and the deinterlacer in interlaced games are the limits there.

## Known Limitations

- **Interlaced / high-resolution performance** — The software VDP2 renderer can be CPU-bound in interlaced titles such as *Virtua Fighter 2* on slower CPUs (much less since the 2026-11 performance round, see "Performance"). When the core cannot complete a frame within the NTSC/PAL frame budget, RetroArch compensates by stretching or dropping audio, causing music to cut out or slow down while video remains smooth. Deinterlacing Mode = `None` provides the best performance in these titles.
- **Controllers** — Ports 1 and 2 only (Ymir emulates no multitap). The six-axis Mission Stick, dual Mission Sticks, Twin-Stick and Stunner are not offered.
- **CD Block LLE** — Low-level CD Block emulation is a work in progress in Ymir. With the real ROMs (`cdb106.bin` v1.06, `cdb105.bin` v1.05) the BIOS menu, Sega Rally Championship, Virtua Fighter 2, Burning Rangers, Guardian Heroes and Panzer Dragoon II Zwei boot to their intros and attract modes and run exactly with run-ahead and from a state on another instance; boots take about 0.2-2.5 s longer (11-152 frames) than with the high-level CD Block. Other games are untested.
- **SH-2 JIT** — Experimental and off by default (core option "SH-2 JIT (Experimental)", key `brimir_sh2_jit`). It compiles every SH-2 instruction except `TRAPA`, `RTE`, `SLEEP` and illegal opcodes, which stay on Ymir's interpreter, and is validated against the interpreter in lockstep (BIOS plus six games, up to 36000 frames each, identical; see [design/sh2-validation.md](design/sh2-validation.md)). On x86-64 builds it generates native x86-64 code (via asmjit); other builds run the compiled blocks through an IR interpreter. Both are validated against the interpreter. The x64 backend's SH-2 time is between about 5% and 28% below the interpreter's across six games, and its native compiles run on an extra thread where the CPU has at least 4 threads (core option "SH-2 JIT Compile Thread (Experimental)"); the IR interpreter backend is about 3x slower than the interpreter (see [design/sh2-x64-performance.md](design/sh2-x64-performance.md)). It can still stutter while a game loads a lot of new code: Burning Rangers over a long session has SH-2 time about 5% below the interpreter's, but 358 of 33,600 frames take over 16.7 ms (the interpreter: none; the worst 48 ms), and with threaded VDP, the default, the average frame of Sega Rally, Street Fighter Zero 3 and that session is 2-6% longer than the interpreter's. It uses up to 128 MB of native code per SH-2 CPU, allocated on demand. It is not used while SH-2 cache emulation is active. It stays an experimental, opt-in option for testing; the current state and the next steps are in [design/sh2-jit.md](design/sh2-jit.md) ("Where to pick up").

## Save states, run-ahead, rewind and netplay

A save state followed by a load continues exactly like the run that never stopped, so the core declares
deterministic save states and RetroArch allows run-ahead (one instance or "Use Second Instance") and netplay; rewind is
exact too. This was checked frame by frame on nine games (one-frame run-ahead against a normal run, 900 frames each,
from boot and from a gameplay state, software and Vulkan renderers: every frame and every state identical), and with
run-ahead against a normal run on the BIOS menu with each controller type and each accuracy option (and on four games
with the controllers).

- **RTC Mode** (System, applies when content is loaded): `Virtual` (default) runs the Saturn's clock on emulated time,
  so states replay the same date. It starts at your computer's local time and continues from the clock saved in the
  per-region RTC file in the system folder. A first boot with no RTC file no longer shows the BIOS language and clock
  setup screen (English, default settings). `Host` is the previous behaviour: the clock follows the computer's.
  In Virtual mode the clock is part of the state: loading a state or rewinding moves it back to the state's time, and
  the clock continues (and is saved to the RTC file) from there. States from v0.6.x, which ran on the computer's clock,
  keep the current clock instead.
- **Backup RAM is in save states** (the internal one and a backup memory cartridge), as in Beetle Saturn: loading a
  state also rolls back in-game saves made after it was saved.
- **BIOS settings are in save states** (the language and the other settings the BIOS keeps): loading a state applies
  the state's settings for the session only. The settings saved in the RTC file change only when the BIOS or a game
  writes them (the BIOS settings screen), never from a loaded state. States from before version 12 keep the session's.
  A hard reset keeps a loaded state's settings for the rest of the session (reload the content to get your own back).
  A settings change made in a frame that run-ahead or netplay later rolls back, or that rewind goes back past, still
  reaches the RTC file (as in v0.7.0; it takes the BIOS settings screen during netplay or rewind).
- **Save-state format**: version 12. States from older versions (version 4 on, which includes the v0.6.x states) still
  load. States are larger than before (about 800 KiB more before compression). One exception: Dezaemon 2 and Sega
  Ages: Galaxy Force II now get a backup memory cartridge under Cartridge = Auto, so their v0.6.x states (saved without
  one) are refused; set Cartridge to None and reload the content to load them.
- **Netplay**: both machines need the same BIOS and content, and should also use:
  - the same core options, in particular SH-2 Cache Emulation and SCSP Step Granularity, which are not emulation state:
    a state saved with other values loads, keeps the local settings and logs a warning (once per content);
  - the same cheats, enabled alike: cheats are not in save states and write memory every frame;
  - the same time zone (the state holds the clock; the date shown goes through each computer's local time);
  - the same kind of computer: states hold host-order data, so netplay between different CPU architectures is not
    supported.
- **Vulkan at 2x-4x**: with VDP1 upscaling, the upscaled sprite detail of the first few frames after a load (1-5 in
  the tests) can differ from the run that never stopped (GPU output only; the states are identical).

## Cheats

Quick Menu > Cheats > Load Cheat File > `Sega - Saturn`: the Saturn cheat files of RetroArch's database work. Every
code of the 192 files parses.

- Pro Action Replay codes: 16-bit writes `1AAAAAAA VVVV`, 8-bit writes `3AAAAAAA 00VV`, and "if equal" conditionals
  `DAAAAAAA VVVV` (the next code of the same cheat applies only when the 16-bit word equals the value). Codes can be
  written as `16033c7400c0`, `1601dba4 0900`, `D6033C60+270F`, joined with `+`, or typed with spaces only
  (`1601dba4 0900 16033c5e 270f`). Master codes (`F...`, `B...`) are accepted and need nothing here. A plain address
  without a type digit (`06048072 0005`, in a few database files) is a 16-bit write.
- A cheat with a placeholder value (`00??`, to be edited first) or a typo is ignored as a whole, with a warning in the
  log.
- The cheats write work RAM once per frame, before the frame runs, including run-ahead's hidden frames, so they stay
  exact with save states, rewind, run-ahead and netplay. Disabling a cheat stops its writes; cheats are not part of
  save states.
- Cheats RetroArch applies itself (entries with an address and a value instead of a code, and cheat search results)
  are written by RetroArch between frames, so run-ahead's replayed frames do not see them; Beetle Saturn behaves the
  same.
- With SH-2 Cache Emulation on, a CPU may keep using a value it already holds in its cache for a moment after a cheat
  changed it in RAM.

## RetroAchievements

The core declares achievement support and lays out work RAM as Beetle Saturn does (2 MiB: Work RAM Low, then Work RAM
High, the two bytes of each 16-bit word swapped), the layout Saturn achievement sets are made for. The memory map
gives Work RAM Low at `0x00200000` and Work RAM High at `0x06000000`. The same view serves RetroArch's cheat search
and memory viewer; writes from them reach the emulated RAM before the next frame.

## Controllers

Quick Menu > Controls > Port 1/2 Controls > Device Type:

| Device | Notes |
|---|---|
| Control Pad (default) | The same mapping as earlier versions; "RetroPad w/ Analog" in a v0.6.x configuration is the Control Pad |
| 3D Control Pad | Analog stick and triggers; Select switches between analog and digital mode (analog at power-on) |
| Arcade Racer | Steering wheel on the left stick |
| Mission Stick | Stick on the left stick, throttle on the right stick |
| Shuttle Mouse | Mouse movement and buttons; mouse buttons 4/5 are Start. Movement is kept until the game reads the mouse (none counted twice; beyond one read's range the rest is dropped, as in Beetle Saturn) |
| Virtua Gun | Light gun: trigger, reload, Start; aiming off screen and pulling the trigger reloads |
| None | Nothing connected |

The analog devices follow Beetle Saturn's mapping:

| RetroPad | Control Pad | 3D Control Pad | Arcade Racer | Mission Stick |
|---|---|---|---|---|
| B / A | A / B | A / B | A / B | A / B |
| Y / X | C / X | X / Y | X / Y | X / Y |
| L / R | L / R | Z / C | Z / C | Z / C |
| L2 / R2 | Y / Z | L / R (analog) | shift up / down | L / R |
| Start | Start | Start | Start | Start |
| Select | - | analog/digital mode | - | - |
| D-pad | D-pad | D-pad | Up/Down (shifter) | - |
| Left stick | - | stick | wheel | stick |
| Right stick Y | - | - | - | throttle |
| R3 | - | - | - | throttle latch (press to hold, again to release) |

Gun Crosshair (Input, applies at once): `Cross` (default), `Dot` or `Off`; red for port 1, blue for port 2. It is
drawn on the shown frame only (the game does not see it), with the software and the Vulkan renderer. A device change
applies between frames, also with run-ahead, and a state saved with one device loads with another.

Analog Stick Deadzone and Trigger Deadzone (Input, apply at once; 15% by default, 0% reads the sticks and triggers as
reported): the stick of the 3D Control Pad, Arcade Racer and Mission Stick reads centred within that share of its
range (a round deadzone), and the 3D Control Pad's analog L/R read released within that share of their travel; beyond
it the rest is stretched to the full range, so there is no jump at the edge. They add to the frontend's own deadzone.
The Mission Stick throttle has none. The 15% defaults (Beetle Saturn's) change these devices' output compared with
0.7.0; 0% restores 0.7.0's mapping exactly, and the Control Pad is unaffected. Mouse Sensitivity (Input, applies at
once) scales the Shuttle Mouse's movement; as in Beetle Saturn, a read beyond the mouse's range reports the limit
with the overflow bit and drops the rest. For netplay, peers need the same input options (deadzones, mouse
sensitivity): they change what the game reads.

## Cartridges

The Cartridge option (System, applies when content is loaded) picks what is in the cartridge slot. `Auto` (default)
inserts what the game needs, from Ymir's game database:

- **RAM expansion** (1, 4 or 6 MB) for the games that use it, as before; saved to `<game>.cart` in the save folder.
- **ROM cartridges**: The King of Fighters '95 needs `mpr-18811-mx.ic1` and Ultraman: Hikari no Kyojin Densetsu
  `mpr-19367-mx.ic1` (2 MiB dumps) in the system folder (the names Beetle Saturn and Kronos use). A missing dump, one of
  the wrong size, or the other game's dump is reported on screen and in the log, and the game starts without the
  cartridge.
- **Backup memory**: a 4 Mbit cartridge for the games that save to one (Dezaemon 2, Sega Ages: Galaxy Force II), saved
  to `<game>.bcr` in the save folder in the same layout as Beetle Saturn's, so a Beetle `.bcr` loads unchanged (Beetle's
  `.4mb.bcr` variant is not read).

The other values force a cartridge: None, 1 MB RAM, 4 MB RAM, Backup Memory, The King of Fighters '95 ROM, Ultraman
ROM. The backup cartridge file is written about every 5 seconds while the game changes it, and when the content is
closed; a RAM cartridge's file (volatile memory on the real hardware) only when the content is closed. A failed write is logged with the file it left the data in (`<file>.tmp`). With run-ahead's "Use
Second Instance", the second instance writes the same files when it closes too, so the last one closed wins (both hold
the game's saves; the second instance runs a few frames ahead).

## Accuracy options

Ymir's accuracy features, off by default (the defaults emulate exactly as before):

| Option | Values | Notes |
|---|---|---|
| SH-2 Cache Emulation (System) | OFF (default), ON | Applies when content is loaded. Emulates the SH-2 caches; costs some speed. Games Ymir's database marks (Astal, Dark Savior, Waku Waku 7 and others) always get it. The SH-2 JIT is not used while it is active |
| CD Block Low-Level Emulation (Media) | OFF (default), ON | Applies when content is loaded. Runs the CD Block's own SH-1 firmware; slower, and a work in progress in Ymir. Needs the CD Block ROM (64 KiB) in the system folder as `cdb106.bin`, `cdb105.bin` or `cdb104.bin`; without one the CD Block stays high-level, with an on-screen message. A high-level save state switches the session to high-level (shown on screen); a low-level state needs this option and the same ROM. Validated with `cdb106.bin` and `cdb105.bin` (see [Known Limitations](#known-limitations)); save states, run-ahead, rewind and netplay are exact with them in the tested games (BIOS menu, Sega Rally, Virtua Fighter 2, Burning Rangers, Guardian Heroes, Panzer Dragoon II Zwei) |
| SCSP Step Granularity (Audio) | 0 (default) to 5 | Applies at once. Steps the sound processor in finer slices (0: 32 slots at a time, 5: one slot) |

Threaded SCSP is not offered: Ymir does not implement it.

## Vulkan renderer

The Vulkan renderer draws the Saturn's two video chips on the GPU: VDP1 (polygons and sprites) and VDP2
(backgrounds). At native resolution (1x) every frame and every save state is identical to the software renderer. At
2x-4x the GPU renders more pixels: 3D edges, rotated and zoomed backgrounds, and distant or shrunk textures get
smoother. Magnified textures stay blocky, as on the Saturn. Normal and scaled sprites match the 1x image at its
sample points; rotated or distorted sprites (for example in Astal) may differ at their edges.

### Turning it on (RetroArch)

1. Settings > Drivers > Video: `vulkan`. Restart RetroArch.
2. Quick Menu > Core Options > Video > Renderer: `Vulkan`. It applies when content is loaded, so close and load the
   content again.
3. Quick Menu > Core Options > Video > Internal Resolution: `2x`, `3x` or `4x`. It applies at once.

With any other video driver the core uses the software renderer; it never asks RetroArch to switch drivers.

### Options

| Option | Values | Notes |
|---|---|---|
| Renderer (restart required) | Software (default), Vulkan | Applies on content load |
| Internal Resolution (Vulkan) | 1x (Native, default), 2x, 3x, 4x | Live. 3x and 4x of the high-resolution modes are above 1080p |
| VDP1 Upscaling (Vulkan) | Enabled (default), Disabled | Live. Disabled keeps polygons and sprites at native resolution (backgrounds still upscale); use it if a game shows gaps, seams or misplaced lines at 2x-4x |

### GPU memory (the core's device memory, measured)

| Internal resolution | VDP1 upscaling on | VDP1 upscaling off |
|---|---|---|
| 1x | about 140 MiB | about 140 MiB |
| 2x | about 210-230 MiB | about 170-185 MiB |
| 3x | about 320-365 MiB | about 225-270 MiB |
| 4x | about 460-530 MiB | about 295-365 MiB |

A live change of the internal resolution needs about 20-40 MiB more for a moment.

### Fallbacks

- No Vulkan video driver, or a GPU without Vulkan 1.2: the software renderer (on an old GPU through a plain Vulkan
  copy of the frames). The reason is logged and shown once.
- The GPU is short of memory: VDP1 upscaling is turned off at that resolution first, then a lower internal resolution
  is used (logged; the requested one is tried again later), then VDP1 is drawn in software. The picture never stops.
  VDP1 upscaling turned off for memory at the requested resolution stays off until the Internal Resolution or VDP1
  Upscaling option changes, the content is loaded again or the video driver is reinitialised.
  Once VDP1 has moved to software it stays there until the content is loaded again or the video driver is
  reinitialised; the internal resolution itself comes back automatically.
- A lost GPU device: the software renderer continues from the last state the GPU handed back; a few frames of VDP1
  drawing may be redrawn by the game.

### macOS

RetroArch's Vulkan driver runs on Metal through MoltenVK, and the renderer is built for it (compute shaders only,
Vulkan 1.2 baseline). Release builds include it, but it has not been tried on a real Mac yet: reports are welcome.

### Known exceptions and issues

- At 2x-4x with VDP1 upscaling on, 3D polygons are rasterised at the higher resolution, so their edges and texture
  steps differ from the 1x image by design; so do rotated and distorted sprites at their edges. Normal and scaled
  sprites match the 1x image at its sample points.
- A few VDP2 corner cases differ from the software renderer on purpose, where the software renderer is itself wrong
  (for example sprites in exclusive monitor mode); they are listed in
  [src/gpu/shaders/vdp2/README.md](src/gpu/shaders/vdp2/README.md#documented-exceptions).
- Daytona USA CE: the SEGA logo and the restart of the attract mode stutter at 4x (and are slow at 1x too). Gameplay
  is not affected.
- Burning Rangers at 4x: its slowest 1% of frames take 10-11 ms of VDP1 GPU time on the test GPU (a Radeon RX 9060
  XT), close to the 12 ms target, and up to 18 ms at worst; a slower GPU may drop frames there. Use 2x or 3x then.
- Run-ahead renders every hidden frame at the internal resolution too, so it multiplies the GPU cost.
- Run-ahead, netplay and save states: a state load keeps the upscaled VDP1 drawing of every framebuffer whose
  contents the loaded state does not change (run-ahead and netplay load one every frame, so this is the usual case);
  a framebuffer the load changes starts again from its native pixels until the game draws it again.

Developer notes: [src/gpu/shaders/vdp2/README.md](src/gpu/shaders/vdp2/README.md) (the GPU passes) and
[tools/README.md](tools/README.md) (`brimir_vkhost`, the compare and fault-injection tool).

## Build Requirements

- Release targets: Windows x64, Linux x64/ARM64, and macOS ARM64
- C++20 compiler: release CI uses MSVC 2022+, GCC 14, or Apple Clang on macOS 15 (macOS builds target macOS 14 and later)
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

### Vulkan renderer

`-DBRIMIR_VULKAN=ON` (the default on 64-bit Windows x64, Linux x64/arm64 and macOS arm64) builds the Vulkan renderer, its core options and the `brimir_vkhost` tool ([tools/README.md](tools/README.md)). `-DBRIMIR_VULKAN=OFF` builds the core as on `master` (no option, no GPU code). The core never links the Vulkan loader (volk loads it at runtime) and a normal build needs neither the Vulkan SDK nor a shader compiler: the SPIR-V and the reflected buffer layouts (`*.layout.h`) are committed in `src/gpu/shaders/generated/`. After editing a shader in `src/gpu/shaders/`, regenerate them with DXC, `spirv-val` and `spirv-cross` on the path (Vulkan SDK):

```bash
cmake -P tools/shaders/build_shaders.cmake
```

## Testing

Unit and regression tests use Catch2 (amalgamated, in `tests/`) and are registered with CTest:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBRIMIR_BUILD_TESTS=ON
cmake --build build --target brimir_tests
ctest --test-dir build --output-on-failure

# Or run the Catch2 binary directly, optionally filtered by tag:
./build/bin/brimir_tests "[core]"
```

The suite covers core options, the `CoreWrapper` bridge (save states, SRAM persistence, rotation, overscan, audio), the media loader (CUE/CCD, multi-file, MP3/OGG tracks), Ymir hardware-layer backports, VDP color calculation, CD block HLE and the game database. It also covers the SH-2 JIT (tag `[jit]`): differential tests that compare every compiled instruction and random programs against the interpreter, backend-against-backend tests, the block cache and compile worker, and whole-system lockstep of a JIT core against an interpreter core. The executor-level JIT tests run on the backend named by `BRIMIR_JIT_BACKEND` (`ir` or `x64`; default: the build's native backend).

A few integration tests boot a real Saturn BIOS. They run only when a BIOS image is placed in `tests/fixtures/` (e.g. `sega_101.bin`) and are skipped otherwise. BIOS images are copyrighted and ignored by git — never commit them.

Continuous integration (`.github/workflows/ci.yml`) builds the core and runs the test suite on Windows x64, Linux x64 and macOS ARM64 for every push to any branch and every pull request, and runs the JIT tests under ThreadSanitizer on Linux x64. Tagged releases are built by `.github/workflows/release.yml`.

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
   - `mpr-17933.bin` (US/EU v1.00)
   - `Sega Saturn BIOS (EUR).bin` (US/EU)
   - `Sega Saturn BIOS v1.01 (JAP).bin`
   - `Sega Saturn BIOS v1.00 (JAP).bin`
   - `sega_101.bin` (JP v1.01, Mednafen's name)
   - `sega_100.bin` (JP v1.00, Mednafen's name)

   With BIOS Selection on Auto, a disc for Japan/Asia only boots with a Japanese BIOS and any other disc with a US/EU one (files identified by their hash), falling back to any BIOS present; keep one of each region to get both. Region autodetection then runs a multi-region disc in the BIOS's region (Japanese BIOS: Japan, Asia NTSC, North America, Europe; US/EU BIOS: North America, Japan, Europe, Asia NTSC, so a disc runs as PAL only when it lists neither North America nor Japan).
3. Load a Saturn game (.cue, .chd, .ccd, .mds, .iso or .m3u playlist) through RetroArch. Raw `.bin`/`.img`/`.mdf` track files must be opened through their `.cue`/`.ccd`/`.mds` sheet.

## Core Options

| Category | Option | Values |
|----------|--------|--------|
| System | BIOS Selection | Auto, JP v1.01, JP v1.00, JP v1.01 (sega_101.bin), US v1.00, JP v1.00 (sega_100.bin), EU alt |
| System | Auto-Detect Region from Disc | On/Off |
| System | RTC Mode | Virtual (default), Host; applies on content load (see [Save states](#save-states-run-ahead-rewind-and-netplay)) |
| System | Cartridge | Auto (default), None, 1 MB RAM, 4 MB RAM, Backup Memory, The King of Fighters '95 ROM, Ultraman ROM; applies on content load (see [Cartridges](#cartridges)) |
| System | SH-2 Cache Emulation | On/Off (default Off; applies on content load) |
| System | SH-2 CPU Overclock | 100% (Stock), 125%, 150%, 175%, 200%, 250%, 300% |
| System | SH-2 JIT (Experimental) | On/Off (default Off; for testing) |
| System | SH-2 JIT Compile Thread (Experimental) | Auto/Off (default Auto; only used while the JIT is on, on x86-64 with at least 4 CPU threads) |
| System | Performance Profiling | On/Off |
| Video | Renderer (restart required) | Software, Vulkan (64-bit desktop builds; see [Vulkan renderer](#vulkan-renderer)) |
| Video | Internal Resolution (Vulkan) | 1x (Native), 2x, 3x, 4x |
| Video | VDP1 Upscaling (Vulkan) | Enabled/Disabled |
| Video | Deinterlacing | On/Off |
| Video | Deinterlacing Mode | Bob, Weave, Blend, Current, None |
| Video | Threaded VDP1 Rendering | On/Off |
| Video | Threaded VDP2 Rendering | On/Off |
| Video | Screen Rotation (TATE) | None, 90°, 180°, 270° |
| Video | Overscan Crop | None, Small (~16px), Medium (~32px), Large (~48px) |
| Audio | Audio Interpolation | Linear, Nearest |
| Audio | Audio Volume | 0%, 25%, 50%, 75%, 100%, 125%, 150%, 175%, 200% |
| Audio | SCSP Step Granularity | 0 (default) to 5 |
| Media | CD Read Speed | 2x, 4x, 6x, 8x, 12x, 16x, 24x, 32x, Max (200x) |
| Media | Preload Disc to RAM | On/Off |
| Media | CD Block Low-Level Emulation | On/Off (default Off; needs the CD Block ROM; applies on content load; see [Accuracy options](#accuracy-options)) |
| Input | Gun Crosshair | Cross (default), Dot, Off |
| Input | Analog Stick Deadzone | 0% to 30% in 5% steps (default 15%) |
| Input | Trigger Deadzone | 0% to 30% in 5% steps (default 15%) |
| Input | Mouse Sensitivity | 5% to 200% in 5% steps (default 100%) |

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
    gpu/              Vulkan GPU renderer (BRIMIR_VULKAN; shaders in gpu/shaders, regenerated with
                      cmake -P tools/shaders/build_shaders.cmake)
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
- **volk** -- Vulkan function loader for the GPU renderer (MIT; `BRIMIR_VULKAN` builds only)
- **Vulkan-Headers** -- Khronos Vulkan C headers (Apache-2.0 or MIT; `BRIMIR_VULKAN` builds only)
- **libretro_vulkan.h** -- libretro Vulkan interface header from libretro-common, in `include/` (MIT)

Upstream test suites, benchmarks and examples are not vendored.

## License

Licensed under the GNU General Public License v3.0 (GPL-3.0), the same license as Ymir. See [LICENSE](LICENSE).

## Credits

Brimir is built on **[Ymir](https://github.com/StrikerX3/ymir)**, a cycle-accurate Sega Saturn emulator by **StrikerX3**. The hardware layer under `src/core/` is synced from upstream Ymir — all Saturn CPU, VDP, audio, and peripheral emulation is Ymir's work. The SH-2 files are a Brimir-maintained fork of Ymir's SH-2 with hooks for the SH-2 JIT, plus a read-only page-table accessor for the JIT in `ymir/sys/bus.hpp`; see `src/core/BRIMIR_FORK.md`. Apart from these, Brimir uses the hardware layer unmodified and wraps it in a libretro core, adding performance optimizations and frontend integration. Both projects are licensed under GPL.

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for coding standards and the pull request process. Please make sure `ctest` passes and add tests for new behavior.
