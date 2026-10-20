# SH-2 JIT Milestone 2D - x64 Speed and Long Sessions - Design

**Status**: design, approved scope (2026-10-15); plan to follow in `design/plans/`
**Date**: 2026-10-15
**Builds on**: [sh2-jit-m2.md](sh2-jit-m2.md) (x64 backend design), [sh2-x64-performance.md](sh2-x64-performance.md) ("Milestone 2C results": measurements, re-profile, long-session behavior, "Next steps"), the target in [sh2-jit.md](sh2-jit.md) ("Target (2026-10-15)")

## 1. Intent

Make Brimir faster in general on x86-64 PCs by turning the SH-2 JIT from break-even into a clear speedup that can be switched on by default. Most testing happens on a PC for now. The titles that do not run well today and should benefit most are **Burning Rangers** and **Virtua Fighter 2**.

Where things stand after milestone 2C (same-session ratios, interpreter SH-2 time / x64 SH-2 time, measurement scene): Virtua Fighter 2 1.25, Guardian Heroes 1.11, Panzer Dragoon II Zwei 1.09, Sega Rally 0.98, Burning Rangers 0.97, Street Fighter Zero 3 0.91. Over a long session Burning Rangers drops to **0.55**, with frames up to 173 ms, because it keeps reloading code: 2.2M block builds and 492k native compiles in 33,600 frames, and full flushes at the 128 MB code cap.

## 2. Goals, non-goals, anti-goals

### Goals

1. **Track B, per-instruction cost (first):** lower the cost of generated code per guest instruction on every title.
2. **Track A, long sessions (second):** stop Burning Rangers-style code reloads from turning the JIT into a slowdown: reuse code that comes back, free stale code individually, avoid whole-cache flushes.
3. Reach the **default-on bar** (section 3) and, if it is met, switch `brimir_sh2_jit` on by default for x86-64 builds.

### Non-goals (this milestone)

- The ARM64 backend (milestone 3), and ARM performance in general. ARM must stay correct (section 4), not faster.
- Slowdowns in hi-res and interpolated (deinterlaced) scenes. These are most likely VDP rendering, which the JIT does not touch. They are a separate project; see section 9.
- Input latency, run-ahead and preemptive frames as goals. (The save-state flush in track A still matters for users who enable run-ahead or rewind.)
- The `brimir_cd_preload` default and the VDP2 render thread's spin-wait (both recorded in `sh2-jit.md` as low-spec items).

### Anti-goals (a failure even if it is faster)

- **Any loss of exactness.** Every change keeps the JIT identical to the interpreter in lockstep (`sh2-jit.md` sections 2 and 7). An optimization that is only "almost always" right is rejected.
- A speedup that comes with new frame-time spikes (compile bursts, flushes) larger than the interpreter's own worst frames.
- Wins that only show in the 1,800-frame measurement window and disappear over a long session.
- Changes to emulation behavior outside the SH-2 to make the JIT faster. Small hooks outside the fork scope are allowed (section 4).

## 3. Default-on bar

> **Replaced (2026-10-20)** by [sh2-jit-m2e.md](sh2-jit-m2e.md) section 3. The user's target was always the one in `sh2-jit.md` ("Target (2026-10-15)"): faster than the interpreter on every title, smooth, and fully compatible. The 1.5 ratio below was never set by the user. The 2D reports measured against it; read their verdicts with that in mind.

The JIT becomes the default on x86-64 when all of these hold, measured on the development PC in one session:

1. **Speed:** interpreter SH-2 total / x64 SH-2 total **≥ 1.5** on each of the six titles (median of three runs per mode, the 2C method).
2. **Long session:** Burning Rangers over frames 2,400–36,000 (`--warmup 2400 --frames 33600`, the 2C long run): x64 SH-2 total **≤** the interpreter's (ratio ≥ 1.0).
3. **Spikes:** in every measured window above (six titles plus the long run), the max frame time of each x64 run is at most **10% above the highest interpreter max frame of the same session and window** (the highest of the three interpreter runs; for the long run, the one interpreter run). *(Builder-check decision: run-to-run noise in max frame time is a few ms with threaded VDP and the host RTC, so a strict comparison would fail titles without compile bursts.)* Street Fighter Zero 3's compile bursts (x64 max 26.8–29.3 ms vs the interpreter's 11.9–12.6 ms in 2C) fail this today, and over the long Burning Rangers run (interpreter max 16.9 ms in 2C, about 200 µs per native compile) it effectively limits compile work per frame.
4. **Exactness:** lockstep identical for 36,000 frames on the BIOS menu and the six titles with x64, and 1,800 frames with the IR backend; all `[jit]` tests, fuzz and ctest pass on Windows x64, Linux x64 and macOS ARM64 (CI).

If the bar is not met, the JIT stays off by default and the report says which conditions failed, as in 2B and 2C.

The report also gives the **executor-only ratio** (interpreter SH-2 total minus the time outside `Executor::Run`, over the x64 executor time; the 2C re-profile method), so gains from shared code (track C) and from the JIT itself are told apart.

## 4. Constraints

- **Exactness rules are unchanged:** instruction-exact boundaries, the same cycle cost per instruction as the interpreter including bus waits and write-back stalls, interrupts taken at exactly the interpreter's instruction (`sh2-jit.md` sections 2, 5.3; `sh2-jit-m2.md` 4.4–4.5). Any check that is moved, merged or removed must be shown to give the same boundary in every case, or keep a per-instruction fallback for the cases where it cannot.
- **Both backends:** IR-level changes (front end, `OptimizeBlock`, block cache) apply to the IR backend too. The IR backend must stay correct; its speed does not matter.
- **ARM stays correct:** after each track, IR lockstep (`tools/bench/run_lockstep.sh 1800 ir`) and the test suite pass on the Raspberry Pi 3. No ARM speed requirement.
- **Fork policy:** keep JIT logic in `src/jit/`. Changes in the SH-2 fork scope follow `src/core/BRIMIR_FORK.md`. Outside it, only small hooks that do not change emulation are allowed, for example a read-only accessor or a change notification (such as which memory a save-state load replaced, for A4, or a page-table change counter, for B2); each needs a `// Brimir:` marker and a `BRIMIR_FORK.md` entry. *(Builder-check decision.)*
- **Memory (track A):** over the long Burning Rangers run, no native-code-cap flushes and a peak of native code no higher than 2C's; any pool of reusable code (A1) counts toward the 128 MB cap per CPU. `brimir_bench` reports the peak native-code bytes per CPU so this can be checked. *(Builder-check decision.)*
- **Measurement:** the development PC (Ryzen 7 5700G, Windows, MSVC, `build-bench`: Release + LTO/IPO, Ninja), the 2C method (six titles plus the BIOS menu, `--warmup 2400 --frames 1800`, three rounds, interpreter / IR / x64 back to back), only same-session ratios compared. Note: the local MSVC `build/` directory currently fails CMake's compiler check (2026-10-15); `build-bench` was not checked.
- **Memory:** no increase in the native-code cap (128 MB per CPU).

## 5. Order and checkpoints

0. **Measurement overhead first.** About 0.6 ms/frame of the ~1.09 ms/frame outside the executor is `brimir_bench`'s own host-time accounting in `SH2::Advance` (`steady_clock` per call). Users under RetroArch never pay it, and it understates the JIT's ratio. Make that accounting cheaper (or account per batch) without changing what it measures, and re-measure the interpreter and x64 baselines before track B. *(Decision, 2026-10-15.)*
1. **Track B** (section 6), one item at a time, measured after each item on two or three titles (the 2C progress-table method), then the full 2C measurement at the end of the track.
2. **Track A** (section 7), the same way, ending with the full measurement plus the long Burning Rangers run and the 36,000-frame lockstep runs.
3. **Track C, optional** (section 7.1), only after A and B.
4. **Final report** in `sh2-x64-performance.md` ("Milestone 2D results"), the bar of section 3 checked item by item, and the rollout of section 8 if it passes.

Within each track the order of items is the builder's call, guided by the measured cost of each (largest first is the default).

## 6. Track B: per-instruction cost

Evidence: 2C re-profile, Panzer Dragoon II Zwei: the executor takes ~3.85 ms/frame, of which native chain runs are 84–91%; generated code costs 4.6–5.6 ns per guest instruction (6.5–7.0 ns with trampolines) against ~8.5 ns for the interpreter; about 1.09 ms/frame of SH-2 time is outside the executor and paid in both modes. For 1.5x on PD2 the executor must fall to about 2.5 ms/frame (5.37 / 1.5 − 1.09); for SFZ3 to about 2.2 (from 4.34), for BR to about 3.4 (from 5.80).

Items, from the 2C "Next steps":

- **B1. Fewer boundary checks.** Today every instruction pays a cycle-budget compare and an interrupt-flag test. Allow one check covering a run of instructions when it is provably exact: the run's maximum possible cycles fit in the remaining budget, and nothing in the run can change the interrupt pending or allowed state (no slow-path call, helper or SR/IMASK write that could). Otherwise keep the per-instruction check. The precise rule is the builder's, but it must be stated in the plan and covered by tests that place the budget end and an interrupt at every instruction of such a run.
- **B2. Compile-time page resolution.** For constant addresses and PC-relative loads, resolve the bus page at compile time when it is an array page that cannot change while the block runs, and keep the dynamic path otherwise. Check-on-entry and invalidation must still cover anything this assumes.
- **B3. Inline `DIV1`.** Emit the divide step in generated code instead of the `Div1` trampoline, and stop treating it as a register-cache barrier (Burning Rangers master: 44.8M calls per 1,800 frames; SFZ3: 8.2M).
- **B4. On-chip register reads.** An inline path for side-effect-free on-chip reads, so the slaves' FTCSR polling (78–96M read trampolines per 1,800 frames) does not leave generated code. Which registers are side-effect-free must come from the interpreter's `OnChipRegRead*` code, and the FRT/WDT sync those reads trigger must stay exact. An exact fast-forward of proven polling loops is allowed only if it is identical in lockstep.
- **B5. Compile cost** (SFZ3: 0.67 ms/frame of compiling; asmjit register allocation is 77% of a native compile), mainly for the spike condition of section 3. Options include a cheaper path for cold blocks and a different `kNativeCompileThreshold`; the choice is the builder's, from measurement.

## 7. Track A: long sessions

Evidence: 2C "Long-session cache behavior": Burning Rangers loads new code over old code in phases; stale native code stays allocated until a flush, so each CPU's 128 MB cap fills about once a minute on average and every fill is a full flush plus a recompile burst. The five slowest frames (151–173 ms) each spent 128–135 ms compiling.

- **A1. Reuse returning code.** When guest code at a PC comes back with the same opcodes as code compiled before, reuse that native code instead of recompiling. A key that includes the opcodes, not only the PC, is the expected approach; the details are the builder's. Reuse must keep check-on-entry semantics: code is reused only when it is identical to what it was compiled from.
- **A2. Individual eviction instead of whole flushes.** Free stale native blocks (dropped after their guest code changed) and cold ones individually, so the code cap is reached rarely or never. A whole flush remains the fallback. Generated code must never free code that is running (deferred as today).
- **A3. Damp recompiles at unstable PCs.** Raise the native-compile threshold for PCs whose blocks keep going stale, so code that is about to be overwritten again runs on the IR path instead of being compiled.
- **A4. Save-state load flush.** Check whether a save-state load still needs to flush all compiled code, given check-on-entry. Keep it if it is required for exactness; otherwise replace it with whatever is needed (for example invalidation of affected pages). Validation must include lockstep across save-state loads.

### 7.1 Track C (optional): shared SH-2 code

About 1.09 ms/frame of SH-2 time is outside the executor in every title (2C re-profile) and is paid by the interpreter and the JIT alike: `SH2::Advance` itself, on-chip timers (FRT, WDT) and DMA, plus the measurement overhead of step 0. Making the non-measurement part cheaper speeds up Brimir in both modes. It is allowed in this milestone after tracks A and B, inside the SH-2 fork scope, exact like everything else, and reported separately (the executor-only ratio of section 3 is unaffected by it). Skip it if A and B already meet the bar and time is better spent on milestone 3.

## 8. Validation and rollout

**Validation**, in addition to the existing suite:
- tests for each item's exactness edge cases (B1: budget end and interrupts at every position of a merged run; B2: page changes and check-on-entry; B3/B4: against the interpreter's helpers, both backends; A1/A2: reuse after reload, eviction while chained blocks still link to an evicted block, flush fallback; A4: lockstep across save-state loads);
- backend-vs-backend fuzz as in milestone 2;
- after each track: lockstep on the BIOS menu and six titles (36,000 frames x64 at the end of the milestone, 1,800 frames x64 and IR after each track), and IR lockstep plus the test suite on the Raspberry Pi 3.

**Rollout** if section 3 holds (unchanged from `sh2-jit-m2.md` section 6): `brimir_sh2_jit` defaults to `enabled` on x86-64 builds and stays `disabled` on ARM64; the option keeps its "(Experimental)" label for one release; README, CHANGELOG and `sh2-jit.md` are updated.

## 9. Follow-ups outside this milestone

- **Hi-res and interpolation slowdowns:** measure first. `brimir_bench` on a hi-res scene (for example Virtua Fighter 2's 704x448 interlaced attract) with the profiler entries, to confirm the cost is in VDP rendering rather than the SH-2s. If so, it is its own project.
- Milestone 3 (ARM64 backend), reusing 2B/2C/2D.
- The `brimir_cd_preload` default on low-RAM devices; the VDP2 render thread spin-wait; upstream reports for both and for the CHD hunk cache.

## 10. Left to the builder

- The order of items inside each track, from measurement.
- The exact rules for B1 and B2, the side-effect-free register list for B4, and the key and data structures for A1/A2, provided they meet the constraints of section 4 and the tests of section 8.
- Whether an item is dropped because measurement shows no gain (record it in the report with the numbers).
