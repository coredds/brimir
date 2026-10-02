# SH-2 JIT Milestone 2 — Full Coverage and x64 Backend — Design

**Status**: Part A done; Part B done (exact, 2x target missed; see [sh2-x64-performance.md](sh2-x64-performance.md)); 2C done (x64 about at interpreter speed: interpreter / x64 SH-2 time 0.91–1.25; 2x target still missed, JIT off by default; next steps in sh2-x64-performance.md, "Milestone 2C results")
**Date**: 2026-10-01
**Builds on**: [sh2-jit.md](sh2-jit.md) (milestone 1: IR, block cache, executor, instruction-exact boundaries, lockstep), [sh2-validation.md](sh2-validation.md), [sh2-jit-handler-table.md](sh2-jit-handler-table.md)

## 1. Goals

- **Speed:** SH-2 host time at least **2x lower than Ymir's interpreter** on the six baseline titles (Virtua Fighter 2, Panzer Dragoon II Zwei, Sega Rally Championship, Burning Rangers, Guardian Heroes, Street Fighter Zero 3), measured with `brimir_bench` on x86-64.
- **Exactness unchanged:** with the JIT on, the system stays bit-identical to the interpreter (milestone 1 rules: same cycles, instruction-exact budget and interrupt checks, deviations only as in `sh2-jit.md` §6.5). Lockstep remains the correctness oracle: any divergence is a bug.
- **Coverage:** every SH-2 instruction is compiled except `TRAPA`, `RTE`, `SLEEP` and illegal opcodes.
- **Rollout:** when the speed target is met and lockstep is clean, `brimir_sh2_jit` defaults to on in x86-64 builds. ARM64 keeps the interpreter by default until milestone 3.

Non-goals:
- ARM64 native code (milestone 3).
- Optimizations beyond what the speed target needs: cross-block guest-register caching and IR optimization passes are added only if measurements show they are needed.
- Relaxed or approximate timing modes.

## 2. Structure

Two parts, each with its own implementation plan:

- **Part A — coverage (plan 2A)**, validated with the existing IR-interpreter backend.
- **Part B — x64 backend (plan 2B)**, validated against Ymir's interpreter and the IR-interpreter backend.

```
front end (sh2 -> IR)  ->  block cache  ->  executor (gating, budget, flush)
                                              |
                                  IBackend: Compile(Block) / Run
                                   /                     \
                     IR interpreter (all hosts,      x64 native (x86-64 hosts,
                     reference, fallback)            asmjit)
```

## 3. Part A: remaining instructions

New IR lowering for:
- `MUL.L`, `MULS.W`, `MULU.W`, `DMULS.L`, `DMULU.L`
- `MAC.W`, `MAC.L`, including the `S` bit saturation behavior
- `DIV0S`, `DIV0U`, `DIV1`
- `TAS.B` (read-modify-write with `AccessCyclesRMWByte` cycles)
- memory forms `LDC.L @Rm+,{SR,GBR,VBR}`, `LDS.L @Rm+,{MACH,MACL,PR}`, `STC.L {SR,GBR,VBR},@-Rn`, `STS.L {MACH,MACL,PR},@-Rn`

Rules:
- Each instruction's exact semantics, cycles (including any multiplier-related or write-back stalls), `m_wbReg` and interrupt-allow behavior are first transcribed from `sh2.cpp` into `sh2-jit-handler-table.md`, as in milestone 1D.
- Multi-step operations with intricate bit-level behavior (`DIV1`, `MAC.W`, `MAC.L`) may be IR ops whose backend implementation calls a small pure helper that transcribes the handler. A one-to-one copy is preferable to re-deriving the arithmetic.
- `LDC.L SR` recomputes interrupt-pending like `LDC SR` (existing `SetSR`). All the memory forms clear interrupt-allow (existing rule).
- Validation: the table-driven opcode tests, delay-slot tests, edge values, fuzz generator and lockstep runs from milestone 1D, extended to the new instructions.

## 4. Part B: x64 backend

### 4.1 Dependency

- [asmjit](https://github.com/asmjit/asmjit), zlib license (GPL-compatible), vendored under `vendor/asmjit`, built only when the target is x86-64. Only its `x86` and `core` parts are compiled in milestone 2. The `a64` part is for milestone 3.
- Executable memory comes from asmjit's `JitRuntime`, which handles W^X (and Apple `MAP_JIT` for milestone 3).

### 4.2 Backend interface

- `IBackend` in `brimir-jit` with `Compile(const Block&) -> CompiledBlock` and `Run(const CompiledBlock&, SH2JitContext&, uint64 target, const bool *abort) -> ExitInfo`.
- Implementations: `IrInterpreterBackend` (the existing `RunBlock`) and `X64Backend`.
- The executor chooses the backend per host: x64 on x86-64, IR elsewhere.
- Tests and `brimir_bench --jit-backend ir|x64` can force either one.

### 4.3 Compiled block

- Each block becomes one function `uint64 block(ctx*, uint64 target)`, returning cycles and writing the rest of `ExitInfo` (`retired`, `busWait`, `aborted`, `boundary`) to a small result struct.
- **Guest state:** a callee-saved host register holds the SH-2 object's address. All guest state (`R0`–`R15`, `PC`, `PR`, `SR`, `GBR`, `VBR`, `MACH`/`MACL`, delay-slot flag and target, fetch buffer, `m_wbReg`, interrupt flags, `m_cyclesExecuted`) is accessed as `[base + offset]`. The offsets come from the `SH2JitContext` pointers, which all point into the same object, so nothing about the SH-2 layout is hard-coded.
- **Values:** SSA values get host registers from a linear-scan allocator, spilling to stack slots. Blocks are at most 32 instructions. Values live across calls are kept in callee-saved registers or spilled.
- **Cycles:** the block's cycle counter lives in a host register.
- **Calls:** asmjit's function-frame support handles the Windows x64 and System V calling conventions.

### 4.4 Memory access (exact by construction)

**Fork addition.** `ymir::sys::Bus` gets one read-only public accessor in `src/core/include/ymir/sys/bus.hpp`. It exposes the page table base pointer and the offsets of the page fields: `array`, `arrayWritable`, and the read/write cycles for 8/16/32 bits. It changes no behavior. This extends the fork scope to that file, and the addition is marked `// Brimir:` and logged in `src/core/BRIMIR_FORK.md`.

**Inline sequence** for a data access of size *N*, mirroring `SH2::MemRead`/`MemWrite`, `SH2::AccessCycles` and `Bus::Read`/`Write`/`GetAccessCycles` with cache emulation off:

1. **Partition** = `addr >> 29`. The fast path handles only `0b000` (cached area: 1 access cycle, data via the bus) and `0b001`/`0b101` (cache-through: page access cycles, data via the bus). Every other partition calls the existing slow-path callback: on-chip I/O, cache address/data arrays, associative purge.
2. **Page lookup:** `page = pages[(addr & 0x7FFFFFF) >> 16]` (`SH2Bus` = `Bus<27, 16>`), after the same misalignment masking as the interpreter.
3. **Array page** (`page.array != nullptr`): do an inline big-endian load, or for a store, store only if `arrayWritable` (`Bus::Write` silently drops the write otherwise). The bus-wait check is skipped, because array pages never wait (`Bus::IsBusWait`). Cycles come from the page's cycle field, or 1 for the cached partition.
4. **Other pages:** call the slow-path callback as in milestone 1. The page's handler, cycles and bus-wait check apply.

`SyncCycles` stores `m_cyclesExecuted` only before slow-path calls. Timers are reached only through the slow path, so this is exact.

Instruction fetch reads (`MOV.W/L @(disp,PC)`), refills and check-on-entry use the same fast path with the instruction-fetch flag.

### 4.5 Boundaries, stalls, helpers

- **CheckBoundary:** an inline compare of the cycle counter against the target, plus one 16-bit test of the interrupt pending and allow flags (the interpreter's `kIntrFlagsPendingAllowed` check).
- **Write-back stall:** an inline compare of `m_wbReg` against the instruction's register mask.
- **Helper calls:**
  - `noexcept` C++ helpers implement delay-slot setup and end, `SetSR`, `DIV1`, `MAC` and the other multi-step operations.
  - After every helper or slow-path call that can reset the CPU or flush the cache, generated code tests the abort flag. If set, it returns immediately without writing `PC`, the same as the IR backend.
  - Flushes requested during a block stay deferred until the block returns, so generated code never frees the code it is running.
- **No exceptions across generated code:** helpers catch everything and report failures through the abort flag and an error record.

### 4.6 Block linking (only if measured necessary)

Without linking, every block returns to the executor's dispatcher. If profiling shows dispatch is a significant cost after 4.3–4.5 are in place:
- A constant exit jumps to the target block's **entry prologue**. The prologue repeats the executor's checks inline: budget, interrupt pending and allowed, delay slot, the fetch-buffer gate at `PC & 2`, and check-on-entry (compare the stored opcodes through the RAM fast path). If any check fails, it returns to the dispatcher.
- Links are patched lazily and cleared on every flush.

### 4.7 Code cache

- Per-CPU code memory from `JitRuntime` has a size cap. Reaching the cap triggers a full flush (deferred if inside a block).
- The flush rules from milestone 1 apply unchanged: reset, save-state load, executor attach, and the size cap.

### 4.8 Error handling

- If asmjit fails to emit a block (out of code memory, unsupported pattern), that block runs on the IR backend instead. The PC is marked so it is not retried, and the failure is logged once.
- The IR backend is always compiled in and serves as the fallback.

### 4.9 Plan 2B refinements

Plan `design/plans/2026-10-01-sh2-jit-m2b-x64-backend.md` refines this section as follows. Where they differ, these take precedence over 4.2–4.8.

1. **Register allocation (4.3):** asmjit's `x86::Compiler` (virtual registers, spilling and the call ABI) replaces the hand-written linear-scan allocator. Blocks are short (at most 32 guest instructions) and compiled once, so compile time does not matter, and this removes the riskiest hand-written component. *Note (2B measurements):* this assumption did not hold. Compiling takes about 100–130 µs per block, and games that overflow the block cache or keep reaching new code stall for up to ~260 ms while recompiling (see [sh2-x64-performance.md](sh2-x64-performance.md)); 2C addresses this.
2. **Interface (4.2, 4.8):** `INativeBackend` (`brimir/jit/backend.hpp`) covers native code only. The "IR backend" is the absence of a native backend: the executor runs every block with `RunBlock`, which stays the fallback and the reference. `BackendKind` (`ir`, `x64`) selects it; `DefaultBackend()` is `x64` in x86-64 builds (CMake option `BRIMIR_JIT_X64`), else `ir`.
3. **Guest state addressing (4.3):** generated code loads `ctx->R` once and addresses every other state field as `[R + offset]`. Each offset is computed at compile time from the `SH2JitContext` pointers. If an offset does not fit in 32 bits, compilation fails and the block runs on `RunBlock`.
4. **Compile failures (4.8)** are counted in `Executor::Stats::compileFallbacks` (printed by `brimir_bench`) instead of logged.
5. **Exceptions (4.5):** trampolines out of generated code are `noexcept`; they catch every exception, store it and stop the block. The backend rethrows it after the generated code returns, so callers see the same exception as with `RunBlock`.
6. **Block linking (4.6)** is not in plan 2B. If plan 2B misses the speed target, the profile goes into a follow-up plan (2C) that adds linking.

## 5. Validation

1. **Part A:** milestone 1D's opcode, delay-slot, edge-value, interrupt-timing and fuzz tests, extended to the new instructions, plus lockstep with the IR backend.
2. **Both backends:** every `[jit]` test runs against both backends.
3. **Backend vs backend:** each fuzz program is compiled with both backends and the results are compared directly. This catches codegen bugs as well as semantics bugs.
4. **x64 unit tests:**
   - allocator spilling and values live across calls;
   - fast path versus slow path for every partition and page type, including read-only array pages, MMIO pages, misaligned addresses, and the cached partition's 1-cycle cost;
   - the abort path after each helper kind;
   - the code-cache cap and flush.
5. **Lockstep:** x64 backend identical to the interpreter for 36,000 frames on the BIOS and the six titles. CI lockstep tests run with each platform's default backend (x64 on Windows and Linux, IR on macOS ARM64).

## 6. Measurement and rollout

- `brimir_bench --jit-backend ir|x64`.
- **Report:** `design/sh2-x64-performance.md` gives ms/frame and SH-2 time for the interpreter, the IR backend and x64 on the six titles. It uses the same machine and method as `sh2-baseline.md`.
- **If the target is missed:** profile, then add block linking (4.6) and only then optimization passes, re-measuring after each.
- **When the target is met and lockstep is clean:**
  - `brimir_sh2_jit` defaults to `enabled` on x86-64 builds and stays `disabled` on ARM64;
  - the option keeps its "(Experimental)" label for one release;
  - README, CHANGELOG and `sh2-jit.md` are updated.
