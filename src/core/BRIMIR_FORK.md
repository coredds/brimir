# Brimir fork of the Ymir SH-2

The Ymir hardware layer under `src/core/` is a verbatim copy of upstream
[Ymir](https://github.com/StrikerX3/ymir), with these exceptions: the SH-2 CPU is
Brimir-owned so the SH-2 JIT (see `design/sh2-jit.md`) can hook into it, the
CHD reader's hunk cache is bounded for low-memory hosts, the VDP can install a
renderer created outside `src/core` (for the Vulkan renderer), and a few upstream bugs
are fixed on Brimir's side only (listed under "Brimir-side fixes of upstream bugs").
Ymir is used as a reference; these fixes are kept here and not reported upstream.

## Fork scope

- `src/core/include/ymir/hw/sh2/*`
- `src/core/src/ymir/hw/sh2/*`
- One read-only accessor in `src/core/include/ymir/sys/bus.hpp`
  (`Bus::PageTableLayout`, `Bus::GetPageTableLayout`) for the SH-2 JIT.
- The CHD reader's hunk cache: `src/core/src/ymir/media/loader/loader_chd.cpp`
  (`CHDBinaryReader`) and the new `src/core/include/ymir/media/loader/chd_hunk_cache.hpp`.
- The external-renderer hook, marked `// Brimir:`: `VDPRendererType::External` and its
  `GetRendererName` case in `src/core/include/ymir/hw/vdp/renderer/vdp_renderer_defs.hpp`;
  `VDP::ExternalRendererFactory`, `VDP::UseExternalRenderer`, `VDP::InstallRenderer` and the
  `UseRenderer` body in `src/core/include/ymir/hw/vdp/vdp.hpp` and `src/core/src/ymir/hw/vdp/vdp.cpp`.
- Brimir-side fixes of upstream bugs, marked `// Brimir:` in place:
  - `src/core/src/ymir/hw/cdblock/cdblock.cpp`: partition number range checks in
    `CmdCalculateActualSize` and `CmdGetSectorInfo`.
  - `src/core/include/ymir/hw/vdp/renderer/vdp_renderer_sw.hpp` and
    `src/core/src/ymir/hw/vdp/renderer/vdp_renderer_sw.cpp`: `VDP1BeginFrame` and the
    `VDP1RenderEvent::BeginFrame` event.
  - `src/core/include/ymir/util/dirty_bitmap.hpp`: two missing includes, the
    `FindNext` shift guard and an explicit template argument in the `FindNextGroup`
    constraint.

Everything else under `src/core/` stays verbatim upstream.

## Permanent software-renderer changes (keep on every sync)

Brimir-local changes in the software renderer (`src/core/{include,src}/ymir/hw/vdp/`) that upstream
Ymir `8cdf0a53` does not have. A sync of these files must keep them (checked present on 2026-10-24,
after GPU stage 0). Hand-port upstream changes around them; do not copy the files wholesale.

| Change | Where | Notes |
|---|---|---|
| SSE2/NEON color gradation fix (Brimir `a974ad6`) | `vdp_renderer_sw.cpp`, `Color888GradationMasked` (and `Color888AverageMasked`) | The SSE2 blend selects `mask ? blend2 : color2` (`_mm_and_si128(mask_x4, blend2_x4)`; upstream ANDs `color2_x4`) and uses `_mm_set1_epi8(-2)`; the NEON `vbslq_u32(mask_x4, blend2_x4, color2_x4)` has the same operand order. Upstream `8cdf0a53` still has the old operands. |
| `#if defined(__SSE2__) \|\| defined(_M_X64)` guards | `vdp_renderer_sw.cpp`, every SSE2 block under `#if defined(_M_X64) \|\| defined(__x86_64__)` | MSVC x64 does not define `__SSE2__`, so upstream's guard skips the SSE2 path there. Upstream still uses `#if defined(__SSE2__)`. |
| `Color888OpaqueAlpha` | `vdp_renderer_sw.cpp` (definition, and the call on `framebufferOutput` after composition) | Vectorised alpha-opaque pass (AVX2/SSE2/NEON) over the line; upstream has no such function. |
| Fetcher reset optimisation (`3f8753c`) | `vdp_renderer_sw.cpp`, `VDP2PrepareLine`, the `m_vramFetchers` reset at its end ("The alt-field fetcher bank is only used while deinterlacing; skip the reset otherwise.") | Skips resetting the unused alt-field VRAM fetcher bank when not deinterlacing. |
| NEON `vreinterpretq_*` casts | `vdp_renderer_sw.cpp`, the NEON blocks of the `Color888*Masked` helpers | GCC 14 and Apple Clang reject upstream's implicit `uint32x4_t`/`uint8x16_t` conversions; still needed at `8cdf0a53` (upstream has no `vreinterpretq` in this file). See `docs/YMIR_SYNC_PATCHES.md` (not tracked). |
| `VDP1BeginFrame` fix and `VDP1RenderEvent::BeginFrame` | `vdp_renderer_sw.hpp`, `vdp_renderer_sw.cpp` | Row under "Brimir-side fixes of upstream bugs" below. |
| `LatchEraseParameters` placement (Ymir `ce270c04`) | `vdp_renderer_sw.cpp` (VDP1 render thread's `SwapBuffers` case: `rctx.vdp1.regs.LatchEraseParameters();`), `vdp.cpp` (`VDP::VDP1SwapFramebuffer`: `m_state.regs1.LatchEraseParameters()`), `vdp1_regs.hpp` | Same text as upstream `8cdf0a53`; listed because the VDP1 erase tests depend on the latch happening at the swap, in order with the queued commands, so the render thread erases with its own latched parameters. Do not move it. |
| VDP1 `tvmr` parameters | `vdp_renderer_sw.hpp`, `vdp_renderer_sw.cpp`: `VDP2RenderingContext::tvmr`, the `VDP1WriteTVMR` event, and the `const VDP1RegTVMR &tvmr` parameters of `VDP2DrawSpritePixel`/`VDP2FetchSpriteData` (and `regs1.tvmr.*` in `VDP1DoEraseFramebuffer`) | From Ymir `23b3dedd` as ported in Brimir, which upstream `8cdf0a53` has since reshaped (it reads `fbSizeH` and friends from the VDP1 registers). Keep Brimir's shape; the sprite layer and erase tests exercise it. |
| `FBRAMWriteByte` write-back comment | `vdp_renderer_sw.cpp`, `case EvtType::FBRAMWriteByte` ("Also write back to the main state, ...") | Comment only; the code is upstream's. The render thread also writes the byte or word into `m_state.mem1.FBRAM`, since an earlier SwapBuffers/EndDraw copy processed after the CPU write may have overwritten it there. Kept through the FBRAM rename (Ymir `2220b167`). |

## Rules

- Keep Brimir changes minimal and mark each one with a `// Brimir:` comment.
- Log every Brimir change and every upstream SH-2 port in the tables below.
- On an upstream sync of a file with a Brimir-side fix, keep the fix (search for
  `// Brimir:`) unless upstream has fixed the same bug, then drop the marker and
  the row's "kept" note.
- On an upstream Ymir sync, do not copy the fork-scope files wholesale. Review
  upstream SH-2 commits since the last port and apply them by hand.
- After porting an upstream change that touches an SH-2 instruction handler,
  re-run the `[jit]` tests and keep `SH2::JitSetSR`,
  `design/sh2-jit-handler-table.md` and the JIT lowering in sync.

## Brimir changes

| Date | Files | Change |
|---|---|---|
| 2026-09-30 | `sh2.hpp`, `sh2.cpp` | Optional host-time accounting in `SH2::Advance` (`SetHostTimeProfiling`, `ConsumeHostTimeNs`) for the profiler and `brimir_bench`. |
| 2026-09-30 | `sh2_jit_iface.hpp` (new), `sh2.hpp`, `sh2.cpp` | SH-2 JIT hook: `SH2JitContext` + callbacks reusing interpreter helpers (cache emulation off), `SetJitExecutor`, executor dispatch in `Advance<false, false>`, flush on `Reset`/`LoadState`. |
| 2026-09-30 | `sh2.hpp`, `sh2.cpp` | `SH2` copy/move constructors and assignments deleted (the JIT context points into the object); `// Brimir:` marker on the `<chrono>` include used by host-time profiling. |
| 2026-10-01 | `sh2_jit_iface.hpp`, `sh2.hpp`, `sh2.cpp` | MAC pointers and setSR callback in SH2JitContext; Advance hook comment updated. |
| 2026-10-01 | `sh2.hpp`, `sh2_dmac.hpp`, `sh2_frt.hpp` | Initialize members upstream left uninitialized: `m_cyclesExecuted` and `m_WDTBusValue` (0), DMAC `srcAddress`/`dstAddress`/`xferCount` (0, constructor only; Reset still leaves these hardware-undefined registers alone), FRT `TOCR.unused` and `TIER.anyEnabled` (cleared in Reset). All but one only make previously indeterminate values deterministic. **Deliberate behavior change:** clearing `TOCR.unused` in Reset means guest-written TOCR bits 3-2 no longer survive an SH-2 reset (upstream kept them). This matches hardware (TOCR initializes to H'E0 on every reset, and upstream's `TIER.Reset` already clears its unused bits); worth including in the upstream report. With these fixes a fresh SH-2 no longer saves heap garbage into its save state, and on-chip register accesses before the first `Advance` no longer sync the FRT/WDT to a garbage cycle count. Found by lockstep control runs diverging on Linux/macOS. |
| 2026-10-01 | `sh2_jit_iface.hpp`, `sh2.hpp`, `sh2.cpp` | `accessCyclesRMWByte` callback for TAS: `SH2JitContext::accessCyclesRMWByte` → `SH2::JitAccessCyclesRMWByte` = `AccessCyclesRMWByte<false>`. |
| 2026-10-01 | `ymir/sys/bus.hpp`, `sh2_jit_iface.hpp`, `sh2.cpp` | Read-only bus page-table layout for the JIT's inline RAM/ROM accesses and access cycles: `Bus::PageTableLayout` + `Bus::GetPageTableLayout()` (page array pointer, stride, shift, address mask, `offsetof` of `array`, `arrayWritable` and the six cycle fields; `static_assert` that `MemoryPage` is standard layout) and `<cstddef>` include in `bus.hpp`; `SH2JitBusLayout` (same fields) as `SH2JitContext::bus`, copied in `SH2::InitJitContext`. No behavior change: nothing in the core reads it. On an upstream sync of `bus.hpp`, re-apply the accessor and check that `MemoryPage`'s fields and `Read`/`Write`/`IsBusWait`/`GetAccessCycles` still match `src/jit/src/bus_fast_path.cpp` and the x64 emitter. |
| 2026-10-01 | `sh2.cpp` | `static_assert(sizeof(SH2JitBusLayout) == sizeof(sys::SH2Bus::PageTableLayout))` in `SH2::InitJitContext`, so a field added to only one of the two structs fails to compile. No behavior change. |
| 2026-10-02 | `sh2_jit_iface.hpp`, `sh2.cpp` | `SH2JitContext::intcPendingLevel` (read-only pointer to `INTC.pending.level`), set in `SH2::InitJitContext`, so the x64 backend can inline the end of a delay slot (`AdvancePC<..., delaySlot = true>`: `pending = INTC.pending.level > SR.ILevel`). No behavior change: nothing in the core reads it. On an upstream sync, check that `AdvancePC` and `SetupDelaySlot` still match the inline lowering in `src/jit/src/x64/x64_emitter.cpp`. |
| 2026-10-15 | `media/loader/loader_chd.cpp`, `media/loader/chd_hunk_cache.hpp` (new) | `CHDBinaryReader` kept every decompressed hunk it read in an unbounded `std::map`, so memory grew with the disc data read over a session, toward the whole decompressed image (674 MB for Virtua Fighter 2), on top of the `chd_precache` copy when preloading. It now keeps the 64 most recently used hunks (`HunkCache`, about 1.25 MB). A read cache only: no emulation change (IR-JIT executor counters of Panzer Dragoon II Zwei and Street Fighter Zero 3 identical before and after, with and without preload). Measured on a Raspberry Pi 3 without preload over 18,000 frames of attract mode: peak RSS 61.4 MB before (still growing) vs 23.9 MB after (flat) for Virtua Fighter 2, 54.7 vs 24.8 MB for Burning Rangers. Found while investigating a two-core lockstep running out of the Pi's 905 MiB; most of that was the preloaded image (one copy per core), not this cache. Upstream has the same code (checked 2026-10-15); worth reporting. On an upstream sync of `loader_chd.cpp`, keep `HunkCache`. |
| 2026-10-15 | `sh2_host_ticks.hpp` (new), `sh2.hpp`, `sh2.cpp` | Cheaper host-time accounting in `SH2::Advance`: `HostTimeScope` adds `HostTicks()` (`rdtsc` on x86-64, `cntvct_el0` on AArch64, `steady_clock` elsewhere) instead of two `steady_clock` reads per call (38.6 ns per pair, 55.6M `Advance` calls per 1,800 frames, about 1.2 ms/frame with profiling on). `ConsumeHostTimeNs()` converts the ticks with the ratio of ticks to `steady_clock` nanoseconds since `SetHostTimeProfiling(true)` (`HostTicksToNs`, no overflow, 0 without a calibration interval); signature and meaning unchanged. The `<chrono>` include moved from `sh2.cpp` to `sh2_host_ticks.hpp`; `m_hostTimeNs` became `m_hostTicks` plus two calibration members. No emulation change: the counters only feed the profiler and `brimir_bench`. |
| 2026-10-15 | `sh2_jit_iface.hpp`, `sh2.hpp`, `sh2.cpp` | Direct on-chip register reads for the x64 JIT (2D B4): `SH2JitContext::onChipRead` → `SH2::JitOnChipRead`, the partition `0b111` data-read branch of `SH2::MemRead<T, false, false, false>` (misaligned masking, `OnChipRegRead<T, false>`, `OpenBusSeqRead<T>`, unhandled-I/O-area zero) without the partition switch; `OnChipRegRead*` are unchanged. On an upstream sync of `MemRead`, check that the `0b111` case and `JitOnChipRead` still match. |
| 2026-10-19 | `sh2.cpp` | No JIT flush in `SH2::LoadState` (2D A4): compiled code survives a state load. Every cached block is checked against current memory before it runs: IR-only and non-self-validating native blocks by `BlockCache::Get`, self-validating x64 blocks by their prologue (code page arrays and code bytes), reused retired blocks by the reuse check; the executor interprets at a pending interrupt, in a delay slot, and at PC & 2 when the loaded fetch buffer differs from memory. `SetJitExecutor` and `SH2::Reset` still flush. Validated by lockstep runs across state loads (`brimir_bench --lockstep N --lockstep-reload K`). On an upstream sync of `LoadState`, check that it still writes no state the blocks depend on without a check (`design/sh2-jit-m2d.md`, A4). |
| 2026-10-23 | `media/loader/loader_chd.cpp` | Upstream CHD read fixes ported (Ymir `64192c4f`): `PGTYPE` starting with `V` means the pregap is stored in the file, otherwise it is generated as zeros (like the postgap) by `SharedSubviewBinaryReader`; `index01FrameAddress = frameAddress + pregap`, real index 00/01 entries, tracks padded to 4 frames in the file, Saturn header read at the first track's index 01 instead of the track-start sector scan and fixed frame 150. `HunkCache` and `CHDBinaryReader` kept as is. On an upstream sync of `loader_chd.cpp`, keep `HunkCache` and check that the track layout still matches. |
| 2026-10-24 | `hw/vdp/renderer/vdp_renderer_defs.hpp`, `hw/vdp/vdp.hpp`, `hw/vdp/vdp.cpp` | External-renderer hook for the Vulkan renderer (GPU stage 1): `VDPRendererType::External` (name "External"; not in `kRendererTypes`, no `VDPRendererTypeMeta`, since the VDP cannot create it) and `VDP::UseExternalRenderer(factory)`. The factory gets the VDP's `VDPState&`, `vdp2DebugRenderOptions` and `vdp2AccessPatternsConfig`, so the state does not need a public accessor; a `nullptr` or invalid result returns `nullptr` and keeps the current renderer. `UseRenderer<T>` (software and null) and `UseExternalRenderer` share the private `VDP::InstallRenderer`: `PreSaveStateSync()` on the old renderer (drains a threaded software renderer's queues before its destructor joins the threads), carry `Callbacks` and keep the software frame callback in `m_swRendererCallbacks` (so `SetSoftwareRenderCallback` survives software → external → software), `ConfigureEnhancements`, `VDP2SetResolution`, `VDP2SetField`, then **`PostLoadStateSync()` on the new renderer**, then swap. The last one is new for the software renderer too: its CRAM colour cache starts empty and upstream only fills it on CRAM writes and state loads, so an unthreaded software renderer installed mid-session (the fallback from an external renderer) drew palette layers black until the game rewrote CRAM; threaded VDP2 already rebuilt its copy when the render thread started. At construction it runs on a freshly reset CRAM and changes nothing (BIOS lockstep identical). Behaviour change: `UseRenderer` now calls `PreSaveStateSync()` on the old renderer (upstream did not). Call between frames on the emulator thread: VDP1 commands queued but not yet written back are lost, as with a mid-draw save state. On an upstream sync of `vdp.hpp`/`vdp.cpp`, keep `InstallRenderer` (upstream's `Create()`/`PointerResult` rework, `56f76c20`, would need merging into it). Tests: `[vdp][renderer][brimir]` in `tests/unit/test_vdp_external_renderer.cpp`. |

## Brimir-side fixes of upstream bugs

Bugs found in upstream Ymir code (checked against Ymir `8cdf0a53`, 2026-10-18) and fixed only here.
Related, outside `src/core/`: `CoreWrapper::LoadState` (`src/bridge/core_wrapper.cpp`) rejects save states
whose CD block read FIFO cursors are out of range, which upstream's `CDBlock::ValidateState` does not check.

| Date | Files | Bug | Fix | Test |
|---|---|---|---|---|
| 2026-10-24 | `hw/cdblock/cdblock.cpp` | `CmdCalculateActualSize` and `CmdGetSectorInfo` reject partition numbers with `> kNumPartitions`, so partition `kNumPartitions` (24, which does not exist) reaches `PartitionManager::GetBufferCount`/`GetTail` and reads past the partition table. Guest-triggerable; Calculate Actual Size returned a status computed from that memory. | `>=`, so partition 24 is rejected like any other invalid partition. | `HLE partition commands reject partition numbers past the last partition` (`[cdblock-hle][brimir]`) |
| 2026-10-24 | `hw/vdp/renderer/vdp_renderer_sw.hpp`, `vdp_renderer_sw.cpp` | `VDP1BeginFrame` runs on the emulator thread. With threaded VDP1 it read the VDP1 render thread's register copy (`VDP1GetRegs()`, since Ymir `ce270c04`) and, with threaded VDP2, the VDP2 render thread's copy, both of which can lag behind the emulator thread's writes. It then wrote `m_VDP1doubleV`, the deinterlace enhancement's vertical doubling flag, while the VDP1 render thread could still be drawing the previous frame's queued commands. A frame could be drawn with the previous frame's FBCR.DIE value, or its commands with the next frame's doubling. Only affects the deinterlace enhancement in double-density interlace modes. | `VDP1BeginFrame` reads `m_state.regs1`/`m_state.regs2` (the emulator thread's registers) and, with threaded VDP1, sends the flag in a new `VDP1RenderEvent::BeginFrame` event that the render thread applies in order with the commands (unthreaded: set directly, as before). | `Threaded VDP1 applies the double-density setting of each frame in order` (`[vdp][vdp1][threaded][brimir]`); failed 50/50 iterations before the fix. |
| 2026-10-24 | `util/dirty_bitmap.hpp` | The header uses `bit::is_power_of_two` and `std::min` without including `<ymir/util/bit_ops.hpp>` or `<algorithm>`, so it only compiles after other headers. `FindNext` shifts a `uint64_t` by `ones`, which is 64 for a fully set entry (for example after `SetAll`): undefined behavior, harmless only because the shifted value is discarded. The `FindNextGroup` constraint `bit::is_power_of_two(N)` is rejected by MSVC 14.44 for every `N` (member template of a class template, deduced `T`). | Added the two includes and `entry = ones == kBitsPerEntry ? 0 : entry >> ones`; the constraint calls `bit::is_power_of_two<size_t>(N)`. | `DirtyBitmap FindNext walks a fully set entry`, `DirtyBitmap FindNextGroup merges groups with any bit set` (`[util][dirty-bitmap]`) |

## Upstream SH-2 ports

Fork base: upstream Ymir hardware-layer sync of 2026-06-23 plus the backports
listed in `CHANGELOG.md` up to v0.5.4.

| Date | Upstream commit | Files | Notes |
|---|---|---|---|
