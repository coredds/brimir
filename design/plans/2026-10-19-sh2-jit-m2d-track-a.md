# SH-2 JIT Milestone 2D (Track A) - Long Sessions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop code that is rewritten in a long session (Burning Rangers) from turning the x64 JIT into a slowdown: free stale native code one block at a time, reuse code that comes back, compile less at PCs that keep changing, and drop the save-state flush if exactness allows, all lockstep-exact.

**Architecture:** Everything except Task 5 lives in `BlockCache` and the native backend. Task 1 adds the counters the design asks for and checks how much returning code there is before A1 is built. Task 2 (A2) frees a dropped block's native code at once (`INativeBackend::Release`). Task 3 (A1) moves dropped self-validating native blocks into a bounded per-PC pool and takes them back when the same opcodes return on the same pages. Task 4 (A3) raises the native compile threshold per PC with the number of times its code went stale. Task 5 (A4) removes `SH2::LoadState`'s flush and adds lockstep across state loads. Task 6 validates and reports. Each of Tasks 2-5 ends with the track A progress measurement, so later tasks can be dropped on data.

**Tech Stack:** C++20, CMake 3.28+, asmjit (vendored, `JitRuntime::release` in `vendor/asmjit/asmjit/core/jit_runtime.h:93`), Catch2 (amalgamated), MSVC 2022 / GCC 14 / Apple Clang.

**Spec:** `design/sh2-jit-m2d.md` (sections 3, 4, 7, 8). **Evidence:** `design/sh2-x64-performance.md` ("2D B5" and "Milestone 2D track B results", including "What track B left"). **Code:** `src/jit/src/block_cache.cpp`, `src/jit/src/executor.cpp`, `src/jit/src/x64/x64_backend.cpp`.

## Global Constraints

- **Exactness:** with the JIT on, either backend, emulation is identical to the interpreter at every `Advance()` return; the only accepted deviations are `design/sh2-jit.md` section 6.5. Lockstep is the oracle.
- **What native code depends on** (the basis of A1 and A4; re-check it if the front end or emitter changes): the block's `startPC`, its `guestOpcodes` (including the tail word), and, for self-validating blocks, the array pointers of its code pages, which the prologue checks. Constant-address accesses (B2) check their page at run time. Known refill values come from `guestOpcodes`. A block does not depend on anything else, and it only runs against the CPU (context) it was compiled for.
- **Code lifetime:** no generated code runs while freed. `BlockCache::Invalidate`, `Release` and pool eviction are only called when no block is running (from `Get`, or from `Executor::StepImpl` after `RunEntry` returns). A link slot never points to freed code.
- **Memory (design section 4):** native code cap stays 128 MB per CPU (`kMaxNativeCodeBytes`); the retired pool (A1) counts toward it; over the long Burning Rangers run, no code-cap flushes and a peak native code no higher than 2C's (128 MB).
- **Both backends:** the IR backend has no native code, so no release and no pool. Its blocks are dropped as today; it must stay correct. `[jit]` tests run on both (`BRIMIR_JIT_BACKEND`).
- **Fork scope:** `src/core/{include,src}/ymir/hw/sh2/*` and the read-only accessor in `ymir/sys/bus.hpp`; edits marked `// Brimir:` and logged in `src/core/BRIMIR_FORK.md`.
- **Portability:** everything builds and passes without x64 (macOS ARM64 CI, Raspberry Pi 3).
- **Tests:** Catch2 in `tests/unit/`, JIT tests tagged `[jit]`. Never weaken an assertion. Where a behavior changes on purpose (Task 5), replace the old assertion with one that pins the new behavior.
- **Workflow:** branch `feature/sh2-jit-2d-a` from `master`; commit messages `type(scope): subject`; never stage `ROADMAP.md` (it has unrelated uncommitted edits), BIOS/ROM, `.chd` or `.smpc` files. Push the branch for Windows/Linux/macOS CI before Task 6's report.

## Review Focus

1. **A block freed while another block still chains to it:** block A chains to B through B's link slot; B's code changes, B reports stale inside the chain and is dropped. Its code must be unpublished before it is released, so the next chain from A misses (or finds the new B), never the freed code. Test in Task 2.
2. **Returning opcodes on a different page:** the same bytes come back at the same PC but the page was remapped to another array (cartridge or ROM swap). The pooled block would report stale at once, and the executor's "freshly compiled block reported stale" assert would fire, so it must not be reused. Test in Task 3.
3. **The code cap reached during a tiered compile while the pool is not empty:** `CompileNative` holds a reference to its entry. Emptying the pool must not free that entry. A flush, if one is still needed, must rebuild it as today. Test in Task 3.
4. **A state load that puts different code under a cached native block, with PC & 2 set and a loaded fetch buffer that differs from memory:** without the flush, the next step must still interpret (fetch-buffer gate), and the block must report stale and be rebuilt rather than run. Test in Task 5.
5. **Damping that never decays inside a session:** a PC that was stale many times, then stays stable, must still get native code (after at most `threshold << kMaxStaleDampShift` runs), never stay on IR forever. Test in Task 4.

## Build, test and measurement commands (Windows)

Unchanged from the track B plan (`design/plans/2026-10-15-sh2-jit-m2d-track-b.md`, "Build, test and measurement commands"): `$env:TEMP\opencode\msvc.ps1` (exists), `build` for tests, `build-bench` for measurements, WSL `build-wsl-gcc14` for the GCC test run, the same content paths and scratch system dir per run.

```powershell
pwsh -NoProfile -File $env:TEMP\opencode\msvc.ps1 -Cmd "cmake --build build --target brimir_tests brimir_libretro brimir_bench"
build\bin\brimir_tests.exe "[jit]"
$env:BRIMIR_JIT_BACKEND='ir'; build\bin\brimir_tests.exe "[jit]"; Remove-Item Env:BRIMIR_JIT_BACKEND
ctest --test-dir build --output-on-failure
build\bin\brimir_bench.exe --bios tests\fixtures\sega_101.bin --lockstep 1200 --jit-backend x64
```

**Track A progress measurement** (end of Tasks 2-5): rebuild `build-bench`. (a) Burning Rangers (US) `--warmup 2400 --frames 33600`: one x64 run, plus one interpreter run per session (the first time in that session). (b) Street Fighter Zero 3 (JP) and Panzer Dragoon II Zwei (US) `--warmup 2400 --frames 1800`: interpreter and x64 back to back, three rounds, as a regression check. Append one row per run to a "2D track A progress" table in `design/sh2-x64-performance.md` (created in Task 1): task; title; interpreter and x64 SH2 total; ratio; x64 avg, p99 and max frame ms; the interpreter's highest max frame; native compiles and compile ms in the window; code-cap flushes (master / slave); peak native code MB (master / slave); and, from Task 3 on, retired reuses. The long run takes about 10 minutes per mode, so run it last and check machine load first (as in "2D B5", Load). If a task does not improve the long run beyond its run-to-run spread and does not reduce flushes or compile time, record that and ask before keeping its code (design section 10).

---

### Task 1: Counters, and how much code comes back

**Files:**
- Modify: `src/jit/include/brimir/jit/block_cache.hpp`, `src/jit/src/block_cache.cpp`, `tools/brimir_bench.cpp` (`PrintJitStats`, `CompileCounters`, `jit window`), `design/sh2-x64-performance.md`
- Test: `tests/unit/test_jit_x64.cpp`

**Interfaces:**
- Produces: `size_t BlockCache::PeakNativeCodeBytes() const` (the highest `m_native->CodeBytes()` seen after any native compile since construction; 0 without a native backend). `uint64_t BlockCache::StaleNativeBytes() const`: the `NativeCode::bytes` (Task 2) of native blocks dropped by `Invalidate`. Until Task 2 it is computed from the `NativeBytesCompiled` delta of each block, which `CachedBlock` then records as `uint32_t nativeBytes`. Task 2 replaces the field with `NativeCode::bytes`.
- `brimir_bench` prints both per CPU on the `cache` line (`peakNativeMB`, `staleNativeMB`) and `peakNativeMB` per CPU on the `jit window` line.

- [ ] **Step 1: Create branch** `feature/sh2-jit-2d-a`; build `build` and `build-bench`; run the suite. Expected: all pass.
- [ ] **Step 2: Write the failing test** `"x64 backend: peak and stale native bytes are counted"` in `test_jit_x64.cpp`: with `kNativeOnFirstRun`, compile one block (`PeakNativeCodeBytes() == backend->CodeBytes()`, `StaleNativeBytes() == 0`), rewrite one opcode and `Get` again: `StaleNativeBytes()` equals the first block's bytes, and `PeakNativeCodeBytes()` equals the new `CodeBytes()` (the old code is still held). Then `Flush()`: the peak is unchanged.
- [ ] **Step 3: Run it.** Expected: compile error (no `PeakNativeCodeBytes`).
- [ ] **Step 4: Implement** the two counters and the bench output.
- [ ] **Step 5: Run** `[jit]` on both backends and ctest. Expected: all pass.
- [ ] **Step 6: Returning-code check** (temporary, not committed; a separate `build-instr` as in "Executor-only ratio"): in `BlockCache::Invalidate`, record a 64-bit hash of a dropped native block's `guestOpcodes` per PC (at most 4 per PC). In `Build`, count builds whose new block's PC and opcode hash match a recorded one (`potentialReuses`). Run the Burning Rangers long run x64 once and Street Fighter Zero 3 short once. Record in a "2D track A baseline" section in `sh2-x64-performance.md`: invalidations, native compiles and builds in the window, `potentialReuses`, the share of native compiles that were potential reuses, peak and stale native MB per CPU. Also create the empty "2D track A progress" table there. **If potential reuses are under 10% of the window's native compiles, report and ask before Task 3** (A1 would not pay for its memory).
- [ ] **Step 7: Commit** `feat(jit): count peak and stale native code bytes (2D track A)` (counters, test, bench, both doc sections).

### Task 2: Free stale native code individually (A2)

**Files:**
- Modify: `src/jit/include/brimir/jit/backend.hpp` (`NativeCode::bytes`, `INativeBackend::Release`), `src/jit/src/x64/x64_backend.hpp`, `src/jit/src/x64/x64_backend.cpp`, `src/jit/include/brimir/jit/block_cache.hpp`, `src/jit/src/block_cache.cpp` (`Invalidate(BlockMap::iterator)`, `CompileNative`)
- Test: `tests/unit/test_jit_x64.cpp`

**Interfaces:**
- Produces: `uint32_t NativeCode::bytes` (generated code size, set by `Compile`). `virtual void INativeBackend::Release(uint32_t pc, const NativeCode &code) = 0`: frees `code` and subtracts `code.bytes` from `CodeBytes()`. The caller has already unpublished `pc`. In debug, assert that the link slot for `pc` does not hold `code.entry`. X64: `m_runtime->release(fn)`.
- `BlockCache::Invalidate` unpublishes, then releases a dropped native block's code. `uint64_t BlockCache::NativeBytesReleased() const` counts the bytes. `StaleNativeBytes()` now counts from `NativeCode::bytes`, and `CachedBlock::nativeBytes` (Task 1) is removed.
- Unchanged: `FlushAll` still resets the backend; the IR cap still evicts only IR-only blocks; the code-cap flush stays as the fallback.

- [ ] **Step 1: Write the failing tests** in `test_jit_x64.cpp`:
  - `"x64 backend: invalidated blocks free their code"`: a `BlockCache` with a 4096-byte cap and `kNativeOnFirstRun`. Rewrite the same block's first opcode 500 times (alternating `add #k,R3` immediates), calling `Get` and `Run` each time. After each `Get`, `CodeBytes()` equals the current block's `code.bytes`, and each run matches the reference rig. At the end: `FlushesCodeCap() == 0`, `Invalidations() == 499`, `NativeBytesReleased() == StaleNativeBytes()`.
  - `"x64 backend: a block dropped inside a chain is not chained to again"`: an `Executor` with chaining, blocks A then B (A's exit chains to B). Run once (both native and published), rewrite B's second opcode, run from A again: B reports stale in the chain and is dropped. Run from A a third time: the chain reaches the rebuilt B. Every run's state, cycles and `retired` match the reference rig's. Add the debug check that `Release` is never passed a published entry.
  - The existing `"x64 backend: the native code cap flushes the cache"` and `"... at a tiered compile"` stay as they are: they have no invalidations, so the cap still fills and flushes.
- [ ] **Step 2: Run them.** Expected: compile errors (no `bytes`, `Release`, `NativeBytesReleased`).
- [ ] **Step 3: Implement** `Release` (IR: no backend, nothing to do), `NativeCode::bytes`, and the release in `Invalidate`, in this order: recent slot, `Unpublish`, `Release`, map erase.
- [ ] **Step 4: Run** `[jit]` on both backends, ctest, and BIOS lockstep 1200 (x64 and IR). Expected: all pass.
- [ ] **Step 5: Progress measurement.** Expected: peak native MB well under 128 on both CPUs, and fewer code-cap flushes in the long run than in track B's 4 / 3. If flushes remain, record the peak and ask before adding cold-block eviction: it needs a "last run" mark that chained runs do not record today, and it is not in this plan.
- [ ] **Step 6: Commit** `perf(jit): free stale native code per block (2D A2)`.

### Task 3: Reuse returning native code (A1)

**Files:**
- Modify: `src/jit/include/brimir/jit/block_cache.hpp`, `src/jit/src/block_cache.cpp` (`Invalidate`, `Build`, `CompileNative`, `FlushAll`), `src/jit/include/brimir/jit/bus_fast_path.hpp` and `src/jit/src/bus_fast_path.cpp` (a page-array query, if `PageEntry` does not already give it)
- Test: `tests/unit/test_jit_x64.cpp`, `tests/unit/test_jit_executor.cpp`

**Interfaces:**
- Produces, in `block_cache.hpp`: `constexpr size_t kMaxRetiredCodeBytes = size_t{32} << 20;` (native code held by retired blocks per CPU; counts toward `kMaxNativeCodeBytes`); `constexpr uint32_t kMaxRetiredPerPc = 4;`. `BlockCache` constructor gains `size_t maxRetiredCodeBytes = kMaxRetiredCodeBytes` after `maxCachedInsts` (tests pass small values).
- `CachedBlock` gains `std::array<const uint8_t *, 2> codePages{}`: the page-table array pointer of the page holding `startPC` and of the page holding the last word of `guestOpcodes`. These are the pages the prologue checks, recorded when the block is compiled natively.
- Retire: `Invalidate` moves a self-validating native block (`code.selfValidating`) into the pool instead of releasing it, after clearing its recent slot and unpublishing it. Other native blocks are released (Task 2). Over `kMaxRetiredPerPc` at a PC, or over `maxRetiredCodeBytes` in total, the least recently retired blocks are released (counted by `RetiredEvictions()`).
- Reuse: `Build(ctx, pc, ...)` first looks in the pool at `pc`. It reuses a block whose `guestOpcodes` equal current memory (`IsCurrent`) and whose `codePages` equal the current page-table arrays: the block moves back into the map and the recent slot, is published, and is returned without running the front end or the backend (`Compiles()` and `NativeCompiles()` unchanged, `RetiredReuses()` + 1). Otherwise it builds as today.
- Code cap: before flushing, `Build` and `CompileNative` release the whole pool. They flush only if `CodeBytes()` is still at the cap. The pool never holds the entry being compiled (that entry is in the map), so `CompileNative`'s reference stays valid.
- `FlushAll` empties the pool (the backend reset frees its code). The data structures are the builder's choice, but lookup is by PC and eviction is oldest first.
- `maxRetiredCodeBytes = 0` disables the pool (every dropped block is released, as in Task 2). Task 2's `"invalidated blocks free their code"` test passes 0, because it pins release, not reuse.
- Counters: `RetiredReuses()`, `RetiredEvictions()`, `size_t RetiredCodeBytes() const`; bench prints them on the `cache` line, and `reuses` on the `jit window` line.

- [ ] **Step 1: Write the failing tests:**
  - `test_jit_x64.cpp` `"Block cache: code that comes back reuses its native code"`: `kNativeOnFirstRun`, program P at `kCode`, run; overwrite with Q, run; overwrite with P again, `Get`: `entry.code.entry` equals the pointer from the first compile, `Compiles() == 2`, `NativeCompiles() == 2`, `RetiredReuses() == 1`, `RetiredCodeBytes()` equals Q's bytes. Every run matches the reference rig.
  - `test_jit_x64.cpp` `"Block cache: retired code is not reused on a remapped page"`: compile P on RAM page X, overwrite with Q, then map the code address to a second RAM array holding P (same bytes, different array) and step through an `Executor`. P is rebuilt (`RetiredReuses() == 0`), with no stale-retry assert, and the outcome matches the reference rig.
  - `test_jit_x64.cpp` `"Block cache: the retired pool is bounded"`: six distinct programs at one PC give at most `kMaxRetiredPerPc` retired blocks (`RetiredEvictions() == 1` after the sixth); with `maxRetiredCodeBytes` set to two blocks' bytes, programs at three PCs keep `RetiredCodeBytes() <= maxRetiredCodeBytes`. `CodeBytes()` always equals live plus retired bytes.
  - `test_jit_x64.cpp` `"Block cache: the code cap empties the pool before flushing"`: cap 4096, threshold 2. Fill the pool with retired blocks, then compile new blocks until the cap is reached at a tiered compile. The first time, the pool is emptied and nothing is flushed (`FlushesCodeCap() == 0`); the compiled block runs and matches the reference. Fill until live code alone exceeds the cap: then it flushes as today.
  - `test_jit_executor.cpp` `"Executor: the IR backend never retires blocks"`: IR backend, rewrite and restore code; `RetiredReuses() == 0`, `RetiredCodeBytes() == 0`, and `Compiles()` grows by one per rewrite.
- [ ] **Step 2: Run them.** Expected: compile errors, then the reuse tests fail.
- [ ] **Step 3: Implement** the pool, `codePages`, reuse in `Build`, and the cap order.
- [ ] **Step 4: Run** `[jit]` on both backends, ctest, and BIOS lockstep 1200 (x64 and IR). Expected: all pass.
- [ ] **Step 5: Progress measurement.** Record reuses next to Task 1's `potentialReuses`. If `RetiredEvictions()` in the long run exceeds 10% of retirements, also measure `kMaxRetiredCodeBytes` = 64 MB (one more long run) and keep the value with the lower long-run SH2 total. It must stay under the cap with no flushes.
- [ ] **Step 6: Commit** `perf(jit): reuse returning native code (2D A1)`.

### Task 4: Damp native compiles at unstable PCs (A3)

**Files:**
- Modify: `src/jit/include/brimir/jit/block_cache.hpp`, `src/jit/src/block_cache.cpp` (`Invalidate`, `Build`, `FlushAll`)
- Test: `tests/unit/test_jit_executor.cpp`

**Interfaces:**
- Produces: `constexpr uint32_t kMaxStaleDampShift` (chosen in Step 5 from 0, 2 and 4; 0 turns damping off). The constructor gains `uint32_t maxStaleDampShift = kMaxStaleDampShift` (last parameter).
- Rule: `Invalidate` (a stale drop, not a flush or IR eviction) increments a per-PC stale count (saturating at 255). `Build` sets `runsUntilNative` from `threshold << min(staleCount(pc), maxStaleDampShift)` instead of `threshold`, minus 1 as today. A reused block (Task 3) is native at once and ignores the count. `FlushAll` clears the counts. `uint64_t DampedBuilds() const` counts builds whose threshold was raised; bench prints it.
- Tests that need native code at once keep `kNativeOnFirstRun`. With threshold 1 the shift still applies (1 << n), so damping must be tested explicitly. Existing tests that rewrite code under `kNativeOnFirstRun` and expect native code at once pass `maxStaleDampShift = 0`. Check `"Executor: modified code in RAM is recompiled"` and the Task 2 and 3 tests.

- [ ] **Step 1: Write the failing tests** in `test_jit_executor.cpp`:
  - `"Block cache: a PC that keeps going stale waits longer for native code"` (x64): threshold 2, shift 2. After k stale rewrites (k = 0, 1, 2, 3), the rebuilt block runs on IR exactly `(2 << min(k, 2)) - 1` times and is native on the next run. `DampedBuilds()` counts the k > 0 builds.
  - `"Block cache: a damped PC still reaches native code"`: after 10 rewrites, with code then unchanged, the block is native after `2 << 2` runs; after `Flush()`, after 2 runs.
  - `"Block cache: damping off keeps the global threshold"`: shift 0, behavior as today.
- [ ] **Step 2: Run them.** Expected: compile errors (no constructor parameter).
- [ ] **Step 3: Implement** the counts and the rule; set `kMaxStaleDampShift = 2` as the provisional value.
- [ ] **Step 4: Run** `[jit]` on both backends, ctest, and BIOS lockstep 1200. Expected: all pass.
- [ ] **Step 5: Choose the shift** with temporary `build-bench` builds at 0, 2 and 4, run interleaved in one session (the B5 method): one Burning Rangers long run each, and SFZ3 and PD2 short runs, three each. Rule: among the values whose SFZ3 and PD2 SH2 totals are within 2% of the best, take the lowest long-run SH2 total; the long-run max frame breaks ties. Record a "2D A3" section in `sh2-x64-performance.md`, update the constant and its comment, and add the progress rows.
- [ ] **Step 6: Commit** `perf(jit): damp native compiles at unstable PCs (2D A3)`.

### Task 5: No flush on state load (A4)

**Files:**
- Modify: `src/core/src/ymir/hw/sh2/sh2.cpp:663-666` (`SH2::LoadState`), `src/core/BRIMIR_FORK.md`, `src/bridge/lockstep.cpp` and `include/brimir/lockstep.hpp`, `tools/brimir_bench.cpp`, `src/jit/include/brimir/jit/block_cache.hpp` (the `FlushesRequested` comment)
- Test: `tests/unit/test_sh2_jit_iface.cpp:77-89`, `tests/unit/test_jit_integration.cpp:58-73`, `tests/unit/test_jit_executor.cpp`, `tests/unit/test_jit_lockstep.cpp`

**Interfaces:**
- `SH2::LoadState` no longer flushes the executor. `SetJitExecutor` and `SH2::Reset` still do. The `// Brimir:` comment there says why (every block is checked against current memory before it runs; see Global Constraints), and `BRIMIR_FORK.md` gets an entry.
- Produces: `bool brimir::LoadLockstepState(CoreWrapper &a, CoreWrapper &b, const ymir::savestate::SaveState &state)` (loads `state` into both cores, as `SyncLockstepCores` does after saving), and the bench option `--lockstep-reload K`, only with `--lockstep N`. After every K compared frames, the run saves the reference core's state, runs and compares `K / 2` more frames, then loads the saved state into both cores and continues. Replayed frames count toward N. The help text describes it.
- If any step shows the flush is needed for exactness, keep it. Record why in `sh2-validation.md`, revert this task's `sh2.cpp` change and keep only the tests and the bench option (design A4: "keep it if it is required").

- [ ] **Step 1: Replace the flush tests with tests of the new behavior:**
  - `test_sh2_jit_iface.cpp`: rename to `"Attaching and Reset flush the executor; LoadState does not"`. The counts become 1, 2, 2 after `Load`, and 2 at the end.
  - `test_jit_integration.cpp`: rename to `"Save state round trip with the SH-2 JIT keeps the block caches"`. After `LoadState`, both caches' `Size()` equal their sizes before it, `FlushesRequested()` is unchanged, and the next frame runs without error.
  - `test_jit_executor.cpp` `"Executor: code changed by a state load is not run from the cache"` (both backends): compile and run P at `kCode` natively. Write Q over it and load an SH-2 state with PC = `kCode + 2` and `fetchedOpcodes` holding P's second word (memory holds Q's). The first `Step` interprets (fetch-buffer gate). Loading again with PC = `kCode` runs Q (the block reported stale, or failed validation, and was rebuilt). The results match the reference rig after each step.
  - `test_jit_lockstep.cpp` `"Lockstep: JIT core matches interpreter across state loads"`: the synthetic interrupt workload of `"Lockstep: interrupt-driven synthetic workload, JIT vs interpreter"`, with `LoadLockstepState` every 20 frames to a state saved 10 frames earlier; identical throughout.
- [ ] **Step 2: Run them.** Expected: the iface and integration tests fail (the flush still happens); the lockstep test fails to compile (no `LoadLockstepState`).
- [ ] **Step 3: Implement:** remove the flush, add `LoadLockstepState` and `--lockstep-reload`.
- [ ] **Step 4: Run** `[jit]` on both backends and ctest. Then `--lockstep 3600 --lockstep-reload 300` on the BIOS menu and the six titles, x64 and IR. Expected: `lockstep: OK`, exit 0, `compileFallbacks 0`. Record the runs in `sh2-validation.md`.
- [ ] **Step 5: Progress measurement** (state loads are not part of it; confirm that `flushes requested` in the window is 0 on both CPUs). Commit `perf(jit): keep compiled code across state loads (2D A4)` (fork log entry included).

### Task 6: Track A validation, measurement and report

**Files:**
- Modify: `design/sh2-x64-performance.md` ("Milestone 2D track A results"), `design/sh2-validation.md`, `design/sh2-jit.md` (status line), `CHANGELOG.md`; if the bar of design section 3 is met, also `src/bridge/` core option default for x86-64, `README.md` (design section 8 rollout)

- [ ] **Step 1: Lockstep:** x64 `--lockstep 36000` on the BIOS menu and the six titles; IR `--lockstep 1800` on all seven; x64 and IR `--lockstep 3600 --lockstep-reload 300` on all seven (Task 5 runs count if the code has not changed since). Expected: all identical, exit 0, `compileFallbacks 0`. Record in `sh2-validation.md` ("Milestone 2D, track A").
- [ ] **Step 2: ARM check** on the Raspberry Pi 3 (`ssh rpi3`): sync the branch, build `/mnt/data/build/bench` and `/mnt/data/build/tests` (never at the same time as a run), run `tools/bench/run_lockstep.sh 1800 ir` and `brimir_tests`. Expected: all identical; tests pass, skipped only where there is no x64 backend.
- [ ] **Step 3: Full measurement** with the 2C method (six titles plus BIOS, three rounds, interpreter / IR / x64), plus the long Burning Rangers run in both modes (`--warmup 2400 --frames 33600`). Write "Milestone 2D track A results": the table and ratios against track B; the long run with compile time, flushes, reuses, damped builds and peak native MB per CPU (bar: no code-cap flushes, peak at most 128 MB); max frames against the spike rule; and the default-on bar condition by condition.
- [ ] **Step 4: If all four conditions hold,** do design section 8's rollout (`brimir_sh2_jit` default `enabled` on x86-64, `disabled` on ARM64, "(Experimental)" label kept, README and `sh2-jit.md` updated). Otherwise list the conditions that failed, keep the JIT off by default, and write "What track A left": the remaining executor shrink needed per title, and whether track C (shared SH-2 code) or a per-block profile of generated code should come next.
- [ ] **Step 5: Update** `sh2-jit.md`'s status and `CHANGELOG.md` (Unreleased, one entry for track A).
- [ ] **Step 6: Commit** `docs(jit): milestone 2D track A results`, push the branch, wait for CI green, then use `superpowers:finishing-a-development-branch`.
