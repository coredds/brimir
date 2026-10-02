# SH-2 JIT Milestone 2C — x64 Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make SH-2 host time with the x64 backend at least 2x lower than Ymir's interpreter on the six baseline titles (`design/sh2-jit-m2.md` §1). Remove the compile stalls, stay bit-identical, then turn the JIT on by default on x86-64.

**Architecture:** The tasks follow the profile in `design/sh2-x64-performance.md` ("Conclusions for a follow-up plan"), largest and best-measured item first:
1. Turn the pipeline-refill and delay-slot calls into inline stores and loads.
2. Replace per-block dispatch with in-backend block chaining through a link table, validating each block in its own prologue.
3. Fold per-instruction timing bookkeeping in an IR pass shared by both backends.
4. Keep guest registers in host registers inside x64 blocks.
5. Bound compile cost and cache churn.
6. Validate, measure and decide on rollout.

Each task ends with a measurement, so later tasks can be re-sized from data.

**Tech Stack:** C++20, CMake 3.28+, asmjit (vendored), Catch2 (amalgamated), MSVC 2022 / GCC 14 / Apple Clang.

**Spec:** `design/sh2-jit-m2.md` §4–§6. **Profile:** `design/sh2-x64-performance.md`. **Semantic reference:** `src/jit/src/interp_backend.cpp` (`RunBlock`), which matches the interpreter.

## Global Constraints

- **Fork scope:** `src/core/include/ymir/hw/sh2/*`, `src/core/src/ymir/hw/sh2/*`, and the read-only page-table accessor in `src/core/include/ymir/sys/bus.hpp`. Fork edits are marked `// Brimir:` and logged in `src/core/BRIMIR_FORK.md`.
- **JIT off:** emulation is byte-identical to today.
- **JIT on, either backend:** identical to the interpreter (state, memory, bus access sequence, cycles, peripherals) at every `Advance()` return. The only accepted deviations are those in `design/sh2-jit.md` §6.5. Lockstep is the oracle: any divergence is a bug.
- **`Executor::Step` keeps its single-block contract** (one block or one interpreted instruction per call, exact `retired`). Chaining happens only under `Executor::Run`.
- **Exceptions:** no exception crosses generated code. **Code lifetime:** no generated code runs while its memory is freed; flushes stay deferred while a block or chain runs.
- **Includes:** `brimir-jit` includes only `ymir/core/types.hpp`, `ymir/hw/sh2/sh2_jit_iface.hpp` and `ymir/hw/sh2/sh2_decode.hpp` from the core. asmjit headers are included only under `src/jit/src/x64/`.
- **Portability:** everything builds and passes without x64 (macOS ARM64 CI).
- **Tests:** Catch2 in `tests/unit/`, JIT tests tagged `[jit]`. Run every `[jit]` test on both backends (`BRIMIR_JIT_BACKEND`). Never weaken an assertion.
- **Workflow:** branch `feature/sh2-jit-2c`. Push the branch for Windows/Linux/macOS CI. Commit messages use `type(scope): subject`. Never stage `ROADMAP.md`, BIOS/ROM or `.smpc` files.

## Build, test and measurement commands (Windows)

`pwsh -NoProfile -File $env:TEMP\opencode\msvc.ps1 -Cmd "<command>"` loads MSVC.

```powershell
cmake --build build --target brimir_tests brimir_libretro brimir_bench
build\bin\brimir_tests.exe "[jit]"                       # x64 (default backend)
$env:BRIMIR_JIT_BACKEND='ir'; build\bin\brimir_tests.exe "[jit]"; Remove-Item Env:BRIMIR_JIT_BACKEND
ctest --test-dir build --output-on-failure
build\bin\brimir_bench.exe --bios tests\fixtures\sega_101.bin --lockstep 1200 --jit-backend x64
```

**Progress measurement** (the end of every task from 1 to 5):
- Rebuild `build-bench` (Release+LTO; `cmake --build build-bench --target brimir_bench`).
- Use the BIOS, games and scratch system dir from `design/plans/2026-10-01-sh2-jit-m1d-coverage-validation.md` Task 8.
- Run Panzer Dragoon II Zwei and Street Fighter Zero 3 with `--warmup 2400 --frames 1800`, three times each, in two modes: the interpreter, and `--sh2-jit --jit-backend x64`.
- Append one row per title to the "2C progress" table in `design/sh2-x64-performance.md`: task, interpreter SH2 total (median), x64 SH2 total (median), ratio, x64 max frame ms, and x64 `nativeBlocksRun`/`compileFallbacks`. Create the table in Task 1.
- Commit the table with the task.

---

### Task 1: Inline pipeline refills and delay-slot calls

**Files:**
- Modify: `src/jit/include/brimir/jit/ir.hpp`, `src/jit/src/ir.cpp`, `src/jit/src/interp_backend.cpp`, `src/jit/src/frontend.cpp`, `src/jit/src/x64/x64_emitter.cpp`, `src/jit/src/x64/x64_backend.cpp`, `design/sh2-x64-performance.md`
- Test: `tests/unit/test_jit_x64.cpp`, `tests/unit/test_jit_diff.cpp`, `tests/unit/test_jit_backend.cpp`, `tests/unit/jit_random_ir.cpp`

**Semantics to keep.** `Refill(addr)` writes `m_fetchedOpcodes = MemRead<uint32, instrFetch>(addr)`.
- The buffer is observable at every exit:
  - the `Executor::Step` gate at `PC & 2`;
  - `SH2::FetchInstruction` when the interpreter continues;
  - save states and lockstep.
- So it must hold the right value at every exit point (boundary stubs, bus-wait exits, abort exits, final exits) and before any callback.
- Generated code itself never reads it.

**Changes:**
- **Front end.** Record one extra tail word in `Block`: `std::vector<uint16_t> guestOpcodes` gains the word after the last instruction when that address is compilable. Add `bool hasTailWord`; `IsCurrent` checks it like the others.
  - **Why:** the refill at the last aligned instruction reads it.
  - **Refills inside the block:** every `Refill(a)` with `a` and `a+2` inside `guestOpcodes` now has a known value. Emit it as `Refill(a)` with a new `flag` = *known*, and `imm2` = `(op[a] << 16) | op[a+2]`.
  - **Refills that read other code** (taken BT/BF, `ExitIf` with refill) stay runtime.
- **IR backend:** a known `Refill` stores `imm2` to `*ctx.fetchedOpcodes` instead of calling `refillPipeline`. That is valid only under the conditions below; when they fail, both backends must call the callback.
- **When a known value may be used (both backends):**
  1. **Instruction fetches from array pages only.** At compile time, record for the block whether every word of `guestOpcodes` lies on an array page (`FastPeek16` succeeds for each), as `Block::fetchFromArrays`. If false, every refill stays a callback.
     - **Remap safety:** because a page could be remapped later, x64 blocks with `fetchFromArrays` check at entry that each code page's `array` pointer equals the compile-time pointer (Task 2 builds this into the prologue; in this task, add it as the first thing the generated function does). On mismatch the block returns `ExitInfo{ .stale = true }` (new field, nothing else written).
     - `Executor::Step` treats `stale` like a failed `IsCurrent`: invalidate, recompile, and run the new block in the same `Step` call. `BlockCache` gets `Invalidate(pc)` for this.
     - The IR backend keeps `IsCurrent` (peek through `FastPeek16` or the callback) and additionally checks `fetchFromArrays` against the current pages before using known values.
  2. **No write to the block's own code before the refill.** A frame flag `codeDirty` starts false at block entry.
     - Both backends classify each data access the same way, with `FastArrayPointer`: an *array access* (partition 0/1/5 on an array page) or a *handler access* (everything else). This holds even though `RunBlock` performs every access through `ctx.read`/`ctx.write`.
     - **Array-page store:** set the flag when its host pointer falls inside the block's host code range, `[array + (start & 0xFFFF), array + (endTail & 0xFFFF))` per page. The range is computed at compile time and stays valid because of the entry check. Using host pointers covers the cached/cache-through aliases and the RAM mirrors.
     - **Handler access:** set the flag after every handler write, and after every handler read outside partition `0b111`.
     - **Audit task:** read `SH2::OnChipRegRead` (`sh2.cpp`) and confirm that on-chip reads cannot write memory. Document the result in a comment. If they can, set the flag after all handler reads.
     - A known refill with `codeDirty` set calls the callback instead.
     - Code that assumes `guestOpcodes.size()` equals the block's instruction word count must use the count without the tail word.
- **Runtime refills** (taken BT/BF targets, `EndDelaySlot` at a target with bit 1 set) use the inline array-page load from Task 4 of 2B (`FastArrayPointer` with size 4, instruction fetch, `bswap`), falling back to the trampoline.
- **Delay slots:** lower `SetupDelaySlot` and `EndDelaySlot` inline in x64 by transcribing `SH2::SetupDelaySlot` and `SH2::AdvancePC<…, delaySlot = true>` with cache emulation off (`sh2.cpp`; read both first). Keep the exact field updates and any refill they make, with the refill as a runtime inline load plus fallback. Keep the trampolines for the non-array fallback.

**Tests:**
- `test_jit_diff.cpp`, **"Self-modifying store before a refill"**:
  - code at `0x06001000` stores over the *next* instruction pair, through the cached alias, the cache-through alias `0x26001000`, and the mirror `0x06101000`;
  - then runs into it;
  - with a cycle target that stops at each boundary, so the fetch buffer is observed;
  - interpreter, IR and x64 must be identical (Advance level, peripherals compared).
- **"Write callback that modifies code":** hook `ctx.write` (as the executor flush tests do) to also poke new opcodes into the block's RAM. Identical on both backends, which shows `codeDirty` after write callbacks.
- **"Remapped code page":**
  - compile a block on RAM;
  - `rig.bus.MapArray` the page to a different array with the same opcodes but a different tail word;
  - step: the block reports `stale` once and is recompiled;
  - state matches the interpreter.
- `jit_random_ir.cpp`: generate known refills (with a consistent `guestOpcodes`) so the random comparison covers them.
- Executor stats: add `staleEntries` (counted). Expect `== 0` in the BIOS lockstep.

- [ ] **Step 1:** Write the tests; run them and record which fail (the stale and remap tests must fail).
- [ ] **Step 2:** Implement in the front end, IR, both backends and the executor.
- [ ] **Step 3:** `[jit]` on both backends, full suite, `ctest`, BIOS lockstep 1200 on both backends.
- [ ] **Step 4:** Progress measurement (create the "2C progress" table with a "2B" baseline row first). Commit `perf(jit): store known fetch-buffer values and inline delay slots`.

---

### Task 2: Block chaining and in-block validation

**Files:**
- Modify: `src/jit/include/brimir/jit/backend.hpp`, `src/jit/include/brimir/jit/interp_backend.hpp` (`ExitInfo`), `src/jit/include/brimir/jit/block_cache.hpp`, `src/jit/src/block_cache.cpp`, `src/jit/include/brimir/jit/executor.hpp`, `src/jit/src/executor.cpp`, `src/jit/src/x64/x64_backend.hpp/.cpp`, `src/jit/src/x64/x64_emitter.cpp`, `design/sh2-x64-performance.md`
- Test: `tests/unit/test_jit_x64.cpp`, `tests/unit/test_jit_executor.cpp`, `tests/unit/test_jit_diff.cpp`

**Design:**
- **Generated function:** each x64 block becomes `const void *block(X64Frame *)`. The return value is the next block's entry, or `nullptr` to return to the executor.
- **Run loop:** `X64Backend::Run` loops `while (fn) fn = fn(&frame);` and accumulates into one `ExitInfo`. It is a plain host loop: no frames between blocks, and no unwinding concerns.
- **Prologue** of every native block, in this order (it mirrors `Executor::Step` exactly):
  1. Page-array check from Task 1, then compare the block's code bytes in host memory with the compiled opcodes (8-byte, then 4- and 2-byte compares of the raw bytes). This replaces `IsCurrent` for native blocks. On mismatch: `stale`.
  2. When entered by chaining (a frame flag `chained`), repeat the executor's gates before the block runs:
     - **Pending interrupt:** if `*intrPending && *intrAllow`, return `nullptr` (the executor then interprets the interrupt entry). Its `delaySlot` is always false here; check that with a debug assert.
     - **Fetch-buffer gate:** when `PC & 2`, compare `(uint16)*fetchedOpcodes` with the first compiled opcode, which step 1 has just shown equals memory. On mismatch, return `nullptr`.
  3. `*intrAllow = true`. Increment `frame->blocksRun`.
- **Exits that may chain:** static `Exit`, taken `ExitIf` and `ExitDynamic` with `frame->allowChain`.
  - After writing PC and `out.cycles` as today, store `*cyclesExecuted = entryCycles + cycles`, as `Executor::Run` does between steps.
  - If `cycles >= limit`, return `nullptr`.
  - Look up the link table (below) with PC. On a hit, set `chained` and return the entry; on a miss, return `nullptr`.
  - Boundary, bus-wait, abort and stale exits never chain.
- **Link table:** owned by `X64Backend`, direct-mapped, 16384 slots of `{uint32 pc; const void *entry;}` indexed by `(pc >> 1) & 16383`. Generated code embeds the table address.
  - `INativeBackend` gains `void Publish(uint32_t pc, const NativeCode &)` and `void Unpublish(uint32_t pc)`.
  - `BlockCache` publishes after a successful compile and unpublishes on invalidation. `Reset` clears the table.
  - A slot only matches its exact `pc`.
- **Executor:**
  - `Run` sets `allowChain = true`. `Step` (the public single-step API) sets it false, so tests that count `retired` per step are unchanged.
  - `ExitInfo` gains `uint32_t blocksRun` (native blocks run in this call) and `bool stale`.
  - Stats add `blocksRun` and `nativeBlocksRun` from it, so the counts stay exact.
  - For native blocks, `BlockCache::Get` no longer calls `IsCurrent`; the prologue validates instead. It still does so for IR blocks.
  - `stale` handling as in Task 1.
  - `m_inBlock`/`BlockScope` cover the whole chain; a deferred flush aborts the running block, which ends the chain.

**Tests:**
- `test_jit_x64.cpp`:
  - **"Chained run equals stepped run":** for 300 seeds of the fuzz program generator (reuse `test_jit_diff.cpp`'s generator by moving it to a shared helper if needed), run one rig with `Executor::Run` (chaining) and one with repeated `Step`. Both must match the interpreter at every `Advance` return, peripherals compared.
  - **"Chain stops at a pending interrupt":** a store to MMIO that the test hooks to raise an interrupt (use the DIVU interrupt setup from the LDC SR tests in `test_jit_diff.cpp`) mid-chain. The interrupt is taken at exactly the interpreter's instruction.
  - **"Chain respects the PC & 2 fetch-buffer gate":** a block exits to an odd-half target whose fetch buffer, written by the previous block's refill, disagrees with memory (code modified after the fetch). Execution matches the interpreter.
  - **"Stale block inside a chain":** modify code at a linked target between `Advance` calls. The prologue detects it, the block is recompiled, and results match.
  - **"Flush during a chain":** the flush-hook pattern on a block reached by chaining. The flush is deferred, the chain aborts, and the cache is emptied after `Run`.
- The executor tests in `test_jit_executor.cpp` keep passing unchanged (`Step` does not chain).

- [ ] **Step 1:** Write the tests; confirm failures.
- [ ] **Step 2:** Implement.
- [ ] **Step 3:** `[jit]` on both backends, full suite, `ctest`, BIOS lockstep 1200 on both backends.
- [ ] **Step 4:** Progress measurement. Commit `perf(jit): chain x64 blocks through a link table`.

---

### Task 3: IR timing-bookkeeping pass

**Files:**
- Create: `src/jit/include/brimir/jit/ir_opt.hpp`, `src/jit/src/ir_opt.cpp`
- Modify: `src/jit/CMakeLists.txt`, `src/jit/src/block_cache.cpp` (run the pass after `BuildBlock`), `src/jit/src/x64/x64_emitter.cpp` (new op variants), `src/jit/src/interp_backend.cpp`, `src/jit/src/ir.cpp`, `design/sh2-x64-performance.md`
- Test: `tests/unit/test_jit_ir_opt.cpp` (new, register it), `tests/unit/test_jit_x64.cpp`

**Pass** `void OptimizeBlock(Block &block);`. It runs for both backends, so the IR backend validates it against the interpreter. Rules:
1. **Write-back folding.**
   - Track the known `m_wbReg` value through the block: unknown at entry, set by `SetWb`.
   - **Audit:** before relying on this, list every op whose callback can write `m_wbReg` (check `SH2::SetupDelaySlot`, `AdvancePC`, `JitSetSR` and the memory paths in `sh2.cpp`). Such ops make it unknown again. Document the list in the pass.
   - A `WbStall(mask)` with a known value becomes `AddCycles(1)` if the value's bit is in `mask` (a value ≤ 16), else it is removed.
   - Keep every `SetWb` (it is a byte store, and every exit needs the value).
2. **Interrupt test elision in `CheckBoundary`.** Add a flag `cyclesOnly` to `CheckBoundary`: when set, only the budget is tested.
   - A `CheckBoundary` may be `cyclesOnly` if an earlier `CheckBoundary` in the same block did the full test, and no op between them can change `*intrPending` or `*intrAllow`.
   - Ops that can change them: `Load`, `Store`, `Refill` (callback form), `AddAccessCycles`, `AddAccessCyclesRMWByte`, `ExitIfBusWait`, `SetSR`, `SetupDelaySlot`, `EndDelaySlot`, `ClearIntrAllow`, `SetIntrAllow`. Treat every callback-capable op as changing, because the pass cannot know which accesses go inline.
   - If `ClearIntrAllow` is the last such op before the check, the interrupt part is known false. That allows `cyclesOnly` too, until the next changing op.
3. **Cycle merging.** Merge consecutive `AddCycles` with no `CheckBoundary`, `SyncCycles`, exit or callback-capable op between them.
4. **`SyncCycles`:** drop a `SyncCycles` when an earlier one in the same block has no `AddCycles`/`AddAccessCycles`/`WbStall`/`AddAccessCyclesRMWByte` between them.

**Tests (`test_jit_ir_opt.cpp`):**
- Hand-written blocks for each rule, checking the exact output ops, plus negative cases (a `Load` between two checks keeps the full test; an unknown wb at entry keeps `WbStall`).
- **Random equivalence:** for 2000 random blocks from `jit_random_ir` (all options), `RunBlock(original)` and `RunBlock(optimized)` on identically seeded rigs give identical `ExitInfo` and `DiffRigs` (peripherals compared).
- The existing fuzz, opcode and lockstep suites run with the pass on (it is always on).
- Report static op counts per guest instruction before/after over the BIOS lockstep's compiled blocks (temporary counter or a test helper; do not commit instrumentation into hot paths).

- [ ] **Step 1:** Write the tests; confirm failures.
- [ ] **Step 2:** Implement the pass and the `cyclesOnly` op variant in both backends.
- [ ] **Step 3:** `[jit]` on both backends, full suite, `ctest`, BIOS lockstep 1200 on both backends.
- [ ] **Step 4:** Progress measurement. Commit `perf(jit): fold write-back stalls and redundant boundary checks`.

---

### Task 4: Guest register caching in x64 blocks

**Files:**
- Modify: `src/jit/src/x64/x64_emitter.hpp/.cpp`, `design/sh2-x64-performance.md`
- Test: `tests/unit/test_jit_x64.cpp`, `tests/unit/jit_random_ir.cpp`

**Design:**
- **What is cached:** the emitter keeps a per-block cache for R0–R15 and SR, each with a virtual register and a dirty bit.
  - `GetReg` loads on the first use and then reuses the value; `SetReg` updates the virtual register and marks it dirty.
  - `GetT`/`SetT`/`SetSRBits` work on the cached SR.
- **Flush points** (store the dirty values, keep the cache valid):
  - before every trampoline call;
  - on every exit path (boundary stubs, bus-wait exits, abort exits, final exits);
  - before `SetSR`, `Div1`, `MacW` and `MacL`, whose helpers read SR through `ctx` — or pass SR by value and reload it.
- **Invalidate after** (drop the cache, reload on the next use):
  - `SetSR`, `Div1`, `EndDelaySlot`;
  - any trampoline that could change guest registers.
  - **Audit:** do the read/write callbacks change guest registers? (They should not.) Document the conclusion.
- **Branch paths:** values computed only on an `ExitIf` path must not leave the cache inconsistent on the fall-through path. Flush dirty state before the conditional branch, or emit the flush only on the taken path while keeping the fall-through cache unchanged.
- Exceptions must leave the same state as IR. The 2B exception-parity tests cover this; keep them passing.

**Tests:**
- **Random comparison** with spill pressure: many guest registers set and read across `Load` (callback), `SetSR` and `ExitIf` at random positions. x64 must match IR on 4000 seeds.
- **Exception and abort parity** tests from 2B, plus a new case: dirty registers when a write callback throws. The state must equal IR's.

- [ ] **Step 1:** Write the new tests (they pass on the current x64, which is fine: they guard the refactor). Record their seed count and runtime.
- [ ] **Step 2:** Implement.
- [ ] **Step 3:** `[jit]` on both backends, full suite, `ctest`, BIOS lockstep 1200 on x64.
- [ ] **Step 4:** Progress measurement. Commit `perf(jit): keep guest registers in host registers inside x64 blocks`.

---

### Task 5: Compile cost and cache churn

**Files:**
- Modify: `src/jit/include/brimir/jit/block_cache.hpp`, `src/jit/src/block_cache.cpp`, `src/jit/include/brimir/jit/executor.hpp`, `src/jit/src/executor.cpp`, `tools/brimir_bench.cpp`, `design/sh2-x64-performance.md`
- Test: `tests/unit/test_jit_executor.cpp`, `tests/unit/test_jit_x64.cpp`

**Changes:**
- **Drop the IR of native blocks.** After a successful native compile, the cache entry keeps only `startPC`, `guestOpcodes`, `guestInstrCount` and the native code; its IR `code` vector is freed.
  - `kMaxCachedInsts` then counts only IR-only blocks.
  - The native cap stays in bytes. Measure the native bytes per block after Tasks 3–4, and set `kMaxNativeCodeBytes` so that Street Fighter Zero 3's master working set (about 30,600 PCs) fits. Record the computation in the performance doc.
- **Tiered compilation.** A new PC is first built as IR and run with `RunBlock`. It is compiled natively on its Nth run, with `constexpr uint32_t kNativeCompileThreshold`. Measure 1, 2, 4 and 8 on Street Fighter Zero 3 and Panzer Dragoon II Zwei, then pick the value with the best max-frame time at no more than 2% loss in average SH2 time.
  - Counting: a run counter per cache entry.
  - The link table is published only once a block is native.
- **Bench output:** per CPU, add `compiles`, `nativeCompiles`, total compile time (ms) and `flushes`.

**Tests:**
- **Threshold behaviour:** a block runs N−1 times on IR (`nativeBlocksRun` stays 0), then natively.
- **IR freed after a native compile:** the entry no longer holds IR. `kMaxCachedInsts` accounting ignores native entries; check with a test-only accessor.
- **Unchanged guarantees:** the existing cap, flush and invalidation tests still pass.

- [ ] **Step 1:** Write the tests; confirm failures.
- [ ] **Step 2:** Implement; run the threshold experiment and record it.
- [ ] **Step 3:** `[jit]` on both backends, full suite, `ctest`, BIOS lockstep 1200 on both backends.
- [ ] **Step 4:** Progress measurement, adding max-frame ms for Virtua Fighter 2. Commit `perf(jit): tiered native compilation and native-only cache accounting`.

---

### Task 6: Validation, measurement and rollout decision

**Files:**
- Modify: `design/sh2-validation.md`, `design/sh2-x64-performance.md`
- If the target is met: also `src/libretro/options.cpp`, `src/libretro/libretro.cpp`, `README.md`, `CHANGELOG.md`, `design/sh2-jit.md`, `design/sh2-jit-m2.md`

**Steps:**
- **Lockstep:** 36,000 frames on x64 for the BIOS menu and all six titles. Use the procedure, content and debugging rules of 1D Task 8, with output in `.superpowers\sdd\m2c-ls-<title>.out`. Then 1,800 frames on `--jit-backend ir` for each title, because the IR pass changed the IR backend too.
  - Record the results under "Milestone 2C" in `design/sh2-validation.md`.
  - On any divergence: write a regression test, fix it, commit `fix(jit): ...`, and rerun.
- **Measurement:** the full 2B method on all six titles plus the BIOS, run three ways (interpreter, IR, x64) as a new "Milestone 2C results" section.
  - **Verdict line:** "target met" if the ratio is at least 2.0 on all six titles, otherwise "target missed".
  - Also record the max-frame times.
- **If met:** apply the rollout from plan 2B Task 7:
  - `brimir_sh2_jit` defaults to `enabled` when `BRIMIR_JIT_HAS_X64`, and stays "(Experimental)";
  - update the description and the docs;
  - run a 1,800-frame lockstep smoke test with the libretro default;
  - commit `feat(libretro): enable the SH-2 JIT by default on x86-64`.
- **If missed:** do not change defaults. Add a re-profile with the per-category breakdown of the 2B profile (same method) and a short list of next steps, then stop and report to the user.

- [ ] **Step 1:** Lockstep runs; fix any divergence.
- [ ] **Step 2:** Measurement and verdict.
- [ ] **Step 3:** Rollout or re-profile, per the verdict.
- [ ] **Step 4:** Commit `docs(design): record milestone 2C validation and performance` (plus the rollout commit if met).

---

## Self-review notes

- **Profile items:**

  | Item | Task |
  |---|---|
  | 1, refills and delay slots | 1 |
  | 2, dispatch and linking | 2 |
  | 3, generated code | 3 (folding, both backends), 4 (register caching) |
  | 4, cache and compile cost | 5 |
  | 5, slave FTCSR polling | not planned: about 0.2–0.4 ms/frame; added only if the Task 6 re-profile needs it |
  | 6, interpreter fallbacks | not planned: under 1% |

- **Exactness hazards called out:**
  - the fetch buffer at every exit;
  - self-modifying stores, aliases and mirrors (handled through host pointers);
  - write callbacks that touch code;
  - page remapping;
  - the PC & 2 gate and interrupt gate at chain points;
  - `cyclesExecuted` between chained blocks;
  - write-back and interrupt state across callbacks;
  - register-cache flushes before callbacks and on every exit, including exceptions.
- **Single-step contract** is preserved (`Step` never chains), so per-step tests keep their meaning. Chaining is covered by `Advance`-level tests and lockstep.
