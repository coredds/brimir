# SH-2 JIT Milestone 2E - Faster Than the Interpreter, Without Stutter - Design

**Status**: design, draft for approval (2026-10-20); plan to follow in `design/plans/`
**Date**: 2026-10-20
**Builds on**: [sh2-jit-m2d.md](sh2-jit-m2d.md) (tracks A and B), [sh2-x64-performance.md](sh2-x64-performance.md) ("Milestone 2D track A results"), the target in [sh2-jit.md](sh2-jit.md) ("Target (2026-10-15)")

## 1. Intent

The goal of the JIT is unchanged: a fully compatible JIT that is faster than the interpreter, with smooth gameplay. That means lower SH-2 time on every title, with compile stalls not cancelling the gain (`sh2-jit.md`, "Target (2026-10-15)"). Milestone 2D measured itself against a ratio of 1.5. The user never set that figure, and this milestone replaces it with the target as the user stated it (section 3).

Where things stand after 2D (track A results, interpreter SH-2 time / x64 SH-2 time):

| Title | Ratio | What holds it back |
|---|---|---|
| Virtua Fighter 2 | 1.27 | nothing |
| Guardian Heroes | 1.14 | nothing |
| Panzer Dragoon II Zwei | 1.14 | nothing |
| Burning Rangers | 1.01 | at the edge |
| Sega Rally Championship | 0.97 | generated code: only 1,056 native compiles in the window |
| Street Fighter Zero 3 | 0.94 | compiling, 0.66 ms/frame (6,333 first compiles); without it about 1.09. Its 23-24 ms frames are compile bursts (interpreter max about 12 ms) |
| Burning Rangers, long session | 0.88 | compiling, 0.71 ms/frame: without it 0.99. Worst frames are 51-74 ms compile bursts (92 ms max, interpreter max 17 ms). The compile time is 11.8 s of front-end builds (mostly code that was dropped and rebuilt) and 14.2 s of native compiles |

Two of the three failing cases are compile cost on the emulation thread, and both of the stutter cases are compile bursts. So this milestone moves native compiling off the emulation thread, stops re-decoding code that comes back, and then works on the generated code for the one title where that is the problem.

## 2. Goals, non-goals, anti-goals

### Goals

1. **Background native compilation (track D, first).** Native compiles run on a worker thread. The block keeps running on the IR backend until its native code is ready, as it does today before its compile threshold.
2. **Reuse of dropped decoded blocks (track E, second).** When code at a PC comes back with the same opcodes, take back its decoded (IR) block as well, not only native blocks (2D A1). This removes most of Burning Rangers' 1.15M front-end builds.
3. **Generated code for Sega Rally (track F, third).** A per-block profile of the generated code (the 2D track A recommendation) on Sega Rally, plus Panzer Dragoon II Zwei and Street Fighter Zero 3 for comparison. Then the changes the profile points to, until Sega Rally is faster than the interpreter.
4. Meet the default-on bar (section 3). If it is met, switch `brimir_sh2_jit` on by default for x86-64 builds (section 8).

### Non-goals

- The ARM64 backend (milestone 3). ARM stays correct (IR lockstep and tests on the Raspberry Pi 3) but gets no speed requirement. The background thread's core-count gate (section 5) must leave room for ARM to require more cores.
- A fixed speed multiplier. The 1.5 of the 2D design is withdrawn.
- Hi-res and interlaced scenes, `brimir_cd_preload`, and the VDP2 render thread's spin-wait (`sh2-jit-m2d.md` section 9).
- Damping (2D A3). It stays off unless background compiling changes the measurement that left it off.

### Anti-goals (a failure even if it is faster)

- **Any loss of exactness.** Lockstep must stay identical to the interpreter. The point at which native code becomes available may vary from run to run, and that is allowed only because the IR and native forms of a block give identical results. Anything that lets that timing change emulated results is a bug.
- A data race between the worker and the emulation thread, or a crash or hang on unload, reset, state load, backend switch or core close (issue #7 was a crash on Close Content).
- Stutter the interpreter does not have (section 3, condition 2), in short or long sessions.
- Slowing the emulation thread by contention, for example through a lock held across a compile, or by spinning while waiting.

## 3. Default-on bar (replaces `sh2-jit-m2d.md` section 3)

The JIT becomes the default on x86-64 when all of these hold. They are measured on the development PC with the 2C method: the six titles plus the BIOS menu, `--warmup 2400 --frames 1800`, three rounds, interpreter and x64 back to back, one session.

1. **Faster:** interpreter SH-2 total / x64 SH-2 total is above 1.0 on each of the six titles (medians of three), and above 1.0 on the long Burning Rangers run (`--warmup 2400 --frames 33600`). *(My call: a title whose ratio is within 0.02 of 1.0 is measured again in a second session and must be above 1.0 in both, because sessions differ by up to about 5%.)* SH-2 total is the emulation thread's time in `SH2::Advance`, as in 2C and 2D. The worker's compile time is not part of it, because it runs on a spare core and does not delay frames. *(My call, following the user's acceptance of an extra thread.)* The report still gives the worker's CPU time per frame, so the cost is visible.
2. **Smooth:** in every window above, the number of frames over 16.7 ms (one frame at 60 fps) in each x64 run is at most the highest count of the interpreter runs of the same window. The x64 run's average whole-frame time is at most the interpreter's. *(My call. The 2D rule, the max frame within 10% of the interpreter's, failed on single noisy frames with no compile burst behind them. A frame over the 60 fps budget is what a player sees.)*
3. **Exact:** lockstep identical for 36,000 frames on x64 (BIOS menu and six titles), 1,800 frames on IR, and `--lockstep 3600 --lockstep-reload 300` on both. All `[jit]` tests, fuzz and ctest pass on Windows x64, Linux x64 and macOS ARM64 (CI). The Raspberry Pi's IR lockstep and test suite pass.
4. **Memory:** over the long Burning Rangers run, no native-code-cap flushes and a peak of native code at most 128 MB per CPU. Native code being compiled counts toward the cap, as does the retired pool.

If the bar is not met, the JIT stays off by default and the report says which conditions failed, with their figures, as in 2D.

## 4. Constraints

- **Exactness rules unchanged** (`sh2-jit.md` sections 2 and 7, `sh2-jit-m2.md` 4.4-4.5). Native code installed from the worker must be exactly what a synchronous compile would have produced for the same block and context. It goes through the same entry checks (prologue, `codePages`, `IsCurrent`) as all other code.
- **Code lifetime:** no generated code runs while it is being freed, and no link slot points at code that is freed or not yet installed. Publishing to the link table, installing into the block cache and freeing code all happen on the emulation thread, outside any running block (the 2D track A rules).
- **No data races.** The worker must not read emulator state the emulation thread may be writing. That includes the bus page table, which the emitter reads at compile time for code pages and constant-address pages (2D B2). Whatever the worker needs is copied on the emulation thread when the compile is queued, or proven unchanging. The Linux CI build should run the JIT tests under ThreadSanitizer, or the plan says why not.
- **Shutdown and resets:** flush, reset, state load, backend switch, disabling the JIT and core unload all work with compiles in flight. Results for blocks that were dropped or flushed in the meantime are discarded and their code freed, never installed. Destroying an executor waits for, or cancels, its compiles.
- **Determinism for tests:** a synchronous mode, with compiles done at once on the emulation thread as today, stays available to tests and diagnostics. Tests that count runs before native code (`kNativeCompileThreshold`, `kNativeOnFirstRun`) keep their meaning in that mode.
- **Both backends:** the IR backend has no native compiles, so no worker. Track E (decoded-block reuse) applies to both and must keep the IR backend correct.
- **Fork policy and includes** as in 2D: JIT logic in `src/jit/`; SH-2 fork-scope edits marked `// Brimir:` and logged in `src/core/BRIMIR_FORK.md`; `brimir-jit` includes only the existing core headers; asmjit only under `src/jit/src/x64/`.
- **Memory:** the IR cap (`kMaxCachedInsts`) and the native-code cap (128 MB per CPU) do not grow. Retired decoded blocks (track E) count toward the IR cap.
- **Measurement:** the development PC (Ryzen 7 5700G, 8 cores / 16 threads, Windows, MSVC, `build-bench`), the 2C method, same-session comparisons only, machine load checked and recorded.

## 5. Track D: background native compilation

Evidence: Street Fighter Zero 3 spends 1,187 ms compiling in its window. About 1.08 s of that is native compiles (6,333 x about 170 µs; asmjit register allocation is 77% of a compile) and about 0.1 s is front-end builds. In Burning Rangers' long run, 14.2 s of 26 s is native compiling.

Decided:
- The block's threshold run, the Nth run that compiles it today, queues the compile instead. The block keeps running with `RunBlock` until the result is installed. Reuse from the retired pool (2D A1) stays immediate: it needs no compile.
- Results are installed on the emulation thread, at a point where no block runs (for example at `BlockCache::Get`), and only if the block they were compiled from is still the cached block for that PC.
- **One extra thread** is used for this, an internal choice the user accepted. It is enabled when the host has enough cores and disabled otherwise, falling back to synchronous compiles. The x86-64 threshold is the builder's call (at least 4 logical cores is the expected value). For ARM64 it is decided in milestone 3 and may be higher.
- A core option lets the user turn it off. *(My call: `brimir_sh2_jit_compile_thread`, values `auto` (default) and `disabled`, labelled experimental like `brimir_sh2_jit`.)*

Left to the builder:
- One worker per executor or one shared by both CPUs.
- The queue structure and its ordering, for example most-run blocks first.
- Whether a block that waits too long in the queue is compiled synchronously after all.
- How the worker gets its inputs without racing (copy or snapshot).
- Whether the queue is bounded, and what happens at its bound.

## 6. Track E: reuse of dropped decoded blocks

Evidence: in the long Burning Rangers run, track A took back 885,877 native blocks, but the front end still built 1.15M blocks, mostly code that was dropped and rebuilt. Those builds took 11.8 s, as much as the native compiles.

Decided: a dropped IR-only block whose code comes back with the same opcodes, on the same pages, is taken back without running the front end, under the same rule as 2D A1. Pages are checked where the block depends on them. IR-only blocks are checked by `IsCurrent`, not by a prologue, so the builder states what a reused IR block must be checked against. Retired decoded blocks count toward the IR cap.

Left to the builder: whether this shares the native retired pool or uses its own, its bounds, and the eviction order.

## 7. Track F: generated code (Sega Rally)

Evidence: Sega Rally's ratio is 0.97 with only 1,056 native compiles in its window, so compile cost is not the cause. Track B's per-instruction changes gave 3-6% overall, and nothing so far has looked at the code shape per block or at the dispatch between blocks.

Decided: first a per-block profile of x64 execution: Sega Rally, Panzer Dragoon II Zwei and Street Fighter Zero 3, both CPUs. For the hottest blocks it records:
- share of executor time;
- guest instructions;
- host cost per guest instruction;
- exits and chain misses;
- trampoline calls by kind.

Dispatch between blocks (`Executor::Step`, `Get`, chain misses) is separated from time inside generated code. The profile is a temporary instrumented build, as in 2C and 2D, and is not committed. It is recorded in `sh2-x64-performance.md`.

Then: the changes the profile points to, one at a time, each measured as in 2D. The track covers every title that is still at or within 0.02 of 1.0 after tracks D and E, not only Sega Rally. Burning Rangers' short run (1.01 today) is the likely second; the profile includes it if so. *(My call.)* The track ends when every title passes condition 1, or when the profile shows no change is likely to get a title there. In that case the report says so with the numbers and recommends what would.

Left to the builder: the instrumentation method and the changes themselves. They must keep exactness and both backends correct.

## 8. Order, validation and rollout

**Order:** track D, then E, then F. Each track ends with the progress measurement: Burning Rangers long run, plus Street Fighter Zero 3, Panzer Dragoon II Zwei and Sega Rally short runs, three rounds. The milestone ends with the full measurement and the bar of section 3, condition by condition, in `sh2-x64-performance.md` ("Milestone 2E results"). Within a track the builder orders the items by measurement, and may drop an item that measurement shows does not help, recording it with the numbers.

**Validation**, in addition to the existing suite:
- **Track D tests:**
  - a compile still in flight across a flush, an invalidation of its block, a reset, a state load and executor destruction;
  - installation only into the unchanged block;
  - synchronous mode matching today's behavior;
  - lockstep identical with the worker on, including `--lockstep-reload`.
- **Track E tests:** reuse after reload, no reuse after a remap or a change, and the IR cap accounting.
- **After each track:** lockstep on the BIOS menu and six titles (1,800 frames x64 and IR), and IR lockstep plus the test suite on the Raspberry Pi 3. At the end of the milestone, 36,000 frames on x64.

**Rollout** if section 3 holds (from `sh2-jit-m2.md` section 6, narrowed): on x86-64 builds the JIT is on by default only where the compile thread runs (`brimir_sh2_jit_compile_thread` at `auto` and the host above the core threshold). Elsewhere, and on ARM64, it stays off by default. *(My call: the bar is only measured with the thread on, and synchronous compiling is what fails the smoothness condition today.)* The option keeps its "(Experimental)" label for one release; README, CHANGELOG and `sh2-jit.md` are updated. The builder decides how the default is expressed, for example as an `auto` value of `brimir_sh2_jit`.

**Documents:** `sh2-jit-m2d.md` section 3 gets a note that this design replaces it. `sh2-jit.md`'s status line stops citing the 1.5 bar.
