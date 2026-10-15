# Brimir fork of the Ymir SH-2

The Ymir hardware layer under `src/core/` is a verbatim copy of upstream
[Ymir](https://github.com/StrikerX3/ymir), with these exceptions: the SH-2 CPU is
Brimir-owned so the SH-2 JIT (see `design/sh2-jit.md`) can hook into it, and the
CHD reader's hunk cache is bounded for low-memory hosts.

## Fork scope

- `src/core/include/ymir/hw/sh2/*`
- `src/core/src/ymir/hw/sh2/*`
- One read-only accessor in `src/core/include/ymir/sys/bus.hpp`
  (`Bus::PageTableLayout`, `Bus::GetPageTableLayout`) for the SH-2 JIT.
- The CHD reader's hunk cache: `src/core/src/ymir/media/loader/loader_chd.cpp`
  (`CHDBinaryReader`) and the new `src/core/include/ymir/media/loader/chd_hunk_cache.hpp`.

Everything else under `src/core/` stays verbatim upstream.

## Rules

- Keep Brimir changes minimal and mark each one with a `// Brimir:` comment.
- Log every Brimir change and every upstream SH-2 port in the tables below.
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

## Upstream SH-2 ports

Fork base: upstream Ymir hardware-layer sync of 2026-06-23 plus the backports
listed in `CHANGELOG.md` up to v0.5.4.

| Date | Upstream commit | Files | Notes |
|---|---|---|---|
