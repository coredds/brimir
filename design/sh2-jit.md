# SH-2 JIT Compiler — Design

**Status**: Milestone 2 in progress: full instruction coverage done (plan 2A); x64 backend done and exact, but the 2x target was missed (plan 2B, see [sh2-x64-performance.md](sh2-x64-performance.md)); optimization next (plan 2C)
**Date**: 2026-09-30
**Scope of this document**: overall architecture for all milestones, detailed scope for milestone 1

## 1. Goals and non-goals

### Goals

- Speed up emulation of the two Saturn SH-2 CPUs with a dynamic recompiler.
- Portable design: one architecture-neutral IR with **x64 and ARM64** native backends (milestones 2 and 3). Targets range from desktop x64 to Cortex-A53/A55-class ARM64 handhelds.
- Keep Ymir's timing model: compiled code computes **the same cycle cost per instruction** as the interpreter, including bus wait states and write-back stalls.
- Keep the emulated system identical to the interpreter (instruction-exact boundaries, section 2).
- Measure before optimizing: know how much of frame time the SH-2s actually use.

### Non-goals

- JIT for the SH-1 (CD block LLE) or the M68K.
- Running compiled code with SH-2 cache emulation enabled, or with debug tracing / breakpoints active (these stay interpreter-only).
- Changing the save-state format.
- Speed in milestone 1 (the first backend is an IR interpreter and may be slower than Ymir's interpreter).

## 2. Timing model

- Each compiled block accumulates exactly the cycles Ymir's `InterpretNext()` would have returned for the same instructions (fixed costs, `AccessCycles` wait states from the bus page table, pipeline refills, `WritebackCycles` load-use stalls).
- **Instruction-exact boundaries.** Before every instruction after the first, a block makes the same two checks the interpreter makes before every instruction (IR op `CheckBoundary`): the `Advance()` cycle budget (`m_cyclesExecuted < target`) and the interrupt check (pending and allowed). A block therefore stops at exactly the instruction where the interpreter stops and takes interrupts at the same instruction, so a JIT-enabled system runs identically to the interpreter. This is verified by whole-system lockstep runs (section 7.2).
- This replaced the original block-granular checks (changed during milestone 1). Those let a block overshoot the master/slave sync step in `Saturn::Run`, and the overshoot delayed the slave SH-2, SCU, VDP and scheduler by up to a whole block.
- The remaining deviations are listed in section 6.5.

## 3. Ownership: forking the SH-2

The ROADMAP rule "Ymir hardware layer stays verbatim upstream" gets one exception.

- **Fork in place**: the SH-2 files keep their paths and namespace (`ymir::sh2`) but become Brimir-owned:
  - `src/core/include/ymir/hw/sh2/*`
  - `src/core/src/ymir/hw/sh2/*`
- Brimir changes inside the fork are kept to what the JIT needs (a state-access context and the executor hook in `SH2::Advance`). In practice only `sh2.hpp` and `sh2.cpp` should change.
- Upstream Ymir SH-2 fixes are ported by hand. Every port and every Brimir-specific change is logged in `src/core/BRIMIR_FORK.md` (upstream commit, date, files, notes).
- Everything outside the fork scope stays verbatim, and `Saturn`, save-state and debugger code are not modified.
- All JIT code lives outside `src/core` in a separate library.

## 4. Architecture

```
src/core/.../hw/sh2/        (ymir::sh2 — Brimir-owned fork)
  SH2::Advance()
    ├─ JIT enabled and eligible ──> jit::Executor::Run(ctx, cycleTarget)
    └─ otherwise ─────────────────> InterpretNext() loop (unchanged)
  SH2JitContext  (narrow state-access struct, defined in the fork)

src/jit/                    (new library: brimir-jit)
  frontend/    guest block -> IR, uses Ymir's DecodeTable opcode classification
  ir/          IR types, builder, verifier, printer
  cache/       per-CPU block cache, check-on-entry invalidation
  backend/interp/   IR interpreter (milestone 1)
  backend/x64/      native x64 (milestone 2)
  backend/arm64/    native ARM64 (milestone 3)
  executor     dispatch loop

tools/brimir_bench/         headless frame benchmark (replaces tools/benchmark_sh2)
tests/unit/test_jit_*.cpp   Catch2 differential tests
```

### 4.1 SH2JitContext

A plain struct the fork fills in once per `SH2` instance. It points at the live SH2 fields instead of copying them, so the JIT holds no architectural state of its own:

- registers `R[16]`, `PC`, `PR`, `GBR`, `VBR`, `SR`, `MACL`, `MACH`
- pipeline state: delay-slot flag and target, `m_wbReg`, the 32-bit instruction fetch buffer
- interrupt state: `intrPending`, `intrAllow`
- `m_cyclesExecuted`, kept current by the executor before every interpreter call and (via the `SyncCycles` IR op) before every memory access, because on-chip timers (WDT, FRT) read it
- an opaque owner pointer passed to every callback
- callbacks into the fork: `interpretOne` (`InterpretNext<false, false>()`), `read`/`write` (`MemRead`/`MemWrite` with cache emulation off), `peekInstruction` (side-effect-free fetch for decoding and validation), `accessCycles`, `busWait`, `refillPipeline`, `setupDelaySlot`, `endDelaySlot`, `setSR` (`SH2::JitSetSR`: the `LDC Rm,SR` state change -- mask to `0x3F3`, recompute the pending interrupt, clear interrupt-allow)

`brimir-jit` includes only `ymir/core/types.hpp`, `sh2_jit_iface.hpp` and (in the front end) `sh2_decode.hpp`, not `sh2.hpp` or the bus header.

### 4.2 Eligibility

`SH2::Advance` dispatches to the executor only when all of these hold:

- the `brimir_sh2_jit` core option is enabled (default **off**: validated against the interpreter, but the IR-interpreter backend is still slower than Ymir's interpreter)
- debug tracing is off (`debug == false` template instance)
- SH-2 cache emulation is off (`emulateCache == false`), which also excludes games with the `ForceSH2Cache` flag

Otherwise the unchanged interpreter loop runs.

### 4.3 Executor loop

```
while cycles < target:
    *ctx.cyclesExecuted = cycles            // on-chip timers read it
    if in delay slot or (interrupt pending and allowed):
        cycles += ctx.interpretOne()        // Ymir handles slot / entry + acknowledge
        continue
    if PC & 2 and fetch buffer low half != memory at PC:
        cycles += ctx.interpretOne()        // run the buffered opcode, as the interpreter does
        continue
    block = cache.Get(PC)                   // compile on miss; check-on-entry recompiles on change
    if block is empty (first opcode unsupported):
        cycles += ctx.interpretOne()
        continue
    intrAllow = true
    cycles += backend.Run(block, ctx, target) // flushes requested inside are deferred (section 6.5)
*ctx.cyclesExecuted = cycles
```

`backend.Run` receives `target`; the block itself stops before any later instruction once the budget is used up or an interrupt becomes pending (`CheckBoundary`), exactly like the interpreter loop.

`SLEEP` is handled the same way as in the interpreter: `Advance` returns early when the CPU is asleep, before the executor runs.

## 5. Blocks and IR

### 5.1 Block formation

Decoding starts at the guest PC. A block ends:

- after a branch and its delay slot
- before `SLEEP`, `TRAPA` and `RTE` (interpreter fallback)
- **before** any opcode the front end does not support yet (the executor interprets it)
- at a length cap of 32 guest instructions (tunable)

A branch inside a delay slot (illegal on the SH-2) is left to the interpreter.

Register forms of `LDC`/`LDS`/`STC`/`STS` (including `LDC Rm,SR` and `LDC Rm,VBR`) are compiled inside blocks. They clear interrupt-allow, so the interpreter accepts no interrupt before the next instruction and sets allow back to true just before executing it. The front end reproduces this rule:

- the instruction emits `ClearIntrAllow` (`LDC Rm,SR` emits `SetSR(value, delaySlot)` instead, which also clears allow and recomputes the pending flag from the new mask)
- the next instruction in the same block gets its `CheckBoundary` as usual (it can still stop on the cycle budget, but cannot take an interrupt), followed by `SetIntrAllow`
- if the allow-clearing instruction is the last one in the block (block end or delay slot), nothing more is emitted: the executor's pre-entry check (`pending && allow`) and its `intrAllow = true` at block entry behave exactly like `InterpretNext`

So an interrupt unmasked by `LDC Rm,SR` is taken after exactly one further instruction, as in the interpreter.

### 5.2 IR

Linear, single-assignment within a block, about 55 operations, designed to map directly to x64 and ARM64:

- **guest state**: load/store general register, `PC`, `PR`, `GBR`, `VBR`, `MACH`/`MACL`, `SR` and individual `SR` bits (T, S, Q, M, interrupt mask)
- **ALU (32-bit)**: add, sub, and, or, xor, not, neg, shifts and rotates (including through T), sign/zero extend, compare to T, add/sub with carry and overflow into T, multiply, the division steps (`DIV0S`, `DIV0U`, `DIV1`)
- **memory**: `Load8/16/32`, `Store8/16/32` with a RAM fast path and a bus-handler slow path
- **control**: conditional exit, exit to a constant target, exit to a register target, boundary check (`CheckBoundary`: cycle budget and pending interrupt before an instruction)
- **cycles**: `AddCycles(const)`, `AddAccessCycles(size, read|write, addr)` (reads the bus page wait-state table at run time)

The IR ships with a builder, a verifier (checks types, single assignment, terminators) and a text printer used in test failure messages.

System-register and interrupt-flag ops: `GetGBR`/`SetGBR`, `GetVBR`/`SetVBR`, `GetPR`/`SetPR`, `GetSR`, `SetSR(value, delaySlot)` (the `LDC Rm,SR` state change through a core callback: mask to `0x3F3`, recompute pending, clear allow), `GetMACH`/`GetMACL`/`SetMACH`/`SetMACL`, `ClearIntrAllow`, `SetIntrAllow` and `GetDelayTarget` (the delay-slot target for PC-relative slot instructions under a dynamic-target branch). The calls `BSR`, `BSRF` and `JSR` write `PR` with `SetPR` before the slot; `BRAF`, `BSRF` and `JSR` have dynamic targets. Section 5.1 gives the interrupt-allow rule these ops implement.

### 5.3 Cycle-fidelity rule

For every block, the cycles computed by the IR must equal the sum the interpreter returns for the same instructions:

- fixed per-opcode costs are taken from the interpreter's handlers
- memory access costs use the same bus page table (`GetAccessCycles`) with cache emulation off, and the same partition rules as `SH2::AccessCycles` (for example, 4 cycles for on-chip I/O)
- pipeline refills (`RefillPipeline`) cost the same as in the interpreter
- load-use stalls: each instruction's `WbStall` op is evaluated at run time against the live `m_wbReg` (a register mask per instruction), and loads update `m_wbReg` with `SetWb`, exactly as the interpreter does.

The differential tests (section 7.1) compare cycles as well as state.

### 5.4 Delay slots

A delayed branch computes its target, runs the slot instruction, then exits to the target. This matches the interpreter's `m_delaySlot` / `m_delaySlotTarget` behavior, including the delay-slot opcode table.

## 6. Block cache, invalidation, memory

### 6.1 Block cache

- One cache per CPU (the CPUs run different code and each has its own on-chip cache data array).
- Keyed by the full guest PC. Block exits write constant PCs that include the partition bits, so the cached (`0x0xxxxxxx`) and cache-through (`0x2xxxxxxx`) aliases of the same code get separate blocks.
- A block starting at `PC & 2` runs only if the fetch buffer's low halfword matches memory; otherwise the interpreter executes the buffered opcode, as the hardware would.
- `std::unordered_map` keyed by the full PC. Each block stores its start PC and a copy of its original opcodes.
- Size cap: `kMaxCachedInsts` = 1M IR instructions per CPU (about 20 MB). When it is reached, the whole cache is flushed before the next compile.
- If the front end ever produced a block that fails verification, a fallback empty block is cached instead, so that PC always runs on the interpreter.

### 6.2 Invalidation (milestone 1)

- **Check on entry**: before running a block, compare its stored opcodes (at most 64 bytes) with current memory through `peekInstruction`. On mismatch, drop and recompile.
- This is correct regardless of who wrote the code: either CPU, SH-2 DMAC, SCU DMA, CD transfers, or save-state loads. No Ymir write paths need hooks.
- Full flush on reset, save-state load and executor attach (JIT option toggle). New content needs no extra flush beyond the reset that boots it: check-on-entry catches changed code. Toggling cache emulation needs no flush: compiled code never runs while it is on, and when it is turned off, check-on-entry and the fetch-buffer gate (section 4.3) catch any code or buffer that changed meanwhile.
- Per-page dirty tracking is deferred until profiling shows the entry check matters. It requires hooking RAM write paths.

### 6.3 Memory access

- Compiled code uses the same bus page table as the interpreter.
- Pages with a direct `array` pointer: inline big-endian load/store plus the page's wait-state cycles.
- All other pages (MMIO, on-chip I/O at `0xFFFFxxxx`, cache address/purge regions): call the slow-path helper, which behaves exactly like `MemRead` / `MemWrite` with cache emulation off.
- Misaligned accesses: same masking as the interpreter. Ymir raises no address-error exception and neither does the JIT.
- Milestone 1 routes every access (including RAM) through the fork's own `MemRead`/`MemWrite`/`AccessCycles`/`IsBusWait` via context callbacks, which is exact by construction. The inline RAM fast path above is a milestone 2 optimization.

### 6.4 Save states, rewind, run-ahead

The JIT holds no architectural state between blocks, so the save-state format does not change. Loading a state flushes both block caches. Rewind and run-ahead keep working, with the flush as the only added cost.

### 6.5 Known deviations

- Self-modifying code: any write into the currently executing block's own code -- a store in any addressing mode, a read-modify-write (`AND.B`/`OR.B`/`XOR.B #imm,@(R0,GBR)`, `TAS.B`), or a DMA transfer started by a store -- takes effect at the next block entry (check-on-entry), whereas the interpreter fetches fresh opcodes at every aligned PC and so sees the change at the next instruction fetch.
- Reset inside an instruction: a compiled access to the WDT registers can trigger a watchdog reset, which calls `SH2::Reset` and flushes the executor. The flush is deferred until the block returns, and the block is aborted right after that access without writing `PC`. The interpreter instead finishes the current instruction after the reset (for example `PC += 2` from the reset vector). Both are artifacts of a reset happening inside an instruction. An aborted block returns only the cycles accumulated before the abort; the interpreter would return the whole instruction's cost.
- Dev-log lines that print the current PC (for example on-chip register access traces) show the block's start PC for accesses made by compiled code, because the JIT does not update `PC` inside a block. Emulated state is unaffected.

## 7. Validation

### 7.1 Strict differential tests (CI)

Catch2, tag `[jit]`, in `tests/unit/`:

- an isolated SH2 on a synthetic RAM-only bus, no interrupts
- **per instruction**: every supported opcode, many randomized register states. The interpreter and the JIT start from identical state and memory. Afterwards the full architectural state, pipeline state, `m_wbReg`, memory contents and cycle totals must match exactly.
- **random sequences**: generated blocks of supported instructions including branches and delay slots, with a fixed seed in CI (seed printed on failure)
- IR verifier and printer unit tests

### 7.2 Whole-system lockstep (CI and real games)

Because blocks stop at the interpreter's instruction boundaries (section 2), a core running the JIT and a core running the interpreter must stay identical. `brimir::RunLockstep` runs two `CoreWrapper` instances frame by frame and compares, after every frame: both SH-2s (field level, including timers, DMAC, cache arrays and the pending interrupt), the slave SH-2 enable flag, low and high work RAM, the 32 KiB internal backup RAM and the contents of an inserted backup memory cartridge (neither is part of the save state), the full save state of every other subsystem (scheduler, system, SCU, SMPC, VDP, SCSP, CD block -- HLE or SH-1/YGR/CD drive/DRAM when LLE -- and spillover counters; compared per subsystem), audio samples and the output frame. Threaded VDP rendering is turned off for lockstep runs, and the RTC runs in virtual mode (emulated time) instead of reading the host clock, which otherwise makes two interpreter cores diverge in the BIOS.

Both cores start from identical state: once set up (content, BIOS, test workload), `brimir::SyncLockstepCores(src, dst)` saves `src`'s full Saturn save state into a zero-filled buffer and loads it into both cores (which also flushes their SH-2 JITs), then copies differing internal/cartridge backup RAM bytes, which are outside the save state. This is needed because some upstream Ymir members are never initialized, so two freshly constructed cores carry different heap garbage. On Linux and macOS this made interpreter-vs-interpreter control runs fail at frame 1, intermittently; Windows passed, probably because there the multi-MB `Saturn` allocation always comes from fresh zeroed pages while glibc and libmalloc can reuse previously freed memory. Synchronizing hides garbage in serialized fields. Uninitialized state that is not serialized is not reset; if it ever affects emulation, the cores diverge and the comparison reports it later through the serialized state it changes. A test build that filled every new allocation with `0xA5` (versus `0x00` for the other core) reproduced the CI divergences exactly and ran 600 frames (null IPL, synthetic workload, interpreter and JIT) and 1200 BIOS frames identically after synchronization.

Known upstream Ymir issue (to report upstream): fields left uninitialized by constructors and `Reset` (found with that test build; offsets are MSVC x64 layout; the VDP offset reported by Linux CI is the same):

- VDP: `VDP1RegsSaveState::nextCommandAddress` (save-state offset 1577024), `renderer.vdp1State.doubleV` (1577332, the CI `vdp state differs (byte offset 1577332)`), `lastCellX` of all 12 VRAM fetchers.
- SCU: every DMA channel's `xfer` transfer state (`buf` .. `started`), DSP `dmaAddrD0`.
- SCSP: per-slot `PLFOS`, `currEGLevel`, `egAttackBug`, `finalLevel`; the MIDI output buffer.
- CD block (HLE): `seekTicks`, `xferSectorPos`, `xferSectorEnd`, `xferPartition`, `xferGetLength`, `xferDelCount`, `xferExtraCount`, and the sector buffers (data and metadata).
- SH-2 (fixed in the Brimir fork, see `src/core/BRIMIR_FORK.md`): `m_cyclesExecuted` (on-chip register accesses before the first `Advance` synced the FRT to a garbage cycle count: the synthetic workload's `master SH-2 R1 differs`), `m_WDTBusValue`, DMAC `SAR`/`DAR`/`TCR`, FRT `TOCR` bits 3-2 (`frt.TOCR differs: a=0xE0 b=0xE8`) and `TIER.anyEnabled`.

- A control run (two interpreter cores) proves the emulator is deterministic, so a JIT divergence points at the JIT.
- CI runs the null-IPL smoke test plus a synthetic interrupt-driven workload (master SH-2 loop with WRAM traffic and FRT compare-match interrupts, 240 frames), each with an interpreter-vs-interpreter control. The workload's loop reads FRCH: that read advances the FRT inside the compiled block, so the compare-match interrupt becomes pending mid-block and exercises the block's interrupt boundary checks (otherwise the FRT advances only between `Advance` slices and the interrupt is always pending at block entry). The slave SH-2 and the other subsystems' interrupts are covered only by BIOS and game runs. (When a BIOS is present in `tests/fixtures/`, the test suite also runs it in lockstep for 600 frames; CI has none.)
- Real games: `brimir_bench --lockstep N` (section 7.3).

This replaces the shadow-verify mode planned earlier: lockstep checks the whole system, including bus side effects that shadow-verify had to skip.

### 7.3 Game-level regression (local)

`brimir_bench --bios <bios> --game <game> --system-dir <dir> --lockstep N` runs N frames of real content on a JIT core and an interpreter core and reports the first divergence (exit code 3). It requires the user's own BIOS and discs, so it does not run in CI. Game validation results (milestones 1 and 2A) are in [sh2-validation.md](sh2-validation.md).

## 8. Measurement

- Add per-frame SH-2 timing (master and slave `Advance`) to the existing profiler, next to VDP and SCSP.
- `tools/brimir_bench`: headless, loads BIOS + content + optional save state, runs N frames, reports ms/frame and per-component shares; `--sh2-jit` switches from the interpreter to the JIT and adds per-CPU executor stats (`blocksRun`, `interpreted`). It replaces the `tools/benchmark_sh2` micro-benchmark, which only times isolated operations.
- The first deliverable of milestone 1 is a baseline report (committed under `design/`) with the SH-2 share of frame time in several games, on at least one x64 machine.

## 9. Error handling

- A block that fails to compile marks its PC interpreter-only.
- Block cache full: flush.
- Host memory allocation failure: disable the JIT for the session and log it.
- The JIT never changes emulation results silently. With `brimir_sh2_jit` off, behavior is byte-identical to today.

## 10. Milestones

### Milestone 1 — foundation and measurement (this document)

1. Fork bookkeeping: `src/core/BRIMIR_FORK.md`, ROADMAP policy exception.
2. SH-2 profiling, `tools/brimir_bench`, and the baseline report.
3. `SH2JitContext`, the executor hook, the `brimir_sh2_jit` core option, and the eligibility rules.
4. `brimir-jit` library:
   - IR, verifier, printer
   - front end for every SH-2 instruction except multiply/divide-step/MAC, `TAS`, `TRAPA`/`RTE`/`SLEEP`, the memory forms of `LDC`/`LDS`/`STC`/`STS` and illegal opcodes (list in [sh2-validation.md](sh2-validation.md), "Opcodes still interpreted"); exact per-opcode lowering in [sh2-jit-handler-table.md](sh2-jit-handler-table.md)
   - block cache with check-on-entry
   - IR-interpreter backend
   - fallback to the interpreter for the remaining opcodes
5. Validation layers 7.1 (in CI) and 7.2.

**Done when** (all met; see [sh2-validation.md](sh2-validation.md)):

- [x] all strict differential tests pass with exact state and cycle totals
- [x] with the JIT on, the BIOS and a set of games run 10 minutes each (36000 frames) in lockstep with the interpreter without divergence (BIOS menu plus six games)
- [x] the baseline report is committed ([sh2-baseline.md](sh2-baseline.md))

### Milestone 2 - full coverage and x64 backend

Design: [sh2-jit-m2.md](sh2-jit-m2.md). Remaining instructions (multiply, MAC, divide step, TAS, memory forms of LDC/LDS/STC/STS; done in milestone 2A), then native x64 code generation from the IR with asmjit, staying bit-exact. Target: SH-2 time at least 2x lower than the interpreter, after which the JIT becomes the default on x86-64.

Outcome of 2B: the x64 backend is exact (BIOS menu plus six games, 36000 frames each, identical in lockstep with the interpreter; [sh2-validation.md](sh2-validation.md)), but the 2x target was missed: it takes 1.7–2.2x the interpreter's SH-2 time and stalls while compiling new code ([sh2-x64-performance.md](sh2-x64-performance.md)). The JIT stays off by default. Milestone 2C optimizes the backend along that report's conclusions (refill and delay-slot calls, block linking, per-block bookkeeping, cache capacity and compile cost).

### Milestone 3 — ARM64 backend (separate spec)

Same as milestone 2 for ARM64, validated on a Cortex-A53/A55-class device.

## 11. Open questions (to settle in later specs)

- Block linking and the dispatch fast path (milestone 2).
- Whether per-page dirty tracking is needed (after profiling milestone 1).
- JIT support with cache emulation enabled (possibly never).
