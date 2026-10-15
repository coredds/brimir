# SH-2 JIT Milestone 2D (Step 0 and Track B) - x64 Per-Instruction Cost Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make generated x64 code cheaper per guest instruction on every title, measured honestly (cheaper bench host-time accounting first), while staying lockstep-exact. This plan covers design steps 0 and 1 (track B); track A (long sessions) gets its own plan once this one's measurements are in.

**Architecture:** Task 1 makes `SH2::Advance`'s host-time accounting cheap and re-baselines. Tasks 2-6 each remove one measured per-instruction cost: guarded runs of boundary checks (an IR op both backends implement, inserted by `OptimizeBlock`), inline `DIV1`, a direct on-chip read path, compile-time page resolution for constant addresses, and the native compile threshold. Every task ends with the progress measurement, so later tasks can be re-ordered or dropped from data. Task 7 validates and reports.

**Tech Stack:** C++20, CMake 3.28+, asmjit (vendored), Catch2 (amalgamated), MSVC 2022 / GCC 14 / Apple Clang.

**Spec:** `design/sh2-jit-m2d.md` (sections 3-6, 8). **Evidence:** `design/sh2-x64-performance.md` ("Milestone 2C results"). **Semantic reference:** `src/jit/src/interp_backend.cpp` (`RunBlock`) and `src/jit/src/sh2_helpers.cpp`.

## Global Constraints

- **Exactness:** with the JIT on, either backend, emulation is identical to the interpreter at every `Advance()` return; the only accepted deviations are `design/sh2-jit.md` section 6.5. Lockstep is the oracle: any divergence is a bug. An optimization that cannot be shown exact in every case keeps a per-instruction fallback for the cases it cannot.
- **Both backends:** IR ops added here are implemented by `RunBlock` first (the reference) and by x64; `[jit]` tests run on both (`BRIMIR_JIT_BACKEND`).
- **Fork scope:** `src/core/{include,src}/ymir/hw/sh2/*` plus the read-only accessor in `ymir/sys/bus.hpp`. Edits are marked `// Brimir:` and logged in `src/core/BRIMIR_FORK.md`. Outside the fork scope only small hooks that do not change emulation (design section 4).
- **Includes:** `brimir-jit` includes only `ymir/core/types.hpp`, `ymir/hw/sh2/sh2_jit_iface.hpp` and `ymir/hw/sh2/sh2_decode.hpp` from the core; asmjit only under `src/jit/src/x64/`.
- **Portability:** everything builds and passes without x64 (macOS ARM64 CI, Raspberry Pi 3).
- **Code lifetime and exceptions** as in milestone 2C: no generated code runs while freed; no exception crosses generated code.
- **Tests:** Catch2 in `tests/unit/`, JIT tests tagged `[jit]`. Never weaken an assertion.
- **Workflow:** branch `feature/sh2-jit-2d`; commit messages `type(scope): subject`; never stage `ROADMAP.md`, BIOS/ROM, `.chd` or `.smpc` files. Push the branch for Windows/Linux/macOS CI before Task 7's report.

## Review Focus

1. **Cycle-target arithmetic near its limit:** a guarded run with the executor's "no target" value (`kNoCycleTarget`) or a target close to 2^64 must not wrap: `cycles + maxRunCycles` saturates. Test in Task 2.
2. **Guarded runs entered by chaining:** a chained block's cycles count from the chain start, so the guard must compare the same chain-relative counter as `CheckBoundary` does (`frame->limit`). Test in Task 2 (a guarded run in the second block of a chain with the target inside the run).
3. **Inline `DIV1` after an op that left SR dirty in the register cache** (a `SetT` from `CMP`, earlier `DIV1`s) and a `GetT`/exit after it: the inline step must read and write the cached SR, not memory. Test in Task 3.
4. **On-chip reads that sync timers** (`FRC`, `FTCSR` call `AdvanceFRT`, which reads `m_cyclesExecuted`): the direct path must see the cycle count synced exactly as before, including after `AddCycles` earlier in the block. Test in Task 4.
5. **A page remapped between compile and run** for a constant-address access (cartridge or ROM remap): the compile-time pointer must not be used. Test in Task 5.
6. **Host-time conversion before any calibration interval** (profiling enabled, `ConsumeHostTimeNs` called with no ticks yet, or a zero-length interval): no division by zero, returns 0. Test in Task 1.

## Build, test and measurement commands (Windows)

**Prerequisite (human):** the Windows SDK's `Lib` and `Include` folders are missing on the development PC (2026-10-15: `C:\Program Files (x86)\Windows Kits\10\Lib` does not exist, so MSVC links fail with `LNK1104: kernel32.lib`). Repair it with Visual Studio Installer (Modify, Windows 11 SDK 10.0.26100) before Task 1.

Task 1 recreates the MSVC helper `$env:TEMP\opencode\msvc.ps1`: it runs `vswhere -latest -property installationPath`, imports the environment of `VC\Auxiliary\Build\vcvarsall.bat x64`, then runs `-Cmd`. Then:

```powershell
pwsh -NoProfile -File $env:TEMP\opencode\msvc.ps1 -Cmd "cmake --build build --target brimir_tests brimir_libretro brimir_bench"
build\bin\brimir_tests.exe "[jit]"
$env:BRIMIR_JIT_BACKEND='ir'; build\bin\brimir_tests.exe "[jit]"; Remove-Item Env:BRIMIR_JIT_BACKEND
ctest --test-dir build --output-on-failure
build\bin\brimir_bench.exe --bios tests\fixtures\sega_101.bin --lockstep 1200 --jit-backend x64
```

If MSVC is still unavailable, the WSL build (`wsl -e bash -lc "ninja -C build-wsl-gcc14 brimir_tests"`) is acceptable for tests, not for measurements.

**Content:** BIOS `F:\OneDrive\Roms\BIOS\mpr-17933.bin` (US) and `F:\OneDrive\Roms\BIOS\Sega Saturn BIOS v1.01 (JAP).bin` (JP; Virtua Fighter 2 and Street Fighter Zero 3), games `F:\OneDrive\Roms\saturn\<No-Intro name>.chd`, and a fresh scratch system dir per run holding `F:\OneDrive\Roms\BIOS\brimir_saturn_rtc_us_eu.smpc` under both `brimir_saturn_rtc_us_eu.smpc` and `brimir_saturn_rtc_jp.smpc` (the 2C method).

**Progress measurement** (end of Tasks 1-6): rebuild `build-bench` (`cmake --build build-bench --target brimir_bench`). Run Panzer Dragoon II Zwei, Street Fighter Zero 3, Burning Rangers and Virtua Fighter 2 with `--warmup 2400 --frames 1800`, three times each, interpreter and `--sh2-jit --jit-backend x64` back to back. Append one row per title to a "2D progress" table in `design/sh2-x64-performance.md` (created in Task 1): task, interpreter SH2 total (median), x64 SH2 total (median), ratio, x64 max frame ms and the interpreter's highest max frame, x64 `compileFallbacks`. Commit the table with the task. If a task's ratio does not improve on any title beyond run-to-run spread, record that and ask before keeping its code.

---

### Task 1: Cheap SH-2 host-time accounting, then re-baseline (design step 0)

**Files:**
- Create: `src/core/include/ymir/hw/sh2/sh2_host_ticks.hpp`
- Modify: `src/core/src/ymir/hw/sh2/sh2.cpp:441-472` (`HostTimeScope`, `SH2::Advance`), `src/core/include/ymir/hw/sh2/sh2.hpp:111-120,752-754` (`SetHostTimeProfiling`, `ConsumeHostTimeNs`, members), `src/core/BRIMIR_FORK.md`, `design/sh2-x64-performance.md`
- Test: `tests/unit/test_profiler.cpp`

**Interfaces:**
- Produces, in `ymir::sh2` (`sh2_host_ticks.hpp`): `uint64 HostTicks() noexcept` (x86-64: `__rdtsc()`; AArch64: `cntvct_el0`; otherwise `steady_clock` nanoseconds) and `uint64 HostTicksToNs(uint64 ticks, uint64 calibTicks, uint64 calibNs) noexcept` (`ticks * calibNs / calibTicks` without overflow, 0 when `calibTicks == 0`).
- `SH2::ConsumeHostTimeNs()` keeps its signature and meaning (host nanoseconds inside `Advance` since the last call).

Calibration needs no busy wait: `SetHostTimeProfiling(true)` records `HostTicks()` and `steady_clock::now()`; each `ConsumeHostTimeNs()` converts the accumulated ticks with the ratio of ticks to nanoseconds elapsed since enabling, read at that moment.

- [ ] **Step 1: Recreate `$env:TEMP\opencode\msvc.ps1`** (above), create branch `feature/sh2-jit-2d`, build `build` and `build-bench`, run the suite. Expected: all pass (271 test cases; x64 tests not skipped on Windows).
- [ ] **Step 2: Write the failing tests** in `test_profiler.cpp`:
  - `"Host tick conversion handles an empty calibration"`: `HostTicksToNs(1000, 0, 0) == 0`, `HostTicksToNs(0, 10, 10) == 0`.
  - `"Host tick conversion scales without overflow"`: `HostTicksToNs(1ull << 62, 3'000'000'000ull, 1'000'000'000ull) == (1ull << 62) / 3` (within 1).
  - `"SH-2 host time tracks steady_clock"`: enable profiling, run `Advance` on a BIOS-less `Saturn` in a loop for at least 50 ms of `steady_clock` time, then `ConsumeHostTimeNs()` is greater than 0 and at most 1.1 times the `steady_clock` time measured around the loop (the loop's own overhead is outside `Advance`, so no lower bound beyond 0 is asserted; CI machines are noisy). The existing `"SH-2 host time accounting is off by default and consumable"` stays as it is.
- [ ] **Step 3: Run them.** Expected: the first two fail to compile (no `sh2_host_ticks.hpp`); the third passes on the old code (it pins today's behavior).
- [ ] **Step 4: Implement** `sh2_host_ticks.hpp`, switch `HostTimeScope` to `HostTicks()` and the accumulated counter to ticks, convert in `ConsumeHostTimeNs()`. Mark `// Brimir:` and add a `BRIMIR_FORK.md` row.
- [ ] **Step 5: Run the suite** on MSVC and WSL. Expected: all pass.
- [ ] **Step 6: Measure the overhead and re-baseline.** Panzer Dragoon II Zwei `--warmup 2400 --frames 1800`, interpreter, three runs with `build-bench` before and after this task: the outside-executor part should fall by about 0.6 ms/frame (2B profile: 38.6 ns per `steady_clock` pair, 55.6M `Advance` calls per 1,800 frames). Then the full 2C measurement (six titles plus BIOS, three rounds, interpreter / IR / x64) as the 2D baseline, plus the "2D progress" table with its first rows.
- [ ] **Step 7: Commit** `perf(sh2): cheaper host-time accounting in SH2::Advance` (code, tests, fork log, both tables).

### Task 2: Guarded runs of boundary checks (B1)

**Files:**
- Modify: `src/jit/include/brimir/jit/ir.hpp` (new `Op::GuardRun`), `src/jit/src/ir.cpp` (`Builder::GuardRun`, `VerifyBlock`, `PrintBlock`), `src/jit/src/interp_backend.cpp`, `src/jit/include/brimir/jit/ir_opt.hpp`, `src/jit/src/ir_opt.cpp`, `src/jit/src/x64/x64_emitter.cpp` (`Lower`, `case Op::CheckBoundary` at 1046)
- Test: `tests/unit/test_jit_ir_opt.cpp`, `tests/unit/test_jit_backend.cpp`, `tests/unit/test_jit_x64.cpp`, `tests/unit/jit_random_ir.cpp`

**Interfaces:**
- Produces: `void Builder::GuardRun(uint32_t maxCycles, uint8_t checks)`. Semantics (both backends): if `cycles + maxCycles < limit` (saturating add, chain-relative `cycles` as `CheckBoundary` uses), the next `checks` `CheckBoundary` ops are skipped; otherwise they run as usual.
- `OptimizeBlock` rule 5 (documented in `ir_opt.hpp`): a guarded run is a maximal sequence of at least 2 cycles-only `CheckBoundary` ops (`flag` set, `kCheckNeedsInlineRefills` clear) with only callback-free ops between the first and the last: no op from rule 2's changing set, no exit, no `SyncCycles`. `maxCycles` is the sum of the `AddCycles` immediates plus 1 per `WbStall` from the first check to the last. `GuardRun` is inserted right before the first check; `checks` is the run length (at most 255; longer runs split).

Why this is exact: cycles only grow inside the run and `limit` is fixed while a block runs, so if `cycles + maxCycles < limit` at the first check, every check in the run would pass; the interrupt test is already elided there (rule 2), and no op in the run can change it.

- [ ] **Step 1: Write the failing tests.**
  - `test_jit_ir_opt.cpp` `"IR opt: guarded runs cover consecutive cycles-only checks"`: a 10-instruction ALU block after `ClearIntrAllow` gets one `GuardRun` with `checks == 9` and `maxCycles` equal to the summed `AddCycles` and `WbStall` count; a `Load` in the middle splits it into two runs; a run of one check gets no guard.
  - `test_jit_backend.cpp` `"Backend: GuardRun skips checks only when the run cannot reach the target"` (both backends): for every target from 0 to the block's total cycles + 1, the exit (`retired`, `boundary`, `PC`, cycles) equals the same block without `OptimizeBlock`.
  - `test_jit_backend.cpp` `"Backend: GuardRun saturates with no cycle target"`: target `kNoCycleTarget` and a target of `UINT64_MAX - 2`; the block runs to its exit.
  - `test_jit_x64.cpp` `"x64 guarded run in a chained block"`: two chained blocks, the second with a guarded run; targets placed at every instruction of the second block; outcome equals `RunBlock`'s.
  - Extend `jit_random_ir.cpp` so random blocks also run through `OptimizeBlock` with random targets (if not already).
- [ ] **Step 2: Run them.** Expected: compile errors (no `GuardRun`), then failures in the rule-5 test.
- [ ] **Step 3: Implement** `Op::GuardRun` in IR, verifier and printer; in `RunBlock`; rule 5 in `OptimizeBlock`; then x64. x64 lowering is the builder's choice with one requirement: on the path where the guard passes, the covered checks emit no compare against `frame->limit` and no register-cache stores (today's `FlushRegs(true)` per check); a check that can still fire stores the dirty set in its stub instead (as `ExitIf` does).
- [ ] **Step 4: Run** `[jit]` on both backends, ctest, BIOS lockstep 1200 (x64 and IR). Expected: all pass.
- [ ] **Step 5: Progress measurement; commit** `perf(jit): guarded runs of boundary checks (2D B1)`.

### Task 3: Inline `DIV1` in x64 code (B3)

**Files:**
- Modify: `src/jit/src/x64/x64_emitter.cpp` (`case Op::Div1` at 1131), `src/jit/src/x64/x64_backend.cpp` and `x64_emitter.hpp` (remove `TrDiv1`, which has no other caller)
- Test: `tests/unit/test_jit_x64.cpp`

**Interfaces:** consumes `Div1Step` (`brimir/jit/sh2_helpers.hpp`) as the reference only; no IR change.

- [ ] **Step 1: Write the failing tests** in `test_jit_x64.cpp`:
  - `"x64 inline Div1 matches Div1Step"`: for SR M, Q, T in all 8 combinations, `rmIsRn` both ways, `rn` and `rm` in {0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678}: x64 result and SR equal `Div1Step`'s.
  - `"x64 inline Div1 uses the cached SR"`: a block `SetT(1)`, then two `Div1`, then `SetReg(0, GetT())`: R0, R2 and SR equal `RunBlock`'s.
- [ ] **Step 2: Run them.** Expected: both pass on today's code (the trampoline is exact); they pin behavior for Step 3. That the call is gone shows in the build (`TrDiv1` removed) and in the measurement.
- [ ] **Step 3: Implement** the inline step on `CachedGet(kSlotSR)` / `CachedSet(kSlotSR, ...)`, with no `FlushRegs`, no call and no `InvalidateRegs`, mirroring `Div1Step` operation by operation (unsigned compares).
- [ ] **Step 4: Run** `[jit]` both backends, `"Division sequences match the interpreter"`, ctest, BIOS lockstep. Expected: all pass.
- [ ] **Step 5: Progress measurement** (Burning Rangers is the title to watch: 44.8M `Div1` calls per 1,800 frames); commit `perf(jit): inline DIV1 in x64 code (2D B3)`.

### Task 4: Direct on-chip register reads (B4)

**Files:**
- Modify: `src/core/include/ymir/hw/sh2/sh2_jit_iface.hpp` (new callback), `src/core/src/ymir/hw/sh2/sh2.cpp` (`SH2::InitJitContext`, the callback), `src/core/BRIMIR_FORK.md`, `src/jit/src/x64/x64_emitter.cpp` (`LowerLoad` slow path, 551-566), `src/jit/src/x64/x64_backend.cpp` (new trampoline), `src/jit/src/x64/x64_emitter.hpp` (trampoline declaration)
- Test: `tests/unit/test_jit_diff.cpp`, `tests/unit/test_sh2_jit_iface.cpp`

**Interfaces:**
- Produces: `uint32_t (*SH2JitContext::onChipRead)(SH2 &sh2, uint32_t address, uint32_t size)`, doing exactly what the interpreter's data read does for partition `0b111` (on-chip registers) with cache emulation off; the builder derives it from `SH2::MemRead` and must not change `OnChipRegRead*`. `uint32_t TrOnChipRead(X64Frame *, uint32_t address, uint32_t size) noexcept`.
- `LowerLoad`'s slow path tests the partition: `0b111` calls `TrOnChipRead` (no code-dirty classification, as today), everything else calls `TrRead`.

Not in scope: inlining FTCSR polling or fast-forwarding polling loops (FTCSR reads call `AdvanceFRT` and write `FTCSR.mask`, so they are not side-effect-free). Revisit only if this task's measurement shows on-chip reads are still a top cost.

- [ ] **Step 1: Write the failing tests:**
  - `test_sh2_jit_iface.cpp` `"onChipRead equals the interpreter's on-chip data read"`: for every on-chip register address the interpreter decodes (0xFFFFFE00-0xFFFFFFFF, byte/word/long where legal), `ctx.onChipRead` returns the same value and leaves the same SH-2 state (`SaveState` bytes) as `ctx.read` on a copy.
  - `test_jit_diff.cpp` `"On-chip timer reads after AddCycles see synced cycles"`: a block with `AddCycles(37)`, then a read of FRC (0xFFFFFE12/13) and FTCSR: values and state equal the interpreter's.
- [ ] **Step 2: Run them.** Expected: compile error (no `onChipRead`).
- [ ] **Step 3: Implement** the callback (marked `// Brimir:`, logged), the trampoline and the slow-path split.
- [ ] **Step 4: Run** `[jit]` both backends, ctest, BIOS lockstep. Expected: all pass.
- [ ] **Step 5: Progress measurement** (watch the slaves: 78-96M on-chip reads per 1,800 frames); commit `perf(jit): direct on-chip register reads in x64 code (2D B4)`.

### Task 5: Compile-time page resolution for constant addresses (B2)

**Files:**
- Modify: `src/jit/src/x64/x64_emitter.cpp` (`LowerLoad`, `LowerStore`, `LowerAccessCycles`, `LowerExitIfBusWait`), `src/jit/src/bus_fast_path.cpp` / `src/jit/include/brimir/jit/bus_fast_path.hpp` (compile-time page query, if not already there)
- Test: `tests/unit/test_jit_x64.cpp`

**Interfaces:** consumes `PageEntry(bus, address)` and `SH2JitBusLayout` (`bus_fast_path.hpp`). A memory op's address is constant when its operand is defined by `Op::Const` in the same block (PC-relative loads and literal addresses).

For a constant address in a fast-path partition whose page is an array page at compile time, emit: compare the page entry's `array` field with the compile-time pointer (as `EmitPageCheck` does); equal: access `[compile-time array + offset]` directly, with the compile-time cycle cost; different: the existing dynamic path. Writes also test `arrayWritable`'s compile-time value the same way. Non-array pages and other partitions keep today's code.

- [ ] **Step 1: Write the failing tests** in `test_jit_x64.cpp`:
  - `"x64 constant-address access follows a page remapped after compile"`: compile a block loading and storing at a constant RAM address, remap that page (RAM to ROM, and RAM to MMIO) before running, and require the same outcome as `RunBlock`.
  - `"x64 constant-address fast path matches the dynamic path"`: for every `FastPathTestAddresses()` entry as a constant, both the outcome and the cycle count equal `RunBlock`'s.
- [ ] **Step 2: Run them.** Expected: they pass on today's code (the dynamic path is exact); they pin behavior for Step 3.
- [ ] **Step 3: Implement** the compile-time path.
- [ ] **Step 4: Run** `[jit]` both backends, `"Remapped code page"`, `"Code page remapped from an array to a handler page"`, ctest, BIOS lockstep. Expected: all pass.
- [ ] **Step 5: Progress measurement; commit** `perf(jit): compile-time page resolution for constant addresses (2D B2)`.

### Task 6: Native compile threshold (B5)

**Files:**
- Modify: `src/jit/include/brimir/jit/block_cache.hpp:34` (`kNativeCompileThreshold`, only if the measurement says so), `design/sh2-x64-performance.md`

- [ ] **Step 1: Measure** `kNativeCompileThreshold` = 8 (today), 16 and 32 with temporary builds of `build-bench`: Street Fighter Zero 3 and Panzer Dragoon II Zwei, x64, `--warmup 2400 --frames 1800`, three runs each, plus one Burning Rangers `--warmup 2400 --frames 33600` run per value.
- [ ] **Step 2: Choose** by the 2C rule: the best SFZ3 max frame among values whose average SH-2 time is within 2% of the best, then the Burning Rangers long-run max frame as the tie-breaker. Record the table in `sh2-x64-performance.md` ("2D B5").
- [ ] **Step 3: If the value changes,** update the constant and its comment, run `[jit]` both backends and ctest (tests that need native code use `kNativeOnFirstRun`, so they are unaffected).
- [ ] **Step 4: Commit** `perf(jit): native compile threshold N (2D B5)` or `docs(jit): native compile threshold stays 8 (2D B5)`.

### Task 7: Track B validation, measurement and report

**Files:**
- Modify: `design/sh2-x64-performance.md` ("Milestone 2D track B results"), `design/sh2-validation.md`, `design/sh2-jit.md` (status line), `CHANGELOG.md`

- [ ] **Step 1: Lockstep** (x64): BIOS menu and the six titles, `--lockstep 1800` for each, then `--lockstep 36000` for Burning Rangers and Virtua Fighter 2. IR backend: `--lockstep 1800` on all seven. Expected: `lockstep: OK, N frames identical`, exit 0, `compileFallbacks 0`. Record in `sh2-validation.md`.
- [ ] **Step 2: ARM check** on the Raspberry Pi 3 (`ssh rpi3`): sync the branch, build `/mnt/data/build/bench` and `/mnt/data/build/tests` (never at the same time as a run), run `tools/bench/run_lockstep.sh 1800 ir` and `brimir_tests`. Expected: all identical; tests pass with only `no x64 backend` skips.
- [ ] **Step 3: Full measurement** with the 2C method (six titles plus BIOS, three rounds, interpreter / IR / x64), plus the executor-only ratio for PD2, SFZ3 and BR with the 2C re-profile method (temporary instrumented build, not committed). Write "Milestone 2D track B results": the table, ratios, executor-only ratios, max frames against the interpreter's highest max + 10%, and the default-on bar of design section 3 condition by condition (condition 2, the long run, is expected to fail until track A).
- [ ] **Step 4: Update** `sh2-jit.md`'s status and `CHANGELOG.md` (Unreleased, one entry for track B).
- [ ] **Step 5: Commit** `docs(jit): milestone 2D track B results`, push the branch, wait for CI green. Then write the track A plan from these results (`superpowers:writing-plans`), with the long Burning Rangers run as its main measurement.
