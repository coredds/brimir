# Brimir fork of the Ymir SH-2

The Ymir hardware layer under `src/core/` is a verbatim copy of upstream
[Ymir](https://github.com/StrikerX3/ymir), with one exception: the SH-2 CPU is
Brimir-owned so the SH-2 JIT (see `design/sh2-jit.md`) can hook into it.

## Fork scope

- `src/core/include/ymir/hw/sh2/*`
- `src/core/src/ymir/hw/sh2/*`

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

## Upstream SH-2 ports

Fork base: upstream Ymir hardware-layer sync of 2026-06-23 plus the backports
listed in `CHANGELOG.md` up to v0.5.4.

| Date | Upstream commit | Files | Notes |
|---|---|---|---|
