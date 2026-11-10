# Upstream Ymir sync triage, 2026-11 (2026-06-23..ff935379)

Every upstream Ymir commit since Brimir's 2026-06-23 sync point that touches `libs/ymir-core` (532 non-merge
commits, `main` from 2026-06-23 to `ff935379`, 2026-11-07), with Brimir's decision for each. Path map: upstream
`libs/ymir-core/{include,src}` = Brimir `src/core/{include,src}`. Decisions were made by comparing each commit's change
with Brimir's current code (not by hash: earlier backports in `CHANGELOG.md` sometimes cite other hashes), plus a
whole-tree comparison of `src/core` with `libs/ymir-core` at `ff935379`: every upstream line Brimir lacks is explained
by a row below. Spec: `docs/superpowers/specs/2026-11-10-upstream-ymir-sync-design.md`; `BRIMIR_FORK.md` section
"2026-11 upstream sync".

- **take**: bring in (own commit `sync(core): <subject> (Ymir <hash>)`, the Brimir commit column is filled when it lands).
- **already**: Brimir has the change (version that took it, or where it is).
- **ours-better**: Brimir has its own, better version; keep it.
- **skip**: not taken (reason given).

## Counts

| Decision | Commits |
|---|---|
| take | 4 |
| already | 93 |
| ours-better | 1 |
| skip | 434 |
| **total** | **532** |

| Batch (takes) | Commits |
|---|---|
| gamedb | 0 |
| cdblock/media | 2 |
| cpu-sys | 1 |
| vdp1 | 0 |
| vdp2 | 0 |
| perf/other | 1 |

Skips: 305 Ymir GPU renderers and graphics backends, 55 host CD drive and the `cd_device`/`cd_interface` rework,
8 fixes to that rework's code (Brimir kept the Disc-model CD block), 5 threaded SCSP (off in Brimir), 12 MP3/OGG
pull-request housekeeping (Brimir has its own MP3/OGG decoding), 49 others (build, documentation, debugger, frontend,
refactors without behaviour value, and fixes that change nothing in Brimir's code).

## What this means for games

Every game fix upstream names in this range is already in Brimir: the game database (`game_db.cpp` is identical to
`ff935379`: Resident Evil fast bus timings, the Marvel Super Heroes vs. Street Fighter hack removal, 3D Baseball slow
VDP1, SH-2 cache for Dragon Force II, Hissatsu! and No-appointment Gals Olympos), the CD block HLE fixes (Virtual On
replays, Gunbird, Amagi Shien and the other Busy-status games, Mass Destruction, Digital Dance Mix), the CUE fixes (Cold
Case, WAV/MP3/OGG tracks), SMPC (Destruction Derby, Fighting Vipers, Fist, Idol Mahjong, Assault Rigs, Guardian Heroes),
SCU (GunBlaze-S), VDP1 (Policenauts, Alone in the Dark, Waialae no Kiseki) and VDP2 (DoDonPachi, Astal, Dark Savior,
Virtua Fighter 2, Virtua Gun games).

The four takes, by likely effect on games:

1. `0a9a0e2` (CD Block LLE only, off by default): resuming a paused CD audio track restarts it from the track's pregap
   in Brimir; upstream names no game. The only take that can change what a game does.
2. `1ff3415`: the CD block mode setting stays in sync after loading a state of the other mode (session behaviour, not
   emulation output).
3. `e2c87ed`, `ff93537`: no output change (debug file lookup; one wasted VRAM/CRAM copy per assignment).

## Takes by batch

- **gamedb**: none.
- **cdblock/media**:
  - `e2c87ed` fix(media): Fix file lookup FAD offset - `Filesystem::LookupFileIndexAtFrameAddress` compares an absolute FAD with file extents stored as LBAs (`extentPos`), 150 frames off; Brimir has the same code. Only feeds the CD drive's debug log and the probes (`GetPathAtFrameAddress`/`GetFileAtFrameAddress`): no emulation change.
  - `0a9a0e2` fix(cdblock-lle): Fix resuming playback of paused audio tracks - CD Block LLE: a drive seek with index 0 means "seek to this FAD", but Brimir's `CDDrive::SetupSeek` (identical to upstream before 6ec6e27) seeks audio tracks to `indices[m_command.index]`, i.e. to the track's pregap start, so resuming a paused audio track restarts it; also `std::min(index, indices.size())` can index one past the end. Port by hand to Brimir's Disc model (only the `cd_drive.cpp` part; the `cdblock.cpp`/`cd_device_image.cpp` BCD parts belong to the rework). LLE only (off by default, validated in 0.7.1 with real ROMs): needs an LLE test with a real CD Block ROM.
- **cpu-sys**:
  - `1ff3415` fix(savestates): Update CD Block LLE setting when loading save states - `Saturn::LoadState` switches the CD block mode with `SetCDBlockLLE(state.cdblockLLE)` but leaves `configuration.cdblock.useLLE` stale (Brimir has the old line); set the observable instead, which calls `SetCDBlockLLE` through its observer. Touches `Saturn::LoadState`, a Brimir-modified function (BRIMIR_FORK "Save-state fidelity" rows: `Saturn::SaveState`/`LoadState`); check `CoreWrapper::ApplyCDBlockLLE` and the LLE state-load tests.
- **vdp1**: none.
- **vdp2**: none.
- **perf/other**:
  - `ff93537` fix(VDP): Assignment operator should return a reference to self, not a copy - `VDP2Memory::operator=` returns a copy, so every assignment (VDP2 render thread sync and reset, `PostLoadStateSync`) copies VRAM and CRAM a second time; return a reference. No output change. `vdp_state.hpp`, not in a Brimir row.

Dependencies, Brimir-modified regions and mirrors:

- `0a9a0e2` builds on upstream's cd_interface rework (6ec6e27: `BeginSeekToFrameAddress`/`BeginSeekToTrackIndex`);
  port only its seek rule to Brimir's `CDDrive::SetupSeek` (Disc model), by hand, with a unit test of the seek target
  (index 0 = the FAD, index n = that index's start, an index past the track's last clamped). `cd_drive.cpp` is not a
  Brimir-modified file. No save-state change (`m_currFAD`/`m_targetFAD` are already saved).
- `1ff3415` touches `Saturn::LoadState`, which Brimir modified (BRIMIR_FORK "Save-state fidelity" rows; keep the
  `// Brimir:` blocks). No save-state format change.
- No take needs a Vulkan (`src/gpu`) or SH-2 JIT mirror: none changes VDP output or SH-2 behaviour.

## Not in this range

Found by the whole-tree comparison, from upstream commits before 2026-06-23 that Brimir's base does not have:
Ymir `a8a17ee` (2026-06-19: the SH-2 clock factor becomes a ratio clamped to 25%..10000%, VDP timings get a minimum of
one cycle, and the VBlank erase cycles are no longer scaled by it). Brimir keeps the older integer `sh2OverclockFactor`,
which no core option exposes, so it is always 100% and none of this changes emulation. Not triaged here.

## Table

Oldest first. Batch shown for takes.

| Upstream hash | Date | Subject | Decision | Reason | Brimir commit |
|---|---|---|---|---|---|
| `c4f50b5` | 2026-06-23 | fix(VDP1): Copy FBRAM to renderer state when loading save states | already | 0.4.4 (2026-06-23 sync); FBRAM is now part of `mem1`, copied by the VDP1 thread's `PostLoadStateSync` case | |
| `2578a83` | 2026-06-23 | fix(gamedb): Force fast bus timings on Resident Evil to fix start menu crashes | already | 0.4.6 (Game DB refresh); `game_db.cpp` identical to ff935379 | |
| `d896fa7` | 2026-06-23 | fix(gamedb): Marvel Super Heroes vs. Street Fighter no longer needs the fast bus timings hack | already | 0.4.6 (Game DB refresh); `game_db.cpp` identical to ff935379 | |
| `5190f9f` | 2026-06-23 | perf(VDP1): Mask/shift color bank value per quad instead of per pixel | already | 0.4.4 (2026-06-23 sync) | |
| `ccdc4d6` | 2026-06-23 | perf(VDP1): Get `VDP1Regs` reference and double density flag once per command instead of per pixel | already | 0.4.4 (2026-06-23 sync) | |
| `090aea2` | 2026-06-23 | perf(SH2): Microoptimizations to interrupt recalculations | already | 0.4.6 (SH2 interrupt flag micro-optimisations); `sh2.cpp` matches | |
| `ae49e07` | 2026-06-23 | fix(input): Make 3D Control Pad peripheral report in digital mode match the Standard Pad | already | 0.4.5 (3D Control Pad digital report) | |
| `fa7d07e` | 2026-06-23 | fix(input): Set least significant bits of 3D Control Pad analog reports to all ones | already | 0.4.5 (3D Control Pad analog LSBs) | |
| `d2cff8d` | 2026-06-23 | feat(media): Parse all known peripheral type codesa | skip | feature: parse all header peripheral codes; Brimir never reads `compatPeripherals` | |
| `baa9e02` | 2026-06-24 | chore(SH2): Use `std::bit_cast` for interrupt checks | already | 0.4.6; `sh2.cpp` matches | |
| `143df9f` | 2026-06-24 | fix(VDP2): Fix coordinate latching via external latch | already | 0.4.5 (external latch, Virtua Gun) | |
| `ead04d2` | 2026-06-24 | fix(SMPC): Update peripheral PDR1/2 registers when reading and when updating EXLE | already | 0.4.5 (SMPC PDR1/2 on read and EXLE) | |
| `4490ead` | 2026-06-24 | perf(SH2): More interrupt flags microoptimizations | already | 0.4.6; `sh2.cpp` matches | |
| `73661e0` | 2026-06-24 | fix(debug): Don't sync VDP1 FBRAM on debug reads | already | 0.4.5 (no FBRAM sync on debug reads) | |
| `ae4017f` | 2026-06-24 | fix(VDP1): Textured sprites with CMDSIZE.H=0 never fetch additional texels | already | 0.4.5 (CMDSIZE.H=0 texels, Policenauts) | |
| `744ca29` | 2026-06-25 | feat(SMPC): Persist SMPC data per region based on IPL region | already | 0.4.6 (SMPC per-region persistent storage) | |
| `e975f69` | 2026-06-26 | chore(VDP2): Reference sprite layer attributes field once | already | 0.4.6 (sprite layer attributes once) | |
| `b4beb6a` | 2026-06-26 | fix(VDP2): Restrict color calculations in certain video modes | already | 0.4.5 (colour calculation restricted in hi-res/exclusive modes, Dark Savior) | |
| `f042cb5` | 2026-06-26 | fix(gamedb): Slow down VDP1 execution speed in 3D Baseball to fix team name plates and announcer voice line glitches | already | 0.4.6 (Game DB refresh, 3D Baseball); `game_db.cpp` identical to ff935379 | |
| `3fcd827` | 2026-06-26 | fix(cdblock-hle): Read reset position flag correctly from parameters | already | 0.4.5 (CD block HLE reset position flag) | |
| `456389f` | 2026-06-26 | fix(media-cue): Don't accumulate pre/postgaps multiple times per track | already | 0.4.6 (CUE pre/postgaps) | |
| `d8fac07` | 2026-06-26 | fix(media-cue): Use CUE sheet timestamps to compute track lengths | already | 0.4.6 (CUE track lengths from timestamps) | |
| `e3f2b8d` | 2026-06-26 | fix(VDP2): Fix off-screen coordinate latching | already | 0.4.5 (off-screen coordinate latching) | |
| `1d6ece6` | 2026-06-26 | fix(input): Introduce a small amount of jitter to the Virtua Gun aim in Death Crimson | ours-better | taken in 0.4.6 (Virtua Gun jitter, Death Crimson), then Brimir made the jitter deterministic and saved in states (BRIMIR_FORK 2026-11-06 "Determinism, Virtua Gun jitter"): keep Brimir's xorshift, not upstream's `rand()` | |
| `5ebe684` | 2026-06-26 | chore: Clean up unused includes | skip | include cleanup | |
| `1d6505b` | 2026-06-27 | build: Graduate Virtua Gun to stable feature | already | 0.5.0 (Virtua Gun stable) | |
| `da71596` | 2026-06-30 | feat(media): Basic machinery for reading discs from physical CD drives | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `db3e9ec` | 2026-06-30 | fix(media): Rename CD device file for Apple systems to match CMakeLists | skip | host CD drive (out of scope) | |
| `5683fc0` | 2026-06-30 | fix(app): Cleanup callback on shutdown to avoid UB | already | 0.5.0 (`SMPC::ClearPersistDataCallback`) | |
| `a46bb90` | 2026-06-30 | feat(media): Read and expose TOCs from physical CDs | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `a356aa1` | 2026-06-30 | feat(media): Preload TOC | skip | host CD drive (out of scope) | |
| `660515f` | 2026-06-30 | feat(media): Check for SCSI CD Read command support on Linux | skip | host CD drive (out of scope) | |
| `f512760` | 2026-06-30 | feat(media): Return (not yet parsed) TOC | skip | host CD drive (out of scope) | |
| `36f7b77` | 2026-06-30 | chore(media): Return early on failure rather than nest success cases | skip | host CD drive (out of scope) | |
| `797c106` | 2026-06-30 | chore(media): Move TOC table documentation to `TOCEntry` | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `6aca55e` | 2026-06-30 | feat(media-linux): Parse TOC entries and build table | skip | host CD drive (out of scope) | |
| `9624f0a` | 2026-06-30 | feat(media-linux): Use SCSI generic operations for everything; now reading sectors | skip | host CD drive (out of scope) | |
| `4f5ee58` | 2026-06-30 | chore(media): Isolate current "Unix-like" CD drive implementation to Linux | skip | host CD drive (out of scope) | |
| `a52897b` | 2026-06-30 | build(media): Fix source file path to dummy CD device implementation | skip | Ymir build-only | |
| `f516856` | 2026-06-30 | build: Use variable for OS-specific source files lists | skip | Ymir build-only | |
| `e500a7b` | 2026-07-01 | fix(media): Don't the Path Table past the size specified in the Volume Descriptor | already | 0.4.6 (ISO path table size guard) | |
| `cc7c8f4` | 2026-07-01 | feat(media): Add dummy CD device implementation | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `d3b7f5d` | 2026-07-04 | feat(media): Move common SCSI definitions to the core library | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `ced8e90` | 2026-07-04 | chore(media): Move more common SCSI definitions to the core library | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `cca2cb6` | 2026-07-04 | feat(debug): Added RBG0 and RBG1 line color single stack views to the VDP2 debug overlay | skip | VDP2 debug overlay views | |
| `540ed11` | 2026-07-04 | feat(sandbox-linux): Observe device events | skip | host CD drive (out of scope) | |
| `215eca9` | 2026-07-05 | perf(VDP1): Use less strict memory ordering for atomic operations | already | 0.5.0 (VDP1 thread memory ordering) | |
| `59676e5` | 2026-07-05 | chore(system): Minor comments updates | skip | comments | |
| `4cd248e` | 2026-07-07 | chore: Remove all CD device stuff to make room for a rework | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `a918157` | 2026-07-07 | feat(media): Add several useful SCSI command definitions | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `426023b` | 2026-07-08 | docs: Update CHANGELOG with M PLUS U font size reduction change | skip | Ymir frontend font table | |
| `b19afae` | 2026-07-09 | fix(VDP2): Avoid unintentional side effects on VDP2 EXTEN register when saving states | already | 0.5.0 (EXTEN peek in save states) | |
| `8c3241b` | 2026-07-07 | feat(cdif): Basic CD interface implementation | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `c01c0ea` | 2026-07-07 | chore: Cleanup unused headers | skip | include cleanup | |
| `6ec6e27` | 2026-07-08 | feat(cdblock): Use CD interface to read discs | already | 0.5.3 selectively adapted (one-based track numbers after FAD seeks, playback position reset, relative sub-Q from INDEX 01); the cd_interface refactor itself is not taken (host-drive plumbing) | |
| `8d09201` | 2026-07-11 | fix(cdblock-hle): Detect end of track correctly when requested the last track and/or index | skip | fixes code from upstream's cd_interface/cd_device rework (6ec6e27 and later), which Brimir did not take; Brimir's Disc-model CD block has no such code path (track/index end-of-play check) | |
| `86903e3` | 2026-07-11 | fix(cdblock-hle): Fix buffer tracing when resetting all partitions | skip | CD block partition tracer (debugger), not in Brimir | |
| `e280cbb` | 2026-07-11 | fix(cdif): Fix crash caused by not converting BCD values for first and last track numbers | skip | fixes code from upstream's cd_interface/cd_device rework (6ec6e27 and later), which Brimir did not take; Brimir's Disc-model CD block has no such code path (`TOC::LoadFrom` in `cd_defs.cpp`) | |
| `b8110b8` | 2026-07-12 | chore: Fix numerous cases of missing `#include`s | already | 0.5.4 (selected `<cassert>` hunk); the other hunks are in files Brimir does not have (`cd_defs.hpp`, `cd_utils.hpp`, `media_defs.cpp`) | |
| `01a2390` | 2026-07-12 | fix(utils): Avoid `is_bitmask_enum_v` false positives on `friend` operator functions | already | 0.5.4 (bitmask enum constraint) | |
| `efa658b` | 2026-07-14 | chore(build): Speed up MSVC builds by disabling inlining for some heavy functions | already | 0.5.0 (MSVC `FORCE_INLINE_EX` build hints) | |
| `aa2bd52` | 2026-07-09 | feat(cdif): Stub host CD interface | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `83d9556` | 2026-07-10 | feat(cdif-host): Basic host CD drive enumeration on Windows | skip | host CD drive (out of scope) | |
| `b1c2833` | 2026-07-10 | fix(cdif-host): Fix device enumeration and state retrieval | skip | host CD drive (out of scope) | |
| `487d487` | 2026-07-10 | feat(cdif-host): Stub "open host device" operation | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `a816bf7` | 2026-07-10 | feat(cdif-win): Open/close device | skip | host CD drive (out of scope) | |
| `212d3d9` | 2026-07-10 | chore(cdif-win): More prep work | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `5c7ea84` | 2026-07-10 | chore(cdif): Rename `ICDInterface` and implementations to `ICDDevice` to avoid confusion with `CDInterface` | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `56b6343` | 2026-07-10 | chore(cdif): Move TOC and disc header handling down to the device implementations | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `412f693` | 2026-07-10 | chore(cdif-win): `IOCTL_STORAGE_CHECK_VERIFY2` is faster and cheaper | skip | host CD drive (out of scope) | |
| `bdf807c` | 2026-07-10 | feat(cdif): Invoke callback on media presence changes, read filesystem from `CDInterface` | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `5a107d2` | 2026-07-11 | feat(cdif-host): Threading infrastructure | skip | host CD drive (out of scope) | |
| `a3cfe80` | 2026-07-11 | feat(cdif-host): Poll and update drive state | skip | host CD drive (out of scope) | |
| `5f9f41e` | 2026-07-11 | fix(cdblock-hle): Fix file system building when there is no disc in the drive | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `cc0051a` | 2026-07-11 | feat(cdif): Move CD filesystem parsing to CD device implementations | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `4a57008` | 2026-07-11 | fix(cdif): Fix devlog groups | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `825f19f` | 2026-07-11 | feat(cdif-host): Read TOC, initial seek implementation | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `6d1bb78` | 2026-07-11 | fix(cdif-host): Fix header sector read | skip | host CD drive (out of scope) | |
| `ee9911f` | 2026-07-11 | chore(media): Introduce abstraction for filesystem reading | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `3906493` | 2026-07-11 | fix(cdif-host): Clamp minimum seek frame address result to 150 | skip | host CD drive (out of scope) | |
| `ac6edc1` | 2026-07-11 | feat(cdif-host): [WIP] Read and cache sectors | skip | host CD drive (out of scope) | |
| `e99cf95` | 2026-07-12 | feat(cdif-host): LRU cache for sector data | skip | host CD drive (out of scope) | |
| `a626765` | 2026-07-12 | feat(cdif-host): Read arbitrary sectors | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `ab72c0a` | 2026-07-12 | feat(cdif-host): Stop reading/caching sectors at the end of the track | skip | host CD drive (out of scope) | |
| `6e4d9fe` | 2026-07-12 | fix(cdif-host): Fix race conditions involving uncached sector reads from clients | skip | host CD drive (out of scope) | |
| `2e6a5cb` | 2026-07-12 | chore(cdif): Add missing `#include`s | skip | host CD drive (out of scope) | |
| `53cd12f` | 2026-07-12 | fix(cdif): Audio tracks must be in little-endian, matching CDDA spec | skip | audio endianness convention of upstream's cd_device layer (and the matching SCSP read); Brimir's CD block swaps big-endian tracks itself (`cd_drive.cpp` `OpReadSector`) | |
| `3b672d2` | 2026-07-12 | fix(media): Fix BCD track:index handling | skip | fixes code from upstream's cd_interface/cd_device rework (6ec6e27 and later), which Brimir did not take; Brimir's Disc-model CD block has no such code path (BCD in `DiscPosition`) | |
| `3f3d737` | 2026-07-12 | feat(sys): Add function to open host CD drives in Saturn instances | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `e2c87ed` | 2026-07-12 | fix(media): Fix file lookup FAD offset | take (cdblock/media) | `Filesystem::LookupFileIndexAtFrameAddress` compares an absolute FAD with file extents stored as LBAs (`extentPos`), 150 frames off; Brimir has the same code. Only feeds the CD drive's debug log and the probes (`GetPathAtFrameAddress`/`GetFileAtFrameAddress`): no emulation change. | |
| `389ff41` | 2026-07-12 | feat(cdif): Periodically poll drive state while emulating CD block | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `ccd8a5b` | 2026-07-12 | fix(cdif-host): Wait for device to be ready | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `9abb1e3` | 2026-07-12 | feat(cdif-host): Properly cleanup on shutdown/timeout | skip | host CD drive (out of scope) | |
| `64dd941` | 2026-07-12 | feat(app): Basic CD drive listing in File menu | skip | Ymir frontend (`apps/`) | |
| `b34f54e` | 2026-07-12 | feat(cdif-host): Determine preferred device path based on operating system | skip | host CD drive (out of scope) | |
| `45f96f5` | 2026-07-12 | feat(cdif-host): Improve cache usage | skip | host CD drive (out of scope) | |
| `1cc5201` | 2026-07-12 | feat(cdif-host): Add a hint to stop the prefetcher when the Saturn stops the CD drive | skip | host CD drive / cd_device rework (out of scope; RetroArch supplies the disc) | |
| `5217f69` | 2026-07-12 | fix(system): Fix region autodetection when loading disc images | skip | fixes call order in upstream's cd_interface `LoadDisc`; Brimir's `Saturn::AutodetectRegion(areaCodes)` gets the area codes from the disc directly (and prefers the BIOS region, 0.7.1) | |
| `f5f1da2` | 2026-07-12 | chore(cdblock-lle): More logs | skip | logs only | |
| `3d1f81c` | 2026-07-12 | fix(cdblock-lle): Fix BCD handling | skip | fixes code from upstream's cd_interface/cd_device rework (6ec6e27 and later), which Brimir did not take; Brimir's Disc-model CD block has no such code path (LLE BCD via `ReadPosition`) | |
| `23ecde1` | 2026-07-12 | fix(cdif-host): Don't constraint seek to FAD upper bound | skip | host CD drive (out of scope) | |
| `94294e8` | 2026-07-13 | chore(utils): Move `TrimWhitespace` function to string utilities header | skip | refactor (`TrimWhitespace` moved to `util/string`, not in Brimir) | |
| `256ab2c` | 2026-07-13 | chore(media): Implement cleanup methods for ISO 9660 objects | skip | chore: `Clear()` helpers for ISO 9660 structs used by the cd_device rework; Brimir's parser fills what it reads | |
| `05d74f3` | 2026-07-13 | feat(media): Allow building file systems for any valid ISO 9660 disc, not just Saturn discs | skip | feature: file systems for non-Saturn ISO 9660 discs, built on the cd_device rework | |
| `ae4debc` | 2026-07-14 | fix(cdblock-hle): Fix resuming playback when playing audio tracks | skip | fixes code from upstream's cd_interface/cd_device rework (6ec6e27 and later), which Brimir did not take; Brimir's Disc-model CD block has no such code path (`BeginSeekToTrackIndex`/`ReadSector` plumbing and BCD); Brimir's HLE resumes from `m_status.frameAddress` | |
| `0a9a0e2` | 2026-07-14 | fix(cdblock-lle): Fix resuming playback of paused audio tracks | take (cdblock/media) | CD Block LLE: a drive seek with index 0 means "seek to this FAD", but Brimir's `CDDrive::SetupSeek` (identical to upstream before 6ec6e27) seeks audio tracks to `indices[m_command.index]`, i.e. to the track's pregap start, so resuming a paused audio track restarts it; also `std::min(index, indices.size())` can index one past the end. Port by hand to Brimir's Disc model (only the `cd_drive.cpp` part; the `cdblock.cpp`/`cd_device_image.cpp` BCD parts belong to the rework). LLE only (off by default, validated in 0.7.1 with real ROMs): needs an LLE test with a real CD Block ROM. | |
| `1ff3415` | 2026-07-14 | fix(savestates): Update CD Block LLE setting when loading save states | take (cpu-sys) | `Saturn::LoadState` switches the CD block mode with `SetCDBlockLLE(state.cdblockLLE)` but leaves `configuration.cdblock.useLLE` stale (Brimir has the old line); set the observable instead, which calls `SetCDBlockLLE` through its observer. Touches `Saturn::LoadState`, a Brimir-modified function (BRIMIR_FORK "Save-state fidelity" rows: `Saturn::SaveState`/`LoadState`); check `CoreWrapper::ApplyCDBlockLLE` and the LLE state-load tests. | |
| `f67f154` | 2026-07-14 | fix(cdblock): More BCD shenanigans | skip | fixes code from upstream's cd_interface/cd_device rework (6ec6e27 and later), which Brimir did not take; Brimir's Disc-model CD block has no such code path (BCD in `DiscPosition`, host seek) | |
| `734a00b` | 2026-07-14 | fix(cdif-host): Add missing function implementation for dummy host CD interface | skip | host CD drive (out of scope) | |
| `2c898ba` | 2026-07-14 | fix(cdif-linux): Implement Linux CD device interface | skip | host CD drive (out of scope) | |
| `4d71f77` | 2026-07-14 | feat(cdif-macos): Stub macOS CD device implementation | skip | host CD drive (out of scope) | |
| `244d5c8` | 2026-07-15 | feat(cdif-host): Hide host CD interfaces behind a feature flag (disabled by default) | skip | host CD drive (out of scope) | |
| `87eaf36` | 2026-07-19 | core(utils): Minor code cleanup | skip | `util/ratio.hpp` is not in Brimir | |
| `1d7a2e4` | 2026-07-20 | fix(VDP1): Ignore 8-bit setting and force-align to 16-bit addresses when writing pixels with MSB enabled | already | 0.5.0 (MSB writes force-aligned to 16 bits) | |
| `b1d4255` | 2026-07-20 | fix(utils): Fix documentation for `FLATTEN_EX` macro | skip | documentation | |
| `c706ce7` | 2026-07-20 | chore: Fix small typo in SCU DMA docs | skip | typo in a comment | |
| `eee2672` | 2026-07-25 | build: added dr_mp3.h and stb_vorbis.c as dependencies and updated include paths to include vendor directory for single file dependencies | already | 0.5.1 (dr_mp3/stb_vorbis vendored) | |
| `8e05e00` | 2026-07-26 | feat: added conversion from mp3 and ogg to raw binary format. | already | 0.5.1 (Brimir's own MP3/OGG to PCM conversion) | |
| `d490389` | 2026-07-26 | chore: reformatting the code with the clang-formatter | skip | MP3/OGG contributor PR housekeeping, superseded within the PR; Brimir has its own MP3/OGG decoding (0.5.1, 0.5.3) | |
| `5795baa` | 2026-07-26 | chore: finishing touches before opening pull request | skip | MP3/OGG contributor PR housekeeping, superseded within the PR; Brimir has its own MP3/OGG decoding (0.5.1, 0.5.3) | |
| `a32aa0d` | 2026-07-27 | fix(cdblock-hle): Fix audio tracks not repeating when reaching the end | skip | fixes code from upstream's cd_interface/cd_device rework (6ec6e27 and later), which Brimir did not take; Brimir's Disc-model CD block has no such code path (the track/index end-of-play check); Brimir ends playback by FAD (`ProcessDriveStatePlay`) and repeats | |
| `29825a3` | 2026-07-27 | fix: fixed comment formatting, reverted changes in the cmake lists due to improper dependency linking and removed the header files as they are in the wrong directory and are already included upstream | skip | MP3/OGG contributor PR housekeeping, superseded within the PR; Brimir has its own MP3/OGG decoding (0.5.1, 0.5.3) | |
| `f38ba99` | 2026-07-27 | updated the include statements | skip | MP3/OGG contributor PR housekeeping, superseded within the PR; Brimir has its own MP3/OGG decoding (0.5.1, 0.5.3) | |
| `60a82ec` | 2026-07-27 | fix: fixed include path | skip | MP3/OGG contributor PR housekeeping, superseded within the PR; Brimir has its own MP3/OGG decoding (0.5.1, 0.5.3) | |
| `2331879` | 2026-07-28 | chore(cdblock-hle): Fix outdated documentation | skip | documentation | |
| `c27fe18` | 2026-07-28 | fix(cdblock-hle): Report current CD status and raise CMOK HIRQ signal when processing unimplemented commands | already | 0.5.0 (unimplemented commands report status and CMOK) | |
| `b8022ce` | 2026-07-28 | fix(SCU): Allow 8-bit writes to DSP registers | already | 0.5.0 (SCU 8-bit DSP register writes) | |
| `c91a348` | 2026-07-28 | dependency: added speexdsp as a git submodule for audio resampling | skip | MP3/OGG contributor PR housekeeping, superseded within the PR; Brimir has its own MP3/OGG decoding (0.5.1, 0.5.3) | |
| `f6986ce` | 2026-07-28 | feat: resampling raw pcm data using speexdsp | skip | MP3/OGG contributor PR housekeeping, superseded within the PR; Brimir has its own MP3/OGG decoding (0.5.1, 0.5.3) | |
| `bf0b4f4` | 2026-07-28 | dependency: added clown resampler as a dependency | skip | MP3/OGG contributor PR housekeeping, superseded within the PR; Brimir has its own MP3/OGG decoding (0.5.1, 0.5.3) | |
| `462306f` | 2026-07-29 | fix(cdblock-lle): Fix audio track playback failing for tracks 16 and higher | skip | fixes code from upstream's cd_interface/cd_device rework (6ec6e27 and later), which Brimir did not take; Brimir's Disc-model CD block has no such code path (BCD track number in `BeginSeekToTrackIndex`, introduced by 3d1f81c; Virtual On); Brimir's LLE finds the track by FAD | |
| `9c30993` | 2026-07-29 | dependency: removed clownresampler and added miniaudio instead | skip | MP3/OGG contributor PR housekeeping, superseded within the PR; Brimir has its own MP3/OGG decoding (0.5.1, 0.5.3) | |
| `04e6431` | 2026-07-29 | feat: implemented resampling using the miniaudio header | skip | MP3/OGG contributor PR housekeeping, superseded within the PR; Brimir has its own MP3/OGG decoding (0.5.1, 0.5.3) | |
| `5c25865` | 2026-07-29 | chore: removing the speexdsp code and implementation | skip | Ymir build-only | |
| `c620dda` | 2026-07-30 | fix(VDP1): Fix MSB shadow offsets in 16-bit mode | already | 0.5.0 (MSB shadow offsets in 16-bit mode) | |
| `5c4ebe9` | 2026-08-01 | removed mini audio as a dependency everywhere | skip | MP3/OGG contributor PR housekeeping, superseded within the PR; Brimir has its own MP3/OGG decoding (0.5.1, 0.5.3) | |
| `3f3cb5b` | 2026-08-01 | feat: implemented linear interpolation resampling by hand | already | 0.5.1 (Brimir's own linear-interpolation resampling to 44.1 kHz) | |
| `2774403` | 2026-08-02 | fix: changed the path.c_str() calls into path.string().c_str() to account for wchar_t's in windows | already | Brimir's loader already passes `path.string().c_str()` to dr_mp3/stb_vorbis | |
| `278faf9` | 2026-08-02 | chore: ran clang-format | skip | MP3/OGG contributor PR housekeeping, superseded within the PR; Brimir has its own MP3/OGG decoding (0.5.1, 0.5.3) | |
| `2434a0c` | 2026-08-03 | fix(media-cue): Properly compute track count on discs with sparse track numbers | already | 0.5.1 (sparse track numbers) | |
| `899db8d` | 2026-08-03 | fix(media-cue): Pad MP3, OGG and WAV audio tracks to 2352 bytes if needed | already | 0.5.1 (audio track padding to 2352 bytes: `loader_bin_cue.cpp` decoded data and WAVE zero reader) | |
| `6ef1ad7` | 2026-08-03 | perf(media-cue): Reorder operations to optimize MP3/OGG loading performance | already | 0.5.3 (selectively adapted: mono MP3/OGG resampled before stereo expansion) | |
| `6ec425d` | 2026-08-08 | feat(gpu): Start second attempt at GPU backend rework | skip | Ymir graphics backend (out of scope) | |
| `8626d81` | 2026-08-09 | feat(gfx): Implement textures for SDL Renderer backend | skip | Ymir graphics backend (out of scope) | |
| `bd9beb2` | 2026-08-09 | feat(gfx): Define graphics backend macros via build system | skip | Ymir graphics backend (out of scope) | |
| `1812a52` | 2026-08-10 | feat(gfx-d3d12): Add D3D12 wrappers and libraries, move spec to a separate file | skip | Ymir graphics backend (out of scope) | |
| `d5f9260` | 2026-08-10 | fix(build): .cpp files should not be PUBLIC | skip | Ymir build-only | |
| `4a68867` | 2026-08-10 | chore(utils): Move convenience Result types to result.hpp | skip | refactor (`util/result.hpp`, used by Ymir's renderer factories) | |
| `aa769c2` | 2026-08-10 | build(vulkan): Find and include Vulkan | skip | Ymir graphics backend (out of scope) | |
| `63143cb` | 2026-08-10 | feat(gfx): Add shader compiler skeleton and Windows implementation with DXC | skip | Ymir graphics backend (out of scope) | |
| `89c9897` | 2026-08-10 | feat(gfx-d3d12): Initialize graphics context | skip | Ymir graphics backend (out of scope) | |
| `24da4d9` | 2026-08-10 | feat(gfx-d3d12): [WIP] Partial D3D12 graphics context implementation | skip | Ymir graphics backend (out of scope) | |
| `301ec4a` | 2026-08-10 | fix(gfx-d3d12): Fix frame pacing and more init/shutdown issues | skip | Ymir graphics backend (out of scope) | |
| `64abd63` | 2026-08-12 | feat(gfx-d3d12): HLSL shaders, add resources for texture drawing | skip | Ymir graphics backend (out of scope) | |
| `59c2c67` | 2026-08-12 | feat(gfx-d3d12): Allocate RTVs for render target textures, partially implement texture drawing | skip | Ymir graphics backend (out of scope) | |
| `6301e6e` | 2026-08-12 | feat(gfx-d3d12): Somewhat working texture drawing | skip | Ymir graphics backend (out of scope) | |
| `e8a6cae` | 2026-08-12 | chore(gfx-d3d12): Use root constants instead of CBV for drawing parameters | skip | Ymir graphics backend (out of scope) | |
| `d8d993f` | 2026-08-12 | chore(gfx): Move GUI shaders to the frontend project | skip | Ymir graphics backend (out of scope) | |
| `d30148f` | 2026-08-12 | feat(gfx): Configure shader compiler flags | skip | Ymir graphics backend (out of scope) | |
| `72d85de` | 2026-08-13 | chore(gfx): Rework CMake shader compiler to hopefully support multiple platforms | skip | Ymir graphics backend (out of scope) | |
| `83ebfd6` | 2026-08-13 | fix(gfx): Another case of reordering file name prefixes/suffixes | skip | Ymir graphics backend (out of scope) | |
| `9ae1e44` | 2026-08-14 | feat(gfx): Allow selecting a graphics adapter | skip | Ymir graphics backend (out of scope) | |
| `56f76c2` | 2026-08-15 | chore(vdp): Set up structure for GPU-based VDP renderers and stub D3D12 | already | 0.6.0 GPU stage 0 (`HardwareVDPRendererBase`, `AsHardwareRenderer`); the D3D12 stub is out of scope | |
| `eafe139` | 2026-08-15 | chore(vdp): Move common hardware renderer configuration to the Configuration struct | skip | configuration reorganisation for Ymir's GPU renderers | |
| `bfcd7e2` | 2026-08-15 | chore(config): Reorganize configuration to separate software and hardware renderer settings | skip | configuration reorganisation for Ymir's GPU renderers | |
| `4ee0e4e` | 2026-08-15 | feat(settings): Add and wire up hardware acceleration option | skip | hardware-acceleration setting for Ymir's GPU renderers | |
| `50fa134` | 2026-08-15 | chore(hwrenderer): Improve handling of sw/hw renderer and graphics context switches | skip | Ymir GPU renderer (out of scope) | |
| `eab0c6b` | 2026-08-15 | chore(vdp): Pass down read-only reference to VDP2 debugger options | skip | refactor (const debugger options reference) | |
| `61bfd5b` | 2026-08-15 | fix(vdp): Fix crash in debug mode due to using the wrong renderer pointer | already | 0.5.2 (renderer switch devlog crash); Brimir now installs renderers through `VDP::InstallRenderer` | |
| `c819b3c` | 2026-08-15 | feat(vdp-d3d12): Initialize the first few resources to test the renderer structure | skip | Ymir GPU renderer (out of scope) | |
| `94f24ad` | 2026-08-15 | feat(vdp-d3d12): Create more resources, improve error handling | skip | Ymir GPU renderer (out of scope) | |
| `cd19853` | 2026-08-15 | fix(VDP2): Disable color gradation if color RAM mode is not 0 | already | 0.5.2 (no colour gradation with CRAM mode != 0, Astal fog) | |
| `0530926` | 2026-08-16 | feat(vdp-d3d12): Set up more VDP2 resources | skip | Ymir GPU renderer (out of scope) | |
| `c044482` | 2026-08-16 | feat(vdp-d3d12): Compile and load the first renderer compute shader | skip | Ymir GPU renderer (out of scope) | |
| `0877b68` | 2026-08-16 | feat(vdp-d3d12): Organize shaders; more resources | skip | Ymir GPU renderer (out of scope) | |
| `0508463` | 2026-08-16 | chore(vdp-d3d12): Extract upload buffer functionality into a struct | skip | Ymir GPU renderer (out of scope) | |
| `dcd0f4b` | 2026-08-16 | feat(vdp-d3d12): Allocate overflow upload buffers if needed | skip | Ymir GPU renderer (out of scope) | |
| `2555661` | 2026-08-16 | chore(vdp-d3d12): It's not a ring buffer | skip | Ymir GPU renderer (out of scope) | |
| `4903bed` | 2026-08-16 | chore(vdp-d3d12): Move VDP2 rendering code to implementation object | skip | Ymir GPU renderer (out of scope) | |
| `b7012c5` | 2026-08-17 | feat(media-cue): Add more sanity checks | already | 0.5.2 (CUE sanity checks) | |
| `76559e9` | 2026-08-17 | feat(vdp-d3d12): Basic non-working VDP2 VRAM uploads | skip | Ymir GPU renderer (out of scope) | |
| `61080d0` | 2026-08-17 | feat(vdp-d3d12): Actually upload VDP2 VRAM chunks | skip | Ymir GPU renderer (out of scope) | |
| `bc02d59` | 2026-08-17 | feat(vdp-d3d12): Add multiple command allocators to improve performance | skip | Ymir GPU renderer (out of scope) | |
| `3c3880f` | 2026-08-17 | feat(vdp-d3d12): Per-frame VDP2 upload buffers to maximize performance | skip | Ymir GPU renderer (out of scope) | |
| `867a23e` | 2026-08-17 | feat(vdp-d3d12): Add per-frame VDP1 resources | skip | Ymir GPU renderer (out of scope) | |
| `1469f52` | 2026-08-17 | feat(vdp-d3d12): Begin structuring shader parameters | skip | Ymir GPU renderer (out of scope) | |
| `284f7e6` | 2026-08-17 | chore(utils): Silence string conversion deprecation warnings on Windows | skip | `util/string.cpp` is not in Brimir | |
| `26ef632` | 2026-08-18 | fix(utils): Use `iconv` to convert strings<->wstrings on non-Windows platforms | skip | `util/string.cpp` (Ymir frontend string conversion) is not in Brimir | |
| `cf96e9f` | 2026-08-18 | fix(build): Find and use `Iconv` on macOS | skip | Ymir build-only | |
| `7627bfd` | 2026-08-18 | fix(build): Attempt to fix libiconv issues in FreeBSD | skip | Ymir build-only | |
| `6a58268` | 2026-08-18 | feat(build): Add Metal shader compilation on macOS | skip | Ymir build-only | |
| `5958b40` | 2026-08-18 | feat(vdp-d3d12): Upload VDP2 CRAM when dirty | skip | Ymir GPU renderer (out of scope) | |
| `96c3ceb` | 2026-08-18 | feat(vdp-d3d12): Also upload CRAM rotation coefficients table view | skip | Ymir GPU renderer (out of scope) | |
| `fa6c77d` | 2026-08-19 | fix(build): Add LIBICONV_PLUG definition for FreeBSD | skip | Ymir build-only | |
| `4d75a8f` | 2026-08-19 | feat(vdp-d3d12): Begin implementing render loop and update some render parameters | skip | Ymir GPU renderer (out of scope) | |
| `b2bd828` | 2026-08-19 | feat(vdp-d3d12): Track register writes | skip | Ymir GPU renderer (out of scope) | |
| `f58456c` | 2026-08-20 | feat(vdp-d3d12): Implement VDP2 frame rendering skeleton | skip | Ymir GPU renderer (out of scope) | |
| `cbc2bc1` | 2026-08-20 | feat(vdp-d3d12): Partial upload buffer refactor | skip | Ymir GPU renderer (out of scope) | |
| `d12cec2` | 2026-08-21 | chore(vdp-d3d12): Housekeeping | skip | Ymir GPU renderer (out of scope) | |
| `8616656` | 2026-08-22 | fix(vdp-d3d12): Fix devlog groups colliding with VDP definitions | skip | Ymir GPU renderer (out of scope) | |
| `2415d5d` | 2026-08-22 | chore(vdp-d3d12): Make fence value sequencing more visible | skip | Ymir GPU renderer (out of scope) | |
| `fd68203` | 2026-08-22 | feat(vdp-d3d12): Use a single upload ring buffer to allocate transfers efficiently | skip | Ymir GPU renderer (out of scope) | |
| `918437c` | 2026-08-22 | fix(vdp-d3d12): Signal end of frame to upload buffer | skip | Ymir GPU renderer (out of scope) | |
| `13b994c` | 2026-08-22 | fix(vdp-d3d12): Incorrect use of `YMIR_DEV_ASSERT` | skip | Ymir GPU renderer (out of scope) | |
| `e1233fd` | 2026-08-22 | feat(vdp-d3d12): Optimize waits for upload buffer space | skip | Ymir GPU renderer (out of scope) | |
| `22e6215` | 2026-08-22 | chore(vdp-d3d12): This cleanup is not needed | skip | Ymir GPU renderer (out of scope) | |
| `3b3d09f` | 2026-08-22 | chore(vdp-d3d12): Cleanup stale TODOs | skip | Ymir GPU renderer (out of scope) | |
| `802b91c` | 2026-08-23 | chore(vdp-d3d12): Add barrier helper to reduce boilerplate | skip | Ymir GPU renderer (out of scope) | |
| `5df774a` | 2026-08-23 | chore(vdp-d3d12): Cleanup, handle edge cases | skip | Ymir GPU renderer (out of scope) | |
| `8625199` | 2026-08-23 | feat(vdp-d3d12): Add LNCL/BACK screen texture and rotparam bases buffer | skip | Ymir GPU renderer (out of scope) | |
| `eec87aa` | 2026-08-23 | chore(vdp-d3d12): Redesign descriptor heap allocator to support allocating ranges of descriptors | skip | Ymir GPU renderer (out of scope) | |
| `d42bee7` | 2026-08-23 | feat(vdp-d3d12): Map descriptors to layer drawing compute shader | skip | Ymir GPU renderer (out of scope) | |
| `d9a0134` | 2026-08-23 | chore(vdp-d3d12): Remove VRAM sync interval options | skip | Ymir D3D12 renderer options | |
| `cdef10a` | 2026-08-23 | feat(vdp-d3d12): Define most of the NBG/RBG layer rendering parameters and create GPU resources for them | skip | Ymir GPU renderer (out of scope) | |
| `eb48b28` | 2026-08-23 | fix(vdp-hw): glslc doesn't ssem to support modern HLSL syntax | skip | Ymir GPU renderer (out of scope) | |
| `650ec99` | 2026-08-24 | feat(vdp-d3d12): Upload and use layer rendering parameters | skip | Ymir GPU renderer (out of scope) | |
| `3a8ffde` | 2026-08-24 | fix(vdp-d3d12): Fix `bool` alignment and a couple of mistakes with renderer parameters | skip | Ymir GPU renderer (out of scope) | |
| `5f2a160` | 2026-08-24 | feat(vdp-d3d12): Begin using parameters in shader code | skip | Ymir GPU renderer (out of scope) | |
| `2fd1d7e` | 2026-08-24 | chore(build): As expected, that does not solve the problem | skip | Ymir build-only | |
| `ceb4295` | 2026-08-24 | fix(build): Of course, that also doesn't work | skip | Ymir build-only | |
| `41bdbc3` | 2026-08-24 | fix(vdp-d3d12): Maybe making the arguments `const` will help | skip | Ymir GPU renderer (out of scope) | |
| `6c60bc2` | 2026-08-24 | chore(build): Marking them `const` also doesn't solve it | skip | Ymir build-only | |
| `86553e6` | 2026-08-24 | fix(vdp-d3d12): Shift SRV slots up by 1 to hopefully fix macOS builds | skip | Ymir GPU renderer (out of scope) | |
| `bbc68cd` | 2026-08-25 | feat(vdp-d3d12): Wire up compositor shader | skip | Ymir GPU renderer (out of scope) | |
| `98e1eec` | 2026-08-25 | chore(vdp-d3d12): Rename some objects for consistency | skip | Ymir GPU renderer (out of scope) | |
| `70a58d3` | 2026-08-25 | feat(vdp-d3d12): Set up and upload compositor parameters | skip | Ymir GPU renderer (out of scope) | |
| `b897bdd` | 2026-08-25 | chore(vdp-hw): Seems like it's better to just keep everything in one file | skip | Ymir GPU renderer (out of scope) | |
| `d034894` | 2026-08-25 | feat(vdp-d3d12): Setup and upload common rendering parameters | skip | Ymir GPU renderer (out of scope) | |
| `79668f9` | 2026-08-25 | feat(vdp-hw): Flesh out base NBG rendering function | skip | Ymir GPU renderer (out of scope) | |
| `7354972` | 2026-08-25 | chore(vdp-hw): Remove dead code | skip | Ymir GPU renderer (out of scope) | |
| `84d7f4b` | 2026-08-26 | fix(gfx-d3d12): FIx infinite loop when attempting to free a descriptor range | skip | Ymir graphics backend (out of scope) | |
| `01ff81f` | 2026-08-26 | feat(vdp-hw): Render NBGs | skip | Ymir GPU renderer (out of scope) | |
| `845a5f1` | 2026-08-26 | feat(vdp-hw): Add RBG rendering | skip | Ymir GPU renderer (out of scope) | |
| `5f5be18` | 2026-08-27 | feat(vdp-hw): Define rotparams shader | skip | Ymir GPU renderer (out of scope) | |
| `01a50d2` | 2026-08-27 | fix(vdp-hw): Actually compile the shader and fix errors | skip | Ymir GPU renderer (out of scope) | |
| `37988fc` | 2026-08-27 | chore(build): Minor cleanup | skip | Ymir build-only | |
| `9c9e1c9` | 2026-08-27 | feat(vdp-hw): Render RBGs, fix some bugs | skip | Ymir GPU renderer (out of scope) | |
| `b3c4b0f` | 2026-08-27 | chore(vdp-hw): Add basic features to the VDP2 compositor shader | skip | Ymir GPU renderer (out of scope) | |
| `880326e` | 2026-08-27 | fix(vdp-hw): Yet another SPIRV-Cross workaround | skip | Ymir GPU renderer (out of scope) | |
| `977c83a` | 2026-08-28 | feat(vdp-hw): Upload LNCL/BACK screens | skip | Ymir GPU renderer (out of scope) | |
| `adbd9a7` | 2026-08-28 | feat(vdp-hw): Compute color calculation window | skip | Ymir GPU renderer (out of scope) | |
| `1302ccf` | 2026-08-28 | feat(vdp-hw): Implement compositor | skip | Ymir GPU renderer (out of scope) | |
| `284a01a` | 2026-08-28 | fix(vdp-hw): FIx some compositor bugs | skip | Ymir GPU renderer (out of scope) | |
| `08dae7d` | 2026-08-28 | fix(vdp-d3d12): Add UAV barriers | skip | Ymir GPU renderer (out of scope) | |
| `e4c034b` | 2026-08-29 | chore(vdp-d3d12): Apply UAV barriers only during compositor stage | skip | Ymir GPU renderer (out of scope) | |
| `552c0a7` | 2026-08-29 | chore(vdp-d3d12): Refactor barrier set to also support texture barriers | skip | Ymir GPU renderer (out of scope) | |
| `36184e3` | 2026-08-29 | fix(vdp-d3d12): Fix barriers | skip | Ymir GPU renderer (out of scope) | |
| `18d4755` | 2026-08-29 | fix(vdp-d3d12): Fix barriers again, still not 100% | skip | Ymir GPU renderer (out of scope) | |
| `9e04434` | 2026-08-29 | feat(vdp-d3d12): VDP2 sprite layer rendering | skip | Ymir GPU renderer (out of scope) | |
| `b4b4d90` | 2026-08-29 | fix(vdp-d3d12): More barrier fixing | skip | Ymir GPU renderer (out of scope) | |
| `a4dbc06` | 2026-08-29 | chore(vdp-d3d12): Minimize barriers | skip | Ymir GPU renderer (out of scope) | |
| `a729c9f` | 2026-08-29 | perf(vdp-hw): Don't waste time rendering the transparent meshes layer if the enhancement is disabled | skip | Ymir GPU renderer (out of scope) | |
| `ba4f24c` | 2026-08-29 | feat(vdp-hw): Remove "transparent" bit as "priority=0" serves the same purpose | skip | Ymir GPU renderer (out of scope) | |
| `922e371` | 2026-08-29 | chore(vdp-hw): Clean up unused variables | skip | Ymir GPU renderer (out of scope) | |
| `8d70f0a` | 2026-08-29 | fix(vdp-hw): Fix LNCL/BACK reads | skip | Ymir GPU renderer (out of scope) | |
| `9835ebf` | 2026-08-29 | chore(vdp-d3d12): Cleanup and prepare for VDP1 implementation | skip | Ymir GPU renderer (out of scope) | |
| `a26bca1` | 2026-08-30 | chore(vdp-hw): Rename shaders for better organization | skip | Ymir GPU renderer (out of scope) | |
| `03f143c` | 2026-08-30 | chore(vdp-hw): Merge color calcuation window shader into the BGs shader | skip | Ymir GPU renderer (out of scope) | |
| `d2250d2` | 2026-08-30 | chore(vdp-hw): Slightly reorganize code for clarity | skip | Ymir GPU renderer (out of scope) | |
| `6fd9b3a` | 2026-08-30 | chore(vdp-hw): Standardize names | skip | Ymir GPU renderer (out of scope) | |
| `2836d6d` | 2026-08-30 | perf(vdp-hw): Merge and specialize rotparams calculations into the layer rendering shaders | skip | Ymir GPU renderer (out of scope) | |
| `b77de38` | 2026-08-30 | fix(vdp-hw): Fix LNCL/BACK indexing and parsing | skip | Ymir GPU renderer (out of scope) | |
| `9c061ee` | 2026-08-30 | chore(vdp-hw): Embed rotation registers in common rendering parameters block | skip | Ymir GPU renderer (out of scope) | |
| `f5455eb` | 2026-08-30 | fix(vdp-hw): Fix color calculation indexing (maybe) | skip | Ymir GPU renderer (out of scope) | |
| `9c30350` | 2026-08-30 | chore(vdp-hw): Move more parameters to root constants | skip | Ymir GPU renderer (out of scope) | |
| `f7b1f30` | 2026-08-30 | feat(vdp-hw): Restrict color calculations | skip | Ymir GPU renderer (out of scope) | |
| `effeaa8` | 2026-08-30 | fix(vdp-hw): Port recent fixes to hardware VDP2 compositor shader | skip | software renderer part is a rename (`colorGradEnabled`, `mesheColor`) with the same output; the fix is in the Ymir GPU shader | |
| `4d36e68` | 2026-08-31 | fix(bup): Remove strict language check when importing files | skip | host-side backup file import (`BackupMemory::ReadHeader` builds the file list for import/export); Brimir never imports files and the guest reads the raw bytes, so no effect | |
| `523530a` | 2026-08-31 | chore(vdp-d3d12): Merge rot regs updates into layer params | skip | Ymir GPU renderer (out of scope) | |
| `a9f0cda` | 2026-08-31 | feat(vdp-d3d12): Duplicate resources across multiple frames to actually benefit from parallelism | skip | Ymir GPU renderer (out of scope) | |
| `5031a06` | 2026-09-02 | chore(vdp-d3d12): Composited output texture has to be a single texture | skip | Ymir GPU renderer (out of scope) | |
| `2f94af2` | 2026-09-02 | chore(vdp-d3d12): Various cleanups and fixes | skip | Ymir GPU renderer (out of scope) | |
| `bb2b685` | 2026-09-03 | feat(vdp-d3d12): Output frame copy callback, compute side | skip | Ymir GPU renderer (out of scope) | |
| `0e6ede9` | 2026-09-04 | feat(vdp-d3d12): Wire up frame callback | skip | Ymir GPU renderer (out of scope) | |
| `ed3231d` | 2026-09-05 | fix(gamedb): Force SH-2 cache emulation for Dragon Force II to fix black screen when entering 100 vs 100 battles | already | 0.5.3 (Dragon Force II, `GS-9184`) | |
| `e926723` | 2026-09-05 | fix(vdp-d3d12): Use generation counter for dirty tracking parameter blocks | skip | Ymir GPU renderer (out of scope) | |
| `9ceb4a6` | 2026-09-05 | fix(hw-vdp): Remove base offset from rotparam calculations | skip | Ymir GPU renderer (out of scope) | |
| `22c022c` | 2026-09-05 | fix(hw-vdp): Remove invalid line scroll table address adjustment | skip | Ymir GPU renderer (out of scope) | |
| `7ca157e` | 2026-09-05 | fix(vdp-hw): Fix color gradation and color calc ratios | skip | software renderer part moves a comment; the fix is in the Ymir GPU shader | |
| `46d524a` | 2026-09-07 | fix(vdp-hw): Fix include guards in HLSL includes | skip | Ymir GPU renderer (out of scope) | |
| `bfe937c` | 2026-09-07 | feat(vdp-hw): VDP1 renderer skeleton, shader variants/specializations | skip | Ymir GPU renderer (out of scope) | |
| `e7dc340` | 2026-09-07 | chore(vdp-d3d12): Refactor upload buffer allocator to be reusable across both buffers | skip | Ymir GPU renderer (out of scope) | |
| `366f452` | 2026-09-07 | feat(vdp-d3d12): Basic VDP1 command processing structure | skip | Ymir GPU renderer (out of scope) | |
| `bb341d8` | 2026-09-07 | chore(vdp-hw): Add notes on suboptimal compute thread group sizes | skip | Ymir GPU renderer (out of scope) | |
| `bd29f63` | 2026-09-07 | chore(vdp-d3d12): Restructure VDP1 shader indexing | skip | Ymir GPU renderer (out of scope) | |
| `db3f974` | 2026-09-07 | chore(vdp-hw): Describe possible implementation focused on minimizing wasted compute resources | skip | Ymir GPU renderer (out of scope) | |
| `df0c250` | 2026-09-07 | feat(vdp-hw): Allocate span buffers, move parameters around, cut shader specializations in half | skip | Ymir GPU renderer (out of scope) | |
| `05396bb` | 2026-09-07 | chore(vdp-hw): Specialize on the transparent meshes enhancement instead of the mesh property | skip | Ymir GPU renderer (out of scope) | |
| `62885c1` | 2026-09-08 | feat(vdp-d3d12): Get started with some basic VDP1 commands | skip | Ymir GPU renderer (out of scope) | |
| `33f0453` | 2026-09-09 | feat(vdp-d3d12): Setup and upload VDP1 line spans | skip | Ymir GPU renderer (out of scope) | |
| `198f0c6` | 2026-09-09 | fix(vdp-d3d12): Fix shared VDP1/VDP2 fence usage | skip | Ymir GPU renderer (out of scope) | |
| `2f0f460` | 2026-09-09 | feat(vdp-d3d12): Dispatch polygon drawing shader | skip | Ymir GPU renderer (out of scope) | |
| `9f1ed22` | 2026-09-09 | perf(vdp-d3d12): Slightly more efficient barrier transitions | skip | Ymir GPU renderer (out of scope) | |
| `54f1f09` | 2026-09-09 | chore(vdp-hw): Add notes about the span skip amount | skip | Ymir GPU renderer (out of scope) | |
| `16ad556` | 2026-09-10 | feat(vdp-d3d12): Limit span dispatches by pixel count | skip | Ymir GPU renderer (out of scope) | |
| `19a9243` | 2026-09-10 | feat(vdp-hw): Attempt to implement span search | skip | Ymir GPU renderer (out of scope) | |
| `e5729b9` | 2026-09-10 | chore(vdp-d3d12): Merge VDP1 and VDP2 commands into a single list | skip | Ymir GPU renderer (out of scope) | |
| `aa3970d` | 2026-09-10 | fix(vdp1-hw): Skip out of range pixels | skip | Ymir GPU renderer (out of scope) | |
| `6d77996` | 2026-09-11 | feat(vdp-hw): Basic line drawing, for testing purposes | skip | Ymir GPU renderer (out of scope) | |
| `d0c720b` | 2026-09-12 | feat(vdp-hw): Setup VDP1 output merger shader | skip | Ymir GPU renderer (out of scope) | |
| `e987199` | 2026-09-12 | feat(vdp-hw): Implement VDP1 output merger | skip | Ymir GPU renderer (out of scope) | |
| `f407951` | 2026-09-12 | feat(vdp-hw): Basic VDP1 colors | skip | Ymir GPU renderer (out of scope) | |
| `cee6d44` | 2026-09-12 | fix(vdp-hw): Fix span parameter uploads and AA coordinates | skip | Ymir GPU renderer (out of scope) | |
| `9f38153` | 2026-09-12 | chore(vdp-hw): Notes for later, plus first specializations | skip | Ymir GPU renderer (out of scope) | |
| `e749f9e` | 2026-09-13 | feat(vdp-hw): Setup VDP1 MSB polygon drawing shader variants | skip | Ymir GPU renderer (out of scope) | |
| `7387c3a` | 2026-09-13 | fix(vdp-d3d12): Insert UAV barrier between polygon drawing and output merger shaders | skip | Ymir GPU renderer (out of scope) | |
| `740524b` | 2026-09-13 | fix(vdp-hw): Span step calculation fixes | skip | Ymir GPU renderer (out of scope) | |
| `8edc3a9` | 2026-09-13 | fix(vdp-hw): Fix gouraud shading | skip | Ymir GPU renderer (out of scope) | |
| `494c322` | 2026-09-13 | fix(vdp-d3d12): Fix VDP1 shader selection | skip | Ymir GPU renderer (out of scope) | |
| `a548281` | 2026-09-13 | fix(vdp-hw): Fix line clipping | already | 0.6.0 GPU stage 0 (`DMajor()` line clipping); Brimir also wraps the accumulators (BRIMIR_FORK 2026-10-27 rows) | |
| `801133f` | 2026-09-13 | fix(vdp-hw): Fix span sequence number and some geometry errors | skip | Ymir GPU renderer (out of scope) | |
| `68d76ef` | 2026-09-13 | fix(vdp-d3d12): Hacky fix for frame splicing | skip | Ymir GPU renderer (out of scope) | |
| `f1a18e5` | 2026-09-13 | feat(vdp-d3d12): Limit number of pixels per VDP1 dispatch | skip | Ymir GPU renderer (out of scope) | |
| `724b5f1` | 2026-09-13 | feat(vdp-hw): VDP1 textures | skip | Ymir GPU renderer (out of scope) | |
| `a2400ad` | 2026-09-13 | fix(vdp-hw): Fix AA filling too many pixels | skip | Ymir GPU renderer (out of scope) | |
| `905122a` | 2026-09-13 | fix(vdp-d3d12): Fix scaled sprite coordinates | skip | Ymir GPU renderer (out of scope) | |
| `6b11602` | 2026-09-13 | fix(vdp-hw): Fix 8-bit sprite FBRAM offset | skip | Ymir GPU renderer (out of scope) | |
| `125d912` | 2026-09-13 | chore(vdp-hw): Reorganize conments, fix typos, more TODOs for later | skip | Ymir GPU renderer (out of scope) | |
| `e9858c9` | 2026-09-13 | fix(vdp-hw): Some MSB fixes | skip | Ymir GPU renderer (out of scope) | |
| `ada39c1` | 2026-09-13 | fix(vdp-hw): More assorted MSB/shadow fixes | skip | Ymir GPU renderer (out of scope) | |
| `7eb7e1d` | 2026-09-13 | fix(vdp-hw): Another macOS/SPIRV-Cross build fix | skip | Ymir GPU renderer (out of scope) | |
| `62f811b` | 2026-09-14 | fix(vdp-hw): Fix MSB shadows for real | skip | Ymir GPU renderer (out of scope) | |
| `baa70d7` | 2026-09-14 | fix(vdp-hw): Fix normal shadows | skip | Ymir GPU renderer (out of scope) | |
| `65b65ae` | 2026-09-14 | fix(vdp-hw): Fix sprite color calc ratio retrieval | skip | Ymir GPU renderer (out of scope) | |
| `8a8e840` | 2026-09-14 | fix(vdp-hw): Fetch texture data regardless of color blending mode | skip | Ymir GPU renderer (out of scope) | |
| `53976a1` | 2026-09-14 | chore(vdp-hw): Reorganize VDP1 polydraw shader for optimal code reuse | skip | Ymir GPU renderer (out of scope) | |
| `66d6022` | 2026-09-14 | feat(vdp-hw): Output merger variants, implement Shadow mode, cleanup shader code | skip | Ymir GPU renderer (out of scope) | |
| `e463678` | 2026-09-14 | feat(vdp-hw): Attempt to implement end codes | skip | Ymir GPU renderer (out of scope) | |
| `709e7e5` | 2026-09-15 | feat(vdp-hw): VDP1 framebuffer erase mostly working | skip | Ymir GPU renderer (out of scope) | |
| `b1d9f28` | 2026-09-15 | fix(vdp-d3d12): Fix barriers | skip | Ymir GPU renderer (out of scope) | |
| `df725f1` | 2026-09-15 | fix(vdp-hw): Fix VBlank erase partial writes | skip | Ymir GPU renderer (out of scope) | |
| `4292018` | 2026-09-16 | chore(vdp-hw): Execute VDP1 erase immediately rather than during swap | skip | Ymir GPU renderer (out of scope) | |
| `946fc85` | 2026-09-16 | fix(vdp-d3d12): Update VDP2 VRAM access flags on every line | skip | Ymir GPU renderer (out of scope) | |
| `ba80362` | 2026-09-16 | fix(vdp-hw): Fix end code skip | skip | Ymir GPU renderer (out of scope) | |
| `17afd43` | 2026-09-17 | fix(vdp-hw): Fix sprite priority retrieval during VDP2 composition | skip | Ymir GPU renderer (out of scope) | |
| `4be08ff` | 2026-09-17 | perf(vdp-hw): Minimize VDP1 context switches by simplifying shader specializations | skip | Ymir GPU renderer (out of scope) | |
| `d6b0ac0` | 2026-09-17 | chore(vdp-hw): Minor code cleanup | skip | Ymir GPU renderer (out of scope) | |
| `99fe790` | 2026-09-17 | perf(vdp-hw): Compress VDP1 span data as much as possible | skip | Ymir GPU renderer (out of scope) | |
| `d0b7227` | 2026-09-18 | chore(vdp-hw): Rename global parameters | skip | Ymir GPU renderer (out of scope) | |
| `1177909` | 2026-09-18 | feat(vdp-hw): Implement OIT transparency | skip | Ymir GPU renderer (out of scope) | |
| `886e033` | 2026-09-18 | feat(vdp-hw): Implement deinterlace enhancement in VDP1 shaders | skip | Ymir GPU renderer (out of scope) | |
| `0ec0a7a` | 2026-09-18 | chore(vdp-hw): Prepare resources for transparent mesh enhancement | skip | Ymir GPU renderer (out of scope) | |
| `27effd9` | 2026-09-18 | fix(vdp-hw): Fix sprite attribute output for transparent meshes | skip | Ymir GPU renderer (out of scope) | |
| `b87a18e` | 2026-09-18 | fix(vdp-hw): More FBRAM addressing fixes | skip | Ymir GPU renderer (out of scope) | |
| `2d4c368` | 2026-09-18 | fix(vdp-d3d12): Ignore shading modes when using 8-bit sprite data | skip | Ymir GPU renderer (out of scope) | |
| `c8093a3` | 2026-09-18 | feat(vdp-hw): Write mesh pixels to correct spot in the internal buffers | skip | Ymir GPU renderer (out of scope) | |
| `ac18925` | 2026-09-18 | fix(vdp-d3d12): Don't dispatch deinterlace field on progressive display modes | skip | Ymir GPU renderer (out of scope) | |
| `da01942` | 2026-09-19 | feat(vdp-hw): Transparent meshes | skip | Ymir GPU renderer (out of scope) | |
| `2148814` | 2026-09-19 | fix(vdp-hw): Fix CP delay | skip | Ymir GPU renderer (out of scope) | |
| `0cae927` | 2026-09-19 | fix(vdp-hw): Fix broken RBG line color effects with deinterlace | skip | Ymir GPU renderer (out of scope) | |
| `3620e8e` | 2026-09-19 | chore(vdp-sw): Minor code simplification | already | code identical in Brimir | |
| `8df453b` | 2026-09-19 | fix(vdp-hw): Fix RBG line color in high horizontal resolution modes | skip | Ymir GPU renderer (out of scope) | |
| `db76262` | 2026-09-19 | fix(vdp-hw): Fix multiple deinterlace issues | skip | Ymir GPU renderer (out of scope) | |
| `247a43e` | 2026-09-19 | chore(vdp-hw): Introduce command parameters list and fix clipping | skip | Ymir GPU renderer (out of scope) | |
| `a386763` | 2026-09-19 | fix(vdp-d3d12): Set bitmap size for RBGs | skip | Ymir GPU renderer (out of scope) | |
| `06c3722` | 2026-09-19 | feat(vdp-hw): FBRAM writes partially working | skip | Ymir GPU renderer (out of scope) | |
| `54c81bb` | 2026-09-19 | chore(vdp): Fix CRAM address mapping documentation error | already | 0.6.0 GPU stage 0 (documentation) | |
| `2b8686f` | 2026-09-19 | chore(vdp-hw): Move `FBRAMWrite` definition to an HLSL include file | skip | Ymir GPU renderer (out of scope) | |
| `ebc7da8` | 2026-09-19 | fix(vdp-d3d12): Fix CRAM color cache updates when using RGB 8:8:8 color modes | already | 0.6.0 GPU stage 0 (`VDP2Memory::UnmapCRAMAddress`) | |
| `0f9be94` | 2026-09-19 | feat(vdp-d3d12): Implement basic frame sync for FBRAM reads | skip | Ymir GPU renderer (out of scope) | |
| `389e928` | 2026-09-19 | feat(vdp-d3d12): Sort of implement FBRAM downloads | skip | Ymir GPU renderer (out of scope) | |
| `1db9398` | 2026-09-19 | fix(vdp-d3d12): Fix descriptors for polygon drawing MSB variant | skip | Ymir GPU renderer (out of scope) | |
| `7c2833f` | 2026-09-19 | fix(utils): Fix dirty bitmap `SetAll` not properly updating the `m_anySet` flag | already | 0.6.0 GPU stage 0 (DirtyBitmap `SetAll`) | |
| `51ebfec` | 2026-09-19 | perf(vdp-d3d12): Slightly speed up FBRAM uploads | skip | Ymir GPU renderer (out of scope) | |
| `a345128` | 2026-09-19 | fix(vdp-hw): Fix FBRAM endianness | skip | Ymir GPU renderer (out of scope) | |
| `be048ef` | 2026-09-19 | fix(vdp-d3d12): Fix FBRAM upload endianness | skip | Ymir GPU renderer (out of scope) | |
| `6a6504d` | 2026-09-19 | perf(vdp-d3d12): Use bit cluster search in FBRAM dirty bitmap | already | 0.6.0 GPU stage 0 (DirtyBitmap bit cluster search; Brimir include/shift/MSVC fixes in BRIMIR_FORK) | |
| `285c3ab` | 2026-09-19 | fix(vdp-d3d12): Fix FBRAM upload timing | skip | Ymir GPU renderer (out of scope) | |
| `ab19029` | 2026-09-19 | fix(vdp-d3d12): Byte-swap erase write value to fix a single-frame glitch in the boot sequence | skip | Ymir GPU renderer (out of scope) | |
| `f6c9c59` | 2026-09-19 | fix(vdp-d3d12): Fix debug VDP1 FBRAM sync | skip | Ymir GPU renderer (out of scope) | |
| `92a04e6` | 2026-09-19 | fix(vdp-hw): Fix RBG bitmap base address selection | skip | Ymir GPU renderer (out of scope) | |
| `49ee054` | 2026-09-20 | fix(vdp): Fix VCNT not loading from save states | already | 0.5.4 (VCNT from states) | |
| `458b849` | 2026-09-20 | feat(debug): Fix VDP2 VCNT in debug view, add current VDP1 display framebuffer index | skip | debugger probe (`GetSpriteDisplayFB`) | |
| `2220b16` | 2026-09-20 | chore(VDP): Rename `spriteFB` to `FBRAM`, move it into the VDP1 state object | already | 0.6.0 (FBRAM in VDP1 memory) | |
| `01beeb0` | 2026-09-20 | chore(vdp-sw): FBRAM naming consistency | already | 0.6.0 | |
| `bcb0f52` | 2026-09-20 | fix(vdp-sw): Write back FBRAM writes from VDP1 renderer thread | already | 0.5.4 (adapted: FBRAM write-back from the VDP1 thread) | |
| `4769bb0` | 2026-09-20 | chore(vdp-sw): Remove unused field | already | 0.6.0 | |
| `9a64f38` | 2026-09-20 | chore(vdp): Introduce FBRAM indexer to clarify draw/display indices throughout the code | already | 0.6.0 (FBRAM indexer) | |
| `12ef24f` | 2026-09-20 | fix(VDP1): Don't clear COPR when beginning a new VDP1 frame | already | 0.5.4 (COPR kept at frame start, Alone in the Dark) | |
| `62e920a` | 2026-09-20 | fix(gamedb): Force-enable SH-2 cache emulation for Hissatsu! to fix crash at startup | already | 0.5.4 (Hissatsu!, `T-23402G`) | |
| `f9f4759` | 2026-09-20 | fix(VDP2): Partial fix for mid-frame DISP and LNCL/BACK changes | already | 0.5.4 (mid-frame DISP and LNCL/BACK) | |
| `3d504d5` | 2026-09-20 | fix(vdp-sw): Fix out of bounds access when the display resolution is changed mid-frame while drawing an RBG | already | 0.5.4 (RBG out-of-bounds on mid-frame resolution change) | |
| `49df0c2` | 2026-09-20 | fix(vdp-sw): Stop updating LNCL/BACK screen once DISP is disabled for the remainder of the frame | already | 0.5.4 (LNCL/BACK stop after DISP off) | |
| `f84acb5` | 2026-09-20 | fix(vdp-d3d12): Apply LNCL/BACK fixes to D3D12 renderer | skip | Ymir GPU renderer (out of scope) | |
| `4ec1e59` | 2026-09-21 | fix(vdp-sw): Fix LNCL/BACK color fetches | already | 0.5.4 (LNCL/BACK fetches) | |
| `9808544` | 2026-09-21 | fix(vdp-hw): Fix LNCL insertion color calc ratio | skip | Ymir GPU renderer (out of scope) | |
| `bdd8d8c` | 2026-09-21 | fix(vdp-sw): Fix color calc and color gradation overlays | skip | VDP2 debug render overlays only; Brimir has no debugger | |
| `00919ac` | 2026-09-21 | fix(vdp-hw): Fix MSB applying to the wrong bit | skip | Ymir GPU renderer (out of scope) | |
| `3de1e00` | 2026-09-21 | fix(vdp-hw): Fix color gradation on the left edge of the screen | skip | Ymir GPU renderer (out of scope) | |
| `3d19fe2` | 2026-09-22 | feat(gfx): Sync resolution changes to framebuffer updates | skip | Ymir frontend resolution callback rework (gfx); no emulation change | |
| `bce4701` | 2026-09-22 | docs: Update documentation on resolution callback | skip | documentation | |
| `74706eb` | 2026-09-22 | chore: Clean up incorrect documentation | skip | documentation | |
| `e340e40` | 2026-09-22 | chore(vdp-d3d12): Centralize enhancements updates in the renderer | skip | Ymir GPU renderer (out of scope) | |
| `184d2fb` | 2026-09-23 | fix(SMPC): Ignore SSHON command when the Slave SH2 is already enabled | already | 0.5.4 (SSHON ignored when the slave SH-2 runs) | |
| `6e6b0ba` | 2026-09-23 | fix(vdp-d3d12): Submit spans before VRAM writes if they modify a pending span's texture | skip | Ymir GPU renderer (out of scope) | |
| `93646d3` | 2026-09-23 | chore: Code style fixes | skip | Ymir GPU renderer (out of scope) | |
| `b3e1fb1` | 2026-09-23 | fix(vdp-d3d12): Initialize the usage array, fix documentation | skip | Ymir GPU renderer (out of scope) | |
| `7a4b109` | 2026-09-24 | fix(vdp-hw): Fix sprite rotation coordinate calculations | skip | Ymir GPU renderer (out of scope) | |
| `f81d9cb` | 2026-09-24 | fix(vdp-d3d12): Fix VDP1 FBRAM reads/writes | skip | Ymir GPU renderer (out of scope) | |
| `d718485` | 2026-09-24 | fix(vdp-d3d12): Fix crash when games attempt to render textured polygons with zero-sized textures | skip | Ymir GPU renderer (out of scope) | |
| `876ca3d` | 2026-09-24 | fix(vdp-hw): Protect against out-of-bounds sprite rotation coordinates | skip | Ymir GPU renderer (out of scope) | |
| `627e016` | 2026-09-24 | fix(gamedb): Force-enable SH-2 cache emulation for No-appointment Gals Olympos to fix crash during animations | already | 0.5.4 (No-appointment Gals Olympos, `T-4304G`) | |
| `2a99d68` | 2026-09-24 | fix(vdp-hw): Fix sprite parameter offsets in VDP2 compositor shader | skip | Ymir GPU renderer (out of scope) | |
| `dc88d9e` | 2026-09-24 | fix(media): Fix crash when loading CUE files with WAV audio tracks | skip | not applicable: fixes a crash 899db8d introduced by no longer opening WAVE files before parsing them; Brimir's loader opens every non-MP3/OGG file before the WAVE check (`loader_bin_cue.cpp`) | |
| `4807d23` | 2026-09-25 | fix(vdp-hw): Fix transparent meshes handling in output merger | skip | Ymir GPU renderer (out of scope) | |
| `5fab3c3` | 2026-09-25 | fix(vdp-sw): Fix more cases of out-of-bounds reads | already | 0.6.0 (out-of-bounds reads, threaded resolution change) | |
| `ce270c0` | 2026-09-25 | fix(vdp-sw): Use VDP1 register state from thread context while running VDP1 in a thread | already | 0.6.0 (VDP1 thread register state, `LatchEraseParameters` pinned by a Brimir test) | |
| `80a3e09` | 2026-09-26 | chore(vdp-hw): Group resolution dimensions into an `uint2` value | skip | Ymir GPU renderer (out of scope) | |
| `18ecec2` | 2026-09-26 | fix(vdp-d3d12): Fix VDP1 span submission when buffer overflows | skip | Ymir GPU renderer (out of scope) | |
| `e792223` | 2026-09-26 | fix(build): Fix compilation errors with MSVC | skip | Ymir build-only | |
| `ff8cce0` | 2026-09-26 | chore(vdp-d3d12): Revert upload buffer to 16 MB | skip | Ymir GPU renderer (out of scope) | |
| `c6bfbb1` | 2026-09-26 | feat(vdp1): Allow dynamically adjusting bit precision of VDP1 steppers | already | 0.6.0 GPU stage 0 (stepper precision) | |
| `f1a70d4` | 2026-09-26 | fix(vdp-d3d12): Use the `size` parameter instead of the constant when creating the upload buffer | skip | Ymir GPU renderer (out of scope) | |
| `ae44e9f` | 2026-09-26 | chore(vdp-d3d12): Fixes to documentation comments | skip | Ymir GPU renderer (out of scope) | |
| `4ea11d0` | 2026-09-26 | chore(vdp-d3d12): Move common definitions to header files | skip | Ymir GPU renderer (out of scope) | |
| `fbf4a24` | 2026-09-27 | chore(vdp-d3d12): Cut down boilerplate code, part 1 | skip | Ymir GPU renderer (out of scope) | |
| `1dddc8d` | 2026-09-27 | chore(vdp-d3d12): Cut down boilerplate code, part 2 | skip | Ymir GPU renderer (out of scope) | |
| `441a6a0` | 2026-09-27 | chore(vdp-d3d12): Cut down boilerplate code, part 3 | skip | Ymir GPU renderer (out of scope) | |
| `e3b5509` | 2026-09-27 | chore(vdp-d3d12): Cut down boilerplate code, part 4 | skip | Ymir GPU renderer (out of scope) | |
| `822cdc1` | 2026-09-27 | chore(vdp-d3d12): Cut down boilerplate code, part 5 | skip | Ymir GPU renderer (out of scope) | |
| `4346b09` | 2026-09-27 | chore(vdp-d3d12): Cut down boilerplate code, part 6 | skip | Ymir GPU renderer (out of scope) | |
| `c9e0ff3` | 2026-09-27 | chore(vdp-hw): Change LNCL/BACK buffer to a primitive buffer | skip | Ymir GPU renderer (out of scope) | |
| `3916405` | 2026-09-27 | chore(vdp-d3d12): Cut down boilerplate code, part 7 | skip | Ymir GPU renderer (out of scope) | |
| `946c301` | 2026-09-27 | chore(vdp-d3d12): Cut down boilerplate code, part 8 | skip | Ymir GPU renderer (out of scope) | |
| `491bd62` | 2026-09-27 | fix(SH2): Properly align address on pipeline refills | skip | no behaviour change in Brimir: `SH2::MemRead` already masks a misaligned address (`kAddressMask`) before the partition switch, cache and bus access, so `RefillPipeline` reads the same word; the change only drops a trace-level "misaligned read" log (and upstream's new debug break, 5645611). JIT `JitRefillPipeline` reads through the same `MemRead`. | |
| `fc0a84f` | 2026-09-27 | chore(vdp-d3d12): Cut down boilerplate code, part 9 | skip | Ymir GPU renderer (out of scope) | |
| `4d50532` | 2026-09-27 | chore(vdp-d3d12): Simplify GPU handle retrieval from `DescriptorTable`s | skip | Ymir GPU renderer (out of scope) | |
| `48021eb` | 2026-09-28 | feat(vdp-d3d12): Add resource deletion queues to frame context | skip | Ymir GPU renderer (out of scope) | |
| `93866d9` | 2026-09-28 | fix(vdp-d3d12): Fix some errors when freeing descriptor ranges | skip | Ymir GPU renderer (out of scope) | |
| `e448c1c` | 2026-09-28 | feat(vdp-d3d12): Allow unregistering textures and buffers from the barrier tracker | skip | Ymir GPU renderer (out of scope) | |
| `8292b10` | 2026-09-28 | fix(vdp-d3d12): Distinguish between offline and online descriptors to be deleted | skip | Ymir GPU renderer (out of scope) | |
| `d5a8a3b` | 2026-10-02 | fix(SCSP): Don't sync WRAM on debugger peeks | skip | debugger peeks with threaded SCSP only; Brimir runs the SCSP unthreaded (`configuration.audio.threadedSCSP` stays false, no core option) | |
| `2d28b5d` | 2026-10-04 | chore: Housekeeping tasks | skip | housekeeping (`Dimensions::operator==` for Ymir's frontend) | |
| `706770f` | 2026-10-04 | feat: Use integer ratio for aspect ratio setting | skip | frontend aspect-ratio setting (`util::Ratio`); no emulation change | |
| `64192c4` | 2026-10-08 | chd read fixes | already | 0.6.0 (CHD pregap metadata; BRIMIR_FORK 2026-10-23 row, `HunkCache` kept) | |
| `58e7643` | 2026-10-09 | fix(d3d12): Don't emit empty list of legacy barriers | skip | Ymir graphics backend (out of scope) | |
| `dfe27b8` | 2026-10-10 | fix(vdp-hw): Fix incorrect priority bit extraction | skip | Ymir GPU renderer (out of scope) | |
| `39a0cb5` | 2026-10-10 | fix(vdp-hw): Fix vertical cell scroll offset calculation | skip | Ymir GPU renderer (out of scope) | |
| `e34d7c9` | 2026-10-10 | fix(vdp-d3d12): Sync FBRAM when saving states | skip | Ymir GPU renderer (out of scope) | |
| `23b3ded` | 2026-10-10 | fix(vdp-sw): Fix race condition reading VDP1 TVMR on VDP2 thread | already | 0.6.0 (TVMR in order on the VDP2 thread, Virtua Fighter 2); Brimir's VDP2 thread reset replaced `VDP2RenderContext::Reset()` (BRIMIR_FORK 2026-10-29) | |
| `1eacc2b` | 2026-10-10 | fix(cdblock-hle): Report wait status when attempting to calculate actual size of more sectors than available in a partition | already | 0.6.0 batch A (Calculate Actual Size wait status) | |
| `87897f8` | 2026-10-10 | fix(cdblock-hle): Adjust calculated sector sizes to requested get length | already | 0.6.0 batch A (sector sizes follow get length) | |
| `2798c50` | 2026-10-10 | fix(cdblock-hle): Get/Calculate Actual Size nuances | already | 0.6.0 batch A (Get/Calculate Actual Size nuances) | |
| `9da025d` | 2026-10-10 | fix(cdblock-hle): Create a dedicated scratch buffer for disc playback | already | 0.6.0 batch A (dedicated playback buffer, Virtual On) | |
| `f3eb28f` | 2026-10-10 | fix(cdblock-hle): Handle default end of disc parameter in Play Disc | already | 0.6.0 batch A (Play Disc end parameter 0, Gunbird) | |
| `acde106` | 2026-10-10 | fix(VDP2): Allow 8-bit register reads | already | 0.6.0 batch A (8-bit VDP2 register reads) | |
| `78ef0f7` | 2026-10-11 | build: Move derived build/version definitions to root CMakeLists | skip | Ymir build-only | |
| `d57f126` | 2026-10-11 | fix(VDP2): Also allow 8-bit writes to VDP2 registers | already | 0.6.0 batch A (8-bit VDP2 register writes, DoDonPachi) | |
| `838b7d3` | 2026-10-11 | fix(cdblock-hle): Reads from `0x5890000` should only pull one entry from the FIFO, not two | already | 0.6.0 batch A (32-bit `0x5890000` read) | |
| `a885676` | 2026-10-11 | feat(cdblock-hle): Emulate read FIFO for all read transfers | already | 0.6.0 (read FIFO); Brimir adds FIFO cursor validation in `CoreWrapper::LoadState` | |
| `624a1e0` | 2026-10-11 | feat(SCSP): Make threaded SCSP honor the step granularity setting | skip | threaded SCSP only; Brimir runs the SCSP unthreaded (`configuration.audio.threadedSCSP` stays false, no core option) | |
| `9c99982` | 2026-10-11 | perf(SCSP): Threading microoptimizations | skip | perf, threaded SCSP only; Brimir runs the SCSP unthreaded (`configuration.audio.threadedSCSP` stays false, no core option) | |
| `db36503` | 2026-10-11 | fix(vdp-d3d12): Fix debug names for PSOs and root signatures | skip | Ymir GPU renderer (out of scope) | |
| `08cfc5f` | 2026-10-11 | fix(vdp-d3d12): Sprite attributes texture needs more than 8 bits per pixel | skip | Ymir GPU renderer (out of scope) | |
| `08f4430` | 2026-10-11 | fix(cdblock-hle): Introduce Busy status when pausing playback | already | 0.6.0 (Busy after Pause, Amagi Shien); later made an overlay by 3474b09, also taken | |
| `7d87a43` | 2026-10-11 | perf(vdp-d3d12): Minimize heap allocations when uploading FBRAM chunks | skip | Ymir GPU renderer (out of scope) | |
| `a7b3046` | 2026-10-11 | fix(vdp-d3d12): Fix out-of-bounds writes when attempting to draw VDP1 sprites whose textures extend beyond VDP1 VRAM | skip | Ymir GPU renderer (out of scope) | |
| `e5e5ae3` | 2026-10-11 | fix(media-cue): Concatenate reads from composite readers | already | 0.6.0 (composite reader concatenates reads, Cold Case) | |
| `3474b09` | 2026-10-11 | fix(cdblock-hle): Make Busy status an overlay to avoid slowing down playback | already | 0.6.0 (Busy status overlay) | |
| `0a9f103` | 2026-10-11 | feat(cdblock-hle): Write busy delay to save state | already | 0.6.0 (busy delay in states) | |
| `07de41e` | 2026-10-11 | fix(cdblocke-hle): Introduce Busy state to End Data Transfer command | already | 0.6.0 (Busy after End Data Transfer) | |
| `e3f8782` | 2026-10-11 | fix(SMPC): Shorten delay for most commands | skip | reverted upstream by ead0f35 ("some games stopped booting") | |
| `10afe85` | 2026-10-11 | fix(vdp-d3d12): Mark FBRAM dirty when uploading it in full | skip | Ymir GPU renderer (out of scope) | |
| `ba6c5a8` | 2026-10-11 | fix(SCSP): Fix SCSP thread never sleeping | skip | threaded SCSP only; Brimir runs the SCSP unthreaded (`configuration.audio.threadedSCSP` stays false, no core option); fixes a regression of 9c99982/624a1e0 | |
| `76e0d02` | 2026-10-12 | fix(vdp-d3d12): Flush FBRAM or spans when switching between the two | skip | Ymir GPU renderer (out of scope) | |
| `aae1dbe` | 2026-10-12 | fix(SCSP): Handle rare one-shot PCM bug | already | 0.6.0 (SCSP one-shot PCM bug) | |
| `a144218` | 2026-10-12 | fix(SCSP): Clear slot PLFOS on reset | already | 0.6.0 batch A (SCSP PLFOS cleared on reset) | |
| `6045c24` | 2026-10-12 | perf(vdp-d3d12): Avoid dispatching the erase shader when there is nothing to erase | skip | Ymir GPU renderer (out of scope) | |
| `a485f59` | 2026-10-12 | fix(vdp-d3d12): Update parameters for FBRAM write shader | skip | Ymir GPU renderer (out of scope) | |
| `ead0f35` | 2026-10-12 | Revert "fix(SMPC): Shorten delay for most commands" | skip | revert of e3f8782 (net zero); Brimir never took e3f8782 | |
| `5645611` | 2026-10-12 | feat(debug-sh2): Break when PC is unaligned or in an illegal area while debug tracing is enabled | skip | debugger only (break on unaligned/illegal PC while debug tracing); Brimir has no debugger | |
| `d400f3d` | 2026-10-12 | fix(SCU): Ignore DMA writes to VDP1 registers | already | 0.6.0 batch A (SCU DMA writes to VDP1 registers ignored, GunBlaze-S); `scu.cpp` differs only in formatting | |
| `7ece34e` | 2026-10-12 | fix(vdp-d3d12): Minor naming fix | skip | Ymir GPU renderer (out of scope) | |
| `5985b5d` | 2026-10-12 | chore(vdp-d3d12): Remove unnecessary code | skip | Ymir GPU renderer (out of scope) | |
| `eb85cc5` | 2026-10-12 | chore(vdp-d3d12): Add TODO for fixing more VDP1 bugs | skip | Ymir GPU renderer (out of scope) | |
| `631b961` | 2026-10-12 | fix(vdp-hw): Correctly adjust vertical coordinate for LNCL/BACK screens | skip | Ymir GPU renderer (out of scope) | |
| `7ceb6e6` | 2026-10-12 | fix(vdp-hw): Fix vertical window coordinates for the sprite layer when in interlaced modes | skip | Ymir GPU renderer (out of scope) | |
| `839aef3` | 2026-10-12 | fix(SMPC): Tweak command response timings again | already | 0.6.0 batch A (SMPC 200-cycle command delay) | |
| `d37d873` | 2026-10-13 | chore(docs): Update stale comment on the threaded SCSP option | skip | comment on the threaded SCSP option | |
| `3e5299d` | 2026-10-13 | fix(vdp-hw): Fix rotation coefficient fetches from CRAM | skip | Ymir GPU renderer (out of scope) | |
| `142632d` | 2026-10-13 | fix(vdp-hw): Mask VRAM and CRAM addresses | skip | Ymir GPU renderer (out of scope) | |
| `65e2255` | 2026-10-13 | fix(vdp-hw): Fix previous character offset | skip | Ymir GPU renderer (out of scope) | |
| `65dfd54` | 2026-10-13 | fix(vdp-hw): Fix vertical coordinate for RBG windows in interlaced modes | skip | Ymir GPU renderer (out of scope) | |
| `0b02277` | 2026-10-13 | fix(vdp-d3d12): Add initial coordinates to VBlank erase limits | skip | Ymir GPU renderer (out of scope) | |
| `b4b27c9` | 2026-10-13 | chore(vdp-d3d12): Clear VDP1 lists on reset | skip | Ymir GPU renderer (out of scope) | |
| `89f64b9` | 2026-10-13 | fix(vdp-d3d12): Flush VDP1 work whenever the type of work changes | skip | Ymir GPU renderer (out of scope) | |
| `c2fed6f` | 2026-10-13 | fix(vdp-d3d12): Sync VDP1 framebuffer swap and erase | skip | Ymir GPU renderer (out of scope) | |
| `8eacd3c` | 2026-10-13 | fix(vdp-hw): Calculate RBG windows before modifying coordinates | skip | Ymir GPU renderer (out of scope) | |
| `c688623` | 2026-10-14 | fix(vdp-hw): Reset Y coordinate increment when writing to SCYN2-3 | skip | Ymir GPU renderer (out of scope) | |
| `7b44936` | 2026-10-14 | fix(vdp-d3d12): Reset scroll Y increment base | skip | Ymir GPU renderer (out of scope) | |
| `b3e6fc6` | 2026-10-14 | fix(SMPC): Reserved SR bits in INTBACK status reports are all set | already | 0.6.0 batch A (INTBACK reserved SR bits) | |
| `44bd8ea` | 2026-10-14 | fix(VDP1): Various VDP1 reset fixes | already | 0.6.0 (VDP1 reset fixes); Brimir's VDP1 thread `Reset` case also clears `m_VDP1doubleV` (BRIMIR_FORK 2026-10-29) | |
| `775ba76` | 2026-10-16 | fix(SMPC): Tweak processing time for INTBACK | already | 0.6.0 batch A (INTBACK 1000-cycle delay, Assault Rigs) | |
| `7ced7fd` | 2026-10-17 | fix(vdp-hw): Double-draw scanlines on the first interlaced frame to cover garbage from previous frame | skip | Ymir GPU renderer (out of scope) | |
| `191ae28` | 2026-10-17 | fix(vdp-hw): Fix more line color indexing issues | skip | Ymir GPU renderer (out of scope) | |
| `ac1a99c` | 2026-08-20 | feat(gfx-vulkan): Refactor vulkan header configurations into new header | skip | Ymir graphics backend (out of scope) | |
| `928fcc1` | 2026-08-19 | feat(gfx-vulkan): Statically initialize vulkan dispatch | skip | Ymir graphics backend (out of scope) | |
| `26f8b01` | 2026-08-19 | feat(gfx-vulkan): Implement vulkan debug tooling | skip | Ymir graphics backend (out of scope) | |
| `516622b` | 2026-08-19 | feat(gfx-vulkan): Refactor vulkan instance creation | skip | Ymir graphics backend (out of scope) | |
| `f41b208` | 2026-08-19 | chore(gfx-vulkan): Fix naming conventions | skip | Ymir graphics backend (out of scope) | |
| `27715b4` | 2026-08-19 | feat(gfx-vulkan): Init swapchain creation | skip | Ymir graphics backend (out of scope) | |
| `86d25ba` | 2026-08-19 | feat(gfx-vulkan): Implement context queue selection | skip | Ymir graphics backend (out of scope) | |
| `4f1cc08` | 2026-08-19 | feat(gfx-vulkan): Init vulkan descriptor heap | skip | Ymir graphics backend (out of scope) | |
| `94e9aa7` | 2026-08-19 | feat(gfx-vulkan): Implement descriptor update batching | skip | Ymir graphics backend (out of scope) | |
| `cfa9dd8` | 2026-08-20 | feat(gfx-vulkan): Add vulkan memory utils | skip | Ymir graphics backend (out of scope) | |
| `d2bf63e` | 2026-08-21 | feat(gfx-vulkan): Disable vulkan-hpp result handling | skip | Ymir graphics backend (out of scope) | |
| `dde80b0` | 2026-08-21 | feat(gfx-vulkan): Validate spirv for Vulkan 1.1 | skip | Ymir graphics backend (out of scope) | |
| `74630e6` | 2026-08-21 | feat(gfx-vulkan): Return optional swap chain index number | skip | Ymir graphics backend (out of scope) | |
| `bffce8c` | 2026-08-21 | feat(gfx-vulkan): Implement trivial swapchain framebuffers | skip | Ymir graphics backend (out of scope) | |
| `c2f1907` | 2026-08-22 | feat(gfx-vulkan): Refactor swapchain recreation | skip | Ymir graphics backend (out of scope) | |
| `bc0a35f` | 2026-08-22 | feat(gfx-vulkan): Init imgui rendering | skip | Ymir graphics backend (out of scope) | |
| `0344bb9` | 2026-08-22 | fix(gfx-vulkan): Reset swapchain synchronization values during recreation | skip | Ymir graphics backend (out of scope) | |
| `dad3d71` | 2026-08-22 | fix(gfx-vulkan): Fix swapchain semaphore reuse race condition | skip | Ymir graphics backend (out of scope) | |
| `9094826` | 2026-08-22 | fix(gfx-vulkan): Handle suboptimal/out-of-date swapchain | skip | Ymir graphics backend (out of scope) | |
| `ad85285` | 2026-08-23 | fix(gfx-vulkan): Fix swapchain resizing synchronization | skip | Ymir graphics backend (out of scope) | |
| `c55046d` | 2026-08-24 | feat(gfx-vulkan): Enable swapchain vsync by default | skip | Ymir graphics backend (out of scope) | |
| `efe4e0f` | 2026-08-24 | feat(gfx-vulkan): Refactor Swapchain recreation to queue-idle rather than device-idle | skip | Ymir graphics backend (out of scope) | |
| `115a217` | 2026-08-24 | feat(gfx-vulkan): Implement host-waits on timeline semaphores | skip | Ymir graphics backend (out of scope) | |
| `4d9dd18` | 2026-08-24 | feat(gfx-vulkan): Implement safe timeline semaphore acquisition | skip | Ymir graphics backend (out of scope) | |
| `750fc7a` | 2026-08-30 | feat(gfx-vulkan): Use dev-assertions for debug-callback errors Rather than manually using multi-platform debug-breaks, just use the `YMIR_DEV_CHECK` to conditionally break based on the project's `Ymir_ENABLE_DEV_ASSERTIONS`-setting. This is purely development-tooling and shouldn't be reaching the users. | skip | Ymir graphics backend (out of scope) | |
| `6f40a8d` | 2026-09-01 | fix(gfx-vulkan): Disable debug messenger functions during non-debug builds | skip | Ymir graphics backend (out of scope) | |
| `036233b` | 2026-10-18 | fix(gfx-vulkan): Reset `nextSwapIndex` when swapchain is minimized | skip | Ymir graphics backend (out of scope) | |
| `78cb1cf` | 2026-10-20 | fix(gfx-vulkan): Skip rendering when window is minimized | skip | Ymir graphics backend (out of scope) | |
| `3c7320f` | 2026-10-20 | fix(gfx-vulkan): Wait for present queue to finish before deleting swapchain | skip | Ymir graphics backend (out of scope) | |
| `45b68d9` | 2026-10-20 | fix(gpu-vulkan): Fix queue allocation | skip | Ymir graphics backend (out of scope) | |
| `9a1ac53` | 2026-10-20 | fix(gfx-vulkan): Use local dispatcher rather than global | skip | Ymir graphics backend (out of scope) | |
| `6356195` | 2026-10-24 | fix(gfx-vulkan): Fix vulkan swapchain creation | skip | Ymir graphics backend (out of scope) | |
| `d7cb2b2` | 2026-10-24 | fix(gfx-vulkan): Fix swapchain size reducing to 2 after minimization | skip | Ymir graphics backend (out of scope) | |
| `45625f9` | 2026-10-26 | chore(vk): Hide Vulkan attributes behind macros to silence DXC warnings | skip | Ymir graphics backend (out of scope) | |
| `fe439e9` | 2026-10-26 | fix(vk): Improve compatibility with older versions of the Vulkan SDK | skip | Ymir graphics backend (out of scope) | |
| `3df82cb` | 2026-10-31 | chore(vk): Avoid unnecessary copy of `VulkanSwapchain` | skip | Ymir graphics backend (out of scope) | |
| `ff93537` | 2026-11-07 | fix(VDP): Assignment operator should return a reference to self, not a copy | take (perf/other) | `VDP2Memory::operator=` returns a copy, so every assignment (VDP2 render thread sync and reset, `PostLoadStateSync`) copies VRAM and CRAM a second time; return a reference. No output change. `vdp_state.hpp`, not in a Brimir row. | |
