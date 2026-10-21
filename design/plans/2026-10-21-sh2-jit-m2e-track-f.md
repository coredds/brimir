# SH-2 JIT Milestone 2E Track F - Dispatch and RTE Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut the dispatch cost between scheduler slices and compile `RTE`, so every title, Sega Rally included, is faster than the interpreter in SH-2 time. Find out why the frame outside the SH-2s is longer with the JIT, and what the long Burning Rangers run's slow frames are. Then measure against the 2E bar.

**Architecture:** The tasks follow the ranked list in `design/sh2-x64-performance.md` ("2E generated-code profile"). Three of them change code:
- Task 1 lets a slice start without `BlockCache::Get` when the previous slice ended at the cycle limit and nothing in the cache changed since. At that exit, generated code stores the link-slot entry for the next PC; a cache version counter guards it; the block's prologue still validates its code.
- Task 2 slims chain entry and exit (`RunEntry`, `StepImpl`'s tail, `X64Backend::Run`).
- Task 3 compiles `RTE`, which the front end interprets today.

Tasks 4 and 5 only measure: the non-SH-2 rise, and the long run's slow frames. Task 6 validates and reports.

**Tech Stack:** C++20, CMake 3.28+, asmjit (vendored), Catch2, MSVC 2022 / GCC 14 / Apple Clang; ThreadSanitizer CI job.

**Spec:** `design/sh2-jit-m2e.md` (sections 3, 4, 7, 8). **Evidence:** `design/sh2-x64-performance.md` ("Milestone 2E results", "2E generated-code profile", "What 2E left"). **Semantic reference:** `src/core/src/ymir/hw/sh2/sh2.cpp` (`SH2::RTE` near 4636, `Advance`, `InterpretNext`), `src/jit/src/interp_backend.cpp` (`RunBlock`).

## Global Constraints

- **Exactness:** with the JIT on, either backend, with or without the worker, emulation is identical to the interpreter at every `Advance()` return; the only accepted deviations are `design/sh2-jit.md` section 6.5. Lockstep is the oracle.
- **Code lifetime:** no generated code runs while freed; a resumed entry is only used while the cache is unchanged since it was recorded (Task 1's version check); a link slot never points to freed code (2D track A).
- **Thread boundary** (2E): only `PreparedCompile::Finalize` runs on the worker. Track F adds nothing to the worker.
- **No data races:** the TSan CI job (`test-linux-tsan`) stays green.
- **Synchronous mode and both backends:** no worker, and the IR backend, behave as before except where a task says otherwise. The IR backend never resumes (it has no native code). Task 3's `RTE` is implemented in `RunBlock` first (the reference), then in x64.
- **Fork scope:** `src/core/{include,src}/ymir/hw/sh2/*` and the read-only accessor in `ymir/sys/bus.hpp`; edits marked `// Brimir:` and logged in `src/core/BRIMIR_FORK.md`. Task 3 may need a context callback or offset; none of the others should touch the core.
- **Includes:** `brimir-jit` includes only `ymir/core/types.hpp`, `ymir/hw/sh2/sh2_jit_iface.hpp` and `ymir/hw/sh2/sh2_decode.hpp` from the core; asmjit only under `src/jit/src/x64/`.
- **Portability:** everything builds and passes without x64 (macOS ARM64 CI, Raspberry Pi 3).
- **Tests:** Catch2 in `tests/unit/`, JIT tests tagged `[jit]`. Never weaken an assertion. Where a stats counter changes meaning on purpose, the plan says so and the test pins the new meaning.
- **Workflow:** branch `feature/sh2-jit-2e-f` from `master`; commit messages `type(scope): subject`; never stage `ROADMAP.md`, BIOS/ROM, `.chd` or `.smpc` files. Push for CI before Task 6's report.

## Review Focus

1. **The cache changes between two slices:**
   - Changes on this CPU: an invalidation, flush, build, reuse, install or IR eviction in a `Get` from another step.
   - Changes from outside: a state load, a reset, another CPU writing the code, a page remap.

   A recorded resume must then be dropped, or caught by the block's prologue. It must never run freed or wrong code. Tests in Task 1.
2. **A slice that would resume, but an interrupt became pending and allowed, a delay slot is pending, or `PC & 2` with a fetch buffer differing from memory:** the executor's `Step` gates apply exactly as without resume. Test in Task 1.
3. **Background-compile results starving:** while a cache only resumes, `Get` never runs, so worker results are never installed. The resume must not be taken while this cache has results waiting. Test in Task 1.
4. **An exception thrown by a callback inside a resumed or leaner chain** (a memory callback throwing): the deferred flush is still applied, `m_inBlock` is cleared, and the exception propagates exactly as before. Test in Task 2.
5. **`RTE` with its stack on a handler page or in the on-chip area, a misaligned R15, or R15 written by the delay slot:** results equal the interpreter's (reads, cycles, SR, PC, the delay slot's effects). Test in Task 3.

## Build, test and measurement commands (Windows)

Unchanged from the 2E plan (`design/plans/2026-10-20-sh2-jit-m2e.md`, "Build, test and measurement commands"):
- `$env:TEMP\opencode\msvc.ps1`; `build` for tests, `build-dbg` for asserts (the full suite passes unfiltered), `build-bench` for measurements, the WSL TSan build;
- content paths and the scratch system dir as in the 2E Task 6 report;
- never pipe a build into `Select-Object -First N`;
- block on long runs.

**Track F progress measurement** (end of Tasks 1, 2 and 3): rebuild `build-bench`. Run Sega Rally, Panzer Dragoon II Zwei, Street Fighter Zero 3 and Burning Rangers with `--warmup 2400 --frames 1800`, interpreter and x64 (worker) back to back, three rounds. Add one row per title to a "Track F progress" table in `design/sh2-x64-performance.md` (created in Task 1), recording:
- task;
- interpreter and x64 SH2 total, and the ratio;
- interpreter and x64 average frame ms;
- frames over 16.7 ms, x64 and the interpreter's highest count;
- from Task 1 on, resumes as a share of `Executor::Run` calls.

If a task does not improve its target beyond run-to-run spread, record that and ask before keeping its code.

---

### Task 1: Resume a slice without `Get`

**Files:**
- Modify:
  - `src/jit/include/brimir/jit/interp_backend.hpp` (`ExitInfo`);
  - `src/jit/src/x64/x64_emitter.cpp` (`ChainExit` near 503-548, the frame/out offsets);
  - `src/jit/src/x64/x64_emitter.hpp` (contract comment);
  - `src/jit/include/brimir/jit/block_cache.hpp` and `src/jit/src/block_cache.cpp` (version counter, `Lookups()`);
  - `src/jit/include/brimir/jit/executor.hpp` and `src/jit/src/executor.cpp` (`Run`, `StepImpl`, `Stats`);
  - `tools/brimir_bench.cpp` (print `resumes`).
- Test: `tests/unit/test_jit_executor.cpp`, `tests/unit/test_jit_x64.cpp`

**Interfaces:**
- Produces:
  - `const void *ExitInfo::next = nullptr;`. Native code only. When a chain ends because its cycles reached the limit at a chainable exit (`ChainExit`'s `jae m_retNull`), it holds the entry of the link slot whose `pc` equals the new PC, or nullptr on a slot miss. Every other exit leaves it nullptr. `RunBlock` never sets it.
  - `uint64_t BlockCache::Version() const`: incremented by every operation that adds, removes, retires, reuses or installs an entry, publishes or unpublishes a link slot, releases native code, evicts IR, or flushes. Re-publishing the same entry in `Validate` need not count.
  - `uint64_t BlockCache::Lookups() const`: the number of `Get` calls. Tests use it.
  - `uint64_t Executor::Stats::resumes`: steps that entered native code through a resume.
- **Rule:**
  - After a native step whose `info.next != nullptr`, the executor records `{*ctx.PC, info.next, m_cache.Version()}`.
  - At the next step, the executor first makes `StepImpl`'s gates: the interrupt and delay-slot check, then `PC & 2`.
  - If `*ctx.PC` and the version equal the record, and `!m_cache.HasWaitingResults()`, it runs `NativeCode{next, selfValidating = true}` through the same `RunEntry` path as a `Get`-returned native block. Otherwise it clears the record and takes the normal path.
  - `bool BlockCache::HasWaitingResults() const`: true when the cache has a worker, compiles pending, and the worker has results for it (the same test `Get` uses before installing).
  - A stale report from a resumed block is handled exactly as for a `Get`-returned one (`Invalidate(pc)` and retry).
  - The record is cleared on every other step outcome and by `Executor::Flush`.
  - Only self-validating blocks are ever published, so a resumed block always checks its own code.

- [ ] **Step 1: Create branch** `feature/sh2-jit-2e-f`; build `build`, `build-dbg`, `build-bench`; run the suite. Expected: all pass.
- [ ] **Step 2: Write the failing tests:**
  - `test_jit_executor.cpp` `"Resume: consecutive slices skip Get"` (x64): a loop program of three chained blocks, `Executor::Run` called with 40-cycle slices for 200 slices. `GetStats().resumes > 150`, and `Cache().Lookups()` grows by fewer than 50 over the loop. The state after every slice equals a reference rig stepped by the interpreter to the same cycle counts.
  - `"Resume: dropped after a cache change"` (Review Focus 1): between two slices, in turn (a) `Invalidate` the recorded block's PC, (b) `Flush()`, (c) rewrite the block's code in RAM, (d) remap its page to another array holding the same bytes. The next slice does not resume in (a) and (b) (`Lookups()` + 1). In (c) and (d) it either does not resume or its prologue reports stale (`staleEntries` + 1). Every outcome matches the reference rig.
  - `"Resume: Step gates still apply"` (Review Focus 2): before a slice that would resume, make an interrupt pending and allowed, then separately a pending delay slot, then a `PC & 2` fetch-buffer mismatch. Each time the executor interprets, `resumes` does not grow, and the state matches the reference.
  - `"Resume: waiting worker results force a Get"` (Review Focus 3): a cache with a `CompileWorker` and a result parked for it (via the Task 3 2E blocker helpers in `test_jit_worker.cpp`). The next slice calls `Get` and installs it (`AsyncCompiles()` + 1).
  - `test_jit_x64.cpp` `"x64: a chain ending at the cycle limit reports the next entry"`: chained blocks A → B with a limit reached at A's exit. `info.next` equals B's published entry. With B unpublished it is nullptr, and on any non-limit exit it is nullptr.
  - Extend the existing `"Chained run equals stepped run"` (or add `"Resumed run equals stepped run"`): random slice lengths from 1 to 80 cycles over the fuzz programs. Final state and cycle count equal the stepped run.
- [ ] **Step 3: Run them.** Expected: compile errors (no `next`, `Version`, `resumes`).
- [ ] **Step 4: Implement.** In `ChainExit`, at the limit exit, do the link-slot lookup before returning and store the entry or nullptr into `out.next`. Keep the non-limit paths unchanged.
- [ ] **Step 5: Run** `[jit]` both backends (`build`, `build-dbg`), ctest, WSL TSan `[jit]`, BIOS lockstep 1200 x64 and IR, and `--lockstep 1800` on Sega Rally and Street Fighter Zero 3 (x64). Expected: all pass, identical.
- [ ] **Step 6: Progress measurement** (create the "Track F progress" table). Expected from the profile: 0.30-0.65 ms/frame off the x64 SH2 total. Commit `perf(jit): resume a slice without a cache lookup (2E F1)`.

### Task 2: Leaner chain entry and exit

**Files:**
- Modify: `src/jit/src/executor.cpp` (`RunEntry`, `StepImpl`), `src/jit/include/brimir/jit/executor.hpp`, `src/jit/src/x64/x64_backend.cpp` (`X64Backend::Run`), `src/jit/src/x64/x64_backend.hpp`, `src/jit/include/brimir/jit/backend.hpp` (only if the run entry point changes)
- Test: `tests/unit/test_jit_executor.cpp`, `tests/unit/test_jit_x64.cpp`

**Interfaces:**
- Consumes: Task 1's resume path, which uses the same entry.
- No new public interface is required. The builder chooses how to cut the 20-22 ns per chain the profile measured. The profile names the candidates:
  - the `BlockScope` guard object;
  - four stats counters on the common path;
  - the zero-initialised `X64Frame` of about 100 bytes;
  - the virtual `INativeBackend::Run`;
  - the `ExitInfo` copy.
- **Requirements:**
  - The deferred flush and `m_inBlock` behave exactly as today, including when a callback throws.
  - `Stats` values at any `Step`/`Run` return equal today's, for every counter, including resumes.
  - `X64Backend::Run` keeps its contract for direct callers (tests).

- [ ] **Step 1: Write the failing or pinning tests:**
  - `test_jit_executor.cpp` `"Executor: a throwing callback inside a chain still applies a deferred flush"` (Review Focus 4): a read callback that requests `Flush()` and then throws, inside the second block of a chain (x64, and the IR backend for the single-block case). The exception reaches the caller, `Cache().Size() == 0` afterwards, the next `Step` runs normally, and the state matches the reference.
  - `"Executor: stats are identical with and without chaining over a fuzz program"`: `blocksRun`, `nativeBlocksRun`, `chainedBlocks`, `interpreted`, `staleEntries`, `resumes`. The totals over 2,000 random slices are identical to the values from the base build, recorded in the test as expected numbers from one deterministic program and seed.
- [ ] **Step 2: Run them.** Expected: they pass on Task 1's code, pinning today's behavior for Step 3.
- [ ] **Step 3: Implement** the changes, one at a time, measuring each with Sega Rally and Panzer Dragoon II Zwei (three rounds). Drop a change that does not measure, and record it.
- [ ] **Step 4: Run** `[jit]` both backends (`build`, `build-dbg`), ctest, WSL TSan `[jit]`, BIOS lockstep 1200 x64. Expected: all pass.
- [ ] **Step 5: Progress measurement.** Expected from the profile: 0.22-0.39 ms/frame. Commit `perf(jit): leaner chain entry and exit (2E F2)`.

### Task 3: Compile `RTE`

**Files:**
- Modify:
  - `src/jit/src/frontend.cpp` (`IsDelayedBranch` near 185, `LowerDelayedBranch`, `BaseOp`, wherever `RTE` is rejected today);
  - `src/jit/include/brimir/jit/ir.hpp` and `src/jit/src/ir.cpp` (only if a new op or flag is needed);
  - `src/jit/src/interp_backend.cpp`;
  - `src/jit/src/x64/x64_emitter.cpp`;
  - if needed, `src/core/include/ymir/hw/sh2/sh2_jit_iface.hpp`, `src/core/src/ymir/hw/sh2/sh2.cpp` (`InitJitContext`) and `src/core/BRIMIR_FORK.md`.
- Test: `tests/unit/jit_opcode_specs.cpp`, `tests/unit/test_jit_opcodes.cpp`, `tests/unit/test_jit_diff.cpp`, `tests/unit/jit_fuzz_programs.cpp`

**Interfaces:**
- Reference: `SH2::RTE`. In this order, it:
  1. reads `target = MemReadLong(R15)` and `SR = MemReadLong(R15 + 4) & 0x000003F3`;
  2. charges cycles of `AccessCycles<uint32, read>(R15) + AccessCycles<uint32, read>(R15 + 4) + WritebackCycles(15) + 2`;
  3. sets up the delay slot to `target`;
  4. advances PC by 2 and sets `R15 += 8`, `m_wbReg = kWBRegNone`.
- The SR write is a plain assignment, not `SetSR`. The compiled form must reproduce exactly what the interpreter does to the interrupt state: what it recomputes and what it does not, and when the next boundary check sees the new SR. Read `InterpretNext`, the delay-slot path and the interrupt check to establish this, and state it in a comment at the lowering.
- `RTE` becomes a delayed branch in the front end, lowered like `RTS` with these differences: the two loads, the SR write and `R15 += 8`. Its delay slot follows the existing delay-slot rules.
- Interrupt entry stays interpreted.

- [ ] **Step 1: Write the failing tests:**
  - `jit_opcode_specs.cpp`: add `RTE` (0x002B) to the specs so the generic opcode tests cover it. That means values on RAM, on a handler page and in the on-chip area for R15; R15 not 4-aligned; SR bits outside 0x3F3 set in memory; every delay-slot instruction class the front end accepts; and a delay slot that writes R15 (Review Focus 5).
  - `test_jit_diff.cpp` `"RTE returns from an interrupt like the interpreter"`: raise an interrupt on the rig, let the interpreter enter it, then run the handler, which ends in `RTE; NOP`, through the JIT (both backends). The state, cycles and the next instruction's interrupt decision equal the interpreter's. Include a second interrupt pending at a level that the restored SR unmasks.
  - `jit_fuzz_programs.cpp`: include `RTE` with a prepared stack in the fuzz program generator.
- [ ] **Step 2: Run them.** Expected: they fail, because the front end rejects `RTE`, so no block is compiled. The spec tests report it as uncompiled or interpreted.
- [ ] **Step 3: Implement** in the front end and `RunBlock` first, then x64.
- [ ] **Step 4: Run** `[jit]` both backends (`build`, `build-dbg`), the fuzz suites, ctest, WSL TSan `[jit]`, BIOS lockstep 1200 x64 and IR, and `--lockstep 1800` on all six titles (x64 and IR). Expected: all identical.
- [ ] **Step 5: Progress measurement.** Watch Sega Rally's slave interpreter fallbacks, 56.4M per window in the profile. Expected: 0.15-0.28 ms/frame off Sega Rally. Commit `perf(jit): compile RTE (2E F3)`.

### Task 4: Why the frame outside the SH-2s is longer with the JIT (measurement only)

**Files:**
- Modify: `design/sh2-x64-performance.md` (new section "Track F: the non-SH-2 rise")

The 2E results put the rise at 0.2-0.46 ms/frame on every title, with or without the worker. Find out where it goes, using temporary instrumentation in `build-instr`, not committed. Its counters must equal the uninstrumented runs'.

- [ ] **Step 1: Split the frame** with the profiler's existing entries (`Ymir_RunFrame`, `SH2_Master`, `SH2_Slave`, and the VDP, SCSP and SCU entries where they exist). Add temporary ones as needed. Measure Sega Rally and Panzer Dragoon II Zwei, interpreter against x64, three rounds each, to name the component that grows.
- [ ] **Step 2: Test the candidate causes**, one experiment each.
  - **Cache pressure:** the native code and the cache structures evicting other components' data. Use a temporary build with a smaller code footprint, e.g. `kNativeCompileThreshold` raised so fewer blocks are native, and check whether the rise shrinks while SH-2 time grows. On Linux (WSL), `perf stat` cache-miss counters on both builds are a second check.
  - **Accounting boundary:** does part of the JIT's work, such as `Executor::Run`'s per-slice overhead or the worker install poll, land outside `SH2::Advance`'s host timer?
- [ ] **Step 3: Record** the split, the experiments and the conclusion (cause found, or cause narrowed to X with evidence). If a fix is evident and small, write it up as a candidate with its expected gain; do not implement it here. Commit `docs(jit): the non-SH-2 rise with the JIT (2E F4)`.

### Task 5: The long Burning Rangers run's slow frames (measurement only)

**Files:**
- Modify: `design/sh2-x64-performance.md` (new section "Track F: long-run slow frames")

The 2E results show 571 frames over 16.7 ms, all with front-end builds, about 1-5 ms of compile time in most, against an excess of about 8.8 ms per frame.

- [ ] **Step 1: Per-frame breakdown** with temporary instrumentation in `build-instr`, not committed, for every frame over 16.7 ms and an equal number of normal frames from the same phases. Record:
  - front-end builds and their time;
  - IR block runs (count, and time in `RunBlock`);
  - interpreter fallbacks;
  - invalidations;
  - worker queue length;
  - native installs;
  - SH-2 time against the non-SH-2 time.
- [ ] **Step 2: Attribute the excess:**
  - IR runs before native code arrives (the IR backend is 3-4x slower than the interpreter);
  - the front end;
  - waiting for the worker;
  - outside the SH-2s.

  For the largest share, estimate the gain of the obvious options with numbers, for example a lower threshold for returning code, interpreting cold new code instead of IR, or more workers.
- [ ] **Step 3: Record** the findings and a ranked list of options with expected gains. Commit `docs(jit): long-run slow frames (2E F5)`.

### Task 6: Validation, measurement against the 2E bar, report

**Files:**
- Modify: `design/sh2-x64-performance.md` ("Track F results"), `design/sh2-validation.md`, `design/sh2-jit.md` (status line), `CHANGELOG.md`; if the bar is met, the rollout files of the 2E plan's Task 6 Step 4.

- [ ] **Step 1: Lockstep** (with the worker): run all of the following.
  - x64 `--lockstep 36000` on the BIOS menu and the six titles;
  - IR `--lockstep 1800` on all seven;
  - x64 and IR `--lockstep 3600 --lockstep-reload 300` on all seven;
  - x64 `--lockstep 1800 --jit-sync-compile` on all seven.

  Expected: all identical, `compileFallbacks 0`. Record in `sh2-validation.md` ("Milestone 2E, track F").
- [ ] **Step 2: Pi check:** `rsync`, excluding `tests/fixtures`; IR lockstep and `brimir_tests` as in 2E Task 6. Expected: identical, and the tests pass.
- [ ] **Step 3: Full measurement:** the 2C method with the worker, plus the long Burning Rangers run, plus the second-session rule. Write "Track F results" with the bar of `sh2-jit-m2e.md` section 3, condition by condition.
- [ ] **Step 4: If all four conditions hold,** do the rollout exactly as the 2E plan's Task 6 Step 4 describes (`brimir_sh2_jit` gains `auto` as the default, and so on). Otherwise list what failed, with the figures, and the next steps that Tasks 4-5 point to.
- [ ] **Step 5: Update** `sh2-jit.md`'s status and `CHANGELOG.md` (Unreleased, one entry for track F). Commit `docs(jit): milestone 2E track F results` (plus the rollout commit if Step 4 ran), push, and wait for CI green, the TSan job included.
