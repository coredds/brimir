# SH-2 x64 backend performance (milestones 2B and 2C)

Milestone 2B results and profile first; milestone 2C progress, final results (verdict: **target missed**) and re-profile are in "2C progress" and "Milestone 2C results" at the end. Milestones 2D and 2E follow them: the 2E results against the bar of `sh2-jit-m2e.md` (verdict: **bar not met**) and the generated-code profile are in "Milestone 2E results" at the very end.

**Date**: 2026-10-01
**Commit**: a86fae7 (the measured code: the parent of the commit that adds this report)
**Machine**: AMD Ryzen 7 5700G, 8 cores / 16 threads, 32 GB RAM, Microsoft Windows NT 10.0.26300.0 (the machine of `design/sh2-baseline.md`)
**Build**: `build-bench`, Ninja, Release, `BRIMIR_LTO=ON`, `Brimir_ENABLE_IPO=ON`, MSVC 19.44.35229 (Visual Studio 2022, toolset 14.44), `CMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG`

**Verdict: target missed.** The target is interpreter SH-2 total / x64 SH-2 total ≥ 2.0 on all six titles. All six titles are below 2.0: Virtua Fighter 2 (0.49), Panzer Dragoon II Zwei (0.58), Sega Rally Championship (0.57), Burning Rangers (0.55), Guardian Heroes (0.59), Street Fighter Zero 3 (0.45). The x64 backend takes 1.7–2.2x the interpreter's SH-2 time. It is 1.3–1.7x faster than the IR backend.

## Method

- Same scenes, warmup and frame counts as `design/sh2-baseline.md`: `brimir_bench --bios <bios> --game <game> --system-dir <scratch dir> --warmup 2400 --frames 1800` for the games, `brimir_bench --bios mpr-17933.bin --warmup 300 --frames 1800` for the BIOS menu. No save states and no input, so games are measured on their title/attract screens. Default settings: threaded VDP, host RTC.
- Three modes per title: the interpreter (no flag), `--sh2-jit --jit-backend ir` and `--sh2-jit --jit-backend x64`.
- Three rounds per title, each round running the three modes back to back (interpreter, IR, x64), so all modes of a title were measured in the same session. The table shows the run with the median `avg` per mode; the last column lists the SH2 total of all three runs.
- SH-2 time is host time inside `SH2::Advance` (both CPUs; includes on-chip DMA/timers and bus accesses), as in the baseline.
- Content and scratch system dir as in the baseline: US BIOS `mpr-17933.bin`, JP BIOS `Sega Saturn BIOS v1.01 (JAP).bin`, the configured `brimir_saturn_rtc_us_eu.smpc` copied also as `brimir_saturn_rtc_jp.smpc`.
- Load: the machine also ran the usual desktop applications (Slack, Zoom, terminal, editor); CPU load before the runs was about 3%. Nothing else heavy ran during the measurements.

## Results

All 63 runs exited 0. ms/frame is wall time per `RunFrame`; SH2 percentages are of `Ymir_RunFrame`.

| Title | Mode | ms/frame avg | p95 | SH2 master ms (%) | SH2 slave ms (%) | SH2 total ms (%) | SH2 total, 3 runs |
|---|---|---|---|---|---|---|---|
| BIOS menu (US) | interpreter | 8.385 | 8.986 | 2.349 (28.0%) | 0.000 (0.0%) | 2.349 (28.0%) | 2.349 / 2.336 / 2.355 |
| | IR | 9.506 | 10.443 | 7.059 (74.3%) | 0.000 (0.0%) | 7.059 (74.3%) | 7.160 / 7.059 / 7.008 |
| | x64 | 8.886 | 9.731 | 4.248 (47.8%) | 0.000 (0.0%) | 4.248 (47.8%) | 4.255 / 4.265 / 4.248 |
| Virtua Fighter 2 (JP) | interpreter | 9.254 | 12.247 | 4.073 (44.0%) | 2.602 (28.1%) | 6.675 (72.1%) | 6.675 / 6.799 / 5.444 |
| | IR | 23.900 | 31.177 | 12.095 (50.6%) | 8.650 (36.2%) | 20.745 (86.8%) | 20.745 / 20.940 / 15.373 |
| | x64 | 16.838 | 20.920 | 7.500 (44.5%) | 6.185 (36.7%) | 13.685 (81.3%) | 13.685 / 13.694 / 8.724 |
| Panzer Dragoon II Zwei (US) | interpreter | 7.286 | 8.474 | 3.349 (46.0%) | 1.917 (26.3%) | 5.266 (72.3%) | 5.301 / 5.266 / 5.257 |
| | IR | 16.247 | 18.322 | 8.754 (53.9%) | 5.054 (31.1%) | 13.808 (85.0%) | 13.801 / 13.839 / 13.808 |
| | x64 | 11.524 | 13.571 | 5.883 (51.1%) | 3.156 (27.4%) | 9.039 (78.4%) | 9.250 / 9.010 / 9.039 |
| Sega Rally Championship (US) | interpreter | 7.523 | 9.173 | 2.838 (37.7%) | 2.230 (29.6%) | 5.068 (67.4%) | 5.068 / 5.077 / 5.046 |
| | IR | 17.049 | 24.171 | 8.485 (49.8%) | 6.010 (35.3%) | 14.495 (85.0%) | 14.442 / 14.495 / 14.537 |
| | x64 | 11.308 | 15.334 | 5.021 (44.4%) | 3.856 (34.1%) | 8.877 (78.5%) | 8.891 / 8.821 / 8.877 |
| Burning Rangers (US) | interpreter | 9.645 | 10.884 | 4.437 (46.0%) | 2.229 (23.1%) | 6.665 (69.1%) | 6.693 / 6.665 / 6.665 |
| | IR | 22.078 | 23.548 | 12.425 (56.3%) | 6.452 (29.2%) | 18.876 (85.5%) | 18.775 / 18.876 / 18.882 |
| | x64 | 15.609 | 17.015 | 8.584 (55.0%) | 3.627 (23.2%) | 12.211 (78.2%) | 12.406 / 12.102 / 12.211 |
| Guardian Heroes (US) | interpreter | 6.924 | 8.220 | 2.744 (39.6%) | 1.731 (25.0%) | 4.475 (64.6%) | 4.393 / 4.484 / 4.475 |
| | IR | 15.153 | 18.186 | 7.751 (51.2%) | 4.518 (29.8%) | 12.269 (81.0%) | 12.269 / 12.273 / 12.255 |
| | x64 | 10.292 | 12.706 | 4.553 (44.2%) | 3.061 (29.7%) | 7.614 (74.0%) | 7.614 / 7.527 / 7.622 |
| Street Fighter Zero 3 (JP, 4 MB cart) | interpreter | 9.263 | 10.004 | 2.911 (31.4%) | 2.128 (23.0%) | 5.039 (54.4%) | 5.026 / 5.044 / 5.039 |
| | IR | 17.137 | 20.317 | 8.956 (52.3%) | 5.654 (33.0%) | 14.610 (85.3%) | 14.610 / 14.623 / 14.543 |
| | x64 | 13.993 | 20.615 | 7.957 (56.9%) | 3.235 (23.1%) | 11.192 (80.0%) | 11.192 / 11.107 / 11.285 |

### Ratios (SH2 total, median runs)

| Title | interpreter / x64 (target ≥ 2.0) | IR / x64 | interpreter / IR |
|---|---|---|---|
| BIOS menu | 0.553 | 1.662 | 0.333 |
| Virtua Fighter 2 | **0.488** | 1.516 | 0.322 |
| Panzer Dragoon II Zwei | **0.583** | 1.528 | 0.381 |
| Sega Rally Championship | **0.571** | 1.633 | 0.350 |
| Burning Rangers | **0.546** | 1.546 | 0.353 |
| Guardian Heroes | **0.588** | 1.611 | 0.365 |
| Street Fighter Zero 3 | **0.450** | 1.305 | 0.345 |

The BIOS menu is not one of the six target titles; it is listed for reference.

Notes:

- Run-to-run spread was within 3% except Virtua Fighter 2, whose third round was faster in all three modes (interpreter 5.44, IR 15.37, x64 8.72 ms SH2 total). Its ratio in that round (0.62) is still far below 2.0. All x64 runs reported `compileFallbacks 0` and `nativeBlocksRun == blocksRun` on both CPUs.
- Street Fighter Zero 3 on x64 has periodic stalls (p99 44–45 ms, max 256–264 ms in all three runs; the IR backend has max 27–28 ms). They come from block-cache flushes and recompilation (profile below).
- Virtua Fighter 2 on x64 has a single spike per run (max 105.6 / 83.9 / 95.2 ms; the IR backend has 33.3 / 37.8 / 28.6 ms). The master's block cache fills to the IR-instruction cap once in the measured window, and the following frames recompile hundreds of blocks each (profile below).
- `blocksRun` differs slightly between runs (Virtua Fighter 2 master: 251,304,014 / 251,304,099 / 251,304,215 in the three IR runs and 251,304,054–251,304,063 in the x64 runs). That is nondeterminism in these default-settings runs (threaded VDP, host RTC), not a backend difference: the three IR runs differ from each other as much as from the x64 runs. Lockstep runs, which are deterministic, give identical counts on both backends (`design/sh2-validation.md`).
- Interpreter SH2 totals for five of the games are 26–29% below the baseline table measured on 2026-09-30 on the same machine (for example Panzer Dragoon II Zwei 5.27 vs 7.42 ms). Virtua Fighter 2 and the BIOS menu are within 2% of it. The ratios above only compare runs from the same session.

## Profile (where SH-2 time goes on x64)

### Method

A sampling profiler was not usable here. Visual Studio's `VSDiagnostics.exe` collector is installed, but there is no command-line analyzer for its sessions, and `xperf`/WPA are not installed (only `wpr.exe`). The profile therefore comes from temporary instrumented builds of `build-bench` (not committed; reverted afterwards, and `build-bench` was rebuilt clean). Each experiment is switched on separately with an environment variable:

- **Counters** (always on): steps, native blocks, retired instructions and cycles per `Executor::Run` call, trampoline calls per CPU, compiles, flushes.
- **Run timer**: one `rdtsc` interval around each `Executor::Run` call.
- **Native timer**: one interval around each `X64Backend::Run` call.
- **Duplicate-call experiments**. Each one makes a side-effect-free operation run twice and takes the cost as the growth of `Executor::Run` time:
  - every `BlockCache::Get` (the second call is a recent-slot hit with a full `IsCurrent` check);
  - every pipeline refill call emitted in generated code (the refill re-reads the same RAM word);
  - every `SetupDelaySlot` call emitted in generated code (it stores the same two fields again).
- **Classification**: exit reason of every native block run, compiled length, per-block run counts, IR op counts, native code bytes per compiled block, callback addresses.
- **Per-frame log**: compiles, compile time, flushes and cache size (blocks, IR instructions, code bytes) per CPU and frame.

An earlier variant of the refill and `SetupDelaySlot` experiments (first instrumented build) made the second call inside the C++ trampoline. It measured only the inner callback (2.2 ns per refill, 1.5 ns per setup) and missed the call from generated code: argument setup, the call, the trampoline frame and the abort check. Those runs are not used; the second build emits the call twice in generated code instead. The `Get` and native-timer experiments come from the first build and the refill/setup ones from the second. Each is compared with the Run-timer-only runs of its own build.

Counts cover the 1,800 measured frames of the baseline scene (warmup 2,400), both CPUs unless a CPU is named. Timed experiments were repeated three times, interleaved, and averaged. The first run of the second instrumented build (Run timer only) was an outlier (SH2 total 15.9 vs 10.6–10.7 ms/frame) and is excluded.

**Timer overhead.** A timed interval costs two `rdtsc` reads plus a counter update. Adding the native timer (266.8M intervals) made `Executor::Run` 1.79 s longer, about 6.7 ns per interval. The trampoline timers used in the first version of this report cost about 10.4 ns per interval: each also tested an enable flag twice and updated two counters. Only part of that cost lands inside the measured interval: the latency of the second `rdtsc` does, the first one's mostly does not. So a timed interval overstates by somewhere between 0 and the full growth. The earlier "about 3 ns" correction was not based on a measurement and was wrong. With up to 10 ns of error on calls that themselves take 5–10 ns, timers cannot measure the trampolines, so the trampoline costs below come from the duplicate-call experiments. Timers are used only for long intervals (`Executor::Run`, `X64Backend::Run`), and those are given as ranges.

### Panzer Dragoon II Zwei (x64): time

**Base.** Instrumented build with only the Run timer on, mean of 3 runs: SH2 total 10.33 ms/frame, `Executor::Run` 9.25 ms/frame. The counters alone cost about 1.1 ms/frame, nearly all inside `Executor::Run`: counters-only build 10.17 ms/frame vs 9.04 for the uninstrumented build in the results table. Shares below are relative to the uninstrumented `Executor::Run` time. That is estimated as 9.04 − 1.08 = **about 7.96 ms/frame**, using the outside-Run time of the base. The 9.04 comes from the earlier session, so allow about ±0.3 ms.

| Part | Source | ms/frame | Share of `Executor::Run` (~7.96 ms) |
|---|---|---|---|
| SH-2 time outside `Executor::Run` (inside `SH2::Advance`) | base: SH2 total − Run | 1.08 | (not in Run) |
| Native block runs (`X64Backend::Run`: frame setup, generated code, trampolines) | native timer, 3 runs: 6.25 ms raw, minus 0–0.99 ms timer overhead, minus 0.24–0.71 ms of instrumentation counters inside the interval (856M trampoline-call increments at 0.5–1.5 ns) | 4.5–6.0 | 57–75% |
| Dispatch (everything in `Run` outside native runs: `Step` pre-checks, `Get`, `BlockScope`, fallbacks, loop) | 7.96 − native; must exceed the `Get` cost | 2.0–3.4 | 25–43% |
| of which `BlockCache::Get` incl. `IsCurrent` | duplicate `Get`, 3 runs: +3.15 s / 266.9M = 11.8 ns per call (warm second call, so a lower bound) | ≥ 1.75 | ≥ 22% |
| Trampolines (inside native runs) | sum of the four rows below | 2.7–3.5 | 34–44% |
| of which pipeline refill, 622.9M calls | duplicate call, 3 runs: +3.88 s = 6.2 ns per call, including ~1 ns of instrumentation counters | 1.7–2.2 | 21–28% |
| of which delay-slot setup + end, 72.3M + 69.0M calls | duplicate setup, 3 runs: +0.34 s = 4.6 ns per call; end assumed similar | 0.3–0.4 | 4–5% |
| of which reads, 85.7M calls (85.5M by the slave, see below) | call cost as for setup, plus an on-chip register read; estimated | 0.2–0.4 | 3–5% |
| of which writes, 3.9M calls (SCU/VDP registers; the interpreter pays the same device work) | earlier timers, ~240 ns per call (timer overhead negligible here) | ~0.5 | ~6% |
| Generated code proper (plus `X64Backend::Run` entry/exit) | native − trampolines | 1.0–3.3 | 13–41% |
| Interpreter fallbacks (part of dispatch), 3.76M steps | earlier timers | ~0.03 | <1% |

The ranges are coupled: native and dispatch sum to about 7.96, and generated code is native minus trampolines. The native interval of the instrumented build also contained the trampoline-call counters (one increment per call, 856M in total). Their estimated cost is subtracted above, which lowers native and generated code and raises dispatch. In round numbers: trampolines 34–44% (refills alone 21–28%), dispatch a quarter to 40%, generated code an eighth to 40%. Trampolines are the largest and the best-determined part.

**Outside `Executor::Run`.** About 1.08 ms/frame of SH-2 time is spent in `SH2::Advance` outside the executor. Part of it is the measurement itself. `SH2::Advance` reads `std::chrono::steady_clock::now()` twice per call for the SH2 host-time counter; a pair costs 38.6 ns here (microbenchmark), and about half of it falls inside the measured interval. At 55.6M `Advance` calls per 1,800 frames, that is about 0.6 ms/frame. The rest, about 0.5 ms/frame, is the `Advance` call itself, WDT/FRT/DMA updates and the sleep check. The interpreter pays all of it, since the scheduler calls `Advance` with the same slices in both modes.

Implications for the target:
- PD2's interpreter SH2 total of 5.27 ms/frame is about 1.08 outside its loop plus 4.2 in it.
- For 2x, the x64 total must be ≤ 2.63 ms/frame, so `Executor::Run` must take ≤ ~1.55 ms/frame: 5x less than today, and 2.7x less than the interpreter's own loop.
- With an executor that took no time at all, the ratio would be at most 5.27 / 1.08 ≈ 4.9.

### Panzer Dragoon II Zwei (x64): structure

| | master | slave |
|---|---|---|
| `Executor::Run` calls (= `SH2::Advance` slices) | 27.78M | 27.78M |
| Steps / native block runs / interpreter steps | 164.6M / 160.9M / 3.69M | 106.0M / 105.9M / 0.07M |
| Guest instructions retired in native blocks | 554.5M | 347.4M |
| **Per slice**: native blocks / guest instructions / cycles | **5.8 / 20.0 / 31.0** | **3.8 / 12.5 / 31.0** |
| Slices of 32–63 cycles | 89% | 64% (31% have 16–31) |
| **Retired guest instructions per native block run** | **3.45** | **3.28** |
| Compiled length, weighted by runs | 3.93 | 3.72 |
| Compiled length, average over all compiled blocks | 9.1 (19,003 blocks) | 6.2 (74 blocks) |

The "3.4 guest instructions per block" of the first version is the number of instructions retired per native block run. The compiled length of the blocks that run is about 3.9. The average compiled block is longer (6–9 instructions), but the hot blocks are short. Master runs by compiled length: 2 instructions 44%, 3 instructions 24%, 4 instructions 21%, 1 instruction 4%, 32 (maximum) 3%. Slave: 4 instructions 82%.

Slices are tiny: the scheduler advances each SH-2 by about 31 cycles per call (15,400 `Advance` calls per CPU per frame). So block linking can chain at most about 6 blocks per call on the master and 4 on the slave before control returns to `Advance`.

Exit reasons of native block runs:

| Exit | master | slave | both |
|---|---|---|---|
| `ExitIf` taken: BT/BF taken (block ends in BT/BF), or BT/S/BF/S not taken (block ends in a delayed branch) | 72.20M (44.9%): 69.84M BT/BF, 2.36M BT/S/BF/S | 85.46M (80.7%) | 157.7M (59.1%) |
| `ExitDynamic` (delayed branch: BRA/BSR/JMP/JSR/RTS/BRAF/BSRF, or BT/S/BF/S taken) | 67.63M (42.0%) | 1.32M (1.3%) | 69.0M (25.8%) |
| Boundary stop, cycle budget (end of the `Advance` slice) | 18.41M (11.4%) | 19.16M (18.1%) | 37.6M (14.1%) |
| Static `Exit` (BT/BF not taken, maximum length, or uncompilable next instruction) | 2.54M (1.6%) | 0.002M | 2.5M (0.95%) |
| Boundary stop, interrupt | 0.08M | 0 | 0.08M |
| Bus wait / abort | 0 / 0 | 0 / 0 | 0 / 0 |

The slave spends most of the scene in a 4-instruction polling loop. It reads FTCSR (`0xFFFFFE11`, the FRT status register) 85.5M times, once per loop iteration and always through the read trampoline, since on-chip registers are not inlined. It is waiting for the master's FRT input-capture signal. The master's loads are almost all inline: 0.22M read callbacks against at most 146.6M loads executed. 2.5M of its at most 54.5M stores go through the write trampoline (SCU registers at `0x25FE00xx`). Execution counts of IR ops are weighted by block runs, an upper bound because some runs stop before the end.

### Panzer Dragoon II Zwei (x64): why the generated code is slow

| | master | slave |
|---|---|---|
| IR ops per guest instruction (all compiled blocks / weighted by runs) | 9.26 / 8.32 | 8.70 / 8.43 |
| Native code bytes per guest instruction (all / weighted) | 297 / 262 | 274 / 224 |
| Native code bytes per block (all / weighted) | 2,696 / 1,029 | 1,700 / 833 |

IR op mix, executions per guest instruction weighted by block runs (static mix in parentheses):

| Category | master | slave |
|---|---|---|
| Timing bookkeeping: `SetWb` 1.00, `CheckBoundary` 0.75, `AddCycles` 0.68, `WbStall` 0.64, `AddAccessCycles` 0.32, `SyncCycles` 0.32 (master) | 3.71 = 45% (45%) | 3.68 = 44% (46%) |
| Guest state get/set: `GetReg` 0.88, `SetReg` 0.47, `GetT`/`SetT` 0.14 each, `GetPR`/`SetPR` 0.05 each | 1.73 = 21% (24%) | 1.99 = 24% (22%) |
| ALU and constants | 1.21 = 15% (14%) | 1.50 = 18% (14%) |
| `Load`/`Store`/`ExitIfBusWait` (0.23 / 0.09 / 0.20) | 0.51 = 6% (8%) | 0.23 = 3% (7%) |
| `Refill` (a call each) | 0.52 = 6% (5%) | 0.48 = 6% (6%) |
| Exits (`ExitIf`, `Exit`, `ExitDynamic`) | 0.39 = 5% (2%) | 0.54 = 6% (3%) |
| Delay-slot setup/end (a call each) | 0.24 = 3% (1%) | 0.01 = 0% (3%) |

Every op is lowered on its own:
- `GetReg`/`SetReg` are a load or store to the guest register file on every use; no guest register stays in a host register.
- `SetWb`/`WbStall` store and test the write-back register in memory.
- `CheckBoundary` compares the cycle counter with the limit in the frame, then tests the interrupt-pending and interrupt-allow bytes.
- `AddAccessCycles` looks up the page table.
- Each `Refill` is an out-of-line call.

So about 45% of executed IR ops do the interpreter's per-instruction timing and boundary work, a fifth move guest registers between memory and host registers, and only about 15% compute anything. That gives about 260 bytes of x64 code per guest instruction.

### Street Fighter Zero 3 (x64)

Structure (classification run):
- **Master**: 4.07 native blocks, 17.3 guest instructions and 30.8 cycles per slice; 4.25 instructions retired per run. Exits: `ExitIf` 72%, boundary-cycle 16%, `ExitDynamic` 8%, static `Exit` 4%.
- **Slave**: 3.52 blocks per slice. It spends the scene in a 5-instruction loop that polls FTCSR (78.3M read callbacks, one address); 79% of its runs are that loop's taken back-edge, the rest are boundary stops.

**Compile churn is the master's alone.** The slave never flushes and has 598 distinct block PCs (72k IR instructions).
- **Flushes**: the master's cache was flushed by the IR-instruction cap (`kMaxCachedInsts`, 2^20) at measured frames 2424, 3109 and 3945, and once in the warmup at frame 746. At each measured flush it held 12,500–14,100 blocks, 1.05M IR instructions and 32.9–33.1 MB of native code (below the 64 MB code cap). Frame numbers count from the start of the run; measurement starts at frame 2400. In the first version of this report, the warmup/measurement split came from a counter reset at the start of measurement (3) against the total number of flush messages (4). The per-frame log now confirms it with frame numbers. In the measured window no flush came from `Executor::Flush` (reset, state load); the only such flushes were at boot (frame 0). No compile in the window replaced an invalidated block.
- **The working set exceeds the cap, and new code keeps appearing.**
  - The master has compiled 30,627 distinct block start PCs since boot. The sum of their IR sizes is 2.58M IR instructions, 2.5x the cap.
  - During the 1,800 measured frames it compiled 35,454 blocks: 25,060 recompiles of PCs compiled before, mostly after the flushes, and 10,394 blocks at PCs never seen before.
  - 1,597 of the 1,800 measured frames compiled at least one block (19.7 per frame on average).
- **Compile time** is 128 µs per block (asmjit `x86::Compiler`), 4.56 s in total (2.5 ms/frame on average). It is not confined to the frames right after a flush:
  - 31% of it falls in the 10 frames after the three flushes, and 58% in the 60 frames after them.
  - Ten frames after a flush the cache holds only 2,900–5,500 blocks again.
  - The rest is spread over most frames, including bursts of new code (frame 2449: 625 new PCs, 87 ms of compiling).
- **Worst frames**:
  - 274 ms, 4 frames after a flush: 1,763 compiles, 252 ms of compiling;
  - 205 ms, the flush frame;
  - 157 ms at frame 3777, not near a flush: 546 compiles (527 recompiles, 19 new PCs), 138 ms of compiling.
- Without compile time, the x64 SH2 total would be about 8.7 ms/frame and the ratio still about 0.58.

### Virtua Fighter 2 (x64): max-frame spikes

The per-frame log of two runs shows the same cause as Street Fighter Zero 3. The master's cache hit the IR-instruction cap once in the measured window, at frame 2689 in one run and 2642 in the other; the frame differs because default-settings runs are not deterministic. At that point it held 16,200 blocks, 1.05M IR instructions and 33.3 MB of code. In the flush frame and the next four frames the master compiled 211, 302, 38, 367 and 857 blocks (run 1, frames 2689–2693) and 823, 249, 371, 382 and 58 blocks (run 2, frames 2642–2646). The heavy frames took 19–73 ms of compiling each (about 100 µs per block). That produced the maxima of these two instrumented runs (84.2 and 88.8 ms) and several 35–44 ms frames. The uninstrumented runs in the results table had maxima of 84–106 ms. Over the window the master compiled 5,540 blocks, 570 ms in total, with 25,000 distinct PCs since boot. The IR backend has the same IR cap, but its compile step is only the front end, which plausibly explains its lower maxima (33.3, 37.8 and 28.6 ms in the three runs). This was not measured.

## Conclusions for a follow-up plan (2C)

The budget: for 2x, Panzer Dragoon II Zwei's `Executor::Run` must drop from about 7.96 to about 1.55 ms/frame, since about 1.08 ms/frame of `SH2::Advance` lies outside the executor and is paid in both modes. No single item below is enough. Items 1–3 cover the three parts of executor time, ordered by their measured share (best-determined first).

1. **Out-of-line calls, mainly pipeline refills.** Trampolines take 34–44% of executor time; refills alone (622.9M calls, about 6 ns each) take 21–28%. A refill cannot simply be dropped, but its call can become a constant store:
   - **The buffer is visible at almost every instruction.** A `Refill` op writes the 32-bit fetch buffer `m_fetchedOpcodes` (`SH2::JitRefillPipeline`: an instruction-fetch `MemRead<uint32>` at an aligned address; on array pages the bus read has no side effects). Generated code never reads the buffer, but nearly every instruction has a reachable exit after it:
     - the `CheckBoundary` stubs, which write PC;
     - `ExitIfBusWait` exits;
     - abort exits after `CheckStop`;
     - the final exits.

     At an exit where PC has bit 1 set, `Executor::Step` compares `*ctx.fetchedOpcodes` with memory, and the interpreter executes the next instruction from the buffer (`SH2::FetchInstruction`). Save states and lockstep see the buffer at every exit. So the buffer must hold the right value at every exit, as it does now.
   - **The value is known at compile time.** On an array page it is the two opcodes at the aligned address. The front end has them in `guestOpcodes`, which `IsCurrent` verifies before every run. `IsCurrent` checks opcode values, not the memory mapping, so the page must still be an array page at run time, or remapping must invalidate the block. With that guarantee, each refill call can become a constant 32-bit store to `m_fetchedOpcodes`. Alternatively, the store can be sunk into each exit path (boundary stubs, bus-wait exits, final exits) with the value current at that exit; abort exits need the value current at their site too. The saving is a call (~6 ns) replaced by one store, or by a store on the exit paths only.
   - **Where a call or runtime load remains**:
     - the refill at the block's last aligned instruction also reads the next word, which is not in `guestOpcodes`; it would have to be added and checked;
     - stores in the block, or write callbacks, that hit the block's own code: memory then differs from `guestOpcodes`;
     - the refill at a taken BT/BF target reads another block's code;
     - fetches from non-array pages.
   - **Delay-slot calls**: setup/end (141M calls, 0.3–0.4 ms/frame) can be inlined as state stores. The refill that `EndDelaySlot` makes when the target has bit 1 set (`SH2::JitEndDelaySlot` → `AdvancePC<…, true>` refills from `m_delaySlotTarget`) is not a compile-time constant. For JMP/JSR/RTS/BRAF/BSRF the target is a runtime register value, and for BRA/BSR it lies in another block's code, the same exception as a taken BT/BF. It stays a runtime load: inline for array pages, otherwise the call.
2. **Per-block dispatch** (25–43%; `Get` alone ≥ 1.75 ms/frame, 11.8 ns per lookup) is paid every 3.3–3.5 retired instructions. Block linking (`design/sh2-jit-m2.md` §4.6) would chain blocks inside generated code, and the design already deferred it to 2C. The `IsCurrent` re-check on every lookup should become write-tracking invalidation (or a per-page generation check), so linked blocks do not need it. The gain is capped by the slice length: about 31 cycles per `Advance` call means at most about 6 (master) or 4 (slave) blocks to chain per call. 59% of block runs end in a taken BT/BF (`ExitIf`) and 26% in a delayed branch, so linking must cover both.
3. **Generated code** (13–41% of executor time) runs 8.3 IR ops and about 260 bytes of x64 per guest instruction. 45% of the ops are per-instruction timing and boundary bookkeeping (`SetWb`, `WbStall`, `CheckBoundary`, `AddCycles`, `SyncCycles`, `AddAccessCycles`), and a fifth are guest-register loads/stores.
   - Fold the static parts at compile time: write-back state known within a block, constant cycle sums, and boundary checks merged where no callback can change the limit or the interrupt state.
   - Keep guest registers in host registers across a block.
   - Both need care to stay exact at every point where the block can stop.
4. **Cache capacity and compile cost**: count native code instead of IR for the flush cap, or drop the IR of natively compiled blocks.
   - Street Fighter Zero 3's master needs 2.5x the current IR cap (30,600 distinct PCs, 2.58M IR instructions), and Virtua Fighter 2 also hits it.
   - Street Fighter Zero 3 also keeps compiling new PCs (10,400 in 30 s), so compile cost (100–130 µs per block with `x86::Compiler`) matters even without flushes. A lighter emitter for cold blocks, or compiling only after a block has run a few times on `RunBlock`, would bound it.
   - That removes the 260 ms stalls (Street Fighter Zero 3) and the ~90 ms spikes (Virtua Fighter 2) and saves about 2.5 ms/frame in Street Fighter Zero 3.
5. **Slave polling loops**: in Panzer Dragoon II Zwei and Street Fighter Zero 3 the slave spends most block runs in a 4–5 instruction loop polling FTCSR through the read trampoline. Two ways to make it cheaper:
   - an inline fast path for on-chip register reads that have no side effects;
   - an exact fast-forward of such a loop to the end of the slice, valid only if nothing can change FTCSR inside the slice; that would need proof against the FRT model.
6. Interrupt entry and pending delay slots go through the interpreter (3.76M steps in Panzer Dragoon II Zwei). That is a small share.

Even with dispatch and all trampolines gone, Panzer Dragoon II Zwei's generated code would still take 1.0–3.3 ms/frame against a budget of about 1.55 ms/frame for the whole executor. Items 1–2 also leave some cost behind:
- the write callbacks' device work (~0.5 ms/frame, paid by the interpreter too);
- the read callbacks, unless item 5 removes them;
- at least one dispatch per `Executor::Run` call (about 30,900 calls per frame × ≥ 11.8 ns ≈ ≥ 0.36 ms/frame).

So even at the low end of the generated-code range, the executor would still take about 1.9–2.1 ms/frame against the 1.55 budget, and generated code would also have to roughly halve. At the high end, item 3 is essential. A 2C plan that aims for 2x should therefore do items 1 and 2 first (largest and best-measured), then re-profile and size item 3. Item 4 is needed in any case for the stalls.

## 2C progress

Measured at the end of each milestone 2C task (`design/plans/2026-10-02-sh2-jit-m2c-performance.md`), on the machine and build configuration above (`build-bench`, Release + LTO). Panzer Dragoon II Zwei (US BIOS) and Street Fighter Zero 3 (JP BIOS), `--warmup 2400 --frames 1800`, default settings, scratch system dir as in the method above. Three rounds per title, each running the interpreter and then `--sh2-jit --jit-backend x64`. SH2 totals are the medians of the three runs (all three in parentheses); "x64 max" is the max frame time of the median x64 run. The interpreter's SH2 totals in this session are about 41% (PD2) and 33% (SFZ3) above the results table above, measured on 2026-10-01; only ratios within one session are compared.

| Task | Title | Interpreter SH2 total ms | x64 SH2 total ms | Ratio (interp / x64) | x64 max frame ms | x64 nativeBlocksRun master / slave | compileFallbacks |
|---|---|---|---|---|---|---|---|
| 2B (5799542) | Panzer Dragoon II Zwei | 7.411 (7.411 / 7.394 / 7.439) | 13.327 (13.327 / 13.362 / 13.303) | 0.556 | 22.854 | 355,340,100 / 205,691,327 | 0 / 0 |
| 2B (5799542) | Street Fighter Zero 3 | 6.690 (6.586 / 6.690 / 6.747) | 15.121 (15.121 / 14.974 / 15.151) | 0.442 | 280.969 | 249,823,349 / 188,696,964 | 0 / 0 |
| 1: known refills, inline delay slots | Panzer Dragoon II Zwei | 7.409 (7.445 / 7.409 / 7.404) | 11.378 (11.378 / 11.398 / 11.336) | 0.651 | 19.320 | 355,340,100 / 205,691,327 | 0 / 0 |
| 1: known refills, inline delay slots | Street Fighter Zero 3 | 6.637 (6.611 / 6.637 / 6.770) | 14.282 (14.279 / 14.327 / 14.282) | 0.465 | 328.892 | 249,823,349 / 188,696,964 | 0 / 0 |
| 2: block chaining, in-block validation | Panzer Dragoon II Zwei | 7.554 (7.641 / 7.554 / 7.545) | 7.963 (7.919 / 7.963 / 8.091) | 0.949 | 15.745 | 355,340,100 / 205,691,327 | 0 / 0 |
| 2: block chaining, in-block validation | Street Fighter Zero 3 | 6.841 (6.875 / 6.841 / 6.791) | 11.610 (11.610 / 11.428 / 11.717) | 0.589 | 369.887 | 249,823,349 / 188,696,964 | 0 / 0 |
| 3: IR timing-bookkeeping pass | Panzer Dragoon II Zwei | 7.596 (7.597 / 7.589 / 7.596) | 7.134 (7.134 / 7.111 / 7.158) | 1.065 | 13.627 | 355,340,100 / 205,691,327 | 0 / 0 |
| 3: IR timing-bookkeeping pass | Street Fighter Zero 3 | 6.744 (6.800 / 6.652 / 6.744) | 9.455 (9.455 / 9.319 / 9.618) | 0.713 | 234.232 | 249,823,349 / 188,696,964 | 0 / 0 |
| 4: guest registers in host registers | Panzer Dragoon II Zwei | 7.750 (7.752 / 7.726 / 7.750) | 7.161 (7.161 / 7.198 / 7.131) | 1.082 | 13.351 | 355,340,100 / 205,691,327 | 0 / 0 |
| 4: guest registers in host registers | Street Fighter Zero 3 | 7.275 (7.275 / 7.309 / 7.221) | 10.026 (10.026 / 10.136 / 10.003) | 0.726 | 268.389 | 249,823,349 / 188,696,964 | 0 / 0 |
| 5: tiered compilation, native-only cache accounting | Panzer Dragoon II Zwei | 7.284 (7.284 / 7.292 / 7.276) | 6.864 (6.864 / 6.817 / 6.879) | 1.061 | 13.319 | 355,251,634 / 205,691,083 | 0 / 0 |
| 5: tiered compilation, native-only cache accounting | Street Fighter Zero 3 | 6.578 (6.570 / 6.599 / 6.578) | 7.161 (7.134 / 7.161 / 7.174) | 0.919 | 32.111 | 249,659,137 / 188,693,274 | 0 / 0 |

Task 1 notes:
- x64 SH2 total fell 14.6% in Panzer Dragoon II Zwei (13.33 to 11.38 ms/frame) and 5.5% in Street Fighter Zero 3 (15.12 to 14.28), whose x64 time is dominated by compile churn (profile above).
- Every x64 run reported `staleEntries 0` on both CPUs; block counts are identical to the 2B runs.
- Street Fighter Zero 3's max frame rose from 278–282 ms to 319–329 ms. The original note attributed this to larger code without measuring it. Measured in Task 5 (bench compile counters, at Task 4's code): the slowest frames are compile bursts, and the number of compiles in them is the same as in 2B; what grew is the time per compile. Frame 3949 (4 frames after a flush, with the IR cap counted as in Tasks 1–2) compiled 1,763 blocks in 301 ms (171 µs per block); the 2B profile measured the same frame at 1,763 compiles and 252 ms (143 µs). asmjit's register allocation takes 77% of a native compile (Task 5 notes). Task 1's own per-block time was not measured; it lies between these two.

Task 2 notes:
- x64 SH2 total fell 30.0% in Panzer Dragoon II Zwei (11.38 to 7.96 ms/frame) and 18.7% in Street Fighter Zero 3 (14.28 to 11.61). Block counts are identical to the earlier rows (the chain runs exactly the blocks the executor's steps ran).
- Share of native blocks entered by chaining (`chainedBlocks / nativeBlocksRun`), and blocks per chain (`nativeBlocksRun / (nativeBlocksRun - chainedBlocks)`): PD2 master 81.8% / 5.5, slave 73.8% / 3.8; SFZ3 master 74.9% / 4.0, slave 72.8% / 3.7.
- `staleEntries` is now 49 (PD2) and 258 (SFZ3) on the master, 0 on the slave: native blocks validate their own code at entry, so a code change found there is counted as stale (it used to be a block-cache invalidation in `IsCurrent`, which native blocks no longer call).
- Street Fighter Zero 3's max frame rose again, to 356–378 ms: the prologue's code compare and the chain exits make each block's code larger still (Task 5).

Task 3 notes:

Task 4 notes (2026-10-15, interpreter and x64 back to back, no IR runs): every x64 median fell (PD2 4.370 to 4.253, -2.7%; SFZ3 4.875 to 4.824, -1.0%; BR 6.217 to 6.148, -1.1%; VF2 3.453 to 3.443, -0.3%), beyond the x64 run-to-run spread of the session (0.013-0.057 ms) on PD2, SFZ3 and BR, within it on VF2. The interpreter column of this session differs from Task 3's by 0.05-0.08 ms on every title (PD2 4.936 to 5.006, SFZ3 4.658 to 4.607, BR 6.451 to 6.389, VF2 4.551 to 4.467), so the ratios (PD2 1.130 to 1.177, SFZ3 0.955 to 0.955, BR 1.038 to 1.039, VF2 1.318 to 1.297) mix the change with interpreter drift; the x64 totals are the evidence. The gain is small: an on-chip read now skips the `ctx.read` size and partition switches, not the register flush, the cycle sync or `OnChipRegRead` itself. `nativeBlocksRun` matches the baseline. Cost, BIOS lockstep 1200 (x64, master): 3063 native bytes per block against 2983 (+2.7%): every Load slow path now holds two calls (a partition test selects `TrOnChipRead` or `TrRead`).
- x64 SH2 total fell 10.4% in Panzer Dragoon II Zwei (7.96 to 7.13 ms/frame), now 1.065x faster than the interpreter, and 18.6% in Street Fighter Zero 3 (11.61 to 9.46). Block counts are identical to the earlier rows.
- Static effect of `OptimizeBlock` over the BIOS lockstep's compiled blocks (`sega_101.bin`, 1200 frames, master: 4218 blocks, 38,065 guest instructions): IR ops per guest instruction 8.92 to 8.17. `WbStall` 0.842 to 0.091 per instruction, `SyncCycles` 0.413 to 0.368, `AddCycles` 0.588 to 0.630 (folded stalls, after merging). Full interrupt tests in `CheckBoundary` 33,847 to 21,485; 3,865 of the 12,362 cycles-only checks rely on inline known refills.
- Street Fighter Zero 3's max frame fell to 224–257 ms: the blocks are smaller, so each flush-and-recompile stall is shorter.
- `staleEntries` (master) is now 53 (PD2) and 394 (SFZ3), from 49 and 258. The likely cause (not measured): the block cache's instruction budget counts the optimized ops, so the cache flushes less often and more code changes are found by the prologue instead of being dropped by a flush.
  - Measured in Task 5 with per-trigger flush counters (SFZ3, Task 4's code, one run each): counting the optimized ops, the master flushes 3 times at the IR cap (1 in warmup, 2 measured) and reports 394 stale entries; counting each block's ops before `OptimizeBlock` (as in Task 2), it flushes 4 times (3 in the measured window, including frames 2424 and 3945, as in the 2B profile) and reports 258, Task 2's value. No flush came from the code cap; the only `Executor::Flush` flushes were at boot (2 master, 7 slave). So the change is the one fewer flush. PD2 was not re-measured.

Task 4 notes:
- The register cache is close to neutral. This session's interpreter totals are 2% (PD2) and 8% (SFZ3) above Task 3's, and the x64 totals moved with them: ratios 1.065 to 1.082 (PD2) and 0.713 to 0.726 (SFZ3), within run-to-run noise of each other. Block counts and `staleEntries` are identical to Task 3.
- In-session A/B with one binary and temporary switches, x64 SH2 total ms/frame, interleaved runs:
  - PD2, 3 rounds: write-back at each `CheckBoundary` (the committed form) 7.074 / 7.132 / 7.265; dirty registers kept across checks, stored by each boundary stub 7.278 / 7.205 / 7.209; no caching (load at every `GetReg`, store at every `SetReg`, as before) 7.404 / 7.177 / 7.120. SH2 share of `Ymir_RunFrame` 70.7–70.8%, 70.8–70.9% and 71.0–71.2%: the cache saves at most about 1% of SH-2 time, below the noise of the totals.
  - SFZ3, 2 rounds: 9.720 / 9.587 (committed form), 10.226 / 9.895 (stores in the stubs), 9.753 / 9.553 (no caching). Max frame 233 / 228, 268 / 254 and 230 / 222 ms. Stores in the stubs make the code larger and compiles slower, which costs SFZ3 more than the cache saves.
- Code bytes per guest instruction over the BIOS lockstep's compiled blocks (`sega_101.bin`, 1200 frames, 4218 blocks, 38,065 guest instructions): 313.3 before, 298.7 committed. The stub form measured 339.7 (324.5 after routing call arguments through copies). Over PD2's run (19,814 blocks): 303.1 without caching, 304.4 committed, 337.2 with stores in the stubs.
- Why the effect is small: a guest register access was an L1 load or store next to a much longer inline bus path, boundary check and cycle bookkeeping per instruction; the cache removes those memory accesses but not the per-instruction work around them. The values that cross a slow-path call are spilled around the call by asmjit (cold paths only), and blocks using more host registers save more callee-saved registers in the prologue.

Task 5 notes:
- **Changes.** A new block is built as IR and runs with `RunBlock`; its 8th run (`kNativeCompileThreshold`) compiles it natively, frees its IR and runs the native code. Only native blocks are published to the link table, so a native block whose successor is still IR-only returns to the executor. `kMaxCachedInsts` counts IR-only blocks; `kMaxNativeCodeBytes` is now 128 MB per CPU. `brimir_bench` prints, per CPU, `compiles` (IR builds), `nativeCompiles`, compile time (build and native) and flushes per trigger, plus the five slowest measured frames with their compile work. The block counts above changed for the first time in 2C: `nativeBlocksRun` is now below `blocksRun` by the IR runs before each block's 8th (PD2 master: 88,466 of 355.3M; SFZ3 master 164,212 of 249.8M).
- **Street Fighter Zero 3 before** (Task 4's code with the new counters, one run in this session; SH2 total 10.10, max 260.6 ms):
  - master: 54,803 blocks built and compiled since boot, 10.10 s of compiling (build 9.5 µs, native compile 175 µs per block), 2,628 bytes of code per block, 394 invalidations (all stale entries), flushes: IR cap 3, code cap 0, `Executor::Flush` 2 (boot);
  - measured window: 28,322 compiles, 5.18 s (2.88 ms/frame), in 1,424 of 1,800 frames;
  - slowest frames are compile bursts: 260.6 ms (frame 3777, 854 compiles, 245 ms compiling), 243.9 ms (2456, 1,554 compiles, 233 ms), 195.6 ms (3774, 1,284, 183 ms), 145.4 ms (3773, the flush, 703, 123 ms). The IR-cap flush in frame 3773 caused the recompile burst in frames 3773–3777; frame 2456 compiled 1,554 blocks without a flush in that frame (the frame of the other measured flush was not recorded).
- **Compile time per native block** (asmjit `x86::Compiler`, temporary timers around each stage, SFZ3, before this task's changes, 55,534 compiles): building the IR (front end, `OptimizeBlock`, `VerifyBlock`) 9.5 µs; native compile 172 µs: emitting into the Compiler 19.7 µs, **its passes (register allocation) 132.0 µs (77%)**, serializing to machine code 17.8 µs, copying into executable memory 2.6 µs. After this task (fixed-size `DirtySet` in the emitter, no heap allocation per `ExitIf`): 178 µs at N = 1 (emit 20.5, passes 137, serialize 17.9, add 2.4), 194 µs at N = 8 (the blocks that reach 8 runs are larger: 2,781 vs 2,750 bytes). The `DirtySet` change is not measurable. **asmjit's register allocation dominates compile time**: a lighter emitter (or a cheaper allocation strategy) is the remaining lever on compile cost.
- **Threshold experiment** (x64 SH2 total ms/frame, median of three runs, all three in parentheses; max frame of each run; master native compiles since boot; same binary, temporary environment override, 128 MB cap):

  | N | SFZ3 SH2 total | SFZ3 max frame ms | SFZ3 native compiles | PD2 SH2 total | PD2 max frame ms | PD2 native compiles |
  |---|---|---|---|---|---|---|
  | 1 | 8.146 (8.146 / 8.052 / 8.430) | 124.3 / 124.2 / 140.3 | 31,476 | 6.943 (7.049 / 6.943 / 6.934) | 14.2 / 13.6 / 13.1 | 16,388 |
  | 2 | 7.861 (8.073 / 7.861 / 7.860) | 83.2 / 78.4 / 79.1 | 25,545 | 6.980 (6.980 / 6.966 / 7.004) | 13.3 / 13.3 / 14.0 | 13,450 |
  | 4 | 7.825 (7.662 / 7.877 / 7.825) | 49.2 / 51.9 / 56.9 | 22,147 | 6.997 (6.997 / 7.017 / 6.996) | 13.7 / 13.2 / 13.6 | 12,070 |
  | 8 | 7.793 (7.867 / 7.793 / 7.753) | 32.3 / 34.3 / 32.3 | 19,434 | 6.991 (6.972 / 6.991 / 7.015) | 13.9 / 13.3 / 14.2 | 10,671 |

  N = 8 has the best SFZ3 max frame (32–34 ms) and the best SFZ3 average; PD2's average is 0.7% above its best (N = 1), within the 2% allowed. PD2's max frame does not depend on N (no compile bursts in its window). N = 8 is the edge of the tested range; N = 16 is worth trying later. The experiment binary had the temporary compile-stage timers active (a few `steady_clock` reads per compile), so its numbers are comparable with each other but not with the progress rows.
- **Native code cap.** With N = 1 and a 1 GB cap (no flush), SFZ3's master compiled 31,476 blocks natively since boot (its ~30,600 PCs plus recompiles of stale blocks, whose old code stays allocated until a flush) at 2,750 bytes each: 86.6 MB held at the end. `kMaxNativeCodeBytes` = 128 MB covers that with 48% headroom (for larger blocks in other titles: PD2 2,991 bytes per block at N = 8, SFZ3's slave 4,795). At N = 8 SFZ3's master holds 19,434 × 2,781 bytes = 54 MB. The IR cap now holds only IR-only blocks: SFZ3's master has at most 12,045 of them at the end (31,479 builds − 19,434 native compiles; an upper bound, as it ignores the 386 IR-only blocks dropped by invalidation), still under the cap. Blocks that never reach 8 runs keep their IR, so a longer session reaches the IR cap (Fix 1 below).
- **Flushes per trigger after the change**: none from either cap in any measured run (SFZ3, PD2, VF2); `Executor::Flush` only at boot (SFZ3 master 2, slave 7; PD2 1 and 2; VF2 master 1). Before: SFZ3 master 3 IR-cap flushes (above).
- **Street Fighter Zero 3 after**: 10,686 blocks built and 7,183 compiled natively in the measured window, 1.39 s (0.77 ms/frame, from 2.88). The slowest frame (32.1 ms, frame 2449) built 646 new blocks but compiled only 116 natively (19.8 ms). Without flushes, more code changes reach a cached block: 852 invalidations, of which 466 stale native blocks (was 394) and 386 IR-only blocks found changed by `IsCurrent`. SFZ3 is still slower than the interpreter (ratio 0.919): its master keeps building new blocks (10,686 in 30 s), and 0.77 ms/frame of compiling plus the IR runs before tier-up remain.
- **Virtua Fighter 2** (JP BIOS, same settings, x64, three runs in this session): before this task (Task 4's code) SH2 total 6.055 (6.031 / 6.055 / 6.064), max frame 12.8 / 13.1 / 13.5 ms; after 5.463 (5.463 / 5.449 / 5.491), max frame 12.0 / 12.0 / 12.6 ms. Neither version has a compile spike in this window (the 2B spikes came from an IR-cap flush near frame 2650; Task 4's code shows none, plausibly because `OptimizeBlock` shrank the IR, not measured), so the max frame barely moves. The average falls 10%: at N = 8 the master compiles 14,993 of its 25,320 built blocks natively (runs 1 and 2; run 3: 14,989 of 25,330), where Task 4's code compiled every built block; no cap flush occurred. VF2's default-settings runs are not deterministic (block counts differ slightly between runs, e.g. master `blocksRun` 251,304,044 vs 251,304,083), unlike PD2 and SFZ3 here.
- SFZ3's x64 max frame over the three progress runs: 30.9 / 32.1 / 38.0 ms (the table gives the median run's).
- Exactness: `[jit]` on both backends, the full suite, ctest, BIOS lockstep 1200 (both backends) pass; game lockstep 4,200 frames (warmup and measured window) on SFZ3 and PD2 with x64 is identical.
- **Fix 1: the IR cap evicts only IR-only blocks.** Reaching `kMaxCachedInsts` used to flush the whole cache, native blocks included (SFZ3: about 19,000 native blocks, 54 MB), and would have brought the recompile bursts back. It now drops only the IR-only blocks (and their recent-table slots; they are never in the link table), inside `Get` only; native blocks keep their code and link slots. The bench reports `cachedInsts` and `irEvictions (blocks)` per CPU, and every 1,800 measured frames a `jit progress` line per CPU.
  - SFZ3 long run (x64, `--warmup 2400 --frames 18000`, one run): the master's IR-only instructions were at 827,697 (79% of the cap) at frame 4,200, the end of the progress window. The first eviction came between frames 4,200 and 6,000 (552,881 left afterwards, 34,369 blocks cached), the second between 13,200 and 15,000; 28,940 IR-only blocks evicted in total, while native compiles rose from 19,434 to 34,117. Neither eviction produced one of the five slowest frames: the maximum, 44.9 ms at frame 4,469, compiled 299 blocks natively (32 ms) with no eviction; next 38.5, 32.1, 30.8 and 30.8 ms, all compile bursts of new code. SH2 total over the 18,000 frames 6.450 ms/frame.
  - The progress window (frames 2,400–4,200) has no eviction, so its numbers are unchanged by this fix (not re-measured).

## Milestone 2C results

**Date**: 2026-10-02
**Commit**: 3aa3090 (the measured code: the parent of the commit that adds this section)
**Machine and build**: as in the 2B method above (`build-bench`, Ninja, Release, `BRIMIR_LTO=ON`, `Brimir_ENABLE_IPO=ON`, MSVC 19.44.35229, `CMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG`), rebuilt at the measured commit.

**Verdict: target missed.** The target is interpreter SH-2 total / x64 SH-2 total ≥ 2.0 on all six titles. All six titles are below 2.0: Virtua Fighter 2 (1.25), Panzer Dragoon II Zwei (1.09), Sega Rally Championship (0.98), Burning Rangers (0.97), Guardian Heroes (1.11), Street Fighter Zero 3 (0.91). Milestone 2C raised the ratio 1.7–2.6x over 2B (0.45–0.59) and removed the compile stalls of the measurement windows, but the x64 backend is now only about as fast as the interpreter, and a long Burning Rangers session still stalls while it recompiles reloaded code (half the interpreter's speed over 33,600 frames; "Long-session cache behavior" below). The JIT stays off by default.

### Method

The 2B method above, unchanged: same scenes, warmup and frames (`--warmup 2400 --frames 1800` for the games, `--warmup 300 --frames 1800` for the BIOS menu), default settings (threaded VDP, host RTC), the same content and scratch system dir. Three rounds per title, each running the interpreter, `--sh2-jit --jit-backend ir` and `--sh2-jit --jit-backend x64` back to back. The table shows the run with the median SH2 total per mode (the progress-table convention); the last two columns give the SH2 totals and max frame times of all three runs. Load: usual desktop applications, nothing else heavy. All 63 runs exited 0.

The interpreter's SH2 totals in this session are close to the 2B results table (PD2 5.37 vs 5.27, SFZ3 4.96 vs 5.04) and about 25–30% below the "2C progress" sessions; only same-session ratios are compared.

| Title | Mode | ms/frame avg | p95 | max | SH2 master ms (%) | SH2 slave ms (%) | SH2 total ms (%) | SH2 total, 3 runs | max frame ms, 3 runs |
|---|---|---|---|---|---|---|---|---|---|
| BIOS menu (US) | interpreter | 8.081 | 8.584 | 9.370 | 1.878 (23.2%) | 0.000 (0.0%) | 1.878 (23.2%) | 1.871 / 1.879 / 1.878 | 9.6 / 9.9 / 9.4 |
| | IR | 8.853 | 9.533 | 12.356 | 6.501 (73.4%) | 0.000 (0.0%) | 6.501 (73.4%) | 6.501 / 6.538 / 6.454 | 12.4 / 13.1 / 11.3 |
| | x64 | 8.015 | 8.781 | 11.350 | 1.525 (19.0%) | 0.000 (0.0%) | 1.525 (19.0%) | 1.513 / 1.554 / 1.525 | 10.8 / 10.7 / 11.4 |
| Virtua Fighter 2 (JP) | interpreter | 7.045 | 9.016 | 12.861 | 3.072 (43.6%) | 1.932 (27.4%) | 5.003 (71.0%) | 5.003 / 4.971 / 5.004 | 12.9 / 11.3 / 11.8 |
| | IR | 18.992 | 23.770 | 32.970 | 9.669 (50.9%) | 6.935 (36.5%) | 16.604 (87.4%) | 16.596 / 16.604 / 16.628 | 30.5 / 33.0 / 28.5 |
| | x64 | 6.326 | 7.728 | 11.306 | 2.585 (40.9%) | 1.409 (22.3%) | 3.994 (63.1%) | 4.009 / 3.959 / 3.994 | 10.9 / 10.4 / 11.3 |
| Panzer Dragoon II Zwei (US) | interpreter | 7.435 | 8.623 | 12.341 | 3.431 (46.2%) | 1.938 (26.1%) | 5.370 (72.2%) | 5.370 / 5.398 / 5.341 | 12.3 / 11.6 / 11.0 |
| | IR | 18.052 | 20.005 | 23.997 | 10.111 (56.0%) | 5.447 (30.2%) | 15.558 (86.2%) | 15.499 / 15.558 / 15.636 | 26.8 / 24.0 / 25.1 |
| | x64 | 7.126 | 8.959 | 12.455 | 3.231 (45.3%) | 1.708 (24.0%) | 4.940 (69.3%) | 4.940 / 4.946 / 4.919 | 12.5 / 11.2 / 11.7 |
| Sega Rally Championship (US) | interpreter | 7.728 | 9.496 | 12.557 | 2.885 (37.3%) | 2.279 (29.5%) | 5.164 (66.8%) | 5.135 / 5.165 / 5.164 | 11.6 / 14.7 / 12.6 |
| | IR | 18.064 | 24.806 | 28.774 | 9.115 (50.5%) | 6.344 (35.1%) | 15.458 (85.6%) | 15.419 / 15.458 / 15.547 | 28.8 / 28.8 / 31.2 |
| | x64 | 8.189 | 10.662 | 13.772 | 2.783 (34.0%) | 2.505 (30.6%) | 5.288 (64.6%) | 5.288 / 5.347 / 5.285 | 13.8 / 14.2 / 14.3 |
| Burning Rangers (US) | interpreter | 9.672 | 11.019 | 13.896 | 4.437 (45.9%) | 2.242 (23.2%) | 6.679 (69.1%) | 6.679 / 6.650 / 6.718 | 13.9 / 13.8 / 14.7 |
| | IR | 23.671 | 25.639 | 30.610 | 13.480 (57.0%) | 6.960 (29.4%) | 20.441 (86.4%) | 20.410 / 20.648 / 20.441 | 28.8 / 33.4 / 30.6 |
| | x64 | 10.155 | 12.142 | 16.636 | 4.919 (48.4%) | 1.966 (19.4%) | 6.884 (67.8%) | 6.884 / 6.842 / 6.902 | 16.6 / 14.4 / 17.0 |
| Guardian Heroes (US) | interpreter | 6.852 | 8.229 | 11.085 | 2.697 (39.4%) | 1.705 (24.9%) | 4.401 (64.2%) | 4.369 / 4.748 / 4.401 | 10.3 / 11.4 / 11.1 |
| | IR | 16.219 | 19.385 | 24.970 | 8.333 (51.4%) | 4.979 (30.7%) | 13.312 (82.1%) | 13.309 / 13.312 / 13.737 | 23.8 / 25.0 / 25.4 |
| | x64 | 6.664 | 8.088 | 10.994 | 2.341 (35.1%) | 1.638 (24.6%) | 3.978 (59.7%) | 3.971 / 3.999 / 3.978 | 11.1 / 12.6 / 11.0 |
| Street Fighter Zero 3 (JP, 4 MB cart) | interpreter | 9.304 | 10.089 | 12.015 | 2.871 (30.9%) | 2.087 (22.4%) | 4.958 (53.3%) | 4.951 / 4.958 / 4.964 | 12.6 / 12.0 / 11.9 |
| | IR | 18.498 | 21.367 | 26.651 | 9.704 (52.5%) | 6.182 (33.4%) | 15.885 (85.9%) | 15.927 / 15.809 / 15.885 | 27.5 / 27.3 / 26.7 |
| | x64 | 10.057 | 12.169 | 26.940 | 3.774 (37.5%) | 1.660 (16.5%) | 5.434 (54.0%) | 5.434 / 5.474 / 5.377 | 26.9 / 26.8 / 29.3 |

### Ratios (SH2 total, median runs)

| Title | interpreter / x64 (target ≥ 2.0) | 2B (for reference) | IR / x64 | interpreter / IR |
|---|---|---|---|---|
| BIOS menu | 1.231 | 0.553 | 4.263 | 0.289 |
| Virtua Fighter 2 | **1.253** | 0.488 | 4.157 | 0.301 |
| Panzer Dragoon II Zwei | **1.087** | 0.583 | 3.149 | 0.345 |
| Sega Rally Championship | **0.977** | 0.571 | 2.923 | 0.334 |
| Burning Rangers | **0.970** | 0.546 | 2.969 | 0.327 |
| Guardian Heroes | **1.106** | 0.588 | 3.346 | 0.331 |
| Street Fighter Zero 3 | **0.912** | 0.450 | 2.923 | 0.312 |

The BIOS menu is not one of the six target titles; it is listed for reference.

Notes:

- **Max frame times**: x64 max frame 10.4–17.0 ms on five titles (the interpreter's 9.4–14.7 ms) and 26.8–29.3 ms on Street Fighter Zero 3 (interpreter 11.9–12.6 ms, IR 26.7–27.5 ms), from 84–106 ms (Virtua Fighter 2) and 256–264 ms (Street Fighter Zero 3) in 2B. Street Fighter Zero 3's slowest frames are bursts of new code (the measured window built 10,686 blocks and compiled 7,183 natively, 1.22 s, 0.68 ms/frame, in 1,090 of 1,800 frames); no x64 window had a flush or an IR-cap eviction.
- Every x64 run reported `compileFallbacks 0` on both CPUs. `nativeBlocksRun` (master / slave): BIOS 172,163,971 / 0; VF2 251,174,352 / 431,461,785; PD2 355,251,634 / 205,691,083; Rally 324,759,821 / 157,245,823; BR 266,131,859 / 219,628,117; GH 282,686,600 / 210,935,962; SFZ3 249,659,137 / 188,693,274 (totals since boot).
- x64 is 2.9–4.3x faster than the IR backend (2B: 1.3–1.7x). The IR backend's own ratio to the interpreter (0.29–0.35) is 5–10% below 2B's (0.32–0.38); the cause was not investigated.
- Virtua Fighter 2's interpreter runs were steady in this session (4.97–5.00 ms), unlike 2B's fast third round.

### Re-profile (where x64 SH-2 time goes now)

Same method as the 2B profile: a temporary instrumented `build-bench` (not committed; reverted, and `build-bench` rebuilt clean at the measured commit), switched with an environment variable:
- level 0: counters only (per CPU: `Executor::Run` calls, steps, `Get` calls, native chain runs and blocks, IR runs, interpreter fallbacks, and calls of each x64 trampoline);
- level 1: plus one `rdtsc` interval around each `Executor::Run`;
- level 2: plus intervals around each `BlockCache::Get` (it includes compiling), each `X64Backend::Run` (a whole chain, trampolines included), each `RunBlock` (IR runs before tier-up) and each interpreter fallback.

Panzer Dragoon II Zwei, Street Fighter Zero 3 and Burning Rangers, x64, the measurement scene, three interleaved rounds of the three levels (27 runs). Values are means of three runs (spread within 2%). The executor time without instrumentation is estimated as the uninstrumented SH2 total of the table above minus the outside-`Run` time of level 1. Timer overhead, from level 2 minus level 1: 7.2–7.7 ns per interval (2B: 6.7 ns), of which an unknown part falls inside the measured intervals, so timed parts are given as ranges (0 to the full overhead subtracted, plus 0.5–1.5 ns per trampoline-call counter inside the native interval). The counters alone cost about 0.3 ms/frame (level 0 vs uninstrumented).

| ms/frame (both CPUs) | Panzer Dragoon II Zwei | Street Fighter Zero 3 | Burning Rangers |
|---|---|---|---|
| Interpreter SH2 total (table above) | 5.370 | 4.958 | 6.679 |
| x64 SH2 total, uninstrumented (table above) | 4.940 | 5.434 | 6.884 |
| Outside `Executor::Run` (level 1: SH2 total − Run) | 1.09 | 1.09 | 1.08 |
| `Executor::Run`, uninstrumented estimate | **3.85** | **4.34** | **5.80** |
| Native chain runs (generated code + trampolines) | 3.24–3.52 (84–91%) | 2.97–3.26 (68–75%) | 4.98–5.28 (86–91%) |
| Compiling (bench counter, inside `Get`) | 0.06 (2%) | 0.67 (15%) | 0.08 (1%) |
| Dispatch: `Get` lookup, `Step` checks, loop | 0.24–0.52 (6–13%) | 0.35–0.64 (8–15%) | 0.32–0.62 (6–11%) |
| IR runs before tier-up | <0.01 | 0.02 | 0.02 |
| Interpreter fallbacks | 0.03 | 0.04 | 0.10 |
| **Budget for 2x**: `Run` ≤ interpreter / 2 − outside | ≤ 1.60 | ≤ 1.39 | ≤ 2.26 |

Structure per 1,800 frames (master / slave):

| | Panzer Dragoon II Zwei | Street Fighter Zero 3 | Burning Rangers |
|---|---|---|---|
| `Executor::Run` calls | 27.78M / 27.78M | 28.01M / 28.01M | 26.70M / 26.70M |
| Native chains / blocks (blocks per chain) | 28.01M / 160.9M (5.7); 27.73M / 105.9M (3.8) | 28.71M / 113.8M (4.0); 27.97M / 98.7M (3.5) | 26.84M / 107.6M (4.0); 26.62M / 113.5M (4.3) |
| Interpreter fallbacks | 3.69M / 0.07M | 2.11M / 0 | 2.54M / 5.93M |
| Read trampolines (on-chip registers) | 0.22M / **85.46M** | 0.53M / **78.33M** | 3.13M / **95.73M** |
| Write trampolines (MMIO) | 2.47M / 1.40M | 2.61M / 0 | 0.98M / 0 |
| Refill trampolines (not constant) | 2.37M / 1.29M | 3.78M / 0 | 1.66M / 0 |
| Bus-wait trampolines | 1.32M / 0 | 2.91M / 0 | 3.42M / 0 |
| `SetSR` / `Div1` trampolines | 0.93M / 0.50M | 1.17M / 8.23M | 0.39M / **44.78M** |
| Delay-slot setup/end, access-cycles, MAC trampolines | 0 | 0 | 0 |

What changed since the 2B profile (Panzer Dragoon II Zwei, the only title profiled in both): the executor fell from about 7.96 to about 3.85 ms/frame. Dispatch fell from 2.0–3.4 to 0.3–0.6 ms/frame (chaining: 83% of master blocks and 74% of slave blocks are entered from the previous block), and the trampolines from 856M to 96M calls (refills and delay-slot calls are inline stores now). What is left is mostly generated code: native runs take 84–91% of the executor. Writes cost about the same as in 2B (3.9M calls, ~0.5 ms/frame of device work the interpreter also pays) and the slave's 85.5M on-chip reads an estimated 0.2–0.4 ms/frame, so generated code proper is about 2.3–2.8 ms/frame. Over 901.9M guest instructions retired in native blocks per 1,800 frames (2B structure; block counts are unchanged), that is 4.6–5.6 ns per guest instruction, 6.5–7.0 ns with trampolines, against about 8.5 ns per instruction for the interpreter's loop (4.28 ms/frame for 905.7M instructions). The 2x budget is 1.60 ms/frame for the whole executor, about 3.2 ns per instruction all-in.

- **Street Fighter Zero 3**: compiling is the second-largest part (0.67 ms/frame, 15% of the executor): the master keeps building new code (10,686 blocks and 7,183 native compiles in 30 s). The master's `Div1` trampoline runs 8.2M times.
- **Burning Rangers**: the master's native runs take 4.0 ms/frame for 59.8k blocks per frame, 67 ns per block (PD2's master: 27 ns). Its hot code runs `DIV1` through a trampoline 44.8M times per 1,800 frames (24,900 per frame), each with an SR write-back and reload around the call (`Div1` is a register-cache barrier). The slave polls FTCSR through the read trampoline (95.7M calls).
- **Outside the executor**: about 1.09 ms/frame of `SH2::Advance` in all three titles, paid in both modes; with a free executor the ratio would be at most 4.9 (PD2), 4.5 (SFZ3) and 6.2 (BR).

### Long-session cache behavior

- **IR-only eviction cost** (temporary timer around `EvictIrOnly`, not committed; Street Fighter Zero 3, x64, `--warmup 2400 --frames 3600`): the master's one eviction, between frames 4,200 and 6,000, took **9.9 ms** and dropped 14,990 IR-only blocks (1.05M IR instructions) of 35,616; the 20,626 native blocks stayed. That frame was not among the five slowest of the window (41.3, 34.0, 27.3, 27.0 and 26.9 ms, all compile bursts of new code). For comparison, an eviction on the IR backend (every block is IR-only, so it empties the cache) took 6.5–8.3 ms for 13,365–15,370 blocks (three evictions in one `--warmup 2400 --frames 1800` run). The cost is about 0.5–0.7 µs per dropped block: freeing each block's IR and map node.
- **36,000-frame lockstep runs** (x64, `design/sh2-validation.md`, Milestone 2C): IR-cap evictions in Virtua Fighter 2 (1), Panzer Dragoon II Zwei (1) and Street Fighter Zero 3 (4), master only; native-code-cap flushes only in Burning Rangers (master 10, slave 9).
- **Burning Rangers** loads new code over old code throughout its attract sequence. Over the 36,000-frame lockstep its master built 1,125,913 blocks and compiled 307,507 natively (883,698 invalidations, 142,387 of them stale native blocks found by their prologue); the slave 1,137,419 and 196,276. Stale native code stays allocated until a flush, so each CPU's 128 MB code cap fills about once a minute on average (10 / 9 flushes in the ~10-minute run), clustered in the code-reload phases, and each fill is a full flush followed by a recompile burst. A long default-settings run shows what that costs (`--warmup 2400 --frames 33600`, one run per mode, measured window = frames 2,400–36,000; build and block counts equal the lockstep run's):

  | Burning Rangers, frames 2,400–36,000 | ms/frame avg | p95 | p99 | max | SH2 total ms/frame |
  |---|---|---|---|---|---|
  | interpreter | 9.856 | 12.282 | 13.495 | 16.879 | 6.369 |
  | x64 | 15.585 | 49.786 | 86.367 | 173.340 | 11.650 (ratio 0.55) |

  - In the window the two CPUs built 2,244,425 blocks and compiled 492,237 natively: 167.8 s of compiling, 5.0 ms/frame on average, in 13,500 of 33,600 frames. 22 flushes (19 at the code cap, the rest requested by the core); no IR-cap eviction.
  - The churn comes in phases: native compiles (master) rose from 12,592 at frame 6,000 to 164,266 at frame 16,800 and from 166,381 at frame 22,200 to 305,570 at frame 31,200, and barely moved in between. The slave's code changes in the same phases.
  - The five slowest frames (151–173 ms) each spent 128–135 ms compiling (416–799 native compiles, about 200 µs each; Burning Rangers' blocks are large: 4.7 KB of code per master block, 6.9 KB per slave block). Only the slowest one (frame 9,757) also had a flush.
  - The 1,800-frame measurement window (frames 2,400–4,200) lies before the first phase, so the results table does not show this.
- **Memory**: the native code cap is 128 MB per CPU (`kMaxNativeCodeBytes`), so 256 MB in the worst case for both CPUs, plus up to about 20 MB of IR per CPU (`kMaxCachedInsts`). asmjit commits code memory on demand, so a title uses only what it compiles (SFZ3's master: about 54 MB after 4,200 frames). Stale native code (a block dropped after its guest code changed) is reclaimed only at a flush.
- **Native-code headroom (Street Fighter Zero 3)**: the Task 5 long run (x64, `--warmup 2400 --frames 18000`) ended at about 90.6 MB of 128 MB (71%), growing about 1.3 KB per frame, so a full code-cap flush (every native block dropped, then a recompile burst) is projected at about 50,000 frames (about 14 minutes). Its 36,000-frame lockstep run had none, consistent with that projection.
- **Known long-session stall sources**: a native-code-cap flush drops every native block of that CPU, and the frames after it recompile the working set; Burning Rangers reaches it about once a minute on average (10 master / 9 slave flushes in 36,000 frames, ~10 minutes), more often during its code-reload phases, Street Fighter Zero 3 after about 14 minutes. Follow-up: evict cold or stale native blocks (release their code individually) instead of flushing the whole cache. Burning Rangers' recompiles of reloaded code cost far more than the flushes themselves (above): with the JIT on, it runs at about half the interpreter's speed in those phases, with frequent 50–170 ms frames.

### Next steps

The executor must shrink a further 2.4x (PD2, 3.85 to 1.60 ms/frame), 3.1x (SFZ3) or 2.6x (BR) for 2x. Generated code is now the bulk of it, so the remaining work is mostly per-instruction cost inside blocks:

1. **Per-instruction cost in generated code** (PD2: 60–73% of the executor): each instruction still pays its own cycle bookkeeping, a boundary check, and the bus page-table path for every memory access. Candidates: one budget check per block (or per run of instructions) when the block's worst-case cycles fit in the remaining budget and no callback in it can raise an interrupt, with the per-instruction checks kept as the fallback; resolving the page of constant and GBR/PC-relative addresses at compile time.
2. **On-chip register reads** (slaves: 78–96M read trampolines per 1,800 frames, mostly polling FTCSR): an inline path for side-effect-free on-chip reads, or an exact fast-forward of proven polling loops.
3. **`DIV1` inline** (Burning Rangers master 44.8M, SFZ3 8.2M calls): emit the divide step in generated code and drop it as a register-cache barrier.
4. **Compile cost** (SFZ3 0.67 ms/frame; asmjit's register allocation is 77% of a native compile): a lighter emitter or cheaper allocation for cold blocks; `kNativeCompileThreshold` = 16 is untested (tested and chosen in 2D B5).
5. **Code that is reloaded** (Burning Rangers: 492,237 native compiles in 33,600 frames, SH-2 time 1.8x the interpreter's over that run): reuse native code when the same guest code comes back (key compiled code by its opcodes, not only its PC), raise the native threshold for blocks at PCs that keep going stale, and evict stale and cold native blocks instead of flushing at the code cap (Burning Rangers flushes about once a minute on average, clustered in its code-reload phases).
6. The remaining dispatch (one `Get` and `Step` per `Executor::Run` call, about 31,000 per frame) and interpreter fallbacks are small (under 0.7 ms/frame together).
7. **Save-state loads** (known issue): loading a save state, including RetroArch run-ahead and rewind (which load states continuously), flushes the JIT cache, so native code is recompiled after each load. Follow-up: check whether the flush is still required (check-on-entry may already catch the changed code).

## Reproduce

```powershell
brimir_bench --bios <BIOS> --game <game> --system-dir <scratch dir> --warmup 2400 --frames 1800 [--sh2-jit --jit-backend ir|x64]
brimir_bench --bios mpr-17933.bin --warmup 300 --frames 1800 [--sh2-jit --jit-backend ir|x64]    # BIOS menu row
```

# Milestone 2D

Milestone 2D (`design/sh2-jit-m2d.md`, plan `design/plans/2026-10-15-sh2-jit-m2d-track-b.md`) starts from the 2C result: x64 about as fast as the interpreter. Step 0 makes the SH-2 host-time accounting cheap, so the baseline below is measured without its overhead.

## Step 0: host-time accounting overhead

`SH2::Advance` used to read `std::chrono::steady_clock` twice per call when profiling is on (38.6 ns per pair in the 2B profile; 55.6M calls per 1,800 frames, about 1.2 ms/frame). It now adds `HostTicks()` (`rdtsc` on x86-64, `cntvct_el0` on AArch64, `steady_clock` elsewhere) and converts to nanoseconds in `ConsumeHostTimeNs()` with the ticks-to-`steady_clock` ratio since profiling was enabled (no busy-wait calibration).

Panzer Dragoon II Zwei, interpreter, `--warmup 2400 --frames 1800`, `build-bench` rebuilt before and after the change, three runs each back to back (2026-10-15, same session):

| | SH2 total ms/frame (3 runs, median) | SH2 master / slave (median run) | `Ymir_RunFrame` ms/frame (3 runs) | ms/frame avg (median run) |
|---|---|---|---|---|
| before (`steady_clock`) | 5.681 (5.660 / 5.681 / 5.684) | 3.644 / 2.036 | 7.807 / 7.829 / 7.852 | 7.831 |
| after (`HostTicks`) | 5.044 (4.992 / 5.044 / 5.059) | 3.263 / 1.781 | 6.490 / 6.537 / 6.552 | 6.538 |

The SH2 total falls 0.64 ms/frame (11%), as predicted (about 0.6): that is the part of the clock reads that fell inside the measured interval. The whole frame falls 1.29 ms (7.83 to 6.54), which is the full cost of the clock reads (2 x 55.6M x 19 ns / 1,800). All profiled SH-2 numbers measured before this step include the overhead. Both modes pay it, so subtracting it raises every ratio a little (Panzer Dragoon II Zwei: 2C 1.087, now 1.126); only same-session ratios are compared.

## 2D baseline

**Date**: 2026-10-15
**Commit**: the commit that adds this section (parent 7594f84); the measured build is that commit's code
**Machine and build**: as in the 2B method (AMD Ryzen 7 5700G, `build-bench`, Ninja, Release, `BRIMIR_LTO=ON`, MSVC 19.44.35229), rebuilt after Step 0.

Method: the 2C method unchanged (same scenes, `--warmup 2400 --frames 1800` for games, `--warmup 300 --frames 1800` for the BIOS menu, default settings, scratch system dir with `brimir_saturn_rtc_us_eu.smpc` also as `brimir_saturn_rtc_jp.smpc`, three rounds, each round running every title in the interpreter, `--sh2-jit --jit-backend ir` and `--sh2-jit --jit-backend x64` back to back). The table shows the run with the median SH2 total per mode. All 63 runs exited 0; every x64 run reported `compileFallbacks 0` on both CPUs. Load: usual desktop applications, nothing else heavy.

| Title | Mode | ms/frame avg | p95 | max | SH2 master ms (%) | SH2 slave ms (%) | SH2 total ms (%) | SH2 total, 3 runs | max frame ms, 3 runs |
|---|---|---|---|---|---|---|---|---|---|
| BIOS menu (US) | interpreter | 8.034 | 8.513 | 12.2 | 1.666 (20.7%) | 0.000 (0.0%) | 1.666 (20.7%) | 1.676 / 1.666 / 1.661 | 9.0 / 12.2 / 9.0 |
| | IR | 8.714 | 9.290 | 12.8 | 6.276 (72.0%) | 0.000 (0.0%) | 6.276 (72.0%) | 6.276 / 6.346 / 6.184 | 12.8 / 13.4 / 10.4 |
| | x64 | 7.829 | 8.500 | 10.4 | 1.227 (15.7%) | 0.000 (0.0%) | 1.227 (15.7%) | 1.240 / 1.227 / 1.214 | 12.0 / 10.4 / 11.7 |
| Virtua Fighter 2 (JP) | interpreter | 6.236 | 7.536 | 8.6 | 2.806 (45.0%) | 1.654 (26.5%) | 4.460 (71.5%) | 4.460 / 4.441 / 4.492 | 8.6 / 10.0 / 9.6 |
| | IR | 17.974 | 22.476 | 24.9 | 9.432 (52.5%) | 6.604 (36.7%) | 16.035 (89.2%) | 16.036 / 16.035 / 16.017 | 24.0 / 24.9 / 25.2 |
| | x64 | 5.756 | 6.576 | 8.1 | 2.356 (40.9%) | 1.169 (20.3%) | 3.525 (61.2%) | 3.525 / 3.520 / 3.541 | 8.1 / 9.7 / 7.9 |
| Panzer Dragoon II Zwei (US) | interpreter | 6.409 | 7.565 | 9.0 | 3.177 (49.6%) | 1.741 (27.2%) | 4.918 (76.7%) | 4.958 / 4.918 / 4.900 | 9.1 / 9.0 / 8.3 |
| | IR | 16.948 | 18.754 | 24.0 | 9.740 (57.5%) | 5.262 (31.0%) | 15.001 (88.5%) | 14.973 / 15.029 / 15.001 | 20.3 / 20.0 / 24.0 |
| | x64 | 6.083 | 7.691 | 8.9 | 2.889 (47.5%) | 1.479 (24.3%) | 4.369 (71.8%) | 4.369 / 4.387 / 4.338 | 8.9 / 9.0 / 9.0 |
| Sega Rally Championship (US) | interpreter | 7.254 | 8.483 | 9.7 | 2.618 (36.1%) | 2.059 (28.4%) | 4.677 (64.5%) | 4.690 / 4.677 / 4.650 | 10.0 / 9.7 / 10.1 |
| | IR | 17.166 | 23.763 | 26.2 | 8.964 (52.2%) | 6.109 (35.6%) | 15.073 (87.8%) | 15.116 / 15.073 / 15.034 | 26.3 / 26.2 / 25.7 |
| | x64 | 7.757 | 9.780 | 11.1 | 2.514 (32.4%) | 2.274 (29.3%) | 4.788 (61.7%) | 4.806 / 4.765 / 4.788 | 13.8 / 11.6 / 11.1 |
| Burning Rangers (US) | interpreter | 8.711 | 9.855 | 11.4 | 4.291 (49.3%) | 2.027 (23.3%) | 6.318 (72.5%) | 6.318 / 6.309 / 6.387 | 11.4 / 10.5 / 11.5 |
| | IR | 22.713 | 23.897 | 28.7 | 13.106 (57.7%) | 6.823 (30.0%) | 19.929 (87.7%) | 19.960 / 19.866 / 19.929 | 25.8 / 26.2 / 28.7 |
| | x64 | 9.145 | 10.784 | 12.6 | 4.656 (50.9%) | 1.766 (19.3%) | 6.422 (70.2%) | 6.406 / 7.317 / 6.422 | 15.5 / 13.0 / 12.6 |
| Guardian Heroes (US) | interpreter | 6.148 | 7.065 | 9.0 | 2.438 (39.6%) | 1.502 (24.4%) | 3.939 (64.1%) | 3.918 / 3.939 / 4.003 | 9.3 / 9.0 / 8.2 |
| | IR | 15.431 | 18.295 | 19.3 | 8.168 (52.9%) | 4.897 (31.7%) | 13.066 (84.7%) | 13.107 / 13.066 / 13.001 | 22.0 / 19.3 / 20.0 |
| | x64 | 6.120 | 6.956 | 10.1 | 2.062 (33.7%) | 1.450 (23.7%) | 3.513 (57.4%) | 3.505 / 3.513 / 3.554 | 8.4 / 10.1 / 8.4 |
| Street Fighter Zero 3 (JP, 4 MB cart) | interpreter | 8.866 | 9.522 | 11.1 | 2.691 (30.4%) | 1.862 (21.0%) | 4.553 (51.4%) | 4.579 / 4.553 / 4.538 | 11.3 / 11.1 / 11.6 |
| | IR | 17.407 | 20.164 | 25.8 | 9.431 (54.2%) | 5.936 (34.1%) | 15.367 (88.3%) | 15.367 / 15.432 / 15.346 | 25.8 / 25.6 / 26.2 |
| | x64 | 9.629 | 11.565 | 27.3 | 3.419 (35.5%) | 1.505 (15.6%) | 4.924 (51.1%) | 4.974 / 4.905 / 4.924 | 26.5 / 26.1 / 27.3 |

### Ratios (SH2 total, median runs)

| Title | interpreter / x64 (target = 2.0) | 2C (with the old accounting) | IR / x64 | interpreter / IR |
|---|---|---|---|---|
| BIOS menu | 1.358 | 1.231 | 5.115 | 0.265 |
| Virtua Fighter 2 | **1.265** | 1.253 | 4.549 | 0.278 |
| Panzer Dragoon II Zwei | **1.126** | 1.087 | 3.434 | 0.328 |
| Sega Rally Championship | **0.977** | 0.977 | 3.148 | 0.310 |
| Burning Rangers | **0.984** | 0.970 | 3.103 | 0.317 |
| Guardian Heroes | **1.121** | 1.106 | 3.719 | 0.301 |
| Street Fighter Zero 3 | **0.925** | 0.912 | 3.121 | 0.296 |

Notes:

- Against 2C the ratios of the six target titles moved by at most +0.04 (the BIOS menu by +0.13): the interpreter and x64 each lose about 0.64 ms/frame of accounting overhead, and the x64 total is the smaller one, so the same subtraction raises the ratio a little. The x64 backend is still about as fast as the interpreter; 2.0 is the target to reach with steps 1-6.
- Absolute SH2 totals fell 0.6-0.8 ms/frame in every mode against 2C (2C interpreter: PD2 5.370, SFZ3 4.958, BR 6.679; now 4.918, 4.553, 6.318); they are not comparable beyond that, as the 2C session ran on a differently loaded machine.
- `nativeBlocksRun` (master / slave) is identical to 2C for PD2 (355,251,634 / 205,691,083), Rally, BR, GH and SFZ3 and within the known VF2 run-to-run variation (251,174,283 / 431,462,354 here, 251,174,352 / 431,461,785 in 2C), so the same code is measured.
- One Burning Rangers x64 run (round 2) had an SH2 total of 7.317 ms against 6.406 / 6.422 for the others, with no compile counters different from the others; it is not the median run and does not affect the table.

## 2D progress

Measured at the end of each milestone 2D task (`design/plans/2026-10-15-sh2-jit-m2d-track-b.md`) on the machine and build configuration above, Panzer Dragoon II Zwei and Burning Rangers (US BIOS), Street Fighter Zero 3 and Virtua Fighter 2 (JP BIOS), `--warmup 2400 --frames 1800`, three rounds each. The Task 1 rows are the interpreter and x64 columns of the baseline above (same runs: in that session each round ran the IR backend between the two); later rows run the interpreter and then `--sh2-jit --jit-backend x64` back to back. SH2 totals are the medians of the three runs (all three in parentheses); "x64 max" is the max frame time of the median x64 run; "interpreter max" is the highest max frame time of the three interpreter runs; `compileFallbacks` is master / slave.

| Task | Title | Interpreter SH2 total ms | x64 SH2 total ms | Ratio (interp / x64) | x64 max frame ms | Interpreter max frame ms | compileFallbacks |
|---|---|---|---|---|---|---|---|
| 1: cheap host-time accounting (baseline) | Panzer Dragoon II Zwei | 4.918 (4.958 / 4.918 / 4.900) | 4.369 (4.369 / 4.387 / 4.338) | 1.126 | 8.9 | 9.1 | 0 / 0 |
| 1: cheap host-time accounting (baseline) | Street Fighter Zero 3 | 4.553 (4.579 / 4.553 / 4.538) | 4.924 (4.974 / 4.905 / 4.924) | 0.925 | 27.3 | 11.6 | 0 / 0 |
| 1: cheap host-time accounting (baseline) | Burning Rangers | 6.318 (6.318 / 6.309 / 6.387) | 6.422 (6.406 / 7.317 / 6.422) | 0.984 | 12.6 | 11.5 | 0 / 0 |
| 1: cheap host-time accounting (baseline) | Virtua Fighter 2 | 4.460 (4.460 / 4.441 / 4.492) | 3.525 (3.525 / 3.520 / 3.541) | 1.265 | 8.1 | 10.0 | 0 / 0 |
| 2: guarded runs of boundary checks | Panzer Dragoon II Zwei | 4.942 (5.006 / 4.942 / 4.937) | 4.307 (4.317 / 4.302 / 4.307) | 1.147 | 8.8 | 10.0 | 0 / 0 |
| 2: guarded runs of boundary checks | Street Fighter Zero 3 | 4.698 (4.698 / 4.856 / 4.676) | 4.880 (4.899 / 4.872 / 4.880) | 0.963 | 27.6 | 11.7 | 0 / 0 |
| 2: guarded runs of boundary checks | Burning Rangers | 6.478 (6.477 / 6.529 / 6.478) | 6.353 (6.353 / 6.499 / 6.323) | 1.020 | 14.6 | 15.9 | 0 / 0 |
| 2: guarded runs of boundary checks | Virtua Fighter 2 | 4.506 (4.513 / 4.502 / 4.506) | 3.493 (3.500 / 3.493 / 3.473) | 1.290 | 11.2 | 11.2 | 0 / 0 |
| 3: inline DIV1 | Panzer Dragoon II Zwei | 4.936 (4.936 / 4.973 / 4.899) | 4.370 (4.370 / 4.379 / 4.272) | 1.130 | 10.8 | 9.5 | 0 / 0 |
| 3: inline DIV1 | Street Fighter Zero 3 | 4.658 (4.649 / 4.716 / 4.658) | 4.875 (4.884 / 4.875 / 4.852) | 0.955 | 27.6 | 11.6 | 0 / 0 |
| 3: inline DIV1 | Burning Rangers | 6.451 (6.538 / 6.418 / 6.451) | 6.217 (6.223 / 6.217 / 6.204) | 1.038 | 12.5 | 15.7 | 0 / 0 |
| 3: inline DIV1 | Virtua Fighter 2 | 4.551 (4.569 / 4.551 / 4.472) | 3.453 (3.453 / 3.522 / 3.452) | 1.318 | 9.0 | 11.0 | 0 / 0 |
| 4: direct on-chip register reads | Panzer Dragoon II Zwei | 5.006 (5.006 / 5.002 / 5.010) | 4.253 (4.253 / 4.283 / 4.234) | 1.177 | 8.4 | 11.7 | 0 / 0 |
| 4: direct on-chip register reads | Street Fighter Zero 3 | 4.607 (4.607 / 4.588 / 4.608) | 4.824 (4.824 / 4.837 / 4.819) | 0.955 | 27.0 | 11.1 | 0 / 0 |
| 4: direct on-chip register reads | Burning Rangers | 6.389 (6.427 / 6.378 / 6.389) | 6.148 (6.161 / 6.148 / 6.104) | 1.039 | 14.8 | 13.2 | 0 / 0 |
| 4: direct on-chip register reads | Virtua Fighter 2 | 4.467 (4.463 / 4.467 / 4.560) | 3.443 (3.421 / 3.472 / 3.443) | 1.297 | 7.4 | 10.1 | 0 / 0 |
| 5: compile-time page resolution (B2) | Panzer Dragoon II Zwei | 6.836 (6.836 / 6.820 / 6.848) | 6.316 (6.257 / 6.316 / 6.334) | 1.082 | 11.6 | 12.4 | 0 / 0 |
| 5: compile-time page resolution (B2) | Street Fighter Zero 3 | 6.316 (6.316 / 6.350 / 6.101) | 6.506 (6.506 / 6.587 / 6.420) | 0.971 | 32.2 | 14.3 | 0 / 0 |
| 5: compile-time page resolution (B2) | Burning Rangers | 8.793 (8.793 / 8.802 / 8.750) | 8.832 (8.832 / 8.788 / 8.894) | 0.996 | 16.7 | 17.1 | 0 / 0 |
| 5: compile-time page resolution (B2) | Virtua Fighter 2 | 6.279 (6.417 / 6.279 / 6.277) | 5.015 (5.015 / 4.976 / 5.047) | 1.252 | 10.8 | 12.8 | 0 / 0 |
| 6: native compile threshold 16 (B5) | Panzer Dragoon II Zwei | 6.982 (6.982 / 7.046 / 6.856) | 6.389 (6.466 / 6.389 / 6.233) | 1.093 | 14.1 | 19.4 | 0 / 0 |
| 6: native compile threshold 16 (B5) | Street Fighter Zero 3 | 6.595 (6.595 / 6.776 / 6.105) | 6.526 (7.089 / 6.526 / 6.364) | 1.011 | 27.8 | 15.2 | 0 / 0 |
| 6: native compile threshold 16 (B5) | Burning Rangers | 8.600 (8.609 / 8.600 / 8.575) | 8.734 (8.734 / 8.772 / 8.663) | 0.985 | 16.9 | 16.3 | 0 / 0 |
| 6: native compile threshold 16 (B5) | Virtua Fighter 2 | 6.261 (6.261 / 6.269 / 6.248) | 4.957 (5.026 / 4.957 / 4.854) | 1.263 | 10.9 | 12.5 | 0 / 0 |

Task 2 notes (2026-10-15, interpreter and x64 back to back, no IR runs): every ratio rose by 0.02-0.04 and every x64 median fell by 0.6-1.4% (PD2 4.369 to 4.307, SFZ3 4.924 to 4.880, BR 6.422 to 6.353, VF2 3.525 to 3.493), about the size of the run-to-run spread within a session (x64 up to 0.03 ms; the interpreter up to 0.16 ms on SFZ3), so the gain is small. `nativeBlocksRun` matches the baseline (same code measured). Rule 5 guards only the checks before a block's first Load or Store (after one, every known refill may call back and the next check is a full test, rule 2), which limits it. Cost, BIOS lockstep 1200 (x64, master): 2923 native bytes per block against 2825 with rule 5 disabled (+3.5%), native compile 529.8 ms against 448.7 ms (+18%, each run once).

Task 3 notes (2026-10-15, interpreter and x64 back to back, no IR runs): Burning Rangers, the title with 44.8M `Div1` trampoline calls per 1,800 frames, fell from 6.353 to 6.217 ms (-2.1%; its three runs span 0.019 ms; ratio 1.020 to 1.038). Virtua Fighter 2 fell from 3.493 to 3.453 (-1.1%) and Street Fighter Zero 3 (8.2M calls) is unchanged within spread (4.880 to 4.875). Panzer Dragoon II Zwei read 4.307 to 4.370 (+1.5%) with its three runs spanning 0.098 ms (4.272-4.379) against 0.015 ms in Task 2; it runs `Div1` only 0.5M times per 1,800 frames, so this is session noise, not the change. `nativeBlocksRun` matches the baseline. Cost, BIOS lockstep 1200 (x64, master): 2983 native bytes per block against 2923 (+2.1%). The step is branch-free (`LowerDiv1`, about 40 instructions) and needs no SR store, call or reload around it.

Task 5 notes (2026-10-15): the machine was about 40% slower than in Tasks 1-4 for this session (the interpreter reads PD2 6.836 ms against 4.9-5.0 before; background load, not the change), so the Task 5 rows are not comparable in absolute terms with the earlier rows; the interpreter and x64 columns of one row are back to back. To isolate the change, x64 was also run from the Task 4 build (`base`) and the Task 5 build (`new`) interleaved, three runs per title in that session (SH2 total, ms, median with the three runs): PD2 6.334 (6.325-6.366) to 6.344 (6.334-6.381), SFZ3 6.953 (6.941-7.017) to 6.816 (6.801-6.885, -2.0%), BR 9.058 (9.037-9.312) to 9.050 (9.010-9.138), VF2 5.138 (5.087-5.212) to 5.083 (5.067-5.092, -1.1%). The gain is small: only SFZ3 moves beyond the run-to-run spread, PD2 and BR are unchanged. Native code is smaller (`nativeBytesPerBlock` master / slave: PD2 3223 / 1504 to 3205 / 1499, SFZ3 2985 / 5019 to 2964 / 5017, BR 3016 / 1965 to 3000 / 1932, VF2 2681 / 2835 to 2661 / 2803), BIOS lockstep 1200 reads 3041 bytes per block. A literal-pool load or store at a constant address keeps its page's array check (one compare and branch per access) but no longer computes the page index, tests the partition or reads the array pointer, and its access cycles cost one add of the page's cycle field (partition 0b001/0b101) or an immediate.

## 2D B5

Native compile threshold (`kNativeCompileThreshold`, the run on which a block is compiled natively), measured at 8 (the 2C value), 16 and 32 with three temporary builds of `build-bench` (same options, only the constant changed; the executables were copied aside and run interleaved in one session, 2026-10-15). Panzer Dragoon II Zwei (US BIOS) and Street Fighter Zero 3 (JP BIOS): `--warmup 2400 --frames 1800`, x64, three rounds; each round ran one interpreter run, then N = 8, 16, 32 for each title (SFZ3 first). Burning Rangers (US): one `--warmup 2400 --frames 33600` x64 run per value (rounds 1, 2, 3 respectively, after that round's short runs) and one interpreter run (round 1). Scratch system dir as in the method above. All 28 runs exited 0 and every x64 run reported `compileFallbacks 0` on both CPUs.

**Load.** Checked before and between batches: CPU load 7-10%; the heaviest processes were the usual desktop applications (Spotify, qBittorrent, Slack, OneDrive idle). No other heavy process. The session was still slow compared with the 2D baseline (interpreter SFZ3 6.1-6.8 ms against 4.55; PD2 6.9-7.0 against 4.9) and drifted downwards within it: every value's SFZ3 total fell from round 1 to round 3 (about 7.0, 6.6, 6.35), so only the interleaved comparison within a round is meaningful.

SH2 total ms/frame (median of three, runs in parentheses) and max frame ms (three runs; the median run's in bold):

| Title | Value | SH2 total ms | mean | max frame ms | master native compiles since boot | native compiles in window |
|---|---|---|---|---|---|---|
| Street Fighter Zero 3 | interpreter | 6.595 (6.595 / 6.776 / 6.105) | 6.492 | 13.6 / 15.2 / 12.8 | - | - |
| | N = 8 | 6.581 (7.066 / 6.581 / 6.323) | 6.657 | 31.9 / **30.9** / 31.5 | 19,434 | 7,183 |
| | N = 16 | **6.526** (7.089 / 6.526 / 6.364) | 6.660 | 31.0 / **27.8** / 25.8 | 16,807 | 6,333 |
| | N = 32 | 6.675 (6.863 / 6.675 / 6.389) | 6.642 | 43.3 / **33.7** / 39.0 | 13,671 | 5,155 |
| Panzer Dragoon II Zwei | interpreter | 6.982 (6.982 / 7.046 / 6.856) | 6.961 | 11.6 / 19.4 / 11.3 | - | - |
| | N = 8 | **6.332** (6.587 / 6.332 / 6.288) | 6.402 | 15.5 / **11.9** / 11.7 | 10,671 | 598 |
| | N = 16 | 6.389 (6.466 / 6.389 / 6.233) | 6.363 | 14.7 / **14.1** / 11.2 | 9,144 | 660 |
| | N = 32 | 6.426 (6.427 / 6.426 / 6.293) | 6.382 | 12.9 / **13.8** / 11.7 | 7,824 | 583 |

Burning Rangers, `--frames 33600`, one run each (x64 runs in rounds 1, 2, 3 for N = 8, 16, 32; the interpreter in round 1):

| Value | SH2 total ms | avg frame ms | p95 | p99 | max frame ms | compile time in window (both CPUs) | master native compiles since boot | code-cap flushes since boot (master / slave) | wall time |
|---|---|---|---|---|---|---|---|---|---|
| interpreter | 8.280 | 12.619 | 15.7 | 18.0 | 20.7 | - | - | - | 451 s |
| N = 8 | 14.398 | 19.202 | 58.4 | 100.3 | 203.2 | 196.2 s (5.84 ms/frame) | 315,957 | 11 / 10 | 675 s |
| N = 16 | 11.178 | 16.039 | 35.1 | 54.4 | 211.6 | 86.2 s (2.57 ms/frame) | 127,856 | 4 / 3 | 568 s |
| N = 32 | 9.232 | 13.977 | 24.8 | 33.1 | 79.6 | 34.8 s (1.03 ms/frame) | 36,478 | 0 / 0 | 498 s |

### Decision (the 2C rule)

1. Average SH-2 time within 2% of the best, per title, on the medians of the table: SFZ3 best is N = 16 (6.526); N = 8 is +0.8%, N = 32 +2.3% (outside). PD2 best is N = 8 (6.332); N = 16 is +0.9%, N = 32 +1.5%. On the means of the three runs all three values are within 0.3% (SFZ3) and 0.6% (PD2) of the best, so N = 32 is eligible by that reading; the choice below is the same either way. SFZ3's total drifted by 7-10% over the three rounds of every value, so the totals do not separate the values.
2. Best SFZ3 max frame among the eligible values: N = 16 (31.0 / 27.8 / 25.8 ms, median run 27.8) against N = 8 (31.9 / 30.9 / 31.5) and N = 32 (43.3 / 33.7 / 39.0). Round by round N = 16 is at or below N = 8 (31.0 vs 31.9, 27.8 vs 30.9, 25.8 vs 31.5).
3. Tie-breaker (Burning Rangers long-run max frame) is not needed. For the record: N = 16 against N = 8 is 211.6 vs 203.2 ms (same, a single run), with a lower SH2 total (11.178 vs 14.398).

**Chosen value: 16.** (Was 8.)

### Notes

- Panzer Dragoon II Zwei is unaffected by N (totals within 1.5%, max frame within run-to-run noise: it has no compile bursts in its window, 187 builds). It compiles 14% fewer blocks natively at 16 than at 8 (9,144 vs 10,671 since boot).
- N = 32 on SFZ3 reaches the IR cap: more blocks stay IR-only (the window builds 15,793 blocks against 10,686 at 8 and 16, as evicted IR-only blocks are rebuilt) and the master's IR-only instructions hit `kMaxCachedInsts` once (`irEvictions 1`, 14,470 blocks evicted; its slowest frame, 43.3 ms in frame 2479, is that eviction plus 472 builds). That, not the compile time, is why N = 32's SFZ3 max frame is the worst.
- Burning Rangers is dominated by code that changes constantly (about 884k master and 1.1M slave invalidations in 36,000 frames at every N, so about 25-31 invalidations per frame per CPU): every rewrite sends the PC back to IR and a new threshold count. The threshold directly sets how many of those rewrites reach a native compile (master 316k, 128k, 36k native compiles for N = 8, 16, 32) and so the compile time per frame (5.8, 2.6, 1.0 ms), the code-cap flushes (21, 7, 0 over both CPUs) and the compile bursts (slowest frames 203 / 212 / 80 ms; the interpreter's max is 20.7). N = 32 would roughly match the interpreter on SH2 total there (9.2 vs 8.3 ms) but is ruled out by SFZ3 (worse max frame, the IR-cap eviction). No value gets this title near the spike condition of the design (section 3: x64 max within 10% of the interpreter's max); a lower per-rewrite cost needs per-PC damping (design A3) or a cheaper compile path rather than a different global N.
- Task 6 row of the "2D progress" table is measured with N = 16 (the interpreter and N = 16 runs of this session; Burning Rangers and Virtua Fighter 2 in a following batch of the same session, interpreter and x64 back to back, three rounds). The session is about 30-40% slower than the Task 1-4 rows, so only the ratios are comparable: they are within noise of Task 5's (PD2 1.082 to 1.093, SFZ3 0.971 to 1.011, BR 0.996 to 0.985, VF2 1.252 to 1.263).

## Milestone 2D track B results

**Date**: 2026-10-16
**Commit**: 2700404 (the measured code: the parent of the commit that adds this section)
**Machine and build**: as in the 2D baseline above (AMD Ryzen 7 5700G, `build-bench`, Ninja, Release, `BRIMIR_LTO=ON`, MSVC), rebuilt at the measured commit.

**Verdict: the default-on bar is not met.** Track B (B1, B3, B4, B2, B5 and the accounting of Step 0) made the x64 backend 3-6% faster than at the 2D baseline in the same kind of session, but the interpreter / x64 SH-2 ratio is still 0.97-1.31 on the six titles (bar: at least 1.5 on each), the long Burning Rangers run is at 0.74 (bar: at least 1.0), and Street Fighter Zero 3's compile bursts and the long run's compile bursts fail the spike condition. Lockstep and the test suites are clean, so condition 4 holds (CI green on the measured commit). The JIT stays off by default. Track A (long sessions) is next.

### Method

The 2C method, unchanged (and the 2D baseline's): same scenes, `--warmup 2400 --frames 1800` for the games and `--warmup 300 --frames 1800` for the BIOS menu, default settings, scratch system dir with `brimir_saturn_rtc_us_eu.smpc` also as `brimir_saturn_rtc_jp.smpc`, three rounds, each round running every title in the interpreter, `--sh2-jit --jit-backend ir` and `--sh2-jit --jit-backend x64` back to back. The table shows the run with the median SH2 total per mode; the last two columns give the SH2 totals and max frame times of all three runs. All 63 runs exited 0; every x64 run reported `compileFallbacks 0` on both CPUs. Content: US BIOS for the BIOS menu, Panzer Dragoon II Zwei, Sega Rally, Burning Rangers and Guardian Heroes; JP BIOS (v1.01) for Virtua Fighter 2 and Street Fighter Zero 3.

**Load.** The development PC read 1-9% CPU load before and between batches. The heaviest processes were the usual desktop applications (qBittorrent, Slack, an idle OneDrive, Spotify, Search indexer); nothing else heavy ran during a timed run (the 36,000-frame lockstep runs finished before the first timed run, and the Raspberry Pi work is on another machine). Unlike the Task 5-6 sessions (30-40% slower), this session was as fast as the 2D baseline session: the interpreter totals agree with the baseline within 1% (PD2 4.923 against 4.918, SFZ3 4.524 against 4.553, BR 6.326 against 6.318, GH 3.964 against 3.939), so this session's x64 totals can be compared with the baseline's too (below). Only same-session ratios are used for the verdicts.

| Title | Mode | ms/frame avg | p95 | max | SH2 master ms (%) | SH2 slave ms (%) | SH2 total ms (%) | SH2 total, 3 runs | max frame ms, 3 runs |
|---|---|---|---|---|---|---|---|---|---|
| BIOS menu (US) | interpreter | 7.817 | 8.203 | 8.7 | 1.686 (21.6%) | 0.000 (0.0%) | 1.686 (21.6%) | 1.686 / 1.675 / 1.690 | 8.7 / 9.2 / 8.7 |
| | IR | 8.559 | 8.943 | 10.5 | 5.964 (69.7%) | 0.000 (0.0%) | 5.964 (69.7%) | 5.924 / 5.964 / 5.970 | 9.7 / 10.5 / 10.8 |
| | x64 | 7.865 | 8.547 | 10.4 | 1.221 (15.5%) | 0.000 (0.0%) | 1.221 (15.5%) | 1.218 / 1.228 / 1.221 | 10.3 / 10.4 / 10.4 |
| Virtua Fighter 2 (JP) | interpreter | 6.166 | 7.553 | 10.9 | 2.784 (45.2%) | 1.649 (26.7%) | 4.433 (71.9%) | 4.643 / 4.433 / 4.433 | 10.3 / 8.5 / 10.9 |
| | IR | 18.828 | 23.119 | 24.3 | 9.878 (52.5%) | 7.039 (37.4%) | 16.917 (89.9%) | 16.878 / 16.964 / 16.917 | 23.8 / 24.1 / 24.3 |
| | x64 | 5.622 | 6.399 | 7.6 | 2.254 (40.1%) | 1.126 (20.0%) | 3.380 (60.1%) | 3.380 / 3.385 / 3.380 | 8.7 / 9.0 / 7.6 |
| Panzer Dragoon II Zwei (US) | interpreter | 6.400 | 7.484 | 8.5 | 3.182 (49.7%) | 1.741 (27.2%) | 4.923 (76.9%) | 4.923 / 4.903 / 4.939 | 8.5 / 8.4 / 13.2 |
| | IR | 16.960 | 18.895 | 20.2 | 9.913 (58.5%) | 5.105 (30.1%) | 15.018 (88.6%) | 15.018 / 15.005 / 15.036 | 20.2 / 19.7 / 20.9 |
| | x64 | 5.897 | 7.399 | 9.0 | 2.834 (48.1%) | 1.353 (23.0%) | 4.187 (71.0%) | 4.167 / 4.190 / 4.187 | 12.0 / 8.5 / 9.0 |
| Sega Rally Championship (US) | interpreter | 7.158 | 8.315 | 10.0 | 2.600 (36.3%) | 2.034 (28.4%) | 4.634 (64.7%) | 4.654 / 4.602 / 4.634 | 9.6 / 8.9 / 10.0 |
| | IR | 17.335 | 25.104 | 28.0 | 8.953 (51.6%) | 6.359 (36.7%) | 15.312 (88.3%) | 15.316 / 15.221 / 15.312 | 27.3 / 27.7 / 28.0 |
| | x64 | 7.653 | 9.516 | 10.6 | 2.464 (32.2%) | 2.185 (28.6%) | 4.650 (60.8%) | 4.679 / 4.640 / 4.650 | 11.0 / 10.8 / 10.6 |
| Burning Rangers (US) | interpreter | 8.691 | 9.879 | 13.7 | 4.255 (49.0%) | 2.071 (23.8%) | 6.326 (72.8%) | 6.407 / 6.260 / 6.326 | 13.9 / 12.6 / 13.7 |
| | IR | 22.640 | 23.718 | 31.3 | 13.260 (58.6%) | 6.663 (29.4%) | 19.923 (88.0%) | 19.960 / 19.923 / 19.858 | 30.4 / 31.3 / 24.9 |
| | x64 | 8.707 | 10.460 | 14.0 | 4.401 (50.5%) | 1.624 (18.7%) | 6.025 (69.2%) | 6.040 / 6.016 / 6.025 | 12.3 / 12.7 / 14.0 |
| Guardian Heroes (US) | interpreter | 6.140 | 7.056 | 8.0 | 2.466 (40.2%) | 1.498 (24.4%) | 3.964 (64.6%) | 3.964 / 3.950 / 3.982 | 8.0 / 10.6 / 8.7 |
| | IR | 14.982 | 18.491 | 19.8 | 7.933 (53.0%) | 4.696 (31.3%) | 12.629 (84.3%) | 12.603 / 12.672 / 12.629 | 20.2 / 19.9 / 19.8 |
| | x64 | 6.008 | 6.742 | 7.7 | 1.987 (33.1%) | 1.328 (22.1%) | 3.315 (55.2%) | 3.323 / 3.315 / 3.313 | 8.5 / 7.7 / 7.9 |
| Street Fighter Zero 3 (JP, 4 MB cart) | interpreter | 8.876 | 9.493 | 11.1 | 2.639 (29.7%) | 1.885 (21.2%) | 4.524 (51.0%) | 4.531 / 4.512 / 4.524 | 11.2 / 12.1 / 11.1 |
| | IR | 17.084 | 20.339 | 27.3 | 9.312 (54.5%) | 5.774 (33.8%) | 15.086 (88.3%) | 15.076 / 15.086 / 15.256 | 30.1 / 27.3 / 26.9 |
| | x64 | 9.561 | 11.391 | 24.0 | 3.372 (35.3%) | 1.305 (13.7%) | 4.677 (48.9%) | 4.677 / 4.642 / 4.706 | 24.0 / 23.2 / 23.2 |

### Ratios (SH2 total, median runs)

| Title | interpreter / x64 (bar: at least 1.5) | 2D baseline | 2C | IR / x64 | interpreter / IR |
|---|---|---|---|---|---|
| BIOS menu | 1.381 | 1.358 | 1.231 | 4.885 | 0.283 |
| Virtua Fighter 2 | **1.312** | 1.265 | 1.253 | 5.005 | 0.262 |
| Panzer Dragoon II Zwei | **1.176** | 1.126 | 1.087 | 3.587 | 0.328 |
| Sega Rally Championship | **0.997** | 0.977 | 0.977 | 3.293 | 0.303 |
| Burning Rangers | **1.050** | 0.984 | 0.970 | 3.307 | 0.318 |
| Guardian Heroes | **1.196** | 1.121 | 1.106 | 3.810 | 0.314 |
| Street Fighter Zero 3 | **0.967** | 0.925 | 0.912 | 3.226 | 0.300 |

The BIOS menu is not one of the six titles. The x64 SH2 totals against the 2D baseline (interpreters unchanged within 1%): Virtua Fighter 2 3.525 to 3.380 (-4.1%), Panzer Dragoon II Zwei 4.369 to 4.187 (-4.2%), Sega Rally 4.788 to 4.650 (-2.9%), Burning Rangers 6.422 to 6.025 (-6.2%), Guardian Heroes 3.513 to 3.315 (-5.6%), Street Fighter Zero 3 4.924 to 4.677 (-5.0%), BIOS menu 1.227 to 1.221 (-0.5%). The ratios rose by 0.02-0.08. The per-task rows are in "2D progress" above: B1 gave 0.6-1.4%, B3 2.1% on Burning Rangers (the title with 44.8M `Div1` calls), B4 1.1-2.7% on BR and PD2, B2 about 1-2% on SFZ3 and VF2, and B5 changed the ratio by nothing measurable (it cut Burning Rangers' compiles instead, see below). The IR backend's ratio to the interpreter (0.26-0.33) is within about 6% of the baseline's (0.27-0.33), as expected for a series of changes that targets the x64 backend; `OptimizeBlock` rule 5 and the constant-address ops are the only changes the IR backend runs, and its lockstep is identical.

### Executor-only ratio

The 2C re-profile method, level 1 only: a temporary instrumented `brimir_bench` (a separate build directory, `build-instr`, same options as `build-bench`; the patch is not committed, and `build-bench` was rebuilt from the clean sources afterwards) that adds one `rdtsc` interval around each `Executor::Run` and prints its total over the measured window. Panzer Dragoon II Zwei, Street Fighter Zero 3 and Burning Rangers, x64, `--warmup 2400 --frames 1800`, three rounds (nine runs, all exit 0, `jit window` counters identical to the uninstrumented runs). Values are means of the three runs (spread within 1%). The instrumentation costs about 0.3 ms/frame on the SH2 total (instrumented 4.50, 4.96 and 6.33 against 4.19, 4.68 and 6.03 uninstrumented), part of which falls inside the measured interval, so the outside-`Run` figure is an estimate (about 0.1 ms). The executor time without instrumentation is estimated as the uninstrumented x64 SH2 total of the table above minus the outside-`Run` time of the instrumented runs (SH2 total minus `Run`); the executor-only ratio is (interpreter SH2 total minus the same outside time) / x64 executor time.

| ms/frame (both CPUs) | Panzer Dragoon II Zwei | Street Fighter Zero 3 | Burning Rangers |
|---|---|---|---|
| Interpreter SH2 total (table above) | 4.923 | 4.524 | 6.326 |
| x64 SH2 total, uninstrumented (table above) | 4.187 | 4.677 | 6.025 |
| Instrumented x64 SH2 total (3 runs) | 4.496 / 4.496 / 4.507 | 4.939 / 4.974 / 4.974 | 6.345 / 6.322 / 6.333 |
| `Executor::Run` (3 runs) | 3.884 / 3.884 / 3.890 | 4.344 / 4.383 / 4.383 | 5.725 / 5.705 / 5.714 |
| Outside `Executor::Run` (SH2 total - Run) | 0.614 | 0.592 | 0.619 |
| `Executor::Run`, uninstrumented estimate | **3.573** | **4.085** | **5.406** |
| Interpreter executor time (interpreter total - outside) | 4.309 | 3.932 | 5.707 |
| **Executor-only ratio** | **1.206** | **0.963** | **1.056** |
| SH2-total ratio (table above) | 1.176 | 0.967 | 1.050 |
| `Executor::Run` calls per window (both CPUs) | 55.56M | 56.01M | 53.39M |
| Compile time in the window (`jit window`, inside `Run`) | 0.066 (118.6 ms in 1,800 frames) | 0.631 (1,136 ms) | 0.083 (148.5 ms) |
| **Executor budget for the bar**: interpreter total / 1.5 - outside | 2.67 | 2.42 | 3.60 |
| Shrink still needed (estimate to budget) | 25% | 41% | 33% |

- The time outside `Executor::Run` fell from about 1.09 ms/frame (2C re-profile) to 0.59-0.62 ms/frame: Step 0 removed the host-time clock reads (about 0.64 ms/frame, in both modes). The executor-only ratios are close to the SH2-total ratios now (1.206 against 1.176, 0.963 against 0.967, 1.056 against 1.050), because the outside time is small.
- The x64 executor estimate fell from 3.85 / 4.34 / 5.80 ms/frame (2C; PD2 / SFZ3 / BR, a differently loaded session) to 3.57 / 4.09 / 5.41 (-7%, -6%, -7%); the executor is still 25-41% above the budget for 1.5x.
- Street Fighter Zero 3: compiling is 0.63 ms/frame (15% of the executor, 13% of the x64 SH2 total). With it free the SH2-total ratio would be about 1.12 (executor-only about 1.14), so compile cost alone does not reach the bar.

### Max frames against the spike rule

Each x64 run's max frame time must be at most 10% above the highest interpreter max frame of the same session and window (design section 3, item 3). BIOS menu listed for reference only.

| Title | Interpreter max, 3 runs (highest) | Limit (+10%) | x64 max, 3 runs | Rule |
|---|---|---|---|---|
| Virtua Fighter 2 | 10.3 / 8.5 / 10.9 (10.9) | 12.0 | 8.7 / 9.0 / 7.6 | pass |
| Panzer Dragoon II Zwei | 8.5 / 8.4 / 13.2 (13.2) | 14.5 | 12.0 / 8.5 / 9.0 | pass |
| Sega Rally Championship | 9.6 / 8.9 / 10.0 (10.0) | 11.0 | 11.0 / 10.8 / 10.6 | pass (the highest, 10.956 ms, is 0.04 ms under the limit of 10.997) |
| Burning Rangers | 13.9 / 12.6 / 13.7 (13.9) | 15.3 | 12.3 / 12.7 / 14.0 | pass |
| Guardian Heroes | 8.0 / 10.6 / 8.7 (10.6) | 11.7 | 8.5 / 7.7 / 7.9 | pass |
| **Street Fighter Zero 3** | 11.2 / 12.1 / 11.1 (12.1) | 13.3 | **24.0 / 23.2 / 23.2** | **FAIL** |
| BIOS menu (reference) | 8.7 / 9.2 / 8.7 (9.2) | 10.1 | 10.3 / 10.4 / 10.4 | over by 0.2-0.3 ms (not a bar title) |
| **Burning Rangers, long run** | 15.9 (one run) | 17.5 | **130.4** | **FAIL** |

Street Fighter Zero 3's slowest frames are compile bursts (the window builds 10,686 blocks and compiles 6,333 natively, about 1.1 s, in 1,090 of 1,800 frames; the same counts as in B5's threshold-16 run); its x64 max fell from 26-29 ms (2C and the baseline) to 23-24 ms, still about 2x the interpreter's.

### Long run: Burning Rangers, frames 2,400-36,000

`--warmup 2400 --frames 33600`, one run per mode, default settings (threaded VDP, host RTC), measured window = frames 2,400-36,000, the same session as above (interpreter first, then x64). Both exited 0.

| Burning Rangers, frames 2,400-36,000 | ms/frame avg | p95 | p99 | max | SH2 total ms/frame |
|---|---|---|---|---|---|
| interpreter | 9.159 | 11.201 | 12.643 | 15.905 | 5.937 |
| x64 | 11.725 | 27.413 | 42.695 | 130.419 | 7.975 (ratio **0.744**) |
| 2C, for reference (another session) | 9.856 / 15.585 | 12.282 / 49.786 | 13.495 / 86.367 | 16.879 / 173.340 | 6.369 / 11.650 (ratio 0.55) |

- In the window the two CPUs built 2,119,633 blocks and compiled 183,302 natively (2C: 2,244,425 and 492,237): 71.7 s of compiling, 2.13 ms/frame on average (2C: 167.8 s, 5.0 ms/frame), in 12,427 of 33,600 frames (2C: 13,500). Native code-cap flushes: 4 on the master and 3 on the slave (2C: 19 in the window); no IR-cap eviction. The 10 flush events counted in the window include flushes the core requests.
- The five slowest frames (94-130 ms) are all compile bursts: frame 13,608 (130.4 ms: 1,060 native compiles, 113.7 ms of compiling), 24,638 (126.1 ms, 1,083 compiles, 112.5 ms), 10,572 (119.7 ms, 867, 102.4 ms), 24,593 (96.1 ms, 3,541 builds plus 428 native compiles, 79.3 ms) and 10,527 (94.5 ms, 3,182 builds, 435 native compiles, 74.9 ms); none had a flush.
- B5's threshold of 16 (and B1-B4) moved the long run from 0.55 to 0.74 on SH-2 time and the worst frame from 173 to 130 ms, but compile bursts still cost 75-114 ms in a frame against the interpreter's 15.9 ms worst frame. The code that is rewritten constantly (about 884k master and 1.1M slave invalidations per 36,000 frames, 25-31 per frame per CPU) still sends 183k blocks per window to native compilation, so this is the track A problem (reuse of returning code, individual eviction, damping at unstable PCs).

### Default-on bar (design section 3), condition by condition

1. **Speed (at least 1.5 on each of the six titles): FAIL.** 1.312 (Virtua Fighter 2), 1.196 (Guardian Heroes), 1.176 (Panzer Dragoon II Zwei), 1.050 (Burning Rangers), 0.997 (Sega Rally), 0.967 (Street Fighter Zero 3); the best is 0.19 short, the worst 0.53. Executor-only: 1.206, 0.963, 1.056 on PD2, SFZ3, BR.
2. **Long session (Burning Rangers over frames 2,400-36,000, x64 SH-2 total at most the interpreter's): FAIL.** Ratio 0.744 (x64 7.975 against 5.937 ms/frame). Expected to fail until track A.
3. **Spikes (x64 max at most 10% above the highest interpreter max in every window): FAIL.** Five of the six 1,800-frame windows pass (Sega Rally with 0.04 ms to spare); Street Fighter Zero 3 fails (23.2-24.0 ms against the limit 13.3) and so does the long run (130.4 ms against 17.5).
4. **Exactness: holds.** Lockstep identical for 36,000 frames on x64 on the BIOS menu and all six titles and 1,800 frames on x64 and IR (`design/sh2-validation.md`, "Milestone 2D, track B"); `[jit]` tests on x64 and IR and ctest pass on Windows x64; the Raspberry Pi's IR lockstep and test suite pass. CI on the measured commit 2700404 (GitHub Actions run 38852417861: Windows, Linux x64, macOS ARM64) is green.

Not met: conditions 1, 2 and 3. The JIT stays off by default; the README and the option label are unchanged.

### What track B left (for the track A plan)

- Speed: the executor must still shrink by 25% (PD2), 33% (BR) and 41% (SFZ3) for 1.5x. Track B's per-instruction changes gave 3-6% in total, so a further 25-41% is unlikely to come from more of the same; Street Fighter Zero 3's executor-only ratio is 0.96 (about 1.14 without its compiling). The remaining executor time is generated code and the dispatch between blocks (per-block `Get`/`Step`, trampolines), which these items did not target; a profile of generated code per block (not done here) should come before more per-instruction work, and the design's 1.5 bar may need a different lever (shared code outside the executor is 0.6 ms/frame, track C).
- Long sessions (track A, the next item): Burning Rangers' 183k native compiles per window and the 75-114 ms compile bursts in its slowest frames are the first target (A1 reuse of returning code, A2 individual eviction, A3 damping), with the long run (`--warmup 2400 --frames 33600`) as the main measurement; Street Fighter Zero 3's 6,333 compiles in its window (about 1.1 s, the source of its 23-24 ms frames) are the second.

## 2D track A baseline

**Date**: 2026-10-19
**Machine and build**: as in the 2D baseline above (AMD Ryzen 7 5700G, Ninja, Release, `BRIMIR_LTO=ON`, `Brimir_ENABLE_IPO=ON`, MSVC 19.44), `build-instr` for the returning-code check, `build-bench` (rebuilt from the clean sources afterwards) for everything else.

New counters (`BlockCache::PeakNativeCodeBytes`, `BlockCache::StaleNativeBytes`; `brimir_bench` prints `peakNativeMB` and `staleNativeMB` per CPU on the `cache` line and `peakNativeMB` per CPU on the `jit window` line). Peak is the most native code the backend held after any native compile since the cache was created (flushes do not lower it); stale is the native code of blocks dropped by `Invalidate`, which stays allocated until the next flush. Both count since initialization, warmup included.

### Returning-code check

A temporary instrumented `brimir_bench` (a separate build directory, `build-instr`, same options as `build-bench`; the patch is not committed, and `build-bench` was rebuilt from the clean sources afterwards). `BlockCache::Invalidate` records a 64-bit hash of a dropped native block's `guestOpcodes` per PC (at most 4 per PC, oldest replaced; cleared on a flush, as a pool would be when the code is reset); `Build` counts a build as a potential reuse when its PC and opcode hash match a recorded one (and consumes that record). Whether the code pages would still match is not checked, so the count is an upper bound for a pool without a bound on its size. One x64 run each, default settings, scratch system dir as in the method above, `--warmup 2400 --frames 33600` (Burning Rangers, US BIOS) and `--warmup 2400 --frames 1800` (Street Fighter Zero 3, JP BIOS). Both exited 0; the window's `compiles`, `nativeCompiles` and `flushes` equal the uninstrumented track B runs above (Burning Rangers 2,119,633 / 183,302 / 10; Street Fighter Zero 3 10,686 / 6,333 / 0), so the instrumentation did not change what the cache does.

| Window | CPU | Invalidations | Builds | Native compiles | Potential reuses | Share of native compiles |
|---|---|---|---|---|---|---|
| Burning Rangers, frames 2,400-36,000 | master | 883,698 | 998,190 | 118,067 | 43,375 | 36.7% |
| | slave | 1,107,082 | 1,121,443 | 65,235 | 51,690 | 79.2% |
| | **both** | 1,990,780 | 2,119,633 | 183,302 | **95,065** | **51.9%** |
| Street Fighter Zero 3, frames 2,400-4,200 | master | 292 | 10,686 | 6,333 | 0 | 0% |
| | slave | 0 | 0 | 0 | 0 | |

Burning Rangers: about half of the native compiles in the window (95,065 of 183,302) rebuild code that was dropped earlier with the same opcodes at the same PC, so A1 clears the 10% bar (design section 7) by a wide margin; on the slave it is four in five. Street Fighter Zero 3 has nothing to reuse (its master drops 292 blocks in the window; its 6,333 compiles are first compiles), so A1 does not help it, and its compile bursts are for A3 or the compile threshold.

### Native code held

| Run | CPU | peakNativeMB | staleNativeMB (since init) | Code-cap flushes (since init) |
|---|---|---|---|---|
| Burning Rangers (to frame 36,000) | master | 128.0 | 337.8 | 4 |
| | slave | 128.0 | 411.2 | 3 |
| Street Fighter Zero 3 (to frame 4,200) | master | 48.1 | 1.1 | 0 |
| | slave | 2.0 | 0.0 | 0 |

Both Burning Rangers CPUs run into the 128 MB cap, and 337.8 MB (master) and 411.2 MB (slave) of the native code they generated since start-up was stale by the time of the end of the run: 2.6 to 3.2 times the cap, which A2 (free a dropped block's code at once) would return to the allocator instead of holding it until a flush. The window's `flushes` (10) also counts flushes the core requests.

Timing of the instrumented runs is not comparable with the uninstrumented ones (Burning Rangers: SH2 total 8.598 ms/frame, average frame 12.584 ms, max 144.4 ms; Street Fighter Zero 3: SH2 total 5.015 ms/frame, max 25.3 ms); only the counters are used.

## 2D track A progress

Measured at the end of each track A task (`design/plans/2026-10-19-sh2-jit-m2d-track-a.md`) on the machine and build configuration above. Burning Rangers (US BIOS): `--warmup 2400 --frames 33600`, one x64 run, plus one interpreter run per session. Street Fighter Zero 3 (JP BIOS) and Panzer Dragoon II Zwei (US BIOS): `--warmup 2400 --frames 1800`, interpreter and x64 back to back, three rounds (medians, as in "2D progress"). Max frame is the highest of the interpreter runs in the session; native compiles, compile ms, flushes and peak native code are the x64 run's `jit window` values (flushes: master / slave from the `cache` line; peak native MB: master / slave).

| Task | Title | Interpreter SH2 total ms | x64 SH2 total ms | Ratio (interp / x64) | x64 avg ms | x64 p99 ms | x64 max frame ms | Interpreter max frame ms | Native compiles in window | Compile ms in window | Code-cap flushes (master / slave) | Peak native MB (master / slave) | Retired reuses |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A2 (free stale code) | Burning Rangers (US), frames 2,400-36,000 | 5.953 | 8.033 | 0.74 | 11.921 | 43.624 | 91.5 | 22.4 | 123,260 | 63,905 | 0 / 0 | 70.5 / 6.4 | n/a |
| A2 (free stale code) | Street Fighter Zero 3 (JP), frames 2,400-4,200 | 4.638 | 4.909 | 0.94 | 9.909 | 15.951 | 25.3 | 17.4 | 6,333 | 1,201 | 0 / 0 | 47.0 / 2.0 | n/a |
| A2 (free stale code) | Panzer Dragoon II Zwei (US), frames 2,400-4,200 | 4.925 | 4.352 | 1.13 | 6.154 | 8.753 | 11.7 | 12.4 | 660 | 128 | 0 / 0 | 28.1 / 0.0 | n/a |
| A1 (reuse returning code) | Burning Rangers (US), frames 2,400-36,000 | 6.285 | 6.652 | 0.94 | 10.508 | 24.234 | 92.4 | 28.7 | 42,871 | 23,798 | 0 / 0 | 74.0 / 6.6 | 885,877 |
| A1 (reuse returning code) | Street Fighter Zero 3 (JP), frames 2,400-4,200 | 4.505 | 4.712 | 0.96 | 9.706 | 15.278 | 22.9 | 12.8 | 6,333 | 1,142 | 0 / 0 | 48.1 / 2.0 | 0 |
| A1 (reuse returning code) | Panzer Dragoon II Zwei (US), frames 2,400-4,200 | 4.909 | 4.285 | 1.15 | 6.013 | 8.172 | 8.8 | 16.8 | 660 | 119 | 0 / 0 | 28.3 / 0.0 | 0 |
| A3 (damping, shift 0 = off, committed) | Burning Rangers (US), frames 2,400-36,000 | 6.118 | 6.540 | 0.94 | 10.378 | 23.864 | 89.3 | 23.4 | 42,871 | 23,663 | 0 / 0 | 74.0 / 6.6 | 885,877 |
| A3 (damping, shift 0 = off, committed) | Street Fighter Zero 3 (JP), frames 2,400-4,200 | 4.743 | 4.824 | 0.98 | 9.675 | 15.137 | 23.1 | 14.2 | 6,333 | 1,178 | 0 / 0 | 48.1 / 2.0 | 0 |
| A3 (damping, shift 0 = off, committed) | Panzer Dragoon II Zwei (US), frames 2,400-4,200 | 5.091 | 4.222 | 1.21 | 5.980 | 8.059 | 9.1 | 11.9 | 660 | 119 | 0 / 0 | 28.3 / 0.0 | 0 |
| A3 (damping, shift 2, measured) | Burning Rangers (US), frames 2,400-36,000 | 6.118 | 6.402 | 0.96 | 10.248 | 21.324 | 85.3 | 23.4 | 27,051 | 18,314 | 0 / 0 | 74.0 / 6.5 | 513,991 |
| A3 (damping, shift 2, measured) | Street Fighter Zero 3 (JP), frames 2,400-4,200 | 4.743 | 4.888 | 0.97 | 9.823 | 15.305 | 22.3 | 14.2 | 5,794 | 1,170 | 0 / 0 | 46.5 / 2.0 | 0 |
| A3 (damping, shift 2, measured) | Panzer Dragoon II Zwei (US), frames 2,400-4,200 | 5.091 | 4.240 | 1.20 | 6.003 | 8.098 | 8.9 | 11.9 | 660 | 124 | 0 / 0 | 28.2 / 0.0 | 0 |
| A3 (damping, shift 4, measured) | Burning Rangers (US), frames 2,400-36,000 | 6.118 | 6.571 | 0.93 | 10.468 | 22.576 | 86.5 | 23.4 | 26,972 | 20,853 | 0 / 0 | 74.0 / 6.4 | 161,606 |
| A3 (damping, shift 4, measured) | Street Fighter Zero 3 (JP), frames 2,400-4,200 | 4.743 | 4.731 | 1.00 | 9.585 | 15.378 | 21.9 | 14.2 | 5,794 | 1,136 | 0 / 0 | 46.5 / 2.0 | 0 |
| A3 (damping, shift 4, measured) | Panzer Dragoon II Zwei (US), frames 2,400-4,200 | 5.091 | 4.350 | 1.17 | 6.085 | 8.105 | 9.4 | 11.9 | 660 | 120 | 0 / 0 | 28.2 / 0.0 | 0 |
| A4 (no flush on state load) | Burning Rangers (US), frames 2,400-36,000 | 5.976 | 6.699 | 0.89 | 10.565 | 24.444 | 96.0 | 28.1 | 42,871 | 23,635 | 0 / 0 | 74.0 / 6.6 | 885,877 |
| A4 (no flush on state load) | Street Fighter Zero 3 (JP), frames 2,400-4,200 | 4.585 | 4.890 | 0.94 | 9.912 | 15.514 | 23.4 | 13.2 | 6,333 | 1,189 | 0 / 0 | 48.1 / 2.0 | 0 |
| A4 (no flush on state load) | Panzer Dragoon II Zwei (US), frames 2,400-4,200 | 4.988 | 4.328 | 1.15 | 6.112 | 8.212 | 9.0 | 10.6 | 660 | 119 | 0 / 0 | 28.3 / 0.0 | 0 |

A2 (task 2): `Invalidate` unpublishes a dropped native block and frees its code at once (`INativeBackend::Release`). Burning Rangers ran with **no code-cap flushes** (track B: 4 master / 3 slave; the `flushes 3` on the `jit window` line are core-requested flushes) and a peak of 70.5 MB (master) and 6.4 MB (slave), against the 128 MB cap (track B peak: 128 / 128). Stale native code over the run is still 339.3 MB (master) and 411.2 MB (slave) in total, but it no longer accumulates. Native compiles in the window fell from 183,302 (track B) to 123,260 because a flush no longer throws away blocks that are still valid, compile time from 71.7 s to 63.9 s, and the worst frame from 130.4 to 91.5 ms. The SH-2 total ratio stays at 0.74 (track B: 0.744; x64 SH-2 total 8.033 against 7.975 ms/frame, within run-to-run spread): A2's gain on this run is memory, compile work and the worst frame, not SH-2 time. Street Fighter Zero 3 and Panzer Dragoon II Zwei are in line with the 2D track B results (medians of three rounds; interpreter max frame is the highest of the three rounds).

A1 (task 3): `Invalidate` retires a dropped self-validating native block (unpublished, code kept) into a per-CPU pool (at most `kMaxRetiredPerPc` = 4 per PC, `kMaxRetiredCodeBytes` = 32 MB in total, oldest released first); `Build` takes one back, without a front-end build or a native compile, when its `guestOpcodes` equal memory and its two code pages still have the arrays it was compiled on (`CachedBlock::codePages`, what the prologue checks). Burning Rangers: **885,877 retired reuses** in the window (master 460,161, slave 425,716 since start-up), against Task 1's 95,065 potential reuses; the counts differ because a reused block is native at once, so its next drop is retired again, while in Task 1 a rebuilt block spent 16 runs on IR and an IR-only drop left nothing to reuse. Native compiles in the window fell from 123,260 (A2) to 42,871, compile time from 63.9 s to 23.8 s, the x64 SH-2 total from 8.033 to 6.652 ms/frame and p99 from 43.6 to 24.2 ms; the ratio went from 0.74 to 0.94 (this session's interpreter run: 6.285 ms/frame, against 5.953 in the A2 session). No code-cap flushes; peak native code 74.0 / 6.6 MB (the pool held 3.4 MB on the master at the end). Pool evictions (15,861 over the run, 7,523 master and 8,338 slave, both bounds together) are under 2% of the about 902,643 retirements (the stale entries, each of which drops one native block), so the 64 MB variant was not measured. Street Fighter Zero 3 and Panzer Dragoon II Zwei have nothing to reuse (0 reuses) and stay in line with A2 (medians of three rounds).

A3 (task 4): rows above are from the 2D A3 session (next section); the shift 0 rows are the committed configuration. Interpreter columns are the session's (the Burning Rangers run is one; Street Fighter Zero 3 and Panzer Dragoon II Zwei use the median of the three interpreter runs and their highest max frame).

A4 (task 5): `SH2::LoadState` no longer flushes the executor. The bench windows contain no state loads, so no change is expected here, and none is outside run-to-run spread: Burning Rangers x64 SH-2 total 6.699 ms/frame against 6.540 (A3, shift 0) and 6.652 (A1), the interpreter 5.976 against 6.118, so the ratio reads 0.89 against 0.94 on a x64 run 2.4% slower and an interpreter run 2.3% faster than the earlier sessions; native compiles (42,871) and retired reuses (885,877) are identical to A1 and A3, and no code-cap flushes (peak native code 74.0 / 6.6 MB). The `jit window` `flushes` is 3 for Burning Rangers, the same as in the A1, A2 and A3 runs: those are core-requested flushes (`requested` since start-up is 2 master / 6 slave, attaching the executor accounting for 1 each; the rest are `SH2::Reset` calls, mostly the slave's, not investigated further), and the bench loads no state; Street Fighter Zero 3 and Panzer Dragoon II Zwei have 0. Street Fighter Zero 3 and Panzer Dragoon II Zwei are medians of three interpreter / x64 rounds run back to back (machine load 0-12% before each run) and in line with A3.

## 2D A3

Damping: `BlockCache` counts, per PC, how often a block there was dropped because its guest code changed (`Invalidate`, saturating at 255; `FlushAll` clears the counts). `Build` then waits `kNativeCompileThreshold << min(count, kMaxStaleDampShift)` runs for a native compile instead of `kNativeCompileThreshold` (16), so a PC that keeps being rewritten compiles natively less often; code that then stays unchanged still gets native code after at most `16 << shift` runs. A block taken back from the retired pool (A1) is native at once and ignores the count, and threshold 1 (`kNativeOnFirstRun`: tests, diagnostics) is never damped. `DampedBuilds()` counts the builds whose threshold was raised.

Method: the B5 method (three temporary `build-bench` builds with `kMaxStaleDampShift` = 0, 2 and 4, executables copied aside and run interleaved in one session, 2026-10-19). Street Fighter Zero 3 (JP BIOS) and Panzer Dragoon II Zwei (US BIOS): `--warmup 2400 --frames 1800`, x64, three rounds; each round ran one interpreter run, then the three values (SFZ3 first, then PD2), the order of the values rotating between rounds (0, 2, 4 / 2, 4, 0 / 4, 0, 2). Burning Rangers (US): `--warmup 2400 --frames 33600`, one interpreter run (round 1) and one x64 run per value, after the short runs of rounds 1, 2 and 3 (shift 0, 2, 4 respectively). Scratch system dir as in the method above. All 28 runs exited 0; machine load before and between runs 4-26% (desktop applications only).

SH2 total ms/frame, x64 (median of three, the runs in order of the rounds in parentheses):

| Title | Interpreter | shift 0 (off) | shift 2 | shift 4 |
|---|---|---|---|---|
| Street Fighter Zero 3 | 4.743 (4.743 / 4.702 / 4.804) | 4.824 (4.893 / 4.824 / 4.807) | 4.888 (4.857 / 4.899 / 4.888) | **4.731** (4.896 / 4.725 / 4.731) |
| Panzer Dragoon II Zwei | 5.091 (5.037 / 5.116 / 5.091) | **4.222** (4.359 / 4.221 / 4.222) | 4.240 (4.304 / 4.210 / 4.240) | 4.350 (4.548 / 4.234 / 4.350) |

Burning Rangers, one run each (the interpreter: SH2 total 6.118, average 9.383, p99 13.2, max frame 23.4 ms):

| Value | SH2 total ms | avg frame ms | p99 | max frame ms | native compiles in window | front-end builds in window | compile ms in window | retired reuses | damped builds (master / slave) |
|---|---|---|---|---|---|---|---|---|---|
| shift 0 (off) | 6.540 | 10.378 | 23.9 | 89.3 | 42,871 | 1,149,860 | 23,663 | 885,877 | 0 / 0 |
| shift 2 | 6.402 | 10.248 | 21.3 | 85.3 | 27,051 | 1,523,209 | 18,314 | 513,991 | 685,596 / 792,274 |
| shift 4 | 6.571 | 10.468 | 22.6 | 86.5 | 26,972 | 1,875,592 | 20,853 | 161,606 | 854,852 / 975,404 |

(Compile ms is front-end builds plus native compiles. All runs: no code-cap flushes, peak native code 74.0 / 6.4-6.6 MB, 884,4xx master and 1,107,130 slave invalidations over the run.)

### Decision (the rule)

1. Among the values whose SFZ3 and PD2 SH2 totals are within 2% of the best (medians): SFZ3 best is shift 4 (4.731): shift 0 is +2.0% (4.824, inside the 2% line at 4.826 by 0.002 ms), shift 2 +3.3% (outside). PD2 best is shift 0 (4.222): shift 2 +0.4%, shift 4 +3.0% (outside). Only **shift 0** is eligible on both titles. On the means of the three runs the answer is the same (SFZ3: 4.841 / 4.881 / 4.784, PD2: 4.267 / 4.251 / 4.377; shift 2 is +2.0% on SFZ3 by a hair and shift 4 +3.0% on PD2).
2. The tie-breaker is not needed (one eligible value).

**Chosen value: 0 (damping off).** The code stays, behind `kMaxStaleDampShift = 0`.

### Notes

- The short titles do not separate the values: shifts 2 and 4 made identical native compile counts on SFZ3 (5,794, 1,145 damped builds) and PD2 (660), so their 3.3% difference on SFZ3 (4.888 against 4.731) is run-to-run noise, which is as large as the rule's 2% band. The rule's outcome on the SFZ3 column is therefore not a measured cost of damping.
- Burning Rangers: damping at shift 2 cut native compiles by 37% (42,871 to 27,051), compile time by 23% (23.7 to 18.3 s) and p99 by 11% (23.9 to 21.3 ms); the SH2 total fell 2.1% (6.540 to 6.402), which is inside the run-to-run spread (the same code at shift 0 measured 6.652 in the A1 session and 6.540 here, 1.7% apart), and the max frame 4% (89.3 to 85.3 ms). Shift 4 compiled the same number of blocks as shift 2 but is slower. None of the values is within reach of the interpreter on this title (6.118 ms/frame, max 23.4 ms).
- The cost of damping is on the other side: a damped block stays on IR for 31 or 63 runs, is never retired, and so cannot be reused. Reuses fell from 885,877 to 513,991 (shift 2) and 161,606 (shift 4), and front-end builds in the window rose from 1.15 M to 1.52 M and 1.88 M, which offsets most of the saved native compile time.
- Not measured: a damping rule that keeps reuse (counting only drops of blocks that never reached native code, or letting a retired block's reuse restore a lower count).

## Milestone 2D track A results

**Date**: 2026-10-19/20
**Commit**: e8adf69 (the measured code: the head of `feature/sh2-jit-2d-a` before the commit that adds this section; the track A commits are 1109f1f counters, 9eb9867 A2, cabae91 A1, d88b8f7 A3 (left off), e8adf69 A4)
**Machine and build**: as in the 2D baseline above (AMD Ryzen 7 5700G, `build-bench`, Ninja, Release, `BRIMIR_LTO=ON`, `Brimir_ENABLE_IPO=ON`, MSVC), rebuilt at the measured commit; the measured executable was copied aside before the runs.

**Verdict: the default-on bar is not met.** Track A did what it was built for: the long Burning Rangers run no longer flushes (0 code-cap flushes against 7, peak native code 74.0 / 6.6 MB against 128 / 128), its compile time fell from 71.7 s to 24.0 s, the SH-2 ratio rose from 0.744 to 0.882 and the worst frame fell from 130.4 to 92.1 ms. It does not reach the bar: the interpreter / x64 ratio is 0.94-1.27 on the six titles (bar: at least 1.5 on each), the long run is at 0.882 (bar: at least 1.0), and the spike condition fails on Street Fighter Zero 3 (24.2 ms against a limit of 14.2), on two titles by a fraction of a millisecond (Sega Rally, Burning Rangers) and on the long run (92.1 ms against 18.7). Exactness holds (condition 4). The JIT stays off by default. The six 1,800-frame titles gain nothing from track A (a same-session A/B against the track B code is below): they have almost no code that returns or is rewritten.

### Method

The 2C method, unchanged (and the 2D baseline's and track B's): `--warmup 2400 --frames 1800` for the games and `--warmup 300 --frames 1800` for the BIOS menu, default settings, scratch system dir with `brimir_saturn_rtc_us_eu.smpc` also as `brimir_saturn_rtc_jp.smpc`, three rounds, each round running every title in the interpreter, `--sh2-jit --jit-backend ir` and `--sh2-jit --jit-backend x64` back to back; the table shows the run with the median SH2 total per mode and, in the last two columns, the SH2 totals and max frame times of all three runs. All 63 runs exited 0 and every x64 run reported `compileFallbacks 0` on both CPUs. Content: US BIOS for the BIOS menu, Panzer Dragoon II Zwei, Sega Rally, Burning Rangers and Guardian Heroes; JP BIOS (v1.01) for Virtua Fighter 2 and Street Fighter Zero 3. The long run is `--warmup 2400 --frames 33600` (default settings, threaded VDP, host RTC), one run per mode, interpreter first, then x64, after the table runs; both exited 0.

**Load.** The development PC read 2-3% CPU load before each batch and the usual desktop applications were the only other processes (Spotify, opencode, a terminal); the lockstep runs (7 x64 runs of 36,000 frames and 7 IR runs, four at a time) had finished before the first timed run, the build of the A/B and instrumented executables ran before their timed runs, and the Raspberry Pi work is on another machine. A 30-second sampler of the whole-machine load ran through the timed batches: it reads the benchmark's own threads too (threaded VDP, the emulation thread), so it shows 26% on average over the table runs (2-71%, the peaks mostly in IR runs), 31% over the long runs (2-74%) and 30% in the A/B runs (7-63%); no reading points to an outside load, and the three rounds agree within 1-4% on the SH2 total of every mode except the BIOS menu's third x64 run (1.390 against 1.289 and 1.292); the max frame times vary more. The interpreter totals are 0-3% above track B's session (PD2 4.927 against 4.923, SFZ3 4.667 against 4.524, BR 6.405 against 6.326, GH 4.047 against 3.964), but the x64 totals are 3-7% above track B's (below), so ratios are compared only with this session's own runs.

| Title | Mode | ms/frame avg | p95 | max | SH2 master ms (%) | SH2 slave ms (%) | SH2 total ms (%) | SH2 total, 3 runs | max frame ms, 3 runs |
|---|---|---|---|---|---|---|---|---|---|
| BIOS menu (US) | interpreter | 8.108 | 8.492 | 9.4 | 1.731 (21.4%) | 0.000 (0.0%) | 1.731 (21.4%) | 1.721 / 1.733 / 1.731 | 9.3 / 9.2 / 9.4 |
| | IR | 8.730 | 9.151 | 10.3 | 5.972 (68.4%) | 0.000 (0.0%) | 5.972 (68.4%) | 5.928 / 5.972 / 6.050 | 10.7 / 10.3 / 11.2 |
| | x64 | 8.003 | 8.757 | 10.9 | 1.292 (16.1%) | 0.000 (0.0%) | 1.292 (16.1%) | 1.289 / 1.292 / 1.390 | 10.7 / 10.9 / 11.3 |
| Virtua Fighter 2 (JP) | interpreter | 6.319 | 7.611 | 9.8 | 2.803 (44.4%) | 1.650 (26.1%) | 4.453 (70.5%) | 4.453 / 4.456 / 4.398 | 9.8 / 9.6 / 10.2 |
| | IR | 19.029 | 23.173 | 27.3 | 9.980 (52.5%) | 7.094 (37.3%) | 17.074 (89.7%) | 17.074 / 17.183 / 16.833 | 27.3 / 26.2 / 23.7 |
| | x64 | 5.839 | 6.753 | 8.8 | 2.369 (40.6%) | 1.125 (19.3%) | 3.494 (59.9%) | 3.494 / 3.544 / 3.425 | 8.8 / 8.0 / 10.4 |
| Panzer Dragoon II Zwei (US) | interpreter | 6.501 | 7.627 | 9.4 | 3.170 (48.8%) | 1.757 (27.0%) | 4.927 (75.8%) | 4.892 / 4.976 / 4.927 | 8.7 / 11.3 / 9.4 |
| | IR | 17.264 | 19.429 | 22.1 | 10.181 (59.0%) | 5.059 (29.3%) | 15.240 (88.3%) | 15.201 / 15.240 / 15.452 | 20.6 / 22.1 / 22.2 |
| | x64 | 6.127 | 7.831 | 10.3 | 2.936 (47.9%) | 1.400 (22.8%) | 4.335 (70.8%) | 4.368 / 4.323 / 4.335 | 10.8 / 8.8 / 10.3 |
| Sega Rally Championship (US) | interpreter | 7.314 | 8.455 | 9.7 | 2.630 (36.0%) | 2.057 (28.1%) | 4.687 (64.1%) | 4.701 / 4.687 / 4.668 | 10.7 / 9.7 / 10.8 |
| | IR | 17.693 | 25.846 | 29.0 | 9.132 (51.6%) | 6.517 (36.8%) | 15.649 (88.5%) | 15.553 / 15.674 / 15.649 | 34.1 / 28.4 / 29.0 |
| | x64 | 7.923 | 9.943 | 12.5 | 2.558 (32.3%) | 2.286 (28.9%) | 4.845 (61.2%) | 4.845 / 4.862 / 4.840 | 12.5 / 11.8 / 11.6 |
| Burning Rangers (US) | interpreter | 8.814 | 10.116 | 12.2 | 4.289 (48.7%) | 2.116 (24.0%) | 6.405 (72.7%) | 6.405 / 6.349 / 6.411 | 12.2 / 10.7 / 10.9 |
| | IR | 23.135 | 24.266 | 26.6 | 13.660 (59.0%) | 6.710 (29.0%) | 20.370 (88.1%) | 20.487 / 20.370 / 20.194 | 26.4 / 26.6 / 30.0 |
| | x64 | 9.014 | 10.765 | 13.6 | 4.684 (52.0%) | 1.648 (18.3%) | 6.332 (70.3%) | 6.358 / 6.332 / 6.194 | 12.6 / 13.6 / 12.9 |
| Guardian Heroes (US) | interpreter | 6.308 | 7.246 | 8.7 | 2.500 (39.6%) | 1.547 (24.5%) | 4.047 (64.2%) | 4.047 / 4.052 / 3.967 | 8.7 / 8.8 / 10.8 |
| | IR | 15.121 | 19.007 | 20.9 | 8.037 (53.2%) | 4.661 (30.8%) | 12.698 (84.0%) | 12.764 / 12.698 / 12.466 | 20.4 / 20.9 / 19.3 |
| | x64 | 6.265 | 7.073 | 8.2 | 2.140 (34.2%) | 1.397 (22.3%) | 3.537 (56.5%) | 3.539 / 3.537 / 3.514 | 10.2 / 8.2 / 9.0 |
| Street Fighter Zero 3 (JP, 4 MB cart) | interpreter | 9.056 | 9.750 | 12.1 | 2.725 (30.1%) | 1.942 (21.5%) | 4.667 (51.5%) | 4.667 / 4.679 / 4.649 | 12.1 / 11.6 / 12.9 |
| | IR | 17.569 | 21.171 | 27.9 | 9.731 (55.4%) | 5.774 (32.9%) | 15.504 (88.3%) | 15.504 / 15.464 / 15.930 | 27.9 / 27.5 / 27.9 |
| | x64 | 9.858 | 11.630 | 23.1 | 3.589 (36.4%) | 1.369 (13.9%) | 4.958 (50.3%) | 4.966 / 4.958 / 4.885 | 24.2 / 23.1 / 24.2 |

### Ratios (SH2 total, median runs)

| Title | interpreter / x64 (bar: at least 1.5) | track B | 2D baseline | 2C | IR / x64 | interpreter / IR |
|---|---|---|---|---|---|---|
| BIOS menu | 1.340 | 1.381 | 1.358 | 1.231 | 4.622 | 0.290 |
| Virtua Fighter 2 | **1.274** | 1.312 | 1.265 | 1.253 | 4.887 | 0.261 |
| Panzer Dragoon II Zwei | **1.137** | 1.176 | 1.126 | 1.087 | 3.516 | 0.323 |
| Sega Rally Championship | **0.967** | 0.997 | 0.977 | 0.977 | 3.230 | 0.300 |
| Burning Rangers | **1.012** | 1.050 | 0.984 | 0.970 | 3.217 | 0.314 |
| Guardian Heroes | **1.144** | 1.196 | 1.121 | 1.106 | 3.590 | 0.319 |
| Street Fighter Zero 3 | **0.941** | 0.967 | 0.925 | 0.912 | 3.127 | 0.301 |

The BIOS menu is not one of the six titles. The ratios are 0.03-0.05 below track B's, which is a difference of sessions and not of code: in this session the x64 totals are 3-7% above track B's session record (VF2 3.494 against 3.380, PD2 4.335 against 4.187, Sega Rally 4.845 against 4.650, BR 6.332 against 6.025, GH 3.537 against 3.315, SFZ3 4.958 against 4.677, BIOS menu 1.292 against 1.221) while the interpreters are 0-3% above. The same-session A/B below measures the track B code (550f982, rebuilt) and the track A code (e8adf69) in alternation and finds them equal within about 1%.

### Track A against the track B code, same session

x64 only, `--warmup 2400 --frames 1800`, the six titles, three rounds; each round runs, per title, the executable built from `550f982` (master, the track B code) and the one built from `e8adf69` back to back (18 runs each, all exit 0; `build-bench` options, the `550f982` build in a detached worktree). SH2 total ms/frame (runs in order of the rounds; the median in bold).

| Title | 550f982 (track B) | e8adf69 (track A) | Median change |
|---|---|---|---|
| Virtua Fighter 2 | 3.461 / 3.544 / 3.503 (**3.503**) | 3.513 / 3.568 / 3.539 (**3.539**) | +1.0% |
| Panzer Dragoon II Zwei | 4.480 / 4.391 / 4.327 (**4.391**) | 4.407 / 4.406 / 4.380 (**4.406**) | +0.3% |
| Sega Rally Championship | 4.884 / 4.851 / 4.813 (**4.851**) | 4.848 / 4.838 / 4.807 (**4.838**) | -0.3% |
| Burning Rangers | 6.119 / 6.317 / 6.273 (**6.273**) | 6.081 / 6.268 / 6.219 (**6.219**) | -0.9% |
| Guardian Heroes | 3.330 / 3.482 / 3.564 (**3.482**) | 3.396 / 3.473 / 3.534 (**3.473**) | -0.3% |
| Street Fighter Zero 3 | 4.642 / 4.962 / 4.977 (**4.962**) | 4.652 / 4.856 / 4.971 (**4.856**) | -2.1% |

The change is within the run-to-run spread of every title (the spread between rounds of one executable is 1.5-7%, as large as the largest difference between them, -2.1%), so track A leaves the steady state of the six titles as it was, as expected: A1-A4 only touch what happens when a block is dropped, retired or reused, and the titles other than Burning Rangers drop few blocks (in the 36,000-frame lockstep runs, 0 to 2,753 invalidations on the master over the whole run, against 884,373 for Burning Rangers). Max frame times in the same runs: Sega Rally 11.4 / 11.2 / 11.7 (track B) against 11.6 / 11.7 / 13.1, Burning Rangers 13.8 / 12.9 / 12.1 against 14.4 / 17.4 / 12.9, the other four titles show nothing beyond single frames (Virtua Fighter 2: 11.7 ms in one track B run, 10.2 ms the highest with track A); the Burning Rangers 17.4 ms frame and the Sega Rally 13.1 ms frame are single frames in one run each, not repeated in the other two rounds. The same track B code rebuilt here measures 4-5% above its track B record (PD2 4.391 against 4.187, GH 3.482 against 3.315), so x64 totals of different sessions carry about 5% of noise, and only same-session figures are used for verdicts.

### Executor-only ratio

The 2D track B method (the temporary `Executor::Run` `rdtsc` instrumentation of that section applied to a clean tree, built in `build-instr`, executable copied aside, patch reverted; not committed): Panzer Dragoon II Zwei, Street Fighter Zero 3 and Burning Rangers, x64, `--warmup 2400 --frames 1800`, three rounds (nine runs, all exit 0, `jit window` counters identical to the uninstrumented runs). Values are means of the three runs. The executor time without instrumentation is estimated as the uninstrumented x64 SH2 total of the table above minus the outside-`Run` time of the instrumented runs; the executor-only ratio is (interpreter total minus the same outside time) / x64 executor time.

| ms/frame (both CPUs) | Panzer Dragoon II Zwei | Street Fighter Zero 3 | Burning Rangers |
|---|---|---|---|
| Interpreter SH2 total (table above) | 4.927 | 4.667 | 6.405 |
| x64 SH2 total, uninstrumented (table above) | 4.335 | 4.958 | 6.332 |
| Instrumented x64 SH2 total (3 runs) | 4.626 / 4.639 / 4.617 | 5.197 / 5.192 / 5.151 | 6.527 / 6.654 / 6.629 |
| `Executor::Run` (3 runs) | 4.007 / 4.018 / 3.996 | 4.584 / 4.583 / 4.540 | 5.899 / 6.021 / 5.996 |
| Outside `Executor::Run` (SH2 total - Run) | 0.620 | 0.611 | 0.631 |
| `Executor::Run`, uninstrumented estimate | **3.715** | **4.347** | **5.701** |
| Interpreter executor time (interpreter total - outside) | 4.307 | 4.056 | 5.774 |
| **Executor-only ratio** | **1.159** | **0.933** | **1.013** |
| SH2-total ratio (table above) | 1.137 | 0.941 | 1.012 |
| `Executor::Run` calls per window (both CPUs) | 55.56M | 56.01M | 53.39M |
| Compile time in the window (`jit window`, inside `Run`; mean of 3) | 0.066 (118.4 ms in 1,800 frames) | 0.659 (1,187 ms) | 0.086 (155.2 ms) |
| **Executor budget for the bar**: interpreter total / 1.5 - outside | 2.665 | 2.500 | 3.639 |
| Shrink still needed (estimate to budget) | 28% | 42% | 36% |

Executor shrink and SH2-total reduction still needed for 1.5 on each of the six titles (the SH2-total column assumes the time outside the executor, 0.61-0.63 ms/frame, stays):

| Title | x64 SH2 total | Interpreter total / 1.5 | SH2-total reduction needed | Executor shrink needed (3 titles measured) |
|---|---|---|---|---|
| Virtua Fighter 2 | 3.494 | 2.969 | 15% | |
| Guardian Heroes | 3.537 | 2.698 | 24% | |
| Panzer Dragoon II Zwei | 4.335 | 3.285 | 24% | 28% |
| Burning Rangers | 6.332 | 4.270 | 33% | 36% |
| Sega Rally Championship | 4.845 | 3.125 | 36% | |
| Street Fighter Zero 3 | 4.958 | 3.111 | 37% | 42% |

- The executor-only ratios are 0.03-0.05 below track B's (1.206, 0.963, 1.056 against 1.159, 0.933, 1.013), in a session whose x64 is about 5% slower, and the shrink still needed is the one track B left (25%, 41%, 33%) plus that session difference: track A did not change what the executor needs.
- Street Fighter Zero 3: compiling is 0.659 ms/frame (15% of the executor), as in track B (0.631). With it free the SH2-total ratio would be about 1.09 (executor-only about 1.10), so compile cost alone is far from the bar; its 6,333 native compiles are all first compiles (Task 1: 0 returning), which A1-A4 cannot touch.
- The time outside `Executor::Run` is 0.61-0.63 ms/frame, as in track B (0.59-0.62).

### Max frames against the spike rule

Each x64 run's max frame time must be at most 10% above the highest interpreter max frame of the same session and window (design section 3, item 3). BIOS menu listed for reference only.

| Title | Interpreter max, 3 runs (highest) | Limit (+10%) | x64 max, 3 runs | Rule |
|---|---|---|---|---|
| Virtua Fighter 2 | 9.8 / 9.6 / 10.2 (10.2) | 11.2 | 8.8 / 8.0 / 10.4 | pass |
| Panzer Dragoon II Zwei | 8.7 / 11.3 / 9.4 (11.3) | 12.4 | 10.8 / 8.8 / 10.3 | pass |
| **Sega Rally Championship** | 10.7 / 9.7 / 10.8 (10.8) | 11.9 | **12.5** / 11.8 / 11.6 | **FAIL** (one run, by 0.6 ms; it has no compile burst: 1,056 native compiles in the window, max of the other runs 11.8 against the 11.9 limit) |
| **Burning Rangers** | 12.2 / 10.7 / 10.9 (12.2) | 13.4 | 12.6 / **13.6** / 12.9 | **FAIL** (one run, by 0.2 ms; 821 native compiles in the window) |
| Guardian Heroes | 8.7 / 8.8 / 10.8 (10.8) | 11.9 | 10.2 / 8.2 / 9.0 | pass |
| **Street Fighter Zero 3** | 12.1 / 11.6 / 12.9 (12.9) | 14.2 | **24.2 / 23.1 / 24.2** | **FAIL** |
| BIOS menu (reference) | 9.3 / 9.2 / 9.4 (9.4) | 10.3 | 10.7 / 10.9 / 11.3 | over by 0.4-1.0 ms (not a bar title) |
| **Burning Rangers, long run** | 17.0 (one run) | 18.7 | **92.1** | **FAIL** |

Street Fighter Zero 3's slowest frames are compile bursts as before (the window builds 10,686 blocks and compiles 6,333 natively, 1.19 s of compiling in 1,800 frames); its x64 max is 23-24 ms, as in track B (23.2-24.0). Sega Rally and Burning Rangers have no compile burst in their windows; their x64 max frames exceed the limit by a fraction of a millisecond in one run each, with the same code that passed in track B (Sega Rally passed then by 0.04 ms: the rule has no margin on that title), which is noise in the max frame time, but the rule as written fails.

### Long run: Burning Rangers, frames 2,400-36,000

`--warmup 2400 --frames 33600`, one run per mode, default settings, interpreter first, then x64, after the table runs of the same session. Both exited 0.

| Burning Rangers, frames 2,400-36,000 | ms/frame avg | p95 | p99 | max | SH2 total ms/frame |
|---|---|---|---|---|---|
| interpreter | 9.136 | 11.339 | 12.831 | 16.996 | 5.905 |
| x64 | 10.572 | 18.492 | 24.603 | 92.072 | 6.696 (ratio **0.882**) |
| track B (another session) | 9.159 / 11.725 | 11.201 / 27.413 | 12.643 / 42.695 | 15.905 / 130.419 | 5.937 / 7.975 (ratio 0.744) |
| 2C (another session) | 9.856 / 15.585 | 12.282 / 49.786 | 13.495 / 86.367 | 16.879 / 173.340 | 6.369 / 11.650 (ratio 0.55) |

(The x64 SH2 total over the A1-A4 progress sessions is 6.540-6.699 and the interpreter's 5.905-6.285, so the long ratio reads 0.88-0.94 depending on the session; the figure of this session is the one used for the bar.)

| Long run, x64 (window counters unless noted) | 2C | track B | track A (e8adf69) |
|---|---|---|---|
| Front-end builds plus native compiles | 2,244,425 | 2,119,633 | 1,149,860 |
| Native compiles | 492,237 | 183,302 | 42,871 |
| Compile time (average per frame) | 167.8 s (5.0 ms) | 71.7 s (2.13 ms) | 24.0 s (0.71 ms) |
| Frames with compiling | 13,500 | 12,427 | 9,642 of 33,600 |
| Code-cap flushes (master / slave) | 19 in the window | 4 / 3 | **0 / 0** |
| Core-requested flushes in the window | | 3 (of the 10 flush events counted) | 3 (the window's `flushes 3`, all named below) |
| Retired reuses (A1) | n/a | n/a | 885,877 (master 460,161, slave 425,716 since start-up; retired-pool evictions 15,861 since start-up) |
| Damped builds (A3) | n/a | n/a | 0 (`kMaxStaleDampShift` = 0) |
| IR-cap evictions (master) | not recorded | 0 in the window | 1 (16,843 IR-only blocks, between frames 15,000 and 16,800; no slow frame) |
| Peak native code, master / slave (since start-up) | 128 / 128 | 128 / 128 | **74.0 / 6.6 MB** |
| Worst frame | 173.3 ms | 130.4 ms | 92.1 ms |

- **Requested flushes.** The three flushes the window counts that the code cap did not request are the slave's: the SMPC command SSHON (`Saturn::SMPCOperations::EnableAndResetSlaveSH2`) calls `SH2::Reset(true)` on the slave CPU, which calls `Executor::Flush`. Named from a temporary instrumented run (a `fprintf` in `SH2::Reset`, in `EnableAndResetSlaveSH2` and in the bench's frame loop; build-instr, not committed; its window counters equal the uninstrumented run's: 1,149,860 builds plus compiles, 42,871 native compiles, `flushes 3`, 885,877 reuses): the window's three flushes happen in frames 6,805, 20,715 and 34,570 (`reset slave hard=1 wdt=0`, right after an SSHON), and none of them is a slow frame. Before the window the run has the core's `Saturn::Reset` (both CPUs) and two more SSHON resets of the slave, five SSHON in all. This is the same count as in Tasks 2-5 and the same before and after A4, which removed the flush that a state load made; the bench loads no state. The reset flush is still needed by design (`SH2::Reset` and attaching an executor keep their flush; A4 only removed `LoadState`'s). The `requested` counters since start-up (master 2, slave 6) are one lower than the events the instrumented run printed plus one attach flush each (slave: attach, one `Saturn::Reset`, five SSHON); this was not investigated further, as it does not affect the window figure.
- The five slowest frames (70-92 ms) are all compile bursts, none with a flush: frame 9,264 (92.1 ms: 3,626 front-end builds plus 590 native compiles, 74.1 ms of compiling), 9,270 (78.6 ms, 742 and 435, 60.2 ms), 7,014 (77.3 ms, 750 and 361, 62.8 ms), 9,309 (74.2 ms, 434 and 464, 56.7 ms), 9,279 (70.3 ms, 779 and 286, 51.2 ms). The interpreter's worst frame is 17.0 ms; the p99 is 24.6 ms against 12.8.
- Compile work is now the whole gap: 23,976 ms in the window is 0.714 ms/frame, so without compiling the x64 SH2 total would be 5.982 ms/frame, against the interpreter's 5.905 (ratio 0.987). The compile time since start-up splits into 11.8 s of front-end builds (master 4.7 s, slave 7.0 s) and 14.2 s of native compiles (8.9 s, 5.4 s): the 1.15 million front-end builds, mostly of code that was dropped and rebuilt, cost about as much as the native compiles; A1 only takes native blocks back.
- A3 (damping) was measured and left off (`kMaxStaleDampShift` = 0, "2D A3" above): at shift 2 it cut Burning Rangers' native compiles by 37% and compile time by 23% but halved the reuses and raised the front-end builds from 1.15 M to 1.52 M, with the SH2 total inside the run-to-run spread. This run uses the committed value.

### Default-on bar (design section 3), condition by condition

1. **Speed (at least 1.5 on each of the six titles): FAIL.** 1.274 (Virtua Fighter 2), 1.144 (Guardian Heroes), 1.137 (Panzer Dragoon II Zwei), 1.012 (Burning Rangers), 0.967 (Sega Rally), 0.941 (Street Fighter Zero 3); the best is 0.23 short, the worst 0.56. Executor-only: 1.159, 0.933, 1.013 on PD2, SFZ3, BR.
2. **Long session (Burning Rangers over frames 2,400-36,000, x64 SH-2 total at most the interpreter's): FAIL.** Ratio 0.882 (x64 6.696 against 5.905 ms/frame), up from 0.744 (track B). The memory conditions of the design (section 4) hold: no code-cap flushes (0 / 0) and a peak of 74.0 / 6.6 MB, under 2C's 128 MB.
3. **Spikes (x64 max at most 10% above the highest interpreter max in every window): FAIL.** Three of the six 1,800-frame windows pass (Virtua Fighter 2, Panzer Dragoon II Zwei, Guardian Heroes); Street Fighter Zero 3 fails (23.1-24.2 ms against the limit 14.2), Sega Rally by 0.6 ms in one run (12.5 against 11.9) and Burning Rangers by 0.2 ms in one run (13.6 against 13.4), both without compile bursts and with the code that passed in track B; the long run fails (92.1 ms against 18.7).
4. **Exactness: holds.** Lockstep identical for 36,000 frames on x64 on the BIOS menu and all six titles and for 1,800 frames on IR (`design/sh2-validation.md`, "Milestone 2D, track A"), and identical for 3,600 frames across state loads (`--lockstep-reload 300`) on x64 and IR on all seven; `[jit]` tests (175 test cases) on x64 and IR and ctest pass on Windows x64; the Raspberry Pi's IR lockstep and test suite pass. CI on the measured commit e8adf69 (GitHub Actions run 39365640161): green (`test-windows` 5m28s, `test-linux-x64` 6m02s, `build-macos-arm64` 2m35s).

Not met: conditions 1, 2 and 3. The JIT stays off by default; the README and the option label are unchanged.

### What track A left

- **What it fixed.** The long Burning Rangers run no longer fills or flushes the native-code cap (A2 frees each dropped block's code, A1 takes returning native code back: 885,877 reuses), compiles 77% fewer blocks natively than track B and spends a third of the compile time (71.7 s to 24.0 s), and its ratio went from 0.744 to 0.882 and its worst frame from 130.4 to 92.1 ms; a state load no longer throws the code away (A4, lockstep-exact). None of it shows in the six 1,800-frame titles (same-session A/B above).
- **Long sessions, still failing.** The remaining gap on Burning Rangers (6.696 against 5.905 ms/frame) is compile work (0.714 ms/frame; without it 0.987), and its worst frames are compile bursts of 286-590 native compiles and up to 3,626 front-end builds (51-74 ms of compiling) in the code-load phases at frames 7,014 and 9,264-9,309. What is left to compile there is code the cache has not held before (or has not held in native form), so reuse cannot take it back. The interpreter's worst frame (17.0 ms) is a quarter of that. The options that remain are cheaper front-end builds and native compiles, a damping rule that keeps reuse (not measured, see "2D A3"), or moving compiles off the emulation thread; condition 3 on Street Fighter Zero 3 (6,333 first compiles, 0.659 ms/frame, 23-24 ms frames) needs the same kind of work (it has no returning code, so A1 and A3 do not apply to it).
- **Speed, still failing.** The executor must shrink by 28% (Panzer Dragoon II Zwei), 36% (Burning Rangers) and 42% (Street Fighter Zero 3) for 1.5 (SH2-total reductions of 15-37% across the six titles, table above). Track B's per-instruction changes gave 3-6% and track A none, so a further 28-42% is unlikely to come from more per-instruction work of the same kind.
- **Recommendation: a per-block profile of the generated code before anything else; track C (shared SH-2 code) is not the next step.** Track C speeds up the time outside the executor (0.61-0.63 ms/frame, paid by the interpreter and the JIT alike), so it cannot reach the bar: with the outside time at zero for both modes the SH2-total ratios would be the executor-only ratios, 1.159 against 1.137 (Panzer Dragoon II Zwei), 1.013 against 1.012 (Burning Rangers) and 0.933 against 0.941 (Street Fighter Zero 3, which gets worse), a gain of about 0.02 at most. The 1.5 bar is an executor problem (generated code and the dispatch between blocks, which no item so far has targeted); a profile per block (which blocks of Panzer Dragoon II Zwei, Street Fighter Zero 3 and Burning Rangers take the cycles and what they cost per guest instruction, with the dispatch between blocks separated out) says whether a 28-42% shrink is reachable by a different code shape (register allocation across blocks, fewer exits and re-entries, fused sequences) or whether the bar needs revising. That profile also decides the compile-time work: a cheaper compile path is only worth its cost if the generated code stays as it is.
- Track C remains a valid improvement for both modes outside this bar (about 0.6 ms/frame of 4.3-6.4 ms), as the design says, but it should be scheduled on its own merits.

## 2E progress

Measured at the end of each 2E task (`design/plans/2026-10-20-sh2-jit-m2e.md`) on the machine and build configuration of the 2D baseline (AMD Ryzen 7 5700G, 16 threads, `build-bench`, Ninja, Release, `BRIMIR_LTO=ON`, `Brimir_ENABLE_IPO=ON`, MSVC). Default settings, scratch system dir per run with `brimir_saturn_rtc_us_eu.smpc` also as `brimir_saturn_rtc_jp.smpc`, executable copied aside before the runs. Burning Rangers (US BIOS): `--warmup 2400 --frames 33600`, one x64 run per compile mode plus one interpreter run. Street Fighter Zero 3 (JP BIOS, 4 MB cart), Panzer Dragoon II Zwei (US) and Sega Rally Championship (US): `--warmup 2400 --frames 1800`, three rounds, each round running the interpreter, x64 with the worker (`--sh2-jit --jit-backend x64`, the default) and x64 without it (`--jit-sync-compile`) back to back. All 30 runs exited 0 and every x64 run reported `compileFallbacks 0` on both CPUs.

**Columns.** "Interp SH2 ms" is the same round's interpreter run (the single Burning Rangers run for the long window); "ratio" is interpreter / x64 SH-2 total of the same round. "Frames over 16.7 ms" counts measured frames whose wall time exceeds 16.7 ms (`over16.7` on the bench's `ms/frame` line); for the interpreter the highest count of the title's runs and its highest max frame. "Worker ms/frame" is the worker's time in `Finalize` over the window divided by the frames. "Emulation-thread compile ms" is `compileMs` of the `jit window` line: front-end builds plus, for background compiles, `Prepare` and `Commit` only, or the whole native compile in synchronous mode. "IR reuses in window" (from Task 5 on) is `irReuses` of the `jit window` line: IR blocks taken back from the IR pool instead of being built again; "-" before Task 5.

**Load.** The development PC read 0-7% before the batch (Spotify, opencode and a terminal the only other processes; no WSL run active). A 30-second sampler of the whole-machine load over the batch reads 28% on average (2-57%, 63 samples), which includes the bench's own threads (threaded VDP, the emulation thread, the compile worker).

| Task | Compile | Title | Interp SH2 ms | x64 SH2 ms | Ratio | x64 avg ms | x64 p99 ms | x64 max ms | Frames over 16.7 ms (x64) | Interp frames over 16.7 ms (highest) / max ms | Worker ms/frame | Native compiles in window | Emulation-thread compile ms | Code-cap flushes (master / slave) | IR reuses in window |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2E D | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 1 | 4.724 | 4.531 | 1.043 | 9.594 | 11.327 | 14.6 | 0 | 0 / 12.5 | 0.565 | 6333 | 251.6 | 0 / 0 | - |
| 2E D | sync | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 1 | 4.724 | 4.878 | 0.968 | 9.855 | 15.469 | 24.4 | 15 | 0 / 12.5 | 0.000 | 6333 | 1,198.2 | 0 / 0 | - |
| 2E D | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 2 | 4.709 | 4.481 | 1.051 | 9.496 | 11.153 | 14.5 | 0 | 0 / 12.5 | 0.558 | 6333 | 249.9 | 0 / 0 | - |
| 2E D | sync | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 2 | 4.709 | 4.884 | 0.964 | 9.854 | 15.801 | 23.1 | 14 | 0 / 12.5 | 0.000 | 6333 | 1,191.1 | 0 / 0 | - |
| 2E D | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 3 | 4.678 | 4.518 | 1.035 | 9.499 | 10.860 | 14.8 | 0 | 0 / 12.5 | 0.552 | 6333 | 253.3 | 0 / 0 | - |
| 2E D | sync | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 3 | 4.678 | 4.903 | 0.954 | 9.845 | 15.569 | 24.3 | 15 | 0 / 12.5 | 0.000 | 6333 | 1,233.7 | 0 / 0 | - |
| 2E D | worker | Panzer Dragoon II Zwei (US), frames 2,400-4,200, round 1 | 5.007 | 4.378 | 1.144 | 6.083 | 8.123 | 9.5 | 0 | 0 / 12.3 | 0.059 | 660 | 28.1 | 0 / 0 | - |
| 2E D | sync | Panzer Dragoon II Zwei (US), frames 2,400-4,200, round 1 | 5.007 | 4.315 | 1.160 | 6.051 | 8.091 | 9.9 | 0 | 0 / 12.3 | 0.000 | 660 | 119.8 | 0 / 0 | - |
| 2E D | worker | Panzer Dragoon II Zwei (US), frames 2,400-4,200, round 2 | 4.976 | 4.423 | 1.125 | 6.150 | 8.105 | 11.5 | 0 | 0 / 12.3 | 0.058 | 660 | 28.6 | 0 / 0 | - |
| 2E D | sync | Panzer Dragoon II Zwei (US), frames 2,400-4,200, round 2 | 4.976 | 4.326 | 1.150 | 6.067 | 8.185 | 10.3 | 0 | 0 / 12.3 | 0.000 | 660 | 124.0 | 0 / 0 | - |
| 2E D | worker | Panzer Dragoon II Zwei (US), frames 2,400-4,200, round 3 | 5.061 | 4.319 | 1.172 | 6.054 | 8.084 | 9.4 | 0 | 0 / 12.3 | 0.058 | 660 | 27.3 | 0 / 0 | - |
| 2E D | sync | Panzer Dragoon II Zwei (US), frames 2,400-4,200, round 3 | 5.061 | 4.400 | 1.150 | 6.166 | 8.296 | 9.9 | 0 | 0 / 12.3 | 0.000 | 660 | 126.7 | 0 / 0 | - |
| 2E D | worker | Sega Rally Championship (US), frames 2,400-4,200, round 1 | 4.727 | 4.872 | 0.970 | 7.900 | 10.308 | 11.6 | 0 | 0 / 11.1 | 0.095 | 1056 | 46.9 | 0 / 0 | - |
| 2E D | sync | Sega Rally Championship (US), frames 2,400-4,200, round 1 | 4.727 | 4.853 | 0.974 | 7.894 | 10.365 | 12.3 | 0 | 0 / 11.1 | 0.000 | 1056 | 197.9 | 0 / 0 | - |
| 2E D | worker | Sega Rally Championship (US), frames 2,400-4,200, round 2 | 4.758 | 4.875 | 0.976 | 7.903 | 10.261 | 14.2 | 0 | 0 / 11.1 | 0.095 | 1056 | 45.9 | 0 / 0 | - |
| 2E D | sync | Sega Rally Championship (US), frames 2,400-4,200, round 2 | 4.758 | 4.805 | 0.990 | 7.864 | 10.278 | 11.1 | 0 | 0 / 11.1 | 0.000 | 1056 | 194.8 | 0 / 0 | - |
| 2E D | worker | Sega Rally Championship (US), frames 2,400-4,200, round 3 | 4.780 | 4.876 | 0.980 | 7.932 | 10.279 | 12.0 | 0 | 0 / 11.1 | 0.095 | 1056 | 45.9 | 0 / 0 | - |
| 2E D | sync | Sega Rally Championship (US), frames 2,400-4,200, round 3 | 4.780 | 4.888 | 0.978 | 7.916 | 10.471 | 12.5 | 0 | 0 / 11.1 | 0.000 | 1056 | 199.9 | 0 / 0 | - |
| 2E D | worker | Burning Rangers (US), frames 2,400-36,000 | 6.502 | 6.465 | 1.006 | 10.286 | 21.128 | 47.5 | 1797 | 3 / 17.2 | 0.379 | 27735 | 13,919.9 | 0 / 0 | - |
| 2E D | sync | Burning Rangers (US), frames 2,400-36,000 | 6.502 | 6.634 | 0.980 | 10.485 | 24.189 | 91.5 | 2472 | 3 / 17.2 | 0.000 | 42871 | 23,674.2 | 0 / 0 | - |
| 2E E | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 1 | 5.153 | 5.144 | 1.002 | 10.304 | 13.291 | 23.3 | 2 | 0 / 14.8 | 0.588 | 5842 | 350.4 | 0 / 0 | 0 |
| 2E E | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 2 | 4.940 | 5.071 | 0.974 | 10.074 | 13.043 | 23.6 | 2 | 0 / 14.8 | 0.575 | 5842 | 345.1 | 0 / 0 | 0 |
| 2E E | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 3 | 5.151 | 5.212 | 0.988 | 10.372 | 13.225 | 25.8 | 2 | 0 / 14.8 | 0.581 | 5842 | 355.6 | 0 / 0 | 0 |
| 2E E | worker | Panzer Dragoon II Zwei (US), frames 2,400-4,200, round 1 | 5.517 | 4.899 | 1.126 | 6.809 | 10.941 | 13.1 | 0 | 0 / 12.4 | 0.064 | 660 | 34.3 | 0 / 0 | 0 |
| 2E E | worker | Panzer Dragoon II Zwei (US), frames 2,400-4,200, round 2 | 5.474 | 4.936 | 1.109 | 6.854 | 11.144 | 12.6 | 0 | 0 / 12.4 | 0.064 | 660 | 36.0 | 0 / 0 | 0 |
| 2E E | worker | Panzer Dragoon II Zwei (US), frames 2,400-4,200, round 3 | 5.349 | 4.817 | 1.110 | 6.701 | 10.238 | 12.8 | 0 | 0 / 12.4 | 0.064 | 660 | 34.3 | 0 / 0 | 0 |
| 2E E | worker | Sega Rally Championship (US), frames 2,400-4,200, round 1 | 5.214 | 5.575 | 0.935 | 8.793 | 13.918 | 16.9 | 1 | 1 / 17.5 | 0.106 | 1056 | 58.8 | 0 / 0 | 0 |
| 2E E | worker | Sega Rally Championship (US), frames 2,400-4,200, round 2 | 5.060 | 5.071 | 0.998 | 8.188 | 11.116 | 14.1 | 0 | 1 / 17.5 | 0.101 | 1056 | 54.5 | 0 / 0 | 0 |
| 2E E | worker | Sega Rally Championship (US), frames 2,400-4,200, round 3 | 5.282 | 5.567 | 0.949 | 8.774 | 14.016 | 21.0 | 1 | 1 / 17.5 | 0.108 | 1056 | 58.8 | 0 / 0 | 0 |
| 2E E | worker | Burning Rangers (US), frames 2,400-36,000 | 6.135 | 6.161 | 0.996 | 10.215 | 17.500 | 48.9 | 628 | 9 / 18.7 | 0.381 | 28063 | 5,824.0 | 0 / 0 | 887,487 |
| 2E E (R4) | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 1 | 5.048 | 4.999 | 1.010 | 10.001 | 12.745 | 15.8 | 0 | 0 / 12.7 | 0.604 | 6333 | 301.8 | 0 / 0 | 0 |
| 2E E (R4) | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 2 | 5.083 | 4.930 | 1.031 | 9.990 | 15.885 | 27.0 | 16 | 0 / 12.7 | 0.692 | 6333 | 320.9 | 0 / 0 | 0 |
| 2E E (R4) | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 3 | 4.757 | 4.394 | 1.083 | 9.305 | 10.928 | 14.2 | 0 | 0 / 12.7 | 0.539 | 6333 | 243.3 | 0 / 0 | 0 |
| 2E E (R4) | worker | Burning Rangers (US), frames 2,400-36,000, run 1 | 6.589 | 6.894 | 0.956 | 10.921 | 20.456 | 54.3 | 1900 | 40 / 21.5 | 0.418 | 27657 | 6,421.1 | 0 / 0 | 877,188 |
| 2E E (R4) | worker | Burning Rangers (US), frames 2,400-36,000, run 2 | 6.294 | 6.452 | 0.976 | 10.320 | 18.913 | 48.6 | 1186 | 30 / 30.7 | 0.396 | 27618 | 6,087.6 | 0 / 0 | 880,785 |
| 2E E (R6) | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 1 | 4.743 | 4.623 | 1.026 | 9.531 | 11.805 | 14.9 | 0 | 0 / 13.0 | 0.555 | 6333 | 263.5 | 0 / 0 | 0 |
| 2E E (R6) | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 2 | 4.754 | 4.898 | 0.971 | 9.834 | 12.674 | 15.8 | 0 | 0 / 13.0 | 0.575 | 6333 | 280.0 | 0 / 0 | 0 |
| 2E E (R6) | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 3 | 4.901 | 4.952 | 0.990 | 9.883 | 14.193 | 20.3 | 3 | 0 / 13.0 | 0.577 | 6333 | 282.4 | 0 / 0 | 0 |
| 2E E (R6) | worker | Burning Rangers (US), frames 2,400-36,000, pair 1 | 6.090 | 6.521 | 0.934 | 10.422 | 19.019 | 55.6 | 1221 | 11 / 20.5 | 0.380 | 27631 | 6,003.4 | 0 / 0 | 879,102 |
| 2E E (R6) | worker, pre-R4 build (12ac9ab) | Burning Rangers (US), frames 2,400-36,000, pair 1 | 6.090 | 6.380 | 0.955 | 10.293 | 18.669 | 48.2 | 990 | 11 / 20.5 | 0.407 | 28066 | 6,141.2 | 0 / 0 | 894,608 |
| 2E E (R6) | worker | Burning Rangers (US), frames 2,400-36,000, pair 2 | 6.090 | 6.434 | 0.947 | 10.278 | 18.472 | 51.0 | 1079 | 11 / 20.5 | 0.383 | 27612 | 6,101.7 | 0 / 0 | 883,623 |
| 2E E (R6) | worker, pre-R4 build (12ac9ab) | Burning Rangers (US), frames 2,400-36,000, pair 2 | 6.090 | 6.185 | 0.985 | 10.029 | 17.833 | 53.3 | 773 | 11 / 20.5 | 0.401 | 28064 | 5,898.7 | 0 / 0 | 891,780 |
| 2E E (R7) | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 1 | 4.624 | 4.578 | 1.010 | 9.512 | 11.331 | 23.2 | 1 | 0 / 11.5 | 0.525 | 5842 | 294.0 | 0 / 0 | 0 |
| 2E E (R7) | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 2 | 4.463 | 4.496 | 0.993 | 9.325 | 11.095 | 20.8 | 1 | 0 / 11.5 | 0.506 | 5842 | 270.4 | 0 / 0 | 0 |
| 2E E (R7) | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 3 | 4.576 | 4.679 | 0.978 | 9.558 | 11.671 | 22.1 | 1 | 0 / 11.5 | 0.523 | 5842 | 296.2 | 0 / 0 | 0 |
| 2E E (R7) | worker | Panzer Dragoon II Zwei (US), frames 2,400-4,200, round 1 | 5.091 | 4.450 | 1.144 | 6.188 | 8.330 | 10.0 | 0 | 0 / 11.0 | 0.059 | 660 | 28.7 | 0 / 0 | 0 |
| 2E E (R7) | worker | Panzer Dragoon II Zwei (US), frames 2,400-4,200, round 2 | 5.126 | 4.465 | 1.148 | 6.218 | 8.267 | 9.7 | 0 | 0 / 11.0 | 0.059 | 660 | 27.9 | 0 / 0 | 0 |
| 2E E (R7) | worker | Panzer Dragoon II Zwei (US), frames 2,400-4,200, round 3 | 5.016 | 6.406 | 0.783 | 8.215 | 9.896 | 12.7 | 0 | 0 / 11.0 | 0.054 | 660 | 27.6 | 0 / 0 | 0 |
| 2E E (R7) | worker | Sega Rally Championship (US), frames 2,400-4,200, round 1 | 4.623 | 4.792 | 0.965 | 7.792 | 10.041 | 10.6 | 0 | 0 / 10.7 | 0.093 | 1056 | 43.8 | 0 / 0 | 0 |
| 2E E (R7) | worker | Sega Rally Championship (US), frames 2,400-4,200, round 2 | 4.694 | 4.866 | 0.965 | 7.922 | 10.325 | 12.1 | 0 | 0 / 10.7 | 0.097 | 1056 | 44.7 | 0 / 0 | 0 |
| 2E E (R7) | worker | Sega Rally Championship (US), frames 2,400-4,200, round 3 | 4.729 | 5.016 | 0.943 | 8.024 | 10.588 | 12.8 | 0 | 0 / 10.7 | 0.097 | 1056 | 47.4 | 0 / 0 | 0 |
| 2E E (R7) | worker | Burning Rangers (US), frames 2,400-36,000, pair 1 | 5.961 | 6.210 | 0.960 | 10.005 | 17.586 | 47.9 | 599 | 0 / 16.4 | 0.380 | 28079 | 5,779.9 | 0 / 0 | 889,221 |
| 2E E (R7) | worker, pre-R4 build (12ac9ab) | Burning Rangers (US), frames 2,400-36,000, pair 1 | 5.961 | 6.180 | 0.965 | 9.962 | 17.352 | 47.7 | 558 | 0 / 16.4 | 0.384 | 28063 | 5,822.9 | 0 / 0 | 889,678 |
| 2E E (R7) | worker | Burning Rangers (US), frames 2,400-36,000, pair 2 | 5.961 | 6.238 | 0.956 | 10.027 | 17.460 | 49.5 | 585 | 0 / 16.4 | 0.376 | 28103 | 5,827.0 | 0 / 0 | 893,737 |
| 2E E (R7) | worker, pre-R4 build (12ac9ab) | Burning Rangers (US), frames 2,400-36,000, pair 2 | 5.961 | 6.164 | 0.967 | 9.991 | 17.450 | 47.2 | 615 | 0 / 16.4 | 0.389 | 28069 | 5,842.3 | 0 / 0 | 889,024 |
| 2E E (R8) | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 1 | 4.613 | 4.570 | 1.009 | 9.339 | 11.103 | 14.6 | 0 | 0 / 12.2 | 0.547 | 6333 | 260.9 | 0 / 0 | 0 |
| 2E E (R8) | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 2 | 4.727 | 4.721 | 1.001 | 9.617 | 11.441 | 14.7 | 0 | 0 / 12.2 | 0.566 | 6333 | 274.9 | 0 / 0 | 0 |
| 2E E (R8) | worker | Street Fighter Zero 3 (JP), frames 2,400-4,200, round 3 | 4.647 | 4.672 | 0.995 | 9.645 | 11.380 | 15.2 | 0 | 0 / 12.2 | 0.567 | 6333 | 270.1 | 0 / 0 | 0 |
| 2E E (R8) | worker | Panzer Dragoon II Zwei (US), frames 2,400-4,200, round 1 | 5.048 | 4.437 | 1.138 | 6.185 | 8.319 | 9.3 | 0 | 0 / 11.3 | 0.059 | 660 | 28.4 | 0 / 0 | 0 |
| 2E E (R8) | worker | Panzer Dragoon II Zwei (US), frames 2,400-4,200, round 2 | 5.013 | 4.488 | 1.117 | 6.227 | 8.427 | 10.9 | 0 | 0 / 11.3 | 0.059 | 660 | 28.8 | 0 / 0 | 0 |
| 2E E (R8) | worker | Panzer Dragoon II Zwei (US), frames 2,400-4,200, round 3 | 5.004 | 4.605 | 1.087 | 6.407 | 8.604 | 9.4 | 0 | 0 / 11.3 | 0.060 | 660 | 31.3 | 0 / 0 | 0 |
| 2E E (R8) | worker | Sega Rally Championship (US), frames 2,400-4,200, round 1 | 4.704 | 5.110 | 0.921 | 8.206 | 10.871 | 13.3 | 0 | 0 / 13.1 | 0.100 | 1056 | 48.7 | 0 / 0 | 0 |
| 2E E (R8) | worker | Sega Rally Championship (US), frames 2,400-4,200, round 2 | 4.708 | 4.991 | 0.943 | 8.093 | 10.473 | 13.4 | 0 | 0 / 13.1 | 0.098 | 1056 | 47.5 | 0 / 0 | 0 |
| 2E E (R8) | worker | Sega Rally Championship (US), frames 2,400-4,200, round 3 | 4.788 | 4.968 | 0.964 | 8.065 | 10.470 | 12.1 | 0 | 0 / 13.1 | 0.099 | 1056 | 48.8 | 0 / 0 | 0 |
| 2E E (R8) | worker | Burning Rangers (US), frames 2,400-36,000, pair 1 | 5.975 | 6.298 | 0.949 | 10.120 | 17.618 | 50.0 | 652 | 1 / 16.8 | 0.383 | 28890 | 5,829.6 | 0 / 0 | 889,149 |
| 2E E (R8) | worker, R7 build (4a35b03) | Burning Rangers (US), frames 2,400-36,000, pair 1 | 5.975 | 6.308 | 0.947 | 10.344 | 17.847 | 46.3 | 827 | 1 / 16.8 | 0.381 | 28101 | 5,860.2 | 0 / 0 | 885,181 |
| 2E E (R8) | worker | Burning Rangers (US), frames 2,400-36,000, pair 2 | 5.975 | 6.357 | 0.940 | 10.188 | 18.022 | 49.1 | 902 | 1 / 16.8 | 0.392 | 28901 | 5,940.3 | 0 / 0 | 889,022 |
| 2E E (R8) | worker, R7 build (4a35b03) | Burning Rangers (US), frames 2,400-36,000, pair 2 | 5.975 | 6.246 | 0.957 | 10.023 | 17.471 | 50.0 | 606 | 1 / 16.8 | 0.372 | 28061 | 5,764.0 | 0 / 0 | 877,075 |

### Task 4: the worker on by default (2E D)

Code: `8a70410` on top of `803aec3` (the worker in the block cache); the measured executable is the build of `8a70410` (`brimir_bench` with `--jit-sync-compile`, `workerMs`, `async`, `dropped` and `over16.7`). The synchronous rows are the old behaviour: the Burning Rangers window equals track A's (1,149,860 builds, 42,871 native compiles, flushes 3, 885,877 reuses, worst frame 91.5 against 92.1 ms).

Median SH-2 totals (interpreter / x64 with the worker; ratios from the medians of the three rounds):

| Title | Interpreter | x64, worker | Ratio | x64, synchronous | Ratio | Worst frame (max across rounds), worker / sync / interpreter | Frames over 16.7 ms, worker / sync / interpreter |
|---|---|---|---|---|---|---|---|
| Street Fighter Zero 3 | 4.709 | 4.518 | 1.042 | 4.884 | 0.964 | 14.8 / 24.4 / 12.5 | 0 / 15 / 0 |
| Panzer Dragoon II Zwei | 5.007 | 4.378 | 1.144 | 4.326 | 1.157 | 11.5 / 10.3 / 12.3 | 0 / 0 / 0 |
| Sega Rally Championship | 4.758 | 4.875 | 0.976 | 4.853 | 0.980 | 14.2 / 12.5 / 11.1 | 0 / 0 / 0 |
| Burning Rangers, long | 6.502 | 6.465 | 1.006 | 6.634 | 0.980 | 47.5 / 91.5 / 17.2 | 1,797 / 2,472 / 3 |

- **Street Fighter Zero 3 gains most.** Its emulation-thread compile time falls from 1,191-1,234 ms to 250-253 ms in the window (0.56 ms/frame of worker time carries the rest), the SH-2 total drops 7.5% (4.884 to 4.518) and the ratio goes from 0.964 to 1.042 (track A: 0.941), the 15 frames over 16.7 ms disappear and the worst frame falls from 24 to 14.8 ms. The interpreter's worst frame (12.5) is still 2.3 ms lower, so the spike rule (110% of the interpreter's highest max, 13.8 ms) is not met.
- **Burning Rangers, long.** The ratio rises from 0.980 (synchronous, this session) to 1.006, the worst frame falls from 91.5 to 47.5 ms and the frames over 16.7 ms from 2,472 to 1,797. The interpreter has 3. The emulation thread still compiles 13,920 ms in the window (23,674 ms synchronous): the native compile moved to the worker, but the front-end builds (1.2 M of them; 12.2 s of the 14.3 s the two caches have spent since startup building) and `Prepare` and `Commit` (2.1 s since startup) did not. The five slowest frames (33-47 ms: 9,264, 9,270, 9,309, 9,273, 9,267) are the same compile bursts as in synchronous mode (3,626 builds and 521 native compiles in frame 9,264), now 24 ms of emulation-thread compiling instead of 74 ms. 17,979 of the 45,714 submitted compiles (39%) were dropped because their block was dropped before the result arrived (`dropped`; a flush also drops them), so the window has 27,735 native compiles against 42,871.
- **Panzer Dragoon II Zwei and Sega Rally** compile little (660 and 1,056 native compiles), so the worker changes nothing outside run-to-run spread (PD2 4.378 against 4.326, Rally 4.875 against 4.853; the three rounds agree within 3%). Rally is still below 1.0 (0.976).
- **Cross-session note.** The interpreter's Burning Rangers total in this session is 6.502 ms/frame against 5.905 in the track A session (avg 9.666 against 9.136 ms), while the synchronous x64 total is 6.634 against 6.696: only ratios inside one session are comparable, and track A's 0.882 corresponds to this session's 0.980.

**Verdict for the task:** the worker is what it was built to be (no compile bursts on Street Fighter Zero 3, the long worst frame halved) but it does not remove the stutter or reach 1.0 on every title: Rally is at 0.976, Burning Rangers' long run at 1.006 with 1,797 frames over 16.7 ms against 3, and Street Fighter Zero 3's worst frame is 2.3 ms above the interpreter's. The remaining emulation-thread work in Burning Rangers is the front-end builds, which Task 5 (reuse of decoded blocks) targets.

### Task 5: reuse of dropped IR blocks (2E E)

Code: the Task 5 commit on top of `2d5aae3`; the measured executable is its `build-bench` build (`irReuses` on the `cache` and `jit window` lines). x64 with the worker (the default) and the interpreter only, same protocol as Task 4 otherwise (three rounds of the short titles, one long Burning Rangers run each). All 20 runs exited 0, `compileFallbacks 0` on both CPUs.

**Load.** This session's machine was slower: the 30-second sampler read 34% on average (8-60%, 43 samples) against 28% for Task 4, and the interpreter's medians are 9-10% above Task 4's (Panzer Dragoon II Zwei 5.474 against 5.007, Street Fighter Zero 3 5.151 against 4.709, Rally 5.214 against 4.758; Burning Rangers 6.135 against 6.502 is the exception). Compare ratios within a session only.

Median SH-2 totals (ratios from the medians of the three rounds):

| Title | Interpreter | x64, worker | Ratio | Task 4 ratio | Worst frame (max across rounds), x64 / interpreter | Frames over 16.7 ms (max across rounds), x64 / interpreter | Front-end builds in window | IR reuses in window |
|---|---|---|---|---|---|---|---|---|
| Street Fighter Zero 3 | 5.151 | 5.144 | 1.001 | 1.042 | 25.8 / 14.8 | 2 / 0 | 15,340-15,347 (Task 4: 10,686) | 0 |
| Panzer Dragoon II Zwei | 5.474 | 4.899 | 1.117 | 1.144 | 13.1 / 12.4 | 0 / 0 | 187 | 0 |
| Sega Rally Championship | 5.214 | 5.567 | 0.937 | 0.976 | 21.0 / 17.5 | 1 / 1 | 502 | 0 |
| Burning Rangers, long | 6.135 | 6.161 | 0.996 | 1.006 | 48.9 / 18.7 | 628 / 9 | 316,311 (Task 4: 1,203,647) | 887,487 |

- **Burning Rangers, long: the target, clearly improved.** The front-end builds in the window fall by 74% (1,203,647 to 316,311): 887,487 dropped IR blocks came back and were taken from the IR pool. The emulation thread's compile time in the window falls from 13,920 to 5,824 ms, the p99 frame from 21.1 to 17.5 ms and the frames over 16.7 ms from 1,797 to 628 (the interpreter has 9 this session). The worst frame does not move (48.9 against 47.5 ms): the five slowest frames are still the burst of new code at frames 9,264-9,309 (3,610 builds and 524 native compiles in frame 9,264), which no pool can serve. The ratio (0.996 against 1.006) is within run-to-run spread.
- **Street Fighter Zero 3: worse, partly because of this task.** Its IR pool is never reused (0 reuses), but it counts toward the IR cap as specified, and the master's IR was already near the cap (1,014,557 of 1,048,576 instructions after Task 4's run, no eviction). Now the cap is reached once in the window (13,553 IR-only blocks evicted), so the window has 15,347 front-end builds against 10,686, its emulation-thread compile time rises from 251 to 350 ms (part of it the slower machine), and the worst frame rises from 14.6 to 23.3-25.8 ms with 2 frames over 16.7 ms per round. The ratio falls from 1.042 to 1.001; the slower session explains part of that, the extra builds the rest.
- **Panzer Dragoon II Zwei and Sega Rally** build and reuse nothing in the window (187 and 502 builds, 0 reuses; the same counts as Task 4), so their changes are the slower session. Rally's three rounds spread from 5.071 to 5.575 ms.

**Verdict for the task:** IR reuse removes most of Burning Rangers' front-end work and two thirds of its stutter frames, but not the startup burst behind its worst frame. Counting the pool toward the IR cap costs Street Fighter Zero 3 one IR eviction in its window. A pool bound of its own (or emptying only the pool when the cap is reached, before evicting live IR-only blocks) would avoid that. Ruling R4 chose the latter (below).

### Task 5 (R4): the IR cap releases the pool before evicting live blocks

Code: the R4 commit on top of `12ac9ab`. At the IR cap the cache now releases the whole IR pool first (`irPoolReleases`), and evicts the live IR-only blocks only if it is still at the cap (`irEvictions`), the order the native code cap uses with the retired native pool. The retired IR keeps counting toward the cap. Same protocol, in a new session: Street Fighter Zero 3 three rounds (interpreter and x64 back to back), Burning Rangers long twice (interpreter and x64 each). Rows "2E E (R4)" in the table. The load reading after each SFZ3 run was 5-27%. The first Burning Rangers pair ran during a noisier period (its interpreter run has 40 frames over 16.7 ms).

| Title | Interpreter | x64 | Ratio | Worst frame, x64 / interpreter | Frames over 16.7 ms, x64 / interpreter | Front-end builds in window | IR evictions / pool releases in window | IR reuses |
|---|---|---|---|---|---|---|---|---|
| Street Fighter Zero 3, round medians | 5.048 | 4.930 | 1.024 | 15.8, 27.0, 14.2 / 12.7 | 0, 16, 0 / 0 | 10,686 (as Task 4) | 0 / 1 | 0 |
| Burning Rangers long, run 1 | 6.589 | 6.894 | 0.956 | 54.3 / 21.5 | 1,900 / 40 | 323,360 | 1 / 526 | 877,188 |
| Burning Rangers long, run 2 | 6.294 | 6.452 | 0.976 | 48.6 / 30.7 | 1,186 / 30 | 324,460 | 1 / 536 | 880,785 |
| Burning Rangers long, pre-R4 build (`12ac9ab`), same session | 6.294 (run 2) | 6.377 | 0.987 | 50.0 | 981 | 321,246 | 1 / - | 895,596 |

- **Street Fighter Zero 3 is restored.** The cap now only releases the pool (once in the window), no live IR-only block is evicted, and the window's front-end builds are exactly Task 4's 10,686 (native compiles 6,333, also as Task 4). Rounds 1 and 3 have worst frames of 15.8 and 14.2 ms and no frame over 16.7 ms. Round 2's 16 frames over 16.7 ms (worst 27.0 ms) have the same compile work as the other rounds (its slowest frame, 2,481, built 70 blocks in 3.2 ms), so they are host stalls, not compiles. Median ratio 1.024 (Task 4: 1.042, pre-R4: 1.001).
- **Burning Rangers.** The work counters are within 1-2% of the pre-R4 build (front-end builds 323-324 K against 321 K in the same session, emulation-thread compile ms 6,088-6,421 against 6,059). Releasing the pool instead of evicting everything means the cap is reached often (526-536 times in the window, against 1 eviction), each time freeing the pool on the emulation thread, outside `compileMs`. The frames over 16.7 ms are 1,186 (quieter run) against 981 for the pre-R4 build in the same session. The 628 of the Task 5 session is not reproducible today: this session is noisier (the interpreter has 30-40 frames over 16.7 ms against 9). Ratio 0.976 against 0.987 for the pre-R4 build, within run-to-run spread.

**Verdict for R4:** it removes the Street Fighter Zero 3 regression completely (same builds and compiles as before Task 5) and costs Burning Rangers little, within the spread of this session. The many pool releases at the cap are one candidate for the small remaining difference.

### Task 5 (R6): the IR cap releases the oldest retired IR blocks

Code: the R6 commit on top of `c56db0f`. At the IR cap the cache releases the least recently retired IR blocks (across PCs) until the IR is below 7/8 of the cap, and evicts the live IR-only blocks only if it is still at the cap with the pool empty. The releases are timed (`irPoolReleaseMs`, not part of `compileMs`). One session: Burning Rangers long with this build and with the pre-R4 build (`12ac9ab`, built aside) interleaved (R6, pre-R4, interpreter, R6, pre-R4), then Street Fighter Zero 3 three rounds (interpreter, then x64). All runs exit 0, `compileFallbacks 0`. The sampler read 34% on average (9-68%, 65 samples). Rows "2E E (R6)" in the table.

| Run | Interpreter | x64 | Ratio | Worst frame, x64 / interpreter | Frames over 16.7 ms, x64 / interpreter | Front-end builds in window | Pool releases in window (ms) | IR reuses |
|---|---|---|---|---|---|---|---|---|
| Street Fighter Zero 3, rounds 1 / 2 / 3 | 4.743 / 4.754 / 4.901 | 4.623 / 4.898 / 4.952 | 1.026 / 0.971 / 0.990 (median 0.971) | 14.9 / 15.8 / 20.3, interpreter 13.0 | 0 / 0 / 3, interpreter 0 | 10,686 (as Task 4) | 1 (0.3) | 0 |
| Burning Rangers long, R6, pair 1 | 6.090 | 6.521 | 0.934 | 55.6 / 20.5 | 1,221 / 11 | 324,587 | 535 (5.7) | 879,102 |
| Burning Rangers long, pre-R4, pair 1 | 6.090 | 6.380 | 0.955 | 48.2 / 20.5 | 990 / 11 | 321,300 | - | 894,608 |
| Burning Rangers long, R6, pair 2 | 6.090 | 6.434 | 0.947 | 51.0 / 20.5 | 1,079 / 11 | 328,796 | 560 (4.5) | 883,623 |
| Burning Rangers long, pre-R4, pair 2 | 6.090 | 6.185 | 0.985 | 53.3 / 20.5 | 773 / 11 | 316,524 | - | 891,780 |

- **The releases cost nothing measurable** (4.5-5.7 ms in a 33,600-frame window). They still happen 535-560 times, because in Burning Rangers the live IR alone stays above 7/8 of the cap: each release empties the pool without getting below 7/8, so R6 behaves like R4 there.
- **Burning Rangers: the pre-R4 rule is slightly better in both interleaved pairs.** R6 has 2-4% more front-end builds (324,587-328,796 against 316,524-321,300), 1-2% fewer IR reuses, and 1,079-1,221 frames over 16.7 ms against 773-990; the SH-2 total is 2-4% higher (ratio 0.934-0.947 against 0.955-0.985). With the old rule, the one eviction of the live IR-only blocks (once in the window) leaves room for a large pool afterwards. With R4/R6 the live IR is never evicted, so the pool stays small (it is emptied at every cap hit) and reuses less.
- **Street Fighter Zero 3** keeps Task 4's work exactly (10,686 builds, 6,333 native compiles, no IR eviction, one 0.3 ms release). Its ratio (median 0.971, rounds 0.971-1.026) and round 3's 3 frames over 16.7 ms are this session's noise: the compile work is identical in every round.

**Verdict for R6:** the release is cheap and does what it should when the pool is large enough (Street Fighter Zero 3, and the cap test). In Burning Rangers the live IR alone fills more than 7/8 of the cap, so no release can get below the target, and keeping the live IR costs it a little against the pre-R4 rule. A variant would evict the live IR-only blocks as well when releasing the whole pool still leaves the IR at or above 7/8 of the cap (rather than at or above the cap); that would be the old behaviour for Burning Rangers and R6's for Street Fighter Zero 3.

### Task 5 (R7): live IR evicted when the pool cannot free enough

Code: the R7 commit on top of `b6f76ed`. At the IR cap the oldest retired IR blocks are released toward 7/8 of the cap as in R6. If the IR is still at 7/8 of the cap or more with the pool empty, the live IR-only blocks are evicted too (`EvictIrOnly`). One session: Burning Rangers long with this build and the pre-R4 build (`12ac9ab`) interleaved (R7, pre-R4, interpreter, R7, pre-R4), then three rounds of Street Fighter Zero 3, Panzer Dragoon II Zwei and Sega Rally (interpreter, then x64). All runs exit 0, `compileFallbacks 0`. WSL shut down before the batch; the sampler read 28% on average (4-74%, 76 samples), the quietest of the Task 5 sessions. Rows "2E E (R7)" in the table.

| Run | Interpreter | x64 | Ratio | Worst frame, x64 / interpreter | Frames over 16.7 ms, x64 / interpreter | Front-end builds in window | IR evictions / pool releases (whole run) |
|---|---|---|---|---|---|---|---|
| Street Fighter Zero 3, rounds 1 / 2 / 3 | 4.624 / 4.463 / 4.576 | 4.578 / 4.496 / 4.679 | 1.010 / 0.993 / 0.978 (medians: 1.000) | 23.2 / 20.8 / 22.1, interpreter 11.5 | 1 / 1 / 1, interpreter 0 | 15,336-15,349 | 1 / 1 |
| Panzer Dragoon II Zwei, rounds 1 / 2 / 3 | 5.091 / 5.126 / 5.016 | 4.450 / 4.465 / 6.406 | 1.144 / 1.148 / 0.783 (medians: 1.140) | 10.0 / 9.7 / 12.7, interpreter 11.0 | 0, interpreter 0 | 187 | 0 / 0 |
| Sega Rally Championship, rounds 1 / 2 / 3 | 4.623 / 4.694 / 4.729 | 4.792 / 4.866 / 5.016 | 0.965 / 0.965 / 0.943 (medians: 0.965) | 10.6 / 12.1 / 12.8, interpreter 10.7 | 0, interpreter 0 | 502 | 0 / 0 |
| Burning Rangers long, R7, pair 1 | 5.961 | 6.210 | 0.960 | 47.9 / 16.4 | 599 / 0 | 315,376 | 1 / 1 |
| Burning Rangers long, pre-R4, pair 1 | 5.961 | 6.180 | 0.965 | 47.7 / 16.4 | 558 / 0 | 316,238 | 1 / - |
| Burning Rangers long, R7, pair 2 | 5.961 | 6.238 | 0.956 | 49.5 / 16.4 | 585 / 0 | 320,512 | 1 / 1 |
| Burning Rangers long, pre-R4, pair 2 | 5.961 | 6.164 | 0.967 | 47.2 / 16.4 | 615 / 0 | 319,588 | 1 / - |

- **Burning Rangers: R7 matches the pre-R4 rule** in both interleaved pairs: the same front-end builds (315-321 K), IR reuses (889-894 K), frames over 16.7 ms (585-599 against 558-615), worst frames (48-50 against 47-48 ms) and ratios (0.956-0.960 against 0.965-0.967) within run-to-run spread. The cap now releases the pool once (0.4 ms) and evicts once (15,748 blocks), exactly where the pre-R4 rule evicted.
- **Street Fighter Zero 3 is back to the pre-R4 numbers.** After its single release, its live IR alone is still above 7/8 of the cap, so R7 also evicts the live IR-only blocks. That gives 15,336-15,349 builds in the window (R4/R6: 10,686; pre-R4: 15,340-15,347), 5,842 native compiles (R4/R6: 6,333), worst frames of 20.8-23.2 ms (R6: 14.9-20.3) and 1 frame over 16.7 ms per round. The ratio is 1.000 (medians) in this quiet session.
- **Panzer Dragoon II Zwei and Sega Rally** do not reach the IR cap (no release, no eviction) and build exactly as in Task 4 (187 and 502). Their ratios (1.140 and 0.965 from the medians) are their usual ones. Panzer Dragoon II Zwei's round 3 x64 run (6.406 ms) is a host outlier: the same executed blocks as round 2 (`blocksRun` identical, no compile work), but the master's SH-2 time is 57% higher.

**Verdict for R7:** Burning Rangers and Street Fighter Zero 3 want opposite things at the cap. Both have live IR between 7/8 of the cap and the cap. Burning Rangers runs better when that live IR is evicted (room for a large pool; R7 and pre-R4). Street Fighter Zero 3 runs better when it is kept (R4/R6: 30% fewer builds, worst frame about 6 ms lower). No eviction threshold between 7/8 and the cap serves both; a larger IR cap (outside this plan's constraint) or a live-IR eviction policy finer than all-or-nothing would.

### Task 5 (R8): the IR cap evicts the least recently used IR blocks

Code: the R8 commit on top of `4a35b03`, replacing the R4/R6/R7 rule. Every IR-only block carries a last-use stamp (a cache-local counter, set whenever `Get` returns it: a hit, a build or a reuse from the IR pool; a retired block keeps its stamp). At the IR cap, IR blocks are freed by stamp, oldest first, live and retired together, until the IR is below 7/8 of the cap. A weighted selection with `std::nth_element` on halving ranges does it in linear time on average. `irEvictions` counts cap events, its `blocks` the live blocks freed, `irPoolFreed` the retired ones; `irCapMs` is the cap events' emulation-thread time. One session (load 5-12% before; sampler 28% on average, 4-79%, 77 samples): Burning Rangers long with this build and the R7 build (`4a35b03`) interleaved, two each, plus one interpreter run; then three rounds of Street Fighter Zero 3, Panzer Dragoon II Zwei and Sega Rally (interpreter, then x64). All runs exit 0, `compileFallbacks 0`. Rows "2E E (R8)" in the table.

| Run | Interpreter | x64 | Ratio | Worst frame, x64 / interpreter | Frames over 16.7 ms, x64 / interpreter | Front-end builds in window | Cap events: live / pool blocks freed (ms) |
|---|---|---|---|---|---|---|---|
| Street Fighter Zero 3, rounds 1 / 2 / 3 | 4.613 / 4.727 / 4.647 | 4.570 / 4.721 / 4.672 | 1.009 / 1.001 / 0.995 (medians: 0.995) | 14.6 / 14.7 / 15.2, interpreter 12.2 | 0 / 0 / 0, interpreter 0 | 10,686 (Task 4: 10,686) | 1: 1,045 / 281 (3.0-3.2) |
| Panzer Dragoon II Zwei, rounds 1 / 2 / 3 | 5.048 / 5.013 / 5.004 | 4.437 / 4.488 / 4.605 | 1.138 / 1.117 / 1.087 (medians: 1.117) | 9.3 / 10.9 / 9.4, interpreter 11.3 | 0, interpreter 0 | 187 | none |
| Sega Rally Championship, rounds 1 / 2 / 3 | 4.704 / 4.708 / 4.788 | 5.110 / 4.991 / 4.968 | 0.921 / 0.943 / 0.964 (medians: 0.943) | 13.3 / 13.4 / 12.1, interpreter 13.1 | 0, interpreter 0 | 502 | none |
| Burning Rangers long, R8, pair 1 | 5.975 | 6.298 | 0.949 | 50.0 / 16.8 | 652 / 1 | 305,208 | 1: 853 / 409 (5.0) |
| Burning Rangers long, R7, pair 1 | 5.975 | 6.308 | 0.947 | 46.3 / 16.8 | 827 / 1 | 315,226 | 1: 15,748 live (all) + pool |
| Burning Rangers long, R8, pair 2 | 5.975 | 6.357 | 0.940 | 49.1 / 16.8 | 902 / 1 | 306,607 | 1: 853 / 409 (4.2) |
| Burning Rangers long, R7, pair 2 | 5.975 | 6.246 | 0.957 | 50.0 / 16.8 | 606 / 1 | 315,240 | 1: 15,750 live (all) + pool |

- **Street Fighter Zero 3 is back to Task 4's work and smoothness.** The window's front-end builds and native compiles are exactly Task 4's (10,686 and 6,333 in every round), and no frame is over 16.7 ms, as for the interpreter. The worst frames (14.6-15.2 ms) are as in Task 4 (14.5-14.8). Its one cap event frees 1,045 live IR-only blocks and 281 retired ones (the least recently used 1/8 of the IR) in 3 ms, instead of all 13,550 live blocks.
- **Burning Rangers: at least as good as R7.** One cap event frees 853 live and 409 retired blocks (5 ms) instead of every live IR-only block. That saves 3% of the front-end builds in the window (305-307 K against 315 K) and gives a few more IR reuses (889 K against 877-885 K). The frames over 16.7 ms are 652 / 902 against 827 / 606 (each build wins one pair); the ratio (0.940-0.949 against 0.947-0.957) and worst frames (49-50 against 46-50 ms) are within run-to-run spread.
- **Panzer Dragoon II Zwei and Sega Rally** never reach the IR cap; their builds are Task 4's (187 and 502) and their ratios their usual ones (1.117 and 0.943 from the medians; Rally's rounds spread 0.921-0.964).

**Verdict for R8:** the least recently used eviction gives Street Fighter Zero 3 R4/R6's result (no regression from Task 5, no slow frame) and Burning Rangers R7's (or slightly less work), so it serves both. The remaining Burning Rangers stutter is the startup burst of new code at frame 9,264 and the steady stream of genuinely new blocks, not the IR cap.

## Milestone 2E results

**Date**: 2026-10-20/21
**Commit**: 5f2df05 (the head of `feature/sh2-jit-2e` before the commit that adds this section: tasks 1-5, the compile worker, IR-block reuse and the least-recently-used IR-cap rule R8; no source change since). The measured executable (`build-bench` at that commit, Ninja, Release, `BRIMIR_LTO=ON`, `Brimir_ENABLE_IPO=ON`, MSVC 19.44) was copied aside before the runs.
**Machine**: AMD Ryzen 7 5700G (8 cores / 16 threads), Windows, the 2C method. Defaults: the x64 runs use the compile worker (`SH2 JIT compile : worker thread` in every log); `--jit-sync-compile` rows are for reference.

**Verdict: the default-on bar (`sh2-jit-m2e.md` section 3) is not met.** Conditions 1 and 2 fail, 3 and 4 hold (condition by condition below):

- **Faster (1):** interpreter / x64 SH-2 total is above 1.0 on five of the six titles (1.008-1.281), and below it on Sega Rally (0.957, 0.958 in the second session). The long Burning Rangers run is at 0.969. Burning Rangers' 1,800-frame window (1.008) is within 0.02 of 1.0: a second session reads 1.014, so it is above 1.0 in both.
- **Smooth (2):** the long Burning Rangers run has 571 frames over 16.7 ms against 1 for the interpreter (worst frame 48.3 ms against 17.5 ms), and the x64 average frame time is above the interpreter's on Sega Rally (+8.1%), Burning Rangers (+2.9%), Street Fighter Zero 3 (+3.2%) and the long run (+5.6%), although its SH-2 total is lower on two of them. Street Fighter Zero 3's compile bursts are gone (0 frames over 16.7 ms in the 1,800-frame window, worst frames 13.8-14.8 ms against 23.1-24.2 ms in track A).
- **What the worker bought:** Street Fighter Zero 3 went from 0.941 (track A) to 1.033 (its x64 SH-2 total fell 11.8% while the interpreter's fell 3.3%), the long Burning Rangers run from 0.882 to 0.969 and its worst frame from 92.1 to 48.3 ms. The other titles are unchanged within the session spread.

Step 4 of the plan (the rollout) did not run: the JIT stays off by default, and `brimir_sh2_jit` has no `auto` value. Sega Rally fails condition 1 and four windows fail condition 2, so the generated-code profile (`### 2E generated-code profile` below) was done, on Sega Rally, Panzer Dragoon II Zwei, Street Fighter Zero 3 and Burning Rangers' 1,800-frame window.

### Method

The 2C method, as in 2D: `--warmup 2400 --frames 1800` for the games and `--warmup 300 --frames 1800` for the BIOS menu, default settings, a scratch system dir with `brimir_saturn_rtc_us_eu.smpc` also as `brimir_saturn_rtc_jp.smpc`, three rounds, each round running every title in the interpreter, `--sh2-jit --jit-backend ir` and `--sh2-jit --jit-backend x64` (worker) back to back (63 runs), plus one round of `--sh2-jit --jit-backend x64 --jit-sync-compile` (7 runs, reference), and the long Burning Rangers run (`--warmup 2400 --frames 33600`, US BIOS) in the interpreter, x64 with the worker and x64 with synchronous compiles (one run each). All 73 runs exited 0 and every x64 run reports `compileFallbacks 0` and `flushes codeCap 0 / 0`. The table shows the run with the median SH2 total per mode and the three runs' SH2 totals, max frame times and frames over 16.7 ms. Scripts and raw outputs: `%TEMP%\opencode\t12` (`measure.ps1`, `measure-long.ps1`, `parse.ps1`, logs in `meas`, `meas-long`, `meas-s2`).

**Load.** The development PC read 1% CPU load before the batch and nothing but the usual desktop applications and the measurement ran. A 30-second sampler of the whole-machine load over the first session (it reads the benchmark's own threads too: threaded VDP, emulation thread, compile worker) shows 31% on average (1-85%, 127 samples); the second session (below) 33% (4-53%, 20 samples). The lockstep runs (35 runs, four at a time) and the Raspberry Pi work had finished or ran on another machine. The interpreter totals of this session are 0.9-5.1% below track A's, the x64 totals -6.4% to +0.6% (Street Fighter Zero 3 -11.8% because of the worker), so ratios are compared only with this session's own runs.

### Results (session 1)

| Title | Mode | ms/frame avg | p95 | max | SH2 master ms (%) | SH2 slave ms (%) | SH2 total ms (%) | SH2 total, 3 runs | max frame ms, 3 runs | frames over 16.7 ms, 3 runs |
|---|---|---|---|---|---|---|---|---|---|---|
| BIOS menu (US) | interpreter | 7.964 | 8.402 | 10.4 | 1.643 (20.6%) | 0.000 (0.0%) | 1.643 (20.6%) | 1.643 / 1.639 / 1.655 | 10.4 / 9.2 / 9.2 | 0 / 0 / 0 |
|  | IR | 8.811 | 9.219 | 11.1 | 6.525 (74.1%) | 0.000 (0.0%) | 6.525 (74.1%) | 6.495 / 6.555 / 6.525 | 12.1 / 12.4 / 11.1 | 0 / 0 / 0 |
|  | x64 (worker) | 7.830 | 8.284 | 9.2 | 1.209 (15.4%) | 0.000 (0.0%) | 1.209 (15.4%) | 1.212 / 1.204 / 1.209 | 8.9 / 10.0 / 9.2 | 0 / 0 / 0 |
|  | x64 (sync compile) | 7.886 | 8.637 | 11.0 | 1.257 (15.9%) | 0.000 (0.0%) | 1.257 (15.9%) | 1.257 | 11.0 | 0 |
| Virtua Fighter 2 (JP) | interpreter | 6.240 | 7.545 | 10.1 | 2.766 (44.3%) | 1.646 (26.4%) | 4.413 (70.7%) | 4.565 / 4.393 / 4.413 | 10.9 / 9.7 / 10.1 | 0 / 0 / 0 |
|  | IR | 19.571 | 24.025 | 33.3 | 10.428 (53.3%) | 7.222 (36.9%) | 17.650 (90.2%) | 17.934 / 17.650 / 17.558 | 31.5 / 33.3 / 31.5 | 1616 / 1504 / 1449 |
|  | x64 (worker) | 5.733 | 6.491 | 10.3 | 2.278 (39.7%) | 1.167 (20.4%) | 3.445 (60.1%) | 3.528 / 3.444 / 3.445 | 10.2 / 8.9 / 10.3 | 0 / 0 / 0 |
|  | x64 (sync compile) | 5.772 | 6.701 | 10.5 | 2.345 (40.6%) | 1.160 (20.1%) | 3.506 (60.8%) | 3.506 | 10.5 | 0 |
| Panzer Dragoon II Zwei (US) | interpreter | 6.393 | 7.479 | 10.1 | 3.133 (49.0%) | 1.752 (27.4%) | 4.885 (76.4%) | 5.027 / 4.847 / 4.885 | 10.2 / 8.8 / 10.1 | 0 / 0 / 0 |
|  | IR | 17.997 | 20.059 | 29.8 | 10.527 (58.5%) | 5.511 (30.6%) | 16.038 (89.1%) | 16.325 / 16.038 / 15.998 | 25.6 / 29.8 / 24.6 | 1635 / 1188 / 1178 |
|  | x64 (worker) | 6.078 | 7.650 | 10.6 | 2.898 (47.7%) | 1.464 (24.1%) | 4.361 (71.8%) | 4.468 / 4.354 / 4.361 | 10.6 / 10.6 / 10.6 | 0 / 0 / 0 |
|  | x64 (sync compile) | 6.067 | 7.692 | 12.3 | 2.916 (48.1%) | 1.426 (23.5%) | 4.342 (71.6%) | 4.342 | 12.3 | 0 |
| Sega Rally Championship (US) | interpreter | 7.191 | 8.285 | 10.1 | 2.577 (35.8%) | 2.031 (28.2%) | 4.607 (64.1%) | 5.022 / 4.607 / 4.585 | 11.2 / 10.1 / 10.6 | 0 / 0 / 0 |
|  | IR | 18.404 | 26.612 | 33.1 | 9.666 (52.5%) | 6.676 (36.3%) | 16.342 (88.8%) | 16.682 / 16.342 / 16.290 | 31.6 / 33.1 / 30.8 | 901 / 903 / 902 |
|  | x64 (worker) | 7.777 | 9.633 | 13.6 | 2.522 (32.4%) | 2.293 (29.5%) | 4.815 (61.9%) | 4.936 / 4.813 / 4.815 | 11.9 / 14.3 / 13.6 | 0 / 0 / 0 |
|  | x64 (sync compile) | 7.777 | 9.760 | 12.2 | 2.518 (32.4%) | 2.247 (28.9%) | 4.765 (61.3%) | 4.765 | 12.2 | 0 |
| Burning Rangers (US) | interpreter | 8.668 | 9.870 | 17.0 | 4.216 (48.6%) | 2.082 (24.0%) | 6.298 (72.7%) | 6.362 / 6.298 / 6.267 | 13.1 / 17.0 / 12.1 | 0 / 1 / 0 |
|  | IR | 24.128 | 25.544 | 33.1 | 14.176 (58.8%) | 7.245 (30.0%) | 21.421 (88.8%) | 22.091 / 21.395 / 21.421 | 30.6 / 29.7 / 33.1 | 1800 / 1800 / 1800 |
|  | x64 (worker) | 8.923 | 10.551 | 14.0 | 4.528 (50.7%) | 1.718 (19.3%) | 6.245 (70.0%) | 6.419 / 6.245 / 6.198 | 15.5 / 14.0 / 18.2 | 0 / 0 / 1 |
|  | x64 (sync compile) | 8.845 | 10.622 | 13.9 | 4.503 (50.9%) | 1.668 (18.9%) | 6.171 (69.8%) | 6.171 | 13.9 | 0 |
| Guardian Heroes (US) | interpreter | 6.199 | 7.195 | 9.1 | 2.468 (39.8%) | 1.522 (24.5%) | 3.989 (64.4%) | 4.038 / 3.944 / 3.989 | 10.1 / 8.7 / 9.1 | 0 / 0 / 0 |
|  | IR | 16.079 | 19.882 | 25.9 | 8.662 (53.9%) | 5.054 (31.4%) | 13.716 (85.3%) | 14.117 / 13.648 / 13.716 | 29.8 / 22.0 / 25.9 | 467 / 384 / 386 |
|  | x64 (worker) | 6.127 | 6.955 | 8.1 | 2.060 (33.6%) | 1.445 (23.6%) | 3.506 (57.2%) | 3.445 / 3.506 / 3.516 | 9.5 / 8.1 / 9.8 | 0 / 0 / 0 |
|  | x64 (sync compile) | 6.085 | 6.873 | 8.4 | 2.054 (33.8%) | 1.393 (22.9%) | 3.448 (56.7%) | 3.448 | 8.4 | 0 |
| Street Fighter Zero 3 (JP, 4 MB cart) | interpreter | 8.949 | 9.662 | 12.0 | 2.618 (29.3%) | 1.897 (21.2%) | 4.515 (50.5%) | 4.515 / 4.487 / 4.518 | 12.0 / 11.1 / 11.4 | 0 / 0 / 0 |
|  | IR | 18.485 | 21.870 | 25.3 | 10.187 (55.1%) | 6.260 (33.9%) | 16.447 (89.0%) | 16.447 / 16.442 / 16.501 | 25.3 / 28.8 / 27.3 | 1611 / 1612 / 1632 |
|  | x64 (worker) | 9.238 | 10.140 | 14.1 | 2.967 (32.1%) | 1.404 (15.2%) | 4.371 (47.3%) | 4.412 / 4.371 / 4.317 | 14.8 / 14.1 / 13.8 | 0 / 0 / 0 |
|  | x64 (sync compile) | 9.653 | 11.582 | 23.7 | 3.422 (35.5%) | 1.358 (14.1%) | 4.780 (49.5%) | 4.780 | 23.7 | 17 |

### Ratios (SH2 total, median runs) and track A

| Title | interpreter / x64 (2E, worker) | track A | change | x64 SH2 total 2E / track A | change | interpreter SH2 total 2E / track A | change | interpreter / x64, sync compile (2E, one round) | IR / x64 (2E) |
|---|---|---|---|---|---|---|---|---|---|
| BIOS menu (reference) | **1.359** | 1.340 | +0.019 | 1.209 / 1.292 | -6.4% | 1.643 / 1.731 | -5.1% | 1.307 | 5.40 |
| Virtua Fighter 2 | **1.281** | 1.274 | +0.007 | 3.445 / 3.494 | -1.4% | 4.413 / 4.453 | -0.9% | 1.259 | 5.12 |
| Panzer Dragoon II Zwei | **1.120** | 1.137 | -0.017 | 4.361 / 4.335 | +0.6% | 4.885 / 4.927 | -0.9% | 1.125 | 3.68 |
| Sega Rally Championship | **0.957** | 0.967 | -0.010 | 4.815 / 4.845 | -0.6% | 4.607 / 4.687 | -1.7% | 0.967 | 3.39 |
| Burning Rangers | **1.008** | 1.012 | -0.004 | 6.245 / 6.332 | -1.4% | 6.298 / 6.405 | -1.7% | 1.021 | 3.43 |
| Guardian Heroes | **1.138** | 1.144 | -0.006 | 3.506 / 3.537 | -0.9% | 3.989 / 4.047 | -1.4% | 1.157 | 3.91 |
| Street Fighter Zero 3 | **1.033** | 0.941 | +0.092 | 4.371 / 4.958 | -11.8% | 4.515 / 4.667 | -3.3% | 0.945 | 3.76 |

The BIOS menu is not one of the six titles. Against track A (a different session) Panzer Dragoon II Zwei, Sega Rally, Burning Rangers and Guardian Heroes read 0.004-0.017 lower and Virtua Fighter 2 0.007 higher: the x64 totals of these five are within -1.4% to +0.6% of track A's and the interpreters' 0.9-1.7% lower, i.e. the session and not the code, as expected (tasks 1-5 change compiling, not steady state). Street Fighter Zero 3 is the one title whose steady state changed: the 6,333 native compiles of its window run on the worker, and its x64 SH2 total falls from 4.958 to 4.371 ms/frame. `--jit-sync-compile` gives 0.945 for it in this session, so the 1.033 comes from the worker (the same code in the same session, synchronous: x64 4.780 against 4.371 ms/frame).

### Compile work and the worker (session 1, median x64 run)

| Title | interpreter / x64 (worker) | per-round ratios (interp r / x64 r) | IR / x64 | interpreter / IR | interpreter / x64 (sync) | x64 fallbacks m/s | worker ms/frame (med run) | native compiles in window (med run) | emulation-thread compile ms | async / dropped | flushes in window | code-cap flushes m/s (since start) | IR reuses |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| BIOS menu (US) | 1.359 | 1.356 / 1.361 / 1.369 | 5.397 | 0.252 | 1.307 | 0/0 | 0.071 | 473 | 29.7 | 473/0 | 0 | 0/0 | 0 |
| Virtua Fighter 2 (JP) | 1.281 | 1.294 / 1.276 / 1.281 | 5.123 | 0.250 | 1.259 | 0/0 | 0.080 | 959 | 33.1 | 959/0 | 0 | 0/0 | 0 |
| Panzer Dragoon II Zwei (US) | 1.120 | 1.125 / 1.113 / 1.120 | 3.678 | 0.305 | 1.125 | 0/0 | 0.056 | 660 | 26.4 | 660/0 | 0 | 0/0 | 0 |
| Sega Rally Championship (US) | 0.957 | 1.017 / 0.957 / 0.952 | 3.394 | 0.282 | 0.967 | 0/0 | 0.092 | 1056 | 43.6 | 1056/0 | 0 | 0/0 | 0 |
| Burning Rangers (US) | 1.008 | 0.991 / 1.008 / 1.011 | 3.430 | 0.294 | 1.021 | 0/0 | 0.067 | 821 | 34.5 | 821/0 | 0 | 0/0 | 0 |
| Guardian Heroes (US) | 1.138 | 1.172 / 1.125 / 1.135 | 3.912 | 0.291 | 1.157 | 0/0 | 0.061 | 667 | 27.3 | 667/0 | 0 | 0/0 | 0 |
| Street Fighter Zero 3 (JP, 4 MB cart) | 1.033 | 1.023 / 1.027 / 1.047 | 3.763 | 0.275 | 0.945 | 0/0 | 0.538 | 6333 | 243.4 | 6333/0 | 0 | 0/0 | 0 |

The worker's CPU time is 0.056-0.092 ms/frame on five of the six titles and 0.538 ms/frame on Street Fighter Zero 3 (6,333 native compiles, 243 ms of Prepare and Commit on the emulation thread against 1,191-1,234 ms synchronously in task 4); the long Burning Rangers run uses 0.380 ms/frame (28,895 native compiles submitted: 9,691 installed and 19,204 dropped on arrival because their block had been invalidated meanwhile).

### Second session (titles within 0.02 of 1.0, and the two others at or near it)

Run after the profile's sample runs, the same executable, interpreter and x64 (worker), three rounds each, `--warmup 2400 --frames 1800` (18 runs, all exit 0, load 3% before, sampler average 33%).

| Title | Mode | ms/frame avg | p95 | max | SH2 master ms (%) | SH2 slave ms (%) | SH2 total ms (%) | SH2 total, 3 runs | max frame ms, 3 runs | frames over 16.7 ms, 3 runs |
|---|---|---|---|---|---|---|---|---|---|---|
| Sega Rally Championship (US) | interpreter | 7.130 | 8.230 | 11.6 | 2.567 (36.0%) | 2.018 (28.3%) | 4.584 (64.3%) | 4.658 / 4.584 / 4.572 | 11.6 / 11.6 / 10.8 | 0 / 0 / 0 |
|  | x64 (worker) | 7.771 | 9.612 | 12.4 | 2.497 (32.1%) | 2.290 (29.5%) | 4.787 (61.6%) | 4.787 / 4.791 / 4.778 | 12.4 / 14.1 / 11.9 | 0 / 0 / 0 |
| Burning Rangers (US) | interpreter | 8.606 | 9.753 | 12.3 | 4.179 (48.6%) | 2.070 (24.1%) | 6.249 (72.6%) | 6.328 / 6.249 / 6.232 | 14.2 / 12.3 / 15.0 | 0 / 0 / 0 |
|  | x64 (worker) | 8.821 | 10.547 | 15.5 | 4.452 (50.5%) | 1.713 (19.4%) | 6.165 (69.9%) | 6.200 / 6.165 / 6.159 | 12.4 / 15.5 / 12.4 | 0 / 0 / 0 |
| Street Fighter Zero 3 (JP, 4 MB cart) | interpreter | 8.934 | 9.671 | 11.1 | 2.646 (29.6%) | 1.910 (21.4%) | 4.556 (51.0%) | 4.531 / 4.556 / 4.606 | 11.1 / 11.1 / 11.3 | 0 / 0 / 0 |
|  | x64 (worker) | 9.237 | 10.112 | 19.1 | 2.946 (31.9%) | 1.390 (15.0%) | 4.335 (46.9%) | 4.323 / 4.335 / 4.436 | 14.5 / 19.1 / 15.6 | 0 / 1 / 0 |

| Title | interpreter / x64 (worker) | per-round ratios (interp r / x64 r) | IR / x64 | interpreter / IR | interpreter / x64 (sync) | x64 fallbacks m/s | worker ms/frame (med run) | native compiles in window (med run) | emulation-thread compile ms | async / dropped | flushes in window | code-cap flushes m/s (since start) | IR reuses |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Sega Rally Championship (US) | 0.958 | 0.973 / 0.957 / 0.957 | - | - | - | 0/0 | 0.094 | 1056 | 43.4 | 1056/0 | 0 | 0/0 | 0 |
| Burning Rangers (US) | 1.014 | 1.021 / 1.014 / 1.012 | - | - | - | 0/0 | 0.068 | 821 | 33.1 | 821/0 | 0 | 0/0 | 0 |
| Street Fighter Zero 3 (JP, 4 MB cart) | 1.051 | 1.048 / 1.051 / 1.038 | - | - | - | 0/0 | 0.538 | 6333 | 242.4 | 6333/0 | 0 | 0/0 | 0 |

| Title | session 1 | session 2 | Task 5's R8 session (same code) | Rule |
|---|---|---|---|---|
| Burning Rangers (within 0.02 of 1.0 in session 1) | 1.008 (rounds 0.991 / 1.008 / 1.011) | 1.014 (1.021 / 1.014 / 1.012) | n/a (task 5 measured the long run only) | above 1.0 in both: **passes** by 0.008 and 0.014 |
| Street Fighter Zero 3 | 1.033 | 1.051 | 0.995 (1.009 / 1.001 / 0.995) | not within 0.02 here, above 1.0 in both; its margin is inside the session spread (the R8 session read below 1.0 with this code) |
| Sega Rally Championship | 0.957 | 0.958 | 0.943 | below 1.0 in all three: **fails** |

### Frames over 16.7 ms and average frame time (condition 2, session 1)

Frames over 16.7 ms in each x64 run must be at most the highest count of the interpreter runs of the same window, and the x64 average whole-frame time (median run by SH2 total) at most the interpreter's. Burning Rangers' long run is in its own table below.

| Title | interpreter: frames over 16.7 ms (3 runs), highest | x64: frames over 16.7 ms (3 runs) | rule 1 | interpreter avg frame ms (median run) | x64 avg frame ms (median run) | x64 / interpreter | rule 2 | non-SH2 time in Ymir_RunFrame, interpreter -> x64 (ms) |
|---|---|---|---|---|---|---|---|---|
| BIOS menu (reference) | 0 / 0 / 0, 0 | 0 / 0 / 0 | pass | 7.964 | 7.830 | 0.983 | pass | 6.32 -> 6.62 |
| Virtua Fighter 2 | 0 / 0 / 0, 0 | 0 / 0 / 0 | pass | 6.240 | 5.733 | 0.919 | pass | 1.83 -> 2.29 |
| Panzer Dragoon II Zwei | 0 / 0 / 0, 0 | 0 / 0 / 0 | pass | 6.393 | 6.078 | 0.951 | pass | 1.51 -> 1.72 |
| Sega Rally Championship | 0 / 0 / 0, 0 | 0 / 0 / 0 | pass | 7.191 | 7.777 | 1.081 | **FAIL** | 2.58 -> 2.96 |
| Burning Rangers | 0 / 1 / 0, 1 | 0 / 0 / 1 | pass | 8.668 | 8.923 | 1.029 | **FAIL** | 2.37 -> 2.68 |
| Guardian Heroes | 0 / 0 / 0, 0 | 0 / 0 / 0 | pass | 6.199 | 6.127 | 0.988 | pass | 2.21 -> 2.62 |
| Street Fighter Zero 3 | 0 / 0 / 0, 0 | 0 / 0 / 0 | pass | 8.949 | 9.238 | 1.032 | **FAIL** | 4.43 -> 4.87 |

Session 2 (the three titles above): Sega Rally 7.130 -> 7.771 ms (+9.0%), Burning Rangers 8.606 -> 8.821 (+2.5%), Street Fighter Zero 3 8.934 -> 9.237 (+3.4%): the average-frame rule fails on all three in this session too. Frames over 16.7 ms: 0 / 0 / 0 on Sega Rally and Burning Rangers; Street Fighter Zero 3 has one frame over 16.7 ms in one run (frame 2,449, 19.1 ms, with 646 front-end builds, 60 native compiles and 5.9 ms of compiling: a burst of new code), against 0 for the interpreter, so the first rule fails there by one frame in that session (0 / 0 / 0 in session 1).

**The average frame is not the SH-2 total.** In every title the part of `Ymir_RunFrame` that is not SH-2 time is 0.2-0.46 ms larger with the JIT than with the interpreter (last column above; the same rise appears with `--jit-sync-compile`: Rally 3.01, Burning Rangers 2.67, Street Fighter Zero 3 4.87 ms, so it is not the worker thread). On Street Fighter Zero 3 it more than cancels the SH-2 gain (-0.14 ms of SH-2 total, +0.43 ms elsewhere). The cause is not established. Candidates, none tested: cache and TLB pressure from tens of MB of generated code on the code that the interpreter shares with the rest of the emulation thread; the threaded VDP renderer absorbing slack (the BIOS menu's frame time is 7.8-8.0 ms in both the interpreter and x64 although its SH-2 time differs by 0.4 ms, and 8.8 ms with the IR backend's 6.5 ms of SH-2 time). It means the SH-2 total, which is what the bar measures (`sh2-jit-m2e.md` section 3, condition 1), overstates the whole-frame gain by 0.2-0.5 ms/frame.

### Long run: Burning Rangers, frames 2,400-36,000

`--warmup 2400 --frames 33600`, one run per mode, default settings, interpreter, x64 (worker) and x64 (`--jit-sync-compile`) one after the other after the table runs of the session. All three exited 0.

| Burning Rangers, frames 2,400-36,000 | ms/frame avg | p95 | p99 | max | frames over 16.7 ms | SH2 total ms/frame | interpreter / x64 |
|---|---|---|---|---|---|---|---|
| interpreter | 9.380 | 11.525 | 13.914 | 17.5 | 1 | 5.954 | - |
| x64, compile worker (default) | 9.907 | 15.132 | 17.434 | 48.3 | 571 | 6.147 | 0.969 |
| x64, `--jit-sync-compile` | 10.118 | 16.641 | 21.883 | 93.2 | 1661 | 6.375 | 0.934 |

| Long run, window counters | x64 worker | x64 sync | track A (e8adf69, sync) |
|---|---|---|---|
| Front-end builds | 306,207 | 305,401 | 1,149,860 |
| Native compiles in the window | 28,895 submitted (9,691 installed + 19,204 dropped on arrival) | 44,193 | 42,871 |
| Compile time on the emulation thread | 5.7 s (0.170 ms/frame) | 16.7 s (0.497 ms/frame) | 24.0 s (0.714 ms/frame) |
| Worker CPU time | 0.380 ms/frame | - | - |
| Frames with compiling | 8,295 | 8,294 | 9,642 |
| Code-cap flushes (master / slave, since start) | 0 / 0 | 0 / 0 | 0 / 0 |
| Peak native code, master / slave | 76.9 / 6.5 MB | 76.9 / 6.6 MB | 74.0 / 6.6 MB |
| Retired native reuses | 832,749 | 885,900 | 885,877 |
| IR block reuses | 888,236 | 835,891 | n/a |
| IR-cap events (window) / IR cap time | 1 / 4.5 ms (409 retired IR blocks freed) | 1 / 4.0 ms | 1 (master, 16,843 IR-only blocks) |
| Worst frame | 48.3 ms | 93.2 ms | 92.1 ms |

Earlier same-code sessions (task 5's, R8): 0.949 / 0.940 (two x64 runs against one interpreter run), worst frame 49-50 ms, 652 / 902 frames over 16.7 ms, so the 0.969, 48.3 ms and 571 of this session are within that spread. Track A 0.882, track B 0.744, 2C 0.55.

What the numbers say about the remaining gap and stutter (counters from the runs above and one extra run, `%TEMP%\opencode\t12\meas-long\br-x64-slowlist.log`, a copy of the instrumented executable with a per-frame list, window counters equal to the uninstrumented run's: 8,296 frames compiling, 28,883 native compiles, 3 flushes, 1 IR-cap event; avg frame 10.016, 585 frames over 16.7 ms):

- All 585 frames over 16.7 ms in that run contain front-end builds (394 of them 100 or more; none has zero builds), in code-load phases (209 of them in frames 9,000-12,599, 155 in 12,600-16,199, 201 in 25,200-28,799 and 20 elsewhere, the first at frame 7,014); no frame has a flush. The five slowest frames of the worker run are the 9,264-9,309 burst of new code (3,610 front-end builds in frame 9,264, 24.0 ms of compiling in it), as in track A and task 5.
- The emulation-thread compile time in the slow frames is small: 512 of the 585 have 1-5 ms of it and 53 under 1 ms; 20 have 5 ms or more (1,342 ms over all 585). The mean frame over 16.7 ms is 18.2 ms against the interpreter's 9.4 ms average, about 8.8 ms more, of which 2.3 ms on average is compile time on the emulation thread, so the front-end builds and the Prepare/Commit step do not account for it alone. What else runs in those frames (IR execution before the native code arrives, invalidation work, cold caches on new code) was not measured here.
- The IR cap is not the cause: one cap event in the window, 4.5 ms (task 5's R8).
- Compile work on the emulation thread is now 0.170 ms/frame (5.7 s) against 0.497 ms/frame (16.7 s) synchronously and 0.714 ms/frame in track A.

### Default-on bar (design section 3), condition by condition

1. **Faster: FAIL.** Medians of three, session 1 (second session in brackets where the rule asks for it): Virtua Fighter 2 1.281, Panzer Dragoon II Zwei 1.120, Guardian Heroes 1.138, Street Fighter Zero 3 1.033 (1.051; one earlier session of the same code read 0.995), Burning Rangers 1.008 (1.014: above 1.0 in both sessions, the rule's second-session test passes), **Sega Rally 0.957 (0.958): below 1.0**; the long Burning Rangers run **0.969: below 1.0**.
2. **Smooth: FAIL.** Frames over 16.7 ms: x64 at most the interpreter's highest count in all six 1,800-frame windows of session 1 (0 against 0, Burning Rangers 0 / 0 / 1 against a highest 1), but **the long Burning Rangers run has 571 against 1**; in session 2 Street Fighter Zero 3 has 1 against 0 (a compile-burst frame). Average frame time: x64 at most the interpreter's on the BIOS menu, Virtua Fighter 2, Panzer Dragoon II Zwei and Guardian Heroes, **above it on Sega Rally (7.777 against 7.191 ms), Burning Rangers (8.923 against 8.668), Street Fighter Zero 3 (9.238 against 8.949)** and the long run (9.907 against 9.380).
3. **Exact: holds** (details in `design/sh2-validation.md`, "Milestone 2E"): lockstep identical for 36,000 frames on x64 (BIOS menu and six titles), 1,800 frames on IR, 3,600 frames with `--lockstep-reload 300` on x64 and IR, and 1,800 frames x64 with `--jit-sync-compile`, all 35 runs exit 0 with `compileFallbacks 0` and `asyncCompiles` above 0 in the worker runs; the Raspberry Pi 3's IR lockstep (7 of 7) and test suite pass; CI of the measured code (run 39524423030, `5f2df05`) is green including `test-linux-tsan`, and CI of the commit that adds this section is recorded in the report of the task.
4. **Memory: holds.** Long Burning Rangers run: 0 code-cap flushes on both CPUs, peak native code 76.9 / 6.5 MB (master / slave), retired pool 3.5 MB at the end. The 36,000-frame lockstep runs peak at 111.1 MB (Panzer Dragoon II Zwei master), 97.1 (Virtua Fighter 2), 90.8 (Street Fighter Zero 3), all under 128 MB, with 0 flushes.

Not met: conditions 1 and 2. The JIT stays off by default; the README and the option text only say what is now measured.

### 2E generated-code profile

The per-block profile of `sh2-jit-m2e.md` section 7 on Sega Rally (condition 1 fails), Panzer Dragoon II Zwei and Street Fighter Zero 3 (the plan's comparison titles) and Burning Rangers' 1,800-frame window (within 0.02 of 1.0). Street Fighter Zero 3 (1.033) is not within 0.02 in session 1, but a same-code session read 0.995, so it is treated as at 1.0.

**Method.** A temporary instrumented `brimir_bench` (`build-instr`, same options as `build-bench`; the patch is not committed and was reverted afterwards). Two modes, switched by an environment variable:

- *sample*: a sampler thread suspends the emulation thread about 2,000 times per second (period jittered by +-40% against aliasing with the frame; `SuspendThread` and `GetThreadContext`) and records the instruction pointer and a phase word. The executor sets the phase at its stages (`Run` loop, `Step` checks, `Get` on the recent-slot path and on the map path, building and installing, `RunEntry`, `X64Backend::Run`, interpreter fallback, IR block run). Each trampoline sets its kind and records its return address. A registry of every committed block's address range (add, release and reset events, replayed up to each sample's position so freed and reused code addresses resolve correctly) maps an instruction pointer in generated code to its block, and a sample inside a trampoline to the block that called it. Generated code is unchanged in this mode.
- *count*: the emitter adds per-block counters (runs, retired guest instructions, exits by kind, and at each chaining exit the outcome: next block entered, link slot miss, cycle limit reached, chaining off, flush requested), each block stores its counter address in the frame so that trampolines count their calls per block and kind, and the executor counts `Run` calls, `Step`s, `Get`s (by path), native chains started and interpreter fallbacks (by reason and opcode).

Per title: three sample runs and one count run, `--warmup 2400 --frames 1800`, x64 with the worker (16 runs, all exit 0, `%TEMP%\opencode\t12\prof`). **Checks:** the `jit window` counters of all 16 instrumented runs equal the uninstrumented ones (front-end builds / native compiles / flushes / IR-cap events / reuses / IR reuses: Sega Rally 502 / 1,056 / 0 / 0 / 0 / 0, Panzer Dragoon II Zwei 187 / 660 / 0 / 0 / 0 / 0, Street Fighter Zero 3 10,686 / 6,333 / 0 / 1 / 0 / 0, Burning Rangers 303 / 821 / 0 / 0 / 0 / 0), and the since-start `blocksRun` and `interpreted` equal the uninstrumented runs' on both CPUs (nativeBlocksRun differs by at most 0.04%, from when native code arrives). The count run's per-window numbers reproduce the 2D profile's (Panzer Dragoon II Zwei: 28.01M / 160.9M chains / blocks on the master, 27.73M / 105.9M on the slave, 85.46M on-chip register reads on the slave). **Cost:** the sampled runs' SH2 total is 9-15% above the uninstrumented median (4.76-4.79 against 4.361 on Panzer Dragoon II Zwei, 5.34-5.47 against 4.815 on Sega Rally, 4.76-4.78 against 4.371 on Street Fighter Zero 3, 6.87-7.16 against 6.245 on Burning Rangers), the count runs 9-24% above; times below are scaled by uninstrumented / sampled SH2 total (0.893-0.917). **Limits:** time is attributed by instruction pointer, so cache-miss latency lands where the stalled instruction is (a `Get` that misses the cache shows as `Get`); a block's share has a statistical error of about 1/sqrt(samples): 4% relative for a 1% block (537 executor samples), under 1% for the top block. The first instrumented build (a dispatch split without the `Get` sub-paths) gave the same shares within 1 point (Sega Rally `Get` 13.5% against 13.8%). The sampler's own cost also lands in the sampled thread's wall time, uniformly (hence the scale). The 2D profile (rdtsc intervals) put the time outside `Executor::Run` at 0.61-0.63 ms/frame; the sampled runs give 0.34-0.44 (SH-2 total minus sampled executor time), a method difference that is not resolved here. The dispatch conclusions below are about 1.2-1.5 ms/frame in either reading.

**Where the executor's time goes** (ms/frame, scaled; the percentage is of the sampled `Executor::Run` time; master and slave together):

| ms/frame (scaled) | Sega Rally | Panzer Dragoon II Zwei | Street Fighter Zero 3 | Burning Rangers |
|---|---|---|---|---|
| `Executor::Run` (sampled) | 4.431 (100.0%) | 4.025 (100.0%) | 4.022 (100.0%) | 5.804 (100.0%) |
| generated code in blocks | 2.138 (48.2%) | 1.896 (47.1%) | 1.941 (48.3%) | 3.159 (54.4%) |
| all trampolines (callback + call) | 0.470 (10.6%) | 0.880 (21.9%) | 0.431 (10.7%) | 0.951 (16.4%) |
|   of which on-chip register reads | 0.183 (4.1%) | 0.344 (8.5%) | 0.301 (7.5%) | 0.374 (6.5%) |
|   of which writes | 0.252 (5.7%) | 0.525 (13.0%) | 0.111 (2.8%) | 0.551 (9.5%) |
|   of which refills | 0.025 (0.6%) | 0.007 (0.2%) | 0.009 (0.2%) | 0.003 (0.0%) |
| **dispatch outside blocks and trampolines** | 1.379 (31.1%) | 1.205 (29.9%) | 1.493 (37.1%) | 1.549 (26.7%) |
|   Run loop | 0.055 (1.2%) | 0.026 (0.6%) | 0.027 (0.7%) | 0.032 (0.6%) |
|   Step checks | 0.153 (3.4%) | 0.091 (2.3%) | 0.089 (2.2%) | 0.117 (2.0%) |
|   Get, recent-slot hit path | 0.542 (12.2%) | 0.410 (10.2%) | 0.516 (12.8%) | 0.582 (10.0%) |
|   Get, map path | 0.070 (1.6%) | 0.059 (1.5%) | 0.243 (6.0%) | 0.162 (2.8%) |
|   RunEntry + StepImpl tail | 0.370 (8.4%) | 0.445 (11.1%) | 0.447 (11.1%) | 0.426 (7.3%) |
|   Backend::Run frame setup and loop | 0.190 (4.3%) | 0.174 (4.3%) | 0.171 (4.2%) | 0.229 (4.0%) |
| interpreter fallbacks | 0.427 (9.6%) | 0.034 (0.8%) | 0.042 (1.0%) | 0.115 (2.0%) |
| IR block runs | 0.008 (0.2%) | 0.004 (0.1%) | 0.044 (1.1%) | 0.023 (0.4%) |
| Get: builds, compiles, installs | 0.008 (0.2%) | 0.005 (0.1%) | 0.070 (1.7%) | 0.007 (0.1%) |
| per Step: dispatch ns (all Steps, both CPUs) | 23.8 | 36.5 | 45.6 | 45.0 |
| per chain start: Get hit path ns | 16.8 | 15.1 | 23.9 | 25.1 |
| per chain: Backend::Run + RunEntry ns | 21.5 | 20.0 | 19.6 | 22.1 |

(`Run` loop, `Step` checks and `Get` are `Executor::Run`/`StepImpl`/`BlockCache::Get`; `RunEntry + StepImpl tail` is the in-block flush guard, the stats, the exit handling; `Backend::Run` is the `X64Frame` setup, the call loop and the return. A trampoline's time is the callback plus the call.)

**Structure** (count runs; per 1,800 frames, master / slave):

| Per 1,800 frames, master / slave | Sega Rally | Panzer Dragoon II Zwei | Street Fighter Zero 3 | Burning Rangers (1,800 frames) |
|---|---|---|---|---|
| `Executor::Run` calls (M) | 27.7 / 27.7 | 27.8 / 27.8 | 28.0 / 28.0 | 26.7 / 26.7 |
| Steps (M) | 28.8 / 75.4 | 31.7 / 27.8 | 30.9 / 28.0 | 29.4 / 32.6 |
| `Get` calls (M) | 27.8 / 37.6 | 28.1 / 27.7 | 29.3 / 28.0 | 26.9 / 26.6 |
| Native chains started (M) | 27.8 / 19.0 | 28.0 / 27.7 | 28.7 / 28.0 | 26.8 / 26.6 |
| Interpreter fallbacks (M) | 0.99 / 56.39 | 3.69 / 0.07 | 2.11 / 0.00 | 2.54 / 5.93 |
| Native block runs (M) | 154.1 / 80.4 | 160.8 / 105.9 | 113.7 / 98.7 | 107.6 / 113.5 |
| Blocks per chain | 5.55 / 4.22 | 5.74 / 3.82 | 3.96 / 3.53 | 4.01 / 4.26 |
| Guest instructions retired in native blocks (M) | 483 / 271 | 554 / 347 | 483 / 392 | 584 / 377 |
| Guest instructions per block run | 3.14 / 3.38 | 3.45 / 3.28 | 4.24 / 3.97 | 5.42 / 3.32 |
| Exits, % of runs: static `Exit` | 7% / 10% | 2% / 0% | 4% / 0% | 11% / 0% |
| Exits: taken `ExitIf` | 74% / 63% | 45% / 81% | 72% / 79% | 44% / 0% |
| Exits: `ExitDynamic` | 8% / 10% | 42% / 1% | 8% / 0% | 28% / 79% |
| Exits: boundary (cycle limit or interrupt) | 11% / 17% | 11% / 18% | 16% / 21% | 17% / 21% |
| Exits: stale entry / abort / bus wait | 4 / 0 | 0 / 0 | 116 / 0 | 0 / 0 |
| Chain: next block entered, % of runs | 82% / 76% | 83% / 74% | 75% / 72% | 75% / 77% |
| Chain misses (link slot empty or other PC) | 41,661 / 361 | 164,009 / 0 | 725,163 / 0 | 147,831 / 0 |
| Chains ended by the cycle limit (M) | 11.0 / 5.3 | 9.4 / 8.6 | 10.0 / 7.6 | 8.0 / 2.9 |
| Trampoline calls: on-chip register reads (M) | 0.77 / 47.15 | 0.06 / 85.46 | 0.01 / 78.33 | 0.01 / 95.73 |
| Trampoline calls: writes (M) | 2.70 / 2.16 | 2.47 / 1.40 | 2.60 / 0.00 | 0.98 / 0.00 |
| Trampoline calls: refills (M) | 7.13 / 5.49 | 2.37 / 1.29 | 3.77 / 0.00 | 1.66 / 0.00 |
| Trampoline calls: `MAC.L` (M) | 4.60 / 3.97 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 |
| Trampoline calls: `MAC.W` (M) | 0.34 / 0.34 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 |
| Trampoline calls: bus waits (M) | 0.06 / 0.00 | 1.32 / 0.00 | 2.90 / 0.00 | 3.42 / 0.00 |
| Trampoline calls: `SetSR` (M) | 0.02 / 0.00 | 0.93 / 0.00 | 1.17 / 0.00 | 0.39 / 0.00 |
| Trampoline calls: other reads (M) | 0.06 / 0.00 | 0.16 / 0.00 | 0.51 / 0.00 | 3.12 / 0.00 |

- **Chain ends are slice ends, not misses.** Of the chains that start (19-29M per CPU per 1,800 frames: one `Run` call per scheduler slice of about 31 cycles, about 15,000 per frame per CPU), 97-100% end because the cycle budget is reached or an interrupt is pending (chains ended by the cycle limit plus boundary exits, against chain misses of 0.0-2.5%: e.g. Sega Rally master 10.98M + 16.75M boundary exits of 27.78M chains, 0.04M misses). A chain miss costs nothing noticeable here; the cost is the per-slice entry.
- **Blocks are short**: 3.1-5.4 guest instructions per block run, 3.5-5.7 blocks per chain, so 17-22 guest instructions per slice on the master and 12-14 on the slave.

**The dispatch outside generated code is 27-37% of the executor**, 1.2-1.55 ms/frame (the bold row above):

- `Get` is 12-19% (0.47-0.76 ms/frame; the recent-slot hit path alone 10-13%, 0.41-0.58): 99% of `Get`s hit (Sega Rally: 27.5M of 27.8M master, 37.5M of 37.6M slave), at 15-25 ns per `Get` (Sega Rally 16.8, Panzer Dragoon II Zwei 15.1, Street Fighter Zero 3 23.9, Burning Rangers 25.1). A hit does a worker-results check, the 4,096-entry recent-slot lookup, `Validate` (for a native block a virtual `Publish` that stores into the 16,384-entry link table), `Hit` (stamps `lastUse`, decrements `runsUntilNative`) and reads the `CachedBlock`. The master's map path (recent-slot misses: 0.26M on Sega Rally, 1.03M on Street Fighter Zero 3, 1.38M on Burning Rangers per window) costs 0.06-0.24 ms/frame on top.
- Entering and leaving a chain (`RunEntry` + `StepImpl` tail + `Backend::Run`) is 7-11% + 4% = 0.56-0.66 ms/frame, 20-22 ns per chain in all four titles (the guard object, four stats counters, a zero-initialised `X64Frame` of about 100 bytes, a virtual call, the `ExitInfo` copy).
- `Step` checks and the `Run` loop are 2.6-4.6% (0.12-0.21 ms/frame).
- The interpreter fallbacks are 0.8-2.0% of the executor on three titles (0.03-0.12 ms/frame) and **9.6% (0.43 ms/frame) on Sega Rally**.

**Generated code is 47-54% of the executor** (1.9-3.2 ms/frame): 4.8-5.7 ns per retired guest instruction on the masters (Burning Rangers 7.8) and 2.0-5.4 on the slaves; with trampolines 5.9-9.5 and 3.3-6.8, with dispatch 8.4-12.5 and 5.8-11.5 (the interpreter's loop is about 8.5 ns per instruction, 2D). Blocks are 374-385 bytes of x64 per guest instruction over all committed blocks (419-475 weighted by the instructions the blocks retire). The profile is flat: 7 (Panzer Dragoon II Zwei), 11 (Street Fighter Zero 3), 37 (Burning Rangers) and 44 (Sega Rally) blocks account for 50% of block time, 118-692 for 80%, and the top ten blocks together are 23-40% of the executor.

**The one dominant block is the slave's polling loop.** In every title the hottest block is a 3-5 instruction loop on the slave with one on-chip register read per run (100 on-chip reads per 100 runs, 74-81% of its exits a taken `ExitIf` (an `ExitDynamic`, 75%, on Burning Rangers) and the rest boundary exits; Sega Rally 06017ADC 7.4% of the executor, Panzer Dragoon II Zwei 0600E70C 17.1%, Street Fighter Zero 3 06010198 15.9%, Burning Rangers 06006DD6 15.9%): 47-96M on-chip register reads per 1,800 frames. About half of its time is the trampoline (41-53%); the block as a whole costs 3.5-5.0 ns per guest instruction. The 2D profile identified the register (FTCSR, the slave waiting for the master's input-capture signal).

**Sega Rally's slave takes an interrupt about 10,000 times per frame.** Its 56.4M interpreter fallbacks per window (against 0.0-5.9M elsewhere) are 18.6M interrupt entries, each followed by an interpreted `RTE` (opcode 002B: "block with no compilable first instruction", 18.6M) and its delay slot (a `NOP`, 18.75M "delay slot pending"); 0.43 ms/frame (9.6% of the executor), plus the extra `Step`s (75.4M steps for 37.6M `Get`s). `RTE` is one of the opcodes the JIT does not compile. Panzer Dragoon II Zwei's master has 0.13M of the same.

**Trampolines** other than the polling loop's: master writes (0.11-0.55 ms/frame, 3-13%) are device work the interpreter also pays (a handful of blocks that run once per frame spend 0.08-0.21 ms/frame each in `Write` callbacks: Burning Rangers 0607818A, 0607818C, 06078176, 06078174, Sega Rally 06004D8A, Panzer Dragoon II Zwei 0600D9B8; their "ns per guest instruction" below is device time, not code quality); refills (1.7-7.1M calls on the master) and `MAC.L` (4.6M master, 4.0M slave on Sega Rally) are each under 1% of the executor.

**Compiling is not a cause in these windows**: builds, installs and IR runs are 0.2-2.8% of the executor (Street Fighter Zero 3 1.7% + 1.1%, the largest), consistent with the compile numbers above.

The hottest blocks, ranked by share of executor time (generated code plus the trampolines they call): guest instructions per run (static) and retired per frame, ns per guest instruction, exits by kind as a percentage of the block's runs, chain misses, trampoline calls by kind per 100 runs. (Chain misses are 0% for every block shown.)

#### Sega Rally Championship

- 3082 distinct (CPU, PC) blocks seen in the samples; 44 blocks make 50% and 518 blocks 80% of block time (58.9% of the executor is in blocks and their trampolines).
| # | CPU | PC | share of executor | ms/frame | guest instr per run (static) | instr retired/frame | ns per guest instr | exits, % of runs: static / ExitIf taken / dynamic / boundary / other | chain miss, % of runs | trampoline calls per 100 runs |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | slave | 06017ADC | 7.35% | 0.326 | 3 | 69,173 | 4.71 | 0 / 81 / 0 / 19 / 0 | 0% | 100 OnChipRead |
| 2 | master | 06005226 | 6.35% | 0.281 | 3 | 114,934 | 2.45 | 0 / 90 / 0 / 10 / 0 | 0% | - |
| 3 | master | 06004D8A | 4.65% | 0.206 | 5 | 2 | 83590.87 | 0 / 0 / 95 / 5 / 0 | 0% | 100 Write, 100 Refill, 100 BusWait |
| 4 | master | 0600521E | 1.70% | 0.075 | 3 | 30,070 | 2.50 | 0 / 90 / 0 / 10 / 0 | 0% | - |
| 5 | master | 06017BA4 | 1.00% | 0.044 | 2 | 14,947 | 2.96 | 5 / 90 / 0 / 6 / 0 | 0% | - |
| 6 | master | 0600522A | 0.48% | 0.021 | 1 | 3,736 | 5.74 | 0 / 100 / 0 / 0 / 0 | 0% | - |
| 7 | master | 06019650 | 0.44% | 0.019 | 32 | 2,839 | 6.83 | 0 / 0 / 0 / 100 / 0 | 0% | 67 Write, 184 Refill, 96 MacL |
| 8 | slave | 06017ADE | 0.41% | 0.018 | 2 | 6,673 | 2.72 | 0 / 100 / 0 / 0 / 0 | 0% | - |
| 9 | slave | 06019650 | 0.39% | 0.017 | 32 | 2,186 | 7.96 | 0 / 0 / 0 / 100 / 0 | 0% | 69 Write, 189 Refill, 96 MacL |
| 10 | master | 06018D84 | 0.36% | 0.016 | 21 | 2,705 | 5.82 | 10 / 26 / 0 / 65 / 0 | 0% | 49 MacW |
The top 10 blocks together are 23.1% of the executor.

#### Panzer Dragoon II Zwei

- 1940 distinct (CPU, PC) blocks seen in the samples; 7 blocks make 50% and 118 blocks 80% of block time (69.0% of the executor is in blocks and their trampolines).
| # | CPU | PC | share of executor | ms/frame | guest instr per run (static) | instr retired/frame | ns per guest instr | exits, % of runs: static / ExitIf taken / dynamic / boundary / other | chain miss, % of runs | trampoline calls per 100 runs |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | slave | 0600E70C | 17.07% | 0.687 | 4 | 164,111 | 4.19 | 0 / 78 / 0 / 22 / 0 | 0% | 100 OnChipRead |
| 2 | master | 0600D9B8 | 5.47% | 0.220 | 11 | 486 | 453.21 | 33 / 0 / 0 / 67 / 0 | 0% | 437 Write, 300 Refill, 437 BusWait, 37 SetSR |
| 3 | master | 0600815C | 3.13% | 0.126 | 3 | 50,421 | 2.50 | 0 / 91 / 0 / 9 / 0 | 0% | - |
| 4 | master | 0600FD18 | 2.81% | 0.113 | 2 | 34,536 | 3.28 | 0 / 0 / 99 / 1 / 0 | 0% | - |
| 5 | master | 0600D9BE | 2.75% | 0.111 | 8 | 126 | 876.75 | 94 / 0 / 0 / 6 / 0 | 0% | 392 Write, 287 Refill, 392 BusWait, 95 SetSR |
| 6 | master | 0600D9C6 | 2.70% | 0.109 | 4 | 61 | 1789.59 | 98 / 0 / 0 / 2 / 0 | 0% | 100 Write, 197 Refill, 100 BusWait, 99 SetSR |
| 7 | master | 0600FD1C | 2.58% | 0.104 | 4 | 63,415 | 1.64 | 0 / 83 / 0 / 17 / 0 | 0% | - |
| 8 | master | 06008158 | 2.29% | 0.092 | 2 | 33,225 | 2.77 | 0 / 0 / 92 / 8 / 0 | 0% | - |
| 9 | slave | 0600E70E | 1.03% | 0.042 | 3 | 18,284 | 2.27 | 0 / 99 / 0 / 1 / 0 | 0% | - |
| 10 | slave | 0600E710 | 0.57% | 0.023 | 2 | 6,127 | 3.73 | 0 / 100 / 0 / 0 / 0 | 0% | - |
The top 10 blocks together are 40.4% of the executor.

#### Street Fighter Zero 3

- 3666 distinct (CPU, PC) blocks seen in the samples; 11 blocks make 50% and 692 blocks 80% of block time (59.0% of the executor is in blocks and their trampolines).
| # | CPU | PC | share of executor | ms/frame | guest instr per run (static) | instr retired/frame | ns per guest instr | exits, % of runs: static / ExitIf taken / dynamic / boundary / other | chain miss, % of runs | trampoline calls per 100 runs |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | slave | 06010198 | 15.93% | 0.641 | 5 | 182,384 | 3.51 | 0 / 74 / 0 / 26 / 0 | 0% | 100 OnChipRead |
| 2 | master | 0600F512 | 6.74% | 0.271 | 4 | 127,935 | 2.12 | 0 / 85 / 0 / 15 / 0 | 0% | - |
| 3 | master | 06043AC4 | 1.70% | 0.068 | 22 | 22 | 3096.90 | 0 / 0 / 20 / 80 / 0 | 6% | 84 Write, 329 Refill, 84 BusWait, 38 SetSR |
| 4 | master | 0600F5B4 | 1.59% | 0.064 | 4 | 29,840 | 2.14 | 0 / 85 / 0 / 15 / 0 | 0% | - |
| 5 | slave | 0601019A | 1.17% | 0.047 | 4 | 22,478 | 2.09 | 0 / 99 / 0 / 1 / 0 | 0% | - |
| 6 | master | 060008F0 | 0.64% | 0.026 | 28 | 5,386 | 4.81 | 0 / 0 / 2 / 98 / 0 | 0% | 95 Write, 5 Refill, 95 BusWait, 96 SetSR |
| 7 | slave | 0601019C | 0.50% | 0.020 | 3 | 8,468 | 2.35 | 0 / 100 / 0 / 0 / 0 | 0% | - |
| 8 | master | 06000932 | 0.40% | 0.016 | 12 | 1,976 | 8.10 | 98 / 0 / 0 / 2 / 0 | 98% | 99 Write, 196 Refill, 99 BusWait, 99 SetSR |
| 9 | master | 0600F516 | 0.39% | 0.016 | 2 | 4,567 | 3.41 | 0 / 100 / 0 / 0 / 0 | 0% | - |
| 10 | master | 06048B3A | 0.37% | 0.015 | 4 | 2,398 | 6.15 | 0 / 90 / 0 / 10 / 0 | 0% | 100 Write, 190 Refill, 89 BusWait |
The top 10 blocks together are 29.4% of the executor.

#### Burning Rangers (1,800-frame window)

- 2760 distinct (CPU, PC) blocks seen in the samples; 37 blocks make 50% and 389 blocks 80% of block time (70.8% of the executor is in blocks and their trampolines).
| # | CPU | PC | share of executor | ms/frame | guest instr per run (static) | instr retired/frame | ns per guest instr | exits, % of runs: static / ExitIf taken / dynamic / boundary / other | chain miss, % of runs | trampoline calls per 100 runs |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | slave | 06006DD6 | 15.86% | 0.921 | 4 | 183,303 | 5.02 | 0 / 0 / 75 / 25 / 0 | 0% | 100 OnChipRead |
| 2 | master | 0607818A | 3.37% | 0.196 | 7 | 1 | 199481.78 | 0 / 0 / 99 / 1 / 0 | 0% | 100 Write, 199 Refill, 100 BusWait |
| 3 | master | 0607818C | 3.19% | 0.185 | 6 | 1 | 232713.84 | 0 / 0 / 100 / 0 / 0 | 0% | 100 Write, 199 Refill, 100 BusWait |
| 4 | master | 06078176 | 1.38% | 0.080 | 17 | 1 | 84408.57 | 0 / 0 / 96 / 4 / 0 | 0% | 596 Write, 690 Refill, 596 BusWait |
| 5 | master | 06078174 | 1.33% | 0.077 | 18 | 3 | 30296.15 | 0 / 0 / 25 / 75 / 0 | 0% | 517 Write, 469 Refill, 517 BusWait |
| 6 | slave | 06006DD8 | 1.09% | 0.063 | 3 | 19,579 | 3.24 | 0 / 0 / 99 / 1 / 0 | 0% | - |
| 7 | master | 0600F60C | 0.60% | 0.035 | 3 | 11,239 | 3.12 | 0 / 12 / 79 / 9 / 0 | 0% | - |
| 8 | slave | 06006DDA | 0.54% | 0.031 | 2 | 6,561 | 4.78 | 0 / 0 / 99 / 1 / 0 | 0% | - |
| 9 | master | 06081BF6 | 0.54% | 0.031 | 32 | 6,942 | 4.49 | 0 / 0 / 0 / 100 / 0 | 0% | - |
| 10 | master | 0600F5AC | 0.52% | 0.030 | 3 | 10,309 | 2.94 | 0 / 93 / 0 / 7 / 0 | 0% | - |
The top 10 blocks together are 28.4% of the executor.

**Ranked candidate changes** (Track F). Expected gains are ms/frame off the x64 SH2 total, derived from the rows above (scaled sampled time, never measured on an implementation) and then applied to this task's session 1 medians; ranges are the share of the row a change plausibly removes (stated per item). The resulting ratios use the interpreter totals of the same session. The order is by gain and by how well the profile supports the gain; item 4b has the largest upper bound on three titles but is a semantic change, so it ranks below the dispatch items.

1. **Resume a slice without `Get`.** Chains end at slice boundaries 97-100% of the time and the next block is the one the previous chain would have entered, so the executor can keep the continuation (PC and block) and, if no cache mutation (invalidate, flush, build, reuse, install) happened since and the `Step` gates (interrupt, delay slot, `PC & 2`) do not fire, enter it directly (a native, self-validating block only: its prologue still validates the code). The `Get` rows are 0.47-0.76 ms/frame (12-19% of the executor); removing 65-85% of them (the check itself and cache misses that move into the block cost the rest): **0.30-0.65 ms/frame**, ratios Sega Rally 0.957 to 1.04-1.07, Panzer Dragoon II Zwei 1.12 to 1.20-1.23, Street Fighter Zero 3 1.03 to 1.16-1.21, Burning Rangers 1.008 to 1.09-1.12. Needs a cache generation counter; threshold counting and `lastUse` for blocks entered this way need a rule (they only affect when native code arrives and IR eviction order, not results).
2. **A leaner chain entry and exit.** `RunEntry` + `StepImpl` tail + `Backend::Run` is 20-22 ns per chain (0.56-0.66 ms/frame); keeping the frame initialised once, dropping the guard object and counters from the common path, and devirtualising the call should save 40-60%: **0.22-0.39 ms/frame**. With item 1 the ratios are Sega Rally 1.10-1.16, Panzer Dragoon II Zwei 1.28-1.36, Street Fighter Zero 3 1.24-1.35, Burning Rangers 1.15-1.21.
3. **Compile `RTE`** (or give the delay slot after an interrupt return an inline path) for Sega Rally's slave: 37M of its 56M interpreted instructions are `RTE` and its slot; the 0.43 ms/frame is 9.6% of the executor. Interrupt entry stays interpreted: **0.15-0.28 ms/frame on Sega Rally** (35-65% of the row), under 0.05 elsewhere. With items 1-2 Sega Rally reaches 1.14-1.25. Exactness work: `RTE` pops PC and SR, and `SR` changes recompute the interrupt state (the `SetSR` trampoline already does this).
4. **The slave's polling loop** (7-17% of the executor, 0.33-0.92 ms/frame in the top block):
   a. a cheaper on-chip register read (the trampoline is 41-53% of that block, of which part is device logic that must stay): **0.05-0.19 ms/frame**;
   b. recognising the loop and fast-forwarding its iterations to the next event: **0.16-0.83 ms/frame** (50-90% of the block's time), the largest single item on Panzer Dragoon II Zwei, Street Fighter Zero 3 and Burning Rangers, but not a code-shape change: it needs an exactness argument for the register state, cycle count and FRT catch-up that lockstep can check but a plan must specify, and it fails any title whose loop is not this loop. With items 1-2 it gives Panzer Dragoon II Zwei 1.41-1.64, Street Fighter Zero 3 1.36-1.62, Burning Rangers 1.25-1.43.
5. **Generated-code shape** (47-54% of the executor): the profile shows no block to fix (flat distribution; the best hot blocks run at 2.2-2.5 ns per guest instruction, about 8 cycles) and track B's per-instruction changes gave 3-6%, so a gain cannot be derived from this profile; an estimate of 5-10% of the row, 0.1-0.3 ms/frame, is a guess. Smaller blocks' prologue and epilogue (375-385 bytes per guest instruction) are the natural place to look after items 1-2.

Not candidates: master write trampolines (device work the interpreter also pays), compiling (under 3% of the executor in these windows), the IR cap.

| Title | x64 SH2 total | interpreter / x64 now | 1. no `Get` on resume | 2. leaner chain entry | 3. `RTE` (Sega Rally) | 4a. cheaper on-chip read | 4b. poll-loop fast-forward | ratio with 1+2 | ratio with 1+2+3 | ratio with 1+2+4b |
|---|---|---|---|---|---|---|---|---|---|---|
| Sega Rally | 4.815 | 0.957 | 0.40-0.52 | 0.22-0.34 | 0.15-0.28 | 0.05-0.09 | 0.16-0.29 | 1.10-1.16 | 1.14-1.25 | 1.14-1.26 |
| Panzer Dragoon II Zwei | 4.361 | 1.120 | 0.30-0.40 | 0.25-0.37 | <0.05 | 0.10-0.17 | 0.34-0.62 | 1.28-1.36 | - | 1.41-1.64 |
| Street Fighter Zero 3 | 4.371 | 1.033 | 0.49-0.65 | 0.25-0.37 | <0.05 | 0.09-0.15 | 0.32-0.58 | 1.24-1.35 | - | 1.36-1.62 |
| Burning Rangers (short) | 6.245 | 1.008 | 0.48-0.63 | 0.26-0.39 | <0.05 | 0.11-0.19 | 0.46-0.83 | 1.15-1.21 | - | 1.25-1.43 |

(Gains in ms/frame; ratios use this task's session 1 interpreter totals.) Items 1 and 2 together put every profiled title above 1.0 by a margin larger than the session spread (5% of the SH-2 total is about 0.05-0.07 in ratio), Sega Rally included (1.10-1.16); they say nothing about the long Burning Rangers run's stutter or the whole-frame average, which a code-generation change does not address (see below). The tool, the dumps and the analysis scripts are not committed.

### Native-code peaks against track A (final review)

The 2E lockstep table (`design/sh2-validation.md`) has native-code peaks up to 13 MB above track A's: Panzer Dragoon II Zwei 111.1 against 105.7 MB, Virtua Fighter 2 97.1 against 84.6, Street Fighter Zero 3 90.8 against 80.7. Hypothesis: track A's IR-cap rule freed every IR-only block and so reset its run count, while the least-recently-used rule (R8) keeps most of them, so more blocks reach the native threshold and are compiled.

**Method.** `--lockstep 36000 --jit-backend x64` (the window of the table), default settings, US BIOS for Panzer Dragoon II Zwei and JP BIOS for Virtua Fighter 2, on three executables at once (six runs, all `lockstep: OK, 36000 frames identical`, exit 0, 0 code-cap flushes): track A (`550f982`, pre-2E plus track A, built in a temporary worktree with the `build-bench` options), this branch with the worker, and this branch with `--jit-sync-compile` (to exclude the worker). Master CPU, `cache master` line. Track A's `cache` line has no peak counter, so its peak is the table's value; it is checked below against its own counters. Raw logs: `%TEMP%\opencode\t13\peak-*.log`.

| Counter (master, 36,000 frames) | Panzer Dragoon II Zwei track A | this branch (worker) | this branch (sync) | Virtua Fighter 2 track A | this branch (worker) | this branch (sync) |
|---|---|---|---|---|---|---|
| Front-end builds (`compiles`) | 76,211 | 57,671 | 57,663 | 102,771 | 60,900 | 60,892 |
| IR-cap events (live IR-only blocks freed) | 3 (36,979) | 8 (12,460) | 8 (12,455) | 4 (58,511) | 7 (11,850) | 7 (11,840) |
| `nativeCompiles` | 32,233 | 33,949 | 33,950 | 29,658 | 34,562 | 34,563 |
| `nativeBytesPerBlock` | 3,439 | 3,430 | 3,430 | 2,990 | 2,946 | 2,946 |
| `nativeCompiles` x bytes per block, MiB | 105.7 | 111.0 | 111.0 | 84.6 | 97.1 | 97.1 |
| Peak native code, MB | 105.7 (table) | 111.1 | 111.1 | 84.6 (table) | 97.1 | 97.1 |
| Code-cap flushes | 0 | 0 | 0 | 0 | 0 | 0 |

**Conclusion: confirmed.** (1) The peak is the native code compiled: `nativeCompiles` x bytes per block reproduces every peak to 0.1 MB (almost nothing is released in these windows: the retired pools hold 3.1 and 6.7 MB), so a peak can only rise through more native compiles. (2) The branch compiles 1,716 more blocks on Panzer Dragoon II Zwei (+5.3%) and 4,904 more on Virtua Fighter 2 (+16.5%), about 5.6 and 13.8 MB at the branch's block size, against peak differences of 5.4 and 12.5 MB (the average block is 0.3% and 1.5% smaller on the branch). (3) The same runs do 18,540 and 41,871 fewer front-end builds, and free 12,460 and 11,850 IR blocks against track A's 36,979 and 58,511: the blocks that track A threw away and rebuilt with a run count of zero now keep it and cross the threshold. (4) `--jit-sync-compile` gives the same counts (33,950 and 34,563), so the worker is not the cause. The blocks are real hot code, not waste: `blocksRun` and `interpreted` are identical to track A's and `nativeBlocksRun` equals it within 0.02%. No code change.

**Headroom under the 128 MB cap** (`kMaxNativeCodeBytes`, per CPU; the master's peak, the slave's is far smaller): Panzer Dragoon II Zwei 111.1 MB, 16.9 MB (13%) below the cap, the tightest; Virtua Fighter 2 97.1 MB, 30.9 MB (24%); Street Fighter Zero 3 90.8 MB, 37.2 MB (29%; from the 2E lockstep table, not re-run). A session longer than these 36,000-frame windows can reach the cap on Panzer Dragoon II Zwei: it then releases the retired pool and, if that is not enough, flushes everything (`flushes codeCap`), which costs a recompile and is the bounded fallback of track A, not a failure.

### Sega Rally, worker against synchronous compiles, after the poll guard (final review)

`BlockCache::Get` now polls the worker (`HasResults`) only while the cache has compiles pending (`!m_pendingPcs.empty()`); a result can exist only for a pending ticket. One session, executable built from the fix (`build-bench`), `--warmup 2400 --frames 1800`, three rounds interleaved (worker, then `--jit-sync-compile`, each round), machine load 0-15% before each run:

| Sega Rally Championship (US) | worker, 3 runs (median) | `--jit-sync-compile`, 3 runs (median) |
|---|---|---|
| SH2 total ms/frame | 4.759 / 4.722 / 4.798 (4.759) | 4.797 / 4.853 / 4.860 (4.853) |
| ms/frame avg | 7.808 / 7.809 / 7.902 (7.809) | 7.853 / 7.865 / 7.880 (7.865) |
| max frame ms | 14.0 / 11.2 / 14.3 | 11.7 / 11.7 / 11.0 |
| frames over 16.7 ms | 0 / 0 / 0 | 0 / 0 / 0 |

The worker is 1.9% faster than synchronous compiles in SH-2 time (median) and 0.7% in average frame time, in every round: the 0.5-1% gap against the worker in the 2E table (4.815 worker against 4.765 synchronous, session 1) is gone, within a spread of 1.6% across the three runs. The earlier code was not re-run in this session, so the comparison with the old gap is across sessions; the worker's max frame is 14 ms in two runs against 11-12 ms synchronously, one burst frame each.
### What 2E left

- **Speed.** Five of six titles are faster than the interpreter in SH-2 time (1.008-1.281), Sega Rally is not (0.957). The profile puts 27-37% of the executor in the dispatch between slices (`Get`, entering and leaving the chain), which no earlier track targeted, and one block (the slave's polling loop) at 7-17%. Items 1-2 above are enough on paper to put Sega Rally above 1.0 with margin; they are the first thing for the Track F plan.
- **Whole-frame time.** The JIT adds 0.2-0.46 ms/frame outside the SH-2 total in every title (cause not established), which makes the average frame longer than the interpreter's on Sega Rally, Burning Rangers and Street Fighter Zero 3 even where the SH-2 total is lower or equal. Finding the cause (a cache-pressure experiment, for example a smaller native-code footprint, or a counter-based look at the non-SH-2 part) belongs in the same plan; without it the bar's condition 2 cannot be met on those titles by SH-2 work alone.
- **Long sessions.** Burning Rangers over 33,600 frames has 571 frames over 16.7 ms, every one with front-end builds and clustered in a few code-load phases, with about 1-5 ms of compile time in most of them against an excess of about 8.8 ms per frame; IR-block reuse removed 73% of the front-end builds (306K against 1.15M) but the bursts of genuinely new code remain. A per-frame look at those frames (IR runs before native code, invalidations, cold caches) is needed before choosing a change; moving front-end builds off the emulation thread is the obvious option but its benefit is unknown.
- **Recommendation.** Write the Track F plan from the ranked list (items 1 and 2 first, then 3 for Sega Rally, then the polling loop with its own design), and add to it a measurement of the non-SH-2 rise and of the long run's slow frames. The JIT stays off by default until both conditions 1 and 2 hold.

## Track F progress

Method (Track F plan, "Track F progress measurement"): `build-bench`, `--warmup 2400 --frames 1800`, interpreter and x64 with the worker back to back, three rounds; the x64 executable of the base commit (`7f807b9`, master at the start of track F, built in a separate worktree) runs in the same rounds, interleaved (interpreter, base, task), so each task is compared with its base in one session. Ratio = interpreter / x64 SH2 total (medians). Resumes are `resumes / runs` (`Executor::Run` calls) of the first x64 run, master / slave. Machine load before the batch 2-9%, 1-13% at each run.

| Task | Title | Interpreter SH2 ms/frame | x64 SH2 ms/frame (base `7f807b9`) | Ratio (base) | Avg frame ms: interpreter / x64 (base) | Frames over 16.7 ms: x64, 3 runs / interpreter max | Resumes / Run calls m / s |
|---|---|---|---|---|---|---|---|
| 1 | Panzer Dragoon II Zwei (US) | 4.914 | 4.271 (4.207) | 1.151 (1.168) | 6.382 / 5.954 (5.940) | 0 0 0 / 0 | 88.4% / 99.6% |
| 1 | Sega Rally Championship (US) | 4.682 | 4.576 (4.715) | **1.023** (0.993) | 7.229 / 7.584 (7.734) | 0 0 0 / 0 | 95.1% / 65.4% |
| 1 | Street Fighter Zero 3 (JP, 4 MB cart) | 4.584 | 4.204 (4.284) | 1.090 (1.070) | 8.913 / 9.121 (9.305) | 0 0 0 / 0 | 93.7% / 99.7% |
| 1 | Burning Rangers (US) | 6.361 | 5.985 (6.130) | 1.063 (1.038) | 8.732 / 8.664 (8.778) | 0 0 0 / 0 | 92.3% / 77.7% |
| 2 | Panzer Dragoon II Zwei (US) | 4.911 | 4.112 (Task 1: 4.199) | 1.194 (1.170) | 6.372 / 5.892 (5.889) | 0 0 0 / 0 | 88.4% / 99.6% |
| 2 | Sega Rally Championship (US) | 4.730 | 4.474 (Task 1: 4.564) | **1.057** (1.036) | 7.198 / 7.563 (7.597) | 0 0 0 / 0 | 95.1% / 65.4% |
| 2 | Street Fighter Zero 3 (JP, 4 MB cart) | 4.505 | 4.006 (Task 1: 4.128) | 1.125 (1.091) | 8.993 / 9.178 (9.159) | 0 0 0 / 0 | 93.7% / 99.7% |
| 2 | Burning Rangers (US) | 6.362 | 5.841 (Task 1: 5.951) | 1.089 (1.069) | 8.727 / 8.467 (8.635) | 0 0 0 / 0 | 92.2% / 77.7% |
| 3 | Panzer Dragoon II Zwei (US) | 4.888 | 4.039 (Task 2: 4.068) | 1.210 (1.202) | 6.351 / 5.794 (5.821) | 0 0 0 / 0 | 88.4% / 99.6% |
| 3 | Sega Rally Championship (US) | 4.610 | 4.460 (Task 2: 4.464) | **1.034** (1.033) | 7.137 / 7.545 (7.539) | 0 0 0 / 0 | 95.1% / 65.4% |
| 3 | Street Fighter Zero 3 (JP, 4 MB cart) | 4.524 | 3.967 (Task 2: 3.979) | 1.140 (1.137) | 8.967 / 9.154 (9.155) | 0 0 0 / 0 | 93.8% / 99.7% |
| 3 | Burning Rangers (US) | 6.255 | 5.836 (Task 2: 5.834) | 1.072 (1.072) | 8.597 / 8.452 (8.449) | 0 0 0 / 0 | 92.2% / 77.7% |

From Task 2 on, the value in parentheses is the previous task's executable (Task 1, `8f96e75`), run interleaved in the same rounds (interpreter, previous task, this task), not `7f807b9`.

For Task 3 the parenthesised base is Task 2's executable (`3594712`), interleaved in the same rounds. Task 3 is the `RTE` experiment: **dropped, not part of the branch**.

**Task 3: compile `RTE` (2E F3), measured and dropped.** `RTE` was lowered as a delayed branch in the front end (the two stack reads, the SR write through a new `WriteSR` IR op, `R15 += 8`; interrupt entry stayed interpreted).
- **Result.** Exact: lockstep BIOS 1,200 frames and the six titles at 1,800 frames, x64 and IR, all identical, `compileFallbacks 0`; `[jit]` on both backends, the `build-dbg` suite and WSL TSan passed. Sega Rally's slave interpreter fallbacks (bench `interpreted`, cumulative over 4,200 frames) fell from 110,244,782 to 42,281,374 (-62%), with 36.42M more `blocksRun`. SH-2 time did not improve beyond run-to-run spread on any title (rows above; Sega Rally 4.460 against 4.464 ms/frame; the profile's 0.15-0.28 ms/frame was not reached).
- **Likely reason** (inferred from the counters, not profiled). The added block runs are almost never chained into (`chainedBlocks` +0.1%), so each `RTE` block presumably follows an interpreted interrupt entry (not checked) and pays a lookup and a chain entry (the profile's 20-22 ns, items 1-2), about what the two interpreted instructions cost.
- **Decision.** Dropped. The code is kept on the local branch `archive/sh2-jit-rte` (`bb0ce10`, not pushed). Worth revisiting only together with compiling interrupt entry (or handing entry straight to the handler block).

### Task 1: resume a slice without `Get` (2E F1)

A slice that ends at the cycle limit records the link-table entry for the PC it stopped at (`ExitInfo::next`), guarded by a cache version counter, and the next `Executor::Run` enters it directly after the usual step gates instead of calling `BlockCache::Get`; the block's prologue still validates its code. Two forms were measured:

1. **Only chainable exits at the limit record** (`ChainExit`). Only 11-35% of `Run` calls resumed: most slices stop at a boundary inside a block (the target falls before the block's last instruction), which left no entry. Result in its session: x64 0.05-0.12 ms/frame *slower* than the base on all four titles (PD2 4.293 against 4.169, Rally 4.717 / 4.670, SFZ3 4.269 / 4.224, BR 6.200 / 6.103). Bisection on PD2: the per-step bookkeeping (record, version read, `ExitInfo` growing from 24 to 32 bytes, +0.02-0.04 of it) cost about what the skipped lookups saved; a first `StepImpl` form that restructured the lookup loop cost +0.30 and was replaced.
2. **Boundary exits record too** (kept): every `CheckBoundary` stub (and those of guarded runs' out-of-line copies) writes PC, retired, cycles and its stores exactly as before, then jumps to one shared tail per block that reads PC back, looks up its link slot and stores the entry (nullptr on a miss) when chaining is allowed and no flush is requested. The next slice's lookup builds a block at the stop PC; once it is native and published, later slices that stop there resume. 65-99.7% of `Run` calls now resume (table).

Against the base in the same session (3 rounds; PD2 6 rounds, as its first three overlapped): Sega Rally 4.572-4.598 against 4.709-4.726, **now faster than the interpreter (1.023)**; Burning Rangers 5.916-5.999 against 6.123-6.151; Street Fighter Zero 3 4.162-4.213 against 4.273-4.303; all three beyond the run-to-run spread. Panzer Dragoon II Zwei 4.201-4.291 (median 4.237) against 4.189-4.288 (median 4.205): within the spread. Average frame time is lower than the base's on Rally (-0.15 ms), SFZ3 (-0.18) and BR (-0.11), equal on PD2. The profile's 0.30-0.65 ms/frame estimate is reached only in part (0.08-0.15 ms/frame on the three titles that gain).

Code size: `nativeBytesPerBlock` +83-90 bytes per block against the base (master PD2 3,241 -> 3,324, Rally 2,876 -> 2,960, SFZ3 3,000 -> 3,083, BR 3,025 -> 3,109; slave +82-90; +1.7-5.7%): the shared boundary tail and the limit store in each chainable exit. Exact: lockstep BIOS 1,200 frames x64 and IR, Sega Rally, Street Fighter Zero 3 and Burning Rangers 1,800 frames x64: all identical, `compileFallbacks 0`.
### Task 2: leaner chain entry and exit (2E F2)

The profile's 20-22 ns per chain (`RunEntry` + `StepImpl` tail + `X64Backend::Run`) named five costs. Each change was measured on its own against the previous kept step's executable, Panzer Dragoon II Zwei and Sega Rally, three interleaved rounds, `--warmup 2400 --frames 1800` (x64 SH2 ms/frame per round):

| Step | Change | PD2: previous / this | Rally: previous / this | Kept |
|---|---|---|---|---|
| c1 | `X64Frame` holds no `std::exception_ptr`: trampolines store the exception in the backend's and set a `failed` byte. With MSVC, an `exception_ptr`'s constructor, destructor and `operator bool` are calls into the C++ runtime DLL, three per chain | 4.198 4.231 4.178 / 4.130 4.154 4.137 | 4.535 4.542 4.557 / 4.543 4.511 4.628 | yes (PD2 -0.06; Rally unchanged) |
| c2 | No virtual call: the executor calls `RunX64` (`x64_factory.hpp`, asmjit-free), which calls the `final` `X64Backend::Run` directly; `INativeBackend::Run` stays for every other caller | 4.126 4.148 4.148 / 4.084 4.070 4.076 | 4.509 4.503 4.540 / 4.576 4.436 4.440 | yes (PD2 -0.07; Rally median -0.07) |
| c3 | `RunEntry` without the `BlockScope` guard (try/catch and an explicit end), counters added once after the run | 4.047 4.084 4.033 / 4.056 4.050 4.015 | 4.428 4.463 4.459 / 4.451 4.441 4.463 | no (within spread) |
| c4 | The frame kept in the backend and initialised once; `Run` writes only the inputs and resets the outputs | 4.043 4.060 4.053 / 4.111 4.074 4.115 | 4.467 4.479 4.436 / 4.541 4.529 4.516 | no (+0.05-0.07: slower) |

The `ExitInfo` copy (32 bytes, two stores) was not changed. The guard object and the counters cost nothing measurable (x64 table-based unwinding makes the guard free on the normal path). Keeping the frame in the backend was slower, probably because generated code then addresses heap memory instead of the hot stack frame. The cost was in the runtime-library calls and the indirect call.

Progress (table above, all four titles, 3 rounds against Task 1's executable): x64 is 0.09-0.12 ms/frame faster on every title (PD2 4.112 against 4.199, Rally 4.474 / 4.564, SFZ3 4.006 / 4.128, BR 5.841 / 5.951). The run ranges do not overlap on PD2, SFZ3 and BR. On Rally they overlap by one round: this task's round 1, 4.600, against Task 1's 4.547-4.656, while each round pair improves. The profile's 0.22-0.39 ms/frame is reached in part. Sega Rally's ratio is now 1.057. `nativeBytesPerBlock` is unchanged within block-mix noise (master PD2 3,324, Rally 2,960, SFZ3 3,082, BR 3,109). The emitter is unchanged; only the frame offset of `out` moved. `Stats` are unchanged. A pinning test (`Executor: stats over a fuzz program are unchanged by the lean entry path`) holds the values recorded before the change. BIOS lockstep 1,200 frames x64 with `--jit-sync-compile` gives identical counters for both executables. Exact: lockstep BIOS 1,200 frames x64 and IR identical.

## Track F: the non-SH-2 rise

The question from "What 2E left": with the JIT, `Ymir_RunFrame` minus the SH2 total is larger than with the interpreter on every title. Measured on `fbe20e1` (after Tasks 1-2), Sega Rally and Panzer Dragoon II Zwei (US), `--warmup 2400 --frames 1800`, interpreter and x64 with the worker, three interleaved rounds per variant, four sessions. The rise is now **+0.58 ms/frame on Sega Rally** (2.514 -> 3.090, second session 2.495 -> 3.089) and **+0.27 on PD2** (1.496 -> 1.768; 1.480 -> 1.745), uninstrumented `build-bench`.

**Method.** A temporary build (`build-instr`, not committed, reverted) added time-stamp-counter timers to `Saturn::Run`, per scheduler event type, around the emulation thread's four waits on the render threads (`VDP1EraseFramebuffer`, `VDP1SwapFramebuffer` (two), `VDP2EndFrame`), and the render threads' busy time. A later build also split `Saturn::Run` by vertical phase and recorded, per scanline, when the emulation thread enqueues the line and when the VDP2 thread finishes it. The x64 runs' since-start `blocksRun` / `interpreted` (both CPUs) and window builds / native compiles equal the uninstrumented runs' in sessions 1-3; session 4 ran the instrumented build only and its counters were not compared. The timers add 0.06-0.08 ms/frame to the non-SH-2 part on both modes, and the rise under them equals the uninstrumented one (Rally +0.586, PD2 +0.267).

### Split (session 1, instrumented medians, ms/frame)

| Part of `Ymir_RunFrame` outside the SH2 total | Rally interp | Rally x64 | Rally rise | PD2 interp | PD2 x64 | PD2 rise |
|---|---|---|---|---|---|---|
| Waiting for the render threads (of which `VDP2EndFrame`) | 0.902 (0.876) | 1.302 (1.272) | **+0.400** | 0.084 (0.009) | 0.249 (0.167) | **+0.165** |
| VDP phase events, without the waits | 0.073 | 0.154 | +0.081 | 0.060 | 0.101 | +0.041 |
| SCSP sample events (M68K + SCSP) | 1.062 | 1.124 | +0.062 | 0.805 | 0.851 | +0.046 |
| SH-2/SCU loop outside the SH-2 timers (`SCU.Advance`, loop) | 0.323 | 0.347 | +0.024 | 0.317 | 0.333 | +0.016 |
| `VDP.Advance`, CD block, SMPC, scheduler and `Run` overhead (timers included), outside `Saturn::Run` | 0.216 | 0.228 | +0.012 | 0.316 | 0.311 | -0.005 |
| **Total** | 2.574 | 3.160 | **+0.586** | 1.577 | 1.844 | **+0.267** |

Two thirds of the rise (68% Rally, 62% PD2) is the emulation thread waiting at the end of the frame for the VDP2 render thread. The rest, 0.10-0.19 ms/frame, is real extra work on the emulation thread, spread over every other component: VDP phase handling +0.04-0.08, M68K/SCSP +0.05-0.06 (+5-6%), SCU and the loop +0.02 (+7.4% Rally, +5.0% PD2).

### Experiments (one variable each)

- **Threaded VDP off** (VDP1/VDP2 rendering on the emulation thread; sessions 1 and 2). No waits remain. The rise falls to Rally +0.160 / +0.129 and PD2 +0.100 / +0.039, and the average frame is shorter with x64 on both titles (Rally 14.042 -> 13.943, PD2 10.026 -> 9.187). VDP rendering itself runs about 1% slower after JIT code (Rally VDP phase events 5.868 -> 5.935, PD2 3.870 -> 3.922; session 2 3.896 -> 3.887).
- **Render-thread speed** (session 2). The render threads are not slowed by the JIT. VDP2 thread busy time is Rally 6.098 -> 6.046 ms/frame and PD2 4.147 -> 4.051; VDP1 thread 2.263 -> 2.306 and 0.136 -> 0.140. On Sega Rally the VDP2 thread is busy 6.1 of the 7.2-7.6 ms frame.
- **Where in the frame** (session 3: `Saturn::Run` by phase; session 4: per-line timeline):
  - *Sega Rally.* The emulation thread enqueues lines 16-112 0.12-0.34 ms later with x64 than with the interpreter: the emulation thread is slower early in the active display (all components included). It catches up by line 208 (-0.03) and reaches `VDP2EndFrame` at the same time (6.649 against 6.655 ms after the frame start). But the VDP2 thread is behind from the first lines on (its lag grows from 0.04 to 1.2-1.6 ms), so the delay at lines 48-96 carries to the last line. Lines 96-223 render at the same rate (28.1 against 27.7 us per line), and the last line finishes 0.416 ms later. The emulation thread waits for exactly that: it leaves `VDP2EndFrame` at 7.421 against 7.839. (Means of three runs. x64 round 3 was disturbed, with 17 frames over 16.7 ms. Without it, the pattern is the same per run: line 64 is enqueued at 2.76-2.82 against 2.46-2.52 ms, the emulation thread arrives at 6.46-6.64 against 6.59-6.70, and it leaves at 7.65-7.80 against 7.33-7.55.)
  - *PD2.* The interpreter's emulation thread is slower than the VDP2 thread (lag a constant 0.03 ms). With x64 it enqueues the last line 0.93 ms earlier and becomes faster than the VDP2 thread, so the lag grows to 0.44 ms. The wait at `VDP2EndFrame` grows from 0.01 to 0.11 ms and the other components take about 0.09 ms more, so the frame is 0.75 ms shorter against an SH-2 saving of 0.94. The VDP2 thread now bounds PD2's frame too, so further SH-2 savings late in its active display will not shorten the frame.
  - *By phase* (session 3). In the active display x64 saves 0.19 (Rally) and 0.95 (PD2) ms/frame of SH-2 loop time. In the rest of the frame (bottom border, blanking, top border) it spends 0.07 / 0.03 more. That non-active time, waits excluded, grows by 0.08 / 0.05.
- **Smaller native-code footprint** (`kNativeCompileThreshold` 16 -> 1000, session 1). Peak native code fell from 29.4 to 9.1 MB (Rally master) and from 29.0 to 9.6 MB (PD2), and SH-2 time rose by 0.31 / 0.15. The non-wait part of the rise is unchanged: Rally VDP phase 0.154 -> 0.163, SCSP 1.124 -> 1.125; PD2 likewise. Only the wait shrank (-0.09 / -0.04), because the emulation thread is slower. The effect does not scale with the amount of native code (tested at about a third; 9 MB is still far beyond L2, so this does not exclude an effect of the code's footprint as such).
- **No SMT sharing** (process affinity one logical CPU per core, session 1). The rise is unchanged (Rally +0.556, PD2 +0.258; waits +0.387 / +0.169).
- **Accounting boundary.** All of the JIT's emulation-thread work runs inside `SH2::Advance`'s host timer: `Executor::Run`, the worker's install in `Get`, and the stale checks (prologues, `IsCurrent`). By code reading (not measured), nothing hooks bus writes outside it. Time inside the SH-2/SCU loop but outside the timers grows by only 0.016-0.024 ms/frame, which includes the calls into `Advance`. No meaningful part of the JIT's work is booked outside the SH-2 total.
- **Not run.** WSL `perf stat` cache-miss counters: no `perf` in the WSL install.

### Conclusion

The rise is mostly not work outside the SH-2s. It is the emulation thread waiting for the VDP2 render thread, which bounds the frame. With threaded VDP, the frame lasts roughly until the VDP2 thread has rendered the last line, and on these titles the VDP2 thread falls behind the emulation thread for most of the active display (always on Sega Rally, with x64 on PD2). SH-2 time saved after the VDP2 thread falls behind cannot shorten the frame: it turns into waiting at `VDP2EndFrame`. Time lost on the emulation thread before that point (from any component) delays every later line.
- On Sega Rally the emulation thread is slower with the JIT than with the interpreter in the first half of the active display and faster later. The timeline measures when the emulation thread enqueues each line, which covers every component, not only the SH-2s: it shows that the thread is slower early, not which component causes it, and this was not tested. The non-SH-2 components running 5-7% slower (below) are a possible cause. So the frame is 0.42 ms longer, although the emulation thread reaches the end of the frame at the same time.
- On PD2, 0.10-0.17 ms/frame of the 0.9 ms/frame saved is absorbed this way (the wait's rise in the four sessions).
- The remainder, 0.10-0.19 ms/frame (0.04-0.16 with threaded VDP off), is real: the emulation thread's other components run slower after JIT code, spread thinly: M68K/SCSP +5.8% / +5.7%, the SCU and loop +7.4% / +5.0% (Rally / PD2), VDP rendering (threads off) about +1%, and the small VDP phase handling 0.04-0.08 ms/frame more. It does not scale with the amount of native code (tested at about a third) and is unchanged without SMT sharing. Interference in shared L1/L2 or branch-predictor state on the emulation thread is not excluded, and was not shown: there were no hardware counters.

This has two consequences for the bar's condition 2 (average frame no longer than the interpreter's):
- The average frame rewards SH-2 savings only up to the VDP2 thread's rate, and only where they fall before the VDP2 thread falls behind.
- On Sega Rally, the timeline suggests that condition 2 needs the emulation thread to be faster in the first half of the active display, not only on average; the SH2 total does not show this. Which component slows that part was not tested.

**Candidate fix.** No small, evident fix on the JIT side.
- *Off the JIT path:* the fix is VDP2 rendering speed or parallelism, which would turn the hidden saving into frame time: up to the 0.40 / 0.17 ms/frame of added waiting on these titles, and more on PD2, where the VDP2 thread now bounds the frame.
- *JIT side:* speed in the part of the frame before the VDP2 thread falls behind (on Rally, the early active display) is what could pay off in whole-frame time, not speed after it. The measurements do not say whether the early delay is SH-2 time or the 5-7% slower non-SH-2 components; that needs a per-component split by scanline range first. The remaining 0.10-0.19 ms/frame has no single target.

## Track F: long-run slow frames

The question from "What 2E left": Burning Rangers over `--warmup 2400 --frames 33600` had 571 frames over 16.7 ms against 1 for the interpreter, all with front-end builds, with only 1-5 ms of compile time in most of them against about 8.8 ms of excess. Measured on `49cdfc2` (after Tasks 1-2), x64 with the worker against the interpreter, one run per mode and variant.

**Machine state.** This session's machine was slower than in every earlier session, with either executable. The same long run with the interpreter takes 12.276 ms/frame (SH2 7.980) against 9.380 (5.954) in the 2E record: 31% more time per frame (34% in SH2 time). A 1,800-frame Burning Rangers control with the interpreter gives 12.5-12.9 ms/frame (SH2 8.7-9.0), with Task 4's executable as with this one, against 8.597 (6.255) in the Task 3 rows above: 45-50% more time per frame (39-44% in SH2 time). The PC had rebooted 11 minutes before the first run. A kernel thread used one logical CPU at 100% until about 55 minutes after the reboot; the control stayed slow after it stopped, and also with the process kept off that core. The cause was not established; the memory reports 2400 MT/s, and its earlier speed is not known. So the absolute numbers below are not comparable with earlier sections. Every comparison is made within the session: x64 against the interpreter at the same frame index (the runs are deterministic; per-frame CD-block event time correlates 0.996 at offset 0 and about 0 at offsets 1-60). As the 16.7 ms line no longer separates the same frames, the counts are also given at **21.9 ms**, which is 16.7 scaled by the long run's own slowdown (the interpreter's average frame, 12.276 / 9.380 = 1.31).

### Re-measure (uninstrumented `build-bench`)

| Burning Rangers, frames 2,400-36,000 | ms/frame avg | p95 | p99 | max | over 16.7 ms | over 21.9 ms (instrumented run) | SH2 total | SH2-total ratio, interpreter / x64 |
|---|---|---|---|---|---|---|---|---|
| interpreter | 12.276 | 15.190 | 17.459 | 32.8 | 709 | 0 | 7.980 | - |
| x64, worker | 12.998 | 19.307 | 23.017 | 73.2 | 3,856 | 616 | 8.129 | 0.982 |

The over-21.9 counts come from the instrumented run of each mode (build 1, below), not from the uninstrumented runs: their SH2 totals are within 0.5% of these runs' (8.016 and 8.116) and their `blocksRun` and `interpreted` counts are identical to the x64 run's. The interpreter's 0 is the instrumented interpreter run's (max 20.95 ms); the uninstrumented interpreter run has a maximum of 32.8 ms, and its count over 21.9 ms was not taken. **On the scaled threshold there is no sign that Tasks 1-2 changed the stutter: 616 frames against 0 for the interpreter, and 571 against 1 in 2E.** The two x64 figures are from different machine states, so they are not directly comparable. Window counters (x64): 312,072 front-end builds, 897,456 IR reuses, 817,664 native reuses, 28,751 native compiles installed and another 20,353 dropped on arrival (49,104 compiles submitted to the worker; the 20,353 are 41% of them), 7.8 s (0.232 ms/frame) of compile time on the emulation thread, worker 0.649 ms/frame, 8,296 frames compiling, 3 flushes (the slave's SSHON resets), 1 IR-cap event (4.3 ms). Of the 616 frames, 615 contain front-end builds and 612 have invalidations. They fall in the same code-load phases as in 2E: 375 in frames 9,000-16,199 and 231 in 23,400-28,799, the first at frame 5,751. The five slowest frames are 9,264 (3,610 builds), 9,273, 7,014, 9,270 and 9,267.

### Method

A temporary build (`build-instr`, not committed, reverted) kept Task 4's timers: the emulation thread's four waits on the render threads, and per scheduler event type. It added per-frame deltas:
- executor: rdtsc around each step, each `BlockCache::Get`, each IR run (split by the block's state: compile pending on the worker, runs left before the threshold, other), each interpreter fallback, and each invalidation after a stale report; IR runs, with their instructions;
- cache: the time from submit to install of each worker compile, the largest pending count, and `InstallResults`;
- interpreter mode: a count of the instructions run.

The same frame executes the same instructions in both modes. The interpreter's instruction count for a frame, minus the x64 run's IR and fallback instructions, therefore gives the instructions run as native code, and their cost in the interpreter at that frame's interpreter ns/instr. Native time is the step time minus `Get`, IR runs, fallbacks and stale invalidations; it includes chaining, resumes and block prologues.

The step timer adds 0.84 ms/frame to the x64 SH2 total (34,000 steps per frame in every frame). It cancels out when slow and normal frames are compared. The x64 timings come from that build (second build); the interpreter timings come from the first build, without the instruction counter. Normal frames are, for each slow frame, the nearest frame not over the line, so they come from the same phases. The x64 timer overhead makes the absolute excess in the breakdown about 0.8 ms high in every column.

**Two sets of slow frames.** Two instrumented x64 runs are used, and their slow-frame sets differ.
- **Build 1** (no step timers) is the headline set above: 4,670 frames over 16.7 ms and 616 over 21.9 ms. The by-window distribution comes from it.
- **Build 2** (with the step timers, +0.84 ms/frame of SH-2 time in every frame, which pushes more frames over the line) has 4,709 frames over 16.7 ms in the bench's own count and 862 over 21.9 ms. The per-frame lines print three decimals, and four frames print as exactly 16.700, which most likely accounts for the 4,707 used in the tables (the other two). The breakdown, the attribution and the options below use build 2 only, because their rows need the step timers; their counts are against 4,707 / 862, not 4,670 / 616.

### Breakdown (build 2; ms per frame unless a count)

| | over 16.7 ms (4,707) | their normal frames | over 21.9 ms (862) | their normal frames |
|---|---|---|---|---|
| Frame, x64 / interpreter (same frame) | 19.88 / 14.34 | 12.39 / 11.63 | 24.54 / 14.50 | 14.33 / 12.52 |
| **Excess** | **5.53** | 0.76 | **10.04** | 1.82 |
| SH-2 total, x64 / interpreter | 14.23 / 9.71 | 7.47 / 7.09 | 19.36 / 10.75 | 9.79 / 8.43 |
| Waits for the render threads, x64 / interpreter | 1.08 / 0.99 | 1.21 / 1.13 | 0.11 / 0.14 | 0.45 / 0.49 |
| Other emulation-thread work, x64 / interpreter | 4.56 / 3.64 | 3.71 / 3.40 | 5.07 / 3.61 | 4.10 / 3.60 |
| Front-end builds (count) / their time | 50.0 / 0.85 | 4.2 / 0.07 | 114.4 / 1.90 | 6.9 / 0.10 |
| Prepare + Commit (emulation thread) | 0.46 | 0.05 | 1.16 | 0.15 |
| Rest of `Get` (lookup, validation, reuse, invalidation, install) | 0.86 | 0.22 | 1.44 | 0.41 |
| IR runs (count) / time | 637 / 0.55 | 51 / 0.05 | 1,526 / 1.22 | 101 / 0.10 |
| of which compile pending on the worker | 0.03 | 0.00 | 0.10 | 0.01 |
| IR instructions (of which on blocks waiting for the worker) | 10,478 (638) | 801 | 23,445 | 1,338 |
| Interpreter fallbacks (count) / time | 2,816 / 0.12 | 3,485 / 0.09 | 2,416 / 0.13 | 3,097 / 0.10 |
| Invalidations after a stale report | 0.06 | 0.01 | 0.11 | 0.01 |
| Native code (rest of the steps) | 10.36 | 6.06 | 12.38 | 7.99 |
| Native / interpreter ns per instruction | 17.3 / 15.9 (1.09) | 12.1 / 14.1 (0.86) | 19.8 / 16.5 (1.20) | 14.4 / 15.0 (0.96) |
| IR ns per instruction | 52.5 | 66.9 | 52.1 | 71.0 |
| Invalidations / stale entries | 326 / 133 | 26 / 11 | 667 / 266 | 32 / 11 |
| Native reuses / IR reuses | 133 / 148 | 12 / 11 | 263 / 307 | 14 / 15 |
| Installs / dropped on arrival | 3.9 / 3.6 | 0.8 / 0.3 | 9.2 / 9.4 | 3.0 / 0.6 |
| Pending at the frame's end / most pending | 1.5 / 4.0 | 0.9 / 1.6 | 6.5 / 11.4 | 4.7 / 6.8 |
| Worker busy | 3.55 | 0.38 | 8.93 | 1.05 |
| Instructions (thousands) / native blocks run / chained | 612 / 122k / 91k | 503 / 140k / 109k | 653 / 114k / 83k | 561 / 133k / 102k |

### Where the excess goes

Growth of the excess from normal to slow frames (slow minus normal; build 2). The shares are shares of this growth (4.78 and 8.23 ms), not of the slow frames' excess over the interpreter (5.53 and 10.04 ms), rounded to the nearest percent. IR runs, fallbacks and native code are counted net of the interpreter's cost for the same instructions:

| Part | over 16.7 ms: +4.78 | over 21.9 ms: +8.23 |
|---|---|---|
| Native code slower than usual against the interpreter | +1.84 (38%) | +2.43 (30%) |
| Front-end builds | +0.78 (16%) | +1.79 (22%) |
| Prepare + Commit on the emulation thread | +0.40 (8%) | +1.01 (12%) |
| Rest of `Get` | +0.63 (13%) | +1.02 (12%) |
| IR runs before the native code arrives (net) | +0.34 (7%) | +0.77 (9%) |
| of which waiting for the worker (net) | +0.02 | +0.06 |
| Stale invalidations, fallbacks, `Advance` outside the steps | +0.15 (3%) | +0.23 (3%) |
| Other emulation-thread work (VDP phase events without the waits, SCSP) | +0.61 (13%) | +0.96 (12%) |
| Waiting for the render threads | +0.01 | +0.02 |

- **The compile side on the emulation thread** is three rows together, the front end, Prepare/Commit and the rest of `Get`: 1.8 ms (38%) over 16.7 ms and 3.8 ms (46%) over 21.9 ms, level with the native-code row over 16.7 ms and above it over 21.9 ms. Compile time as the bench counts it (builds plus Prepare/Commit) is 1.3 ms (3.1 ms over 21.9 ms) in the slow frames themselves. Each build costs 17 us and each Prepare/Commit 55-60 us. Only about 16% of builds are ever submitted for a native compile (window: 28,751 installed plus 20,353 dropped = 49,104 submissions, against 312,072 builds), and 41% of the submissions (20,353 of 49,104) are dropped on arrival, because the code was rewritten before the result came back.
- **Native code runs slower per instruction in these frames**: 1.09-1.20 times the interpreter's cost, against 0.86-0.96 in the frames around them, while the interpreter's cost per instruction rises only 10-13%. Over 16.7 ms this is the largest single row (38% of the growth; 30% over 21.9 ms). It rises with the number of builds in the frame: the native / interpreter ratio is 0.88 with no builds, 0.93 with 1-9, 1.05 with 10-49, 1.07 with 50-99 and 1.14 with 100 or more. Over the same buckets, the other emulation-thread work rises from +0.35 to +1.24 ms against the interpreter. In the 54 frames with 50 or more invalidations and no builds, the ratio stays at 0.88. *Inferred, not shown* (no hardware counters): the front-end and asmjit work on the emulation thread displaces the caches and branch predictors that the native code and the other components use; the code-load phases' instruction mix may add to it.
- **IR runs**: 52-71 ns per instruction against 15-16 for the interpreter, so 3.2-4.7 times slower. Almost all are runs before the threshold (0.52 of 0.55 ms; 1.12 of 1.22); runs waiting for the worker are 0.03-0.10 ms.
- **The worker** is not the bottleneck in these frames: the most compiles pending in a frame averages 4-11, and the worker is busy 3.6-8.9 ms per frame. Install latency averages 10-21 ms, which costs only the 0.03-0.10 ms of IR runs above. The exception is the burst frames: frame 9,264 has 3,610 builds, 43.7 ms of `Get` (21.1 ms of builds and 15.9 ms of Prepare/Commit) and 8.9 ms of IR runs waiting for the worker, and frame 9,270 has up to 455 pending, with installs 62 ms after submission.
- **Outside the SH-2s**: 0.6-1.0 ms of real extra work. The waits for the VDP2 thread do not grow (+0.01-0.02): in these frames the emulation thread is too slow to wait.

### Experiment: a lower native threshold (`kNativeCompileThreshold` 16 -> 4, every block)

Same instrumented executable, same run:
- SH2 total 9.990 against 8.957 ms/frame;
- frames over 16.7 ms 6,755 against 4,709 (the bench's counts), worst frame 118.4 against 74.6 ms;
- Prepare/Commit time on the emulation thread 24.9 s against 3.2 s;
- native compiles installed 32,546 against 28,848, and dropped on arrival 299,874 against 20,614.

This is the expected sign. A block before the threshold costs at most 15 IR runs of about 0.6 us more than interpreting them (about 9 us), against 55-60 us of Prepare/Commit per submission. With 4, 90% of the submissions were dropped on arrival (299,874 of the 332,420 submitted, which is 32,546 installed plus 299,874 dropped), against 42% with 16 (20,614 of 49,462): the code was rewritten before they came back. Only the global threshold was tried, not a threshold for returning code alone. That variant would presumably lose as well (inferred, not run), since the code that returns here is the code being rewritten (native and IR reuses rise with the invalidations, table above).

### Options, ranked (estimated from the breakdown; not implemented)

All figures in this section use build 2 and its slow-frame set (4,707 frames over 16.7 ms, 862 over 21.9 ms), with the savings taken from the breakdown rows above. Each frame's saving is subtracted from its own time to count the frames still over the line. A cold instruction interpreted instead of run as IR is counted at 15.9 ns, the interpreter's cost per instruction in the slow frames. Shares are of the slow frames' mean excess over the interpreter (5.53 ms, of which about 0.8 ms is the step timers' overhead, so the shares are somewhat low), rounded to the nearest percent. The counts after each option also include that overhead, so they are pessimistic by a few hundred frames.

| Option | Mean saving, frames over 16.7 ms | Share of the excess | Frames over 16.7 / over 21.9 ms after it |
|---|---|---|---|
| (now) | - | - | 4,707 / 862 |
| 1. Front end and Prepare off the emulation thread, cold code interpreted until the native code arrives | 1.69 ms | 31% | 3,792 / 157 |
| 2. Front end off the emulation thread (Prepare stays), cold code interpreted meanwhile | 1.24 ms | 22% | 4,025 / 295 |
| 3. Interpret cold code and build a block only at its threshold run | 1.10 ms | 20% | 4,086 / 333 |
| 4. Interpret cold code instead of running it as IR, building as now | 0.38 ms | 7% | 4,515 / 643 |
| 5. A cheaper `Get` on the reuse and invalidation paths | at most 0.63 ms (1.02 ms over 21.9 ms) | at most 11% | not estimated |
| 6. More workers | 0.02 ms | 0% | 4,699 / 855 |
| 7. A lower threshold | negative (measured above) | - | 6,755 / not taken |

Formulas, from the breakdown rows (slow frames over 16.7 ms; `IRnet` = IR runs 0.55 ms minus their 10,478 instructions at 15.9 ns, 0.17 ms, = 0.38 ms):
- Option 1 = builds 0.85 + Prepare/Commit 0.46 + `IRnet` 0.38 = **1.69 ms**.
- Option 2 = builds 0.85 + `IRnet` 0.38 = **1.24 ms**.
- Option 3 = 84% of builds 0.85 (0.72) + `IRnet` 0.38 = **1.10 ms**. The 84% is the builds never submitted for a native compile (1 - 49,104 / 312,072), taking all builds as equally costly.
- Option 4 = `IRnet` = **0.38 ms**.
- Option 5 = the growth of the rest of `Get` from normal to slow frames (attribution table), **0.63 ms**. It is not a design's saving but the most any change to those paths could recover, and it is listed below options 1-4 for that reason, although its bound is larger than option 4's estimate. The same growth over 21.9 ms is 1.02 ms.
- Option 6 = IR time of blocks waiting for the worker 0.03 ms minus their 638 instructions at 15.9 ns (0.01 ms) = **0.02 ms**. In single frames it can be more: frame 9,264, the worst, has 8.9 ms of such IR runs, 6.0 ms net of interpreting their 181,986 instructions; only 14 frames have more than 1 ms and 3 more than 3 ms.
- Option 7 is the measured experiment above.

The estimates leave out the cost of the new designs (a snapshot of the guest code for off-thread builds, per-PC run counters, a whole-block interpreted path), and the interpreter rate used is optimistic: 15.9 ns is the interpreter loop's, while the executor's own one-instruction fallback costs 26-52 ns per instruction because it repeats the step gates and the lookup. An interpreted cold path must run whole blocks, otherwise it costs what IR costs.

Options 1 and 2 cross the 2E thread boundary: only `Finalize` runs on the worker today, and `Prepare` and the front end read guest memory and the page table on the emulation thread. They need a design for a consistent snapshot of the guest code, and for what runs until the result is back. Option 3 stays on the emulation thread, but needs a run counter per cold PC and an interpreted path that runs whole blocks cheaply. If the native slowdown and the extra non-SH-2 work (2.4-3.4 ms, 41-51% of the growth) are cache displacement by the compile work (inferred), options 1-3 would also recover part of them; their numbers above do not include that.

**Conclusion.** The long run's slow frames are code-load phases, in which the guest rewrites and reloads code (on average 300-700 invalidations per frame, against about 30 in the frames around them). The excess over the interpreter is not compile time alone. Most of the named part is the emulation thread doing JIT work for code that mostly never becomes native. The growth of the excess over the frames around them breaks down as:
- builds, Prepare/Commit and `Get` work: 38-46%;
- native code at 1.09-1.20 times the interpreter's cost per instruction instead of 0.86-0.96, and the other components 0.6-1 ms slower, in the same frames (inferred cache displacement): 41-51%;
- IR runs before the threshold: 7-9%.

The worker and the render threads are not the bottleneck. The obvious next step is to keep cold code off the JIT's slow paths: interpret it, and build or compile only what reaches the threshold, ideally away from the emulation thread (options 1-3).

**Caveat: machine state.** All of this was measured on a machine that took 31-50% more time per frame than in earlier sessions, for a reason not found (see "Machine state"; its memory ran at 2400 MT/s). If memory or cache latency is what changed, the native-slowdown share and the other-components share, which are the cache-displacement part, may be overstated against a healthy machine, and the compile-side shares may shift too. Re-measure on a healthy machine before relying on the split or on the option ranking.

## Track F results

**Date**: 2026-10-22
**Commit**: 628fa69 (`feat(bench): --no-threaded-vdp`; the head of `feature/sh2-jit-2e-f` before the commit that adds this section. The only source change since task 2 (`3594712`, the last commit that changed the JIT; the commits between are docs) is the bench option. Tasks 1 and 2 are the resume and lean-entry changes; task 3 was dropped). The measured executable (`build-bench`, Ninja, Release, LTO/IPO, MSVC) was copied aside before the runs.
**Machine**: AMD Ryzen 7 5700G (8 cores / 16 threads), Windows, the 2C method. Memory: all four DIMMs run at 3200 MT/s (`Get-CimInstance Win32_PhysicalMemory`: `ConfiguredClockSpeed` 3200 on all four), after the BIOS memory configuration was fixed; the task 5 session ran at 2400 MT/s and 30-40% slow. **Machine check before any timed run:** CPU load 8-13%, and Panzer Dragoon II Zwei in the interpreter (`--warmup 2400 --frames 1800`) read 4.966 ms/frame of SH-2 total against 4.885 in the 2E record (+1.7%, under the 8% limit), so the machine is back to its earlier speed.

**Verdict: the default-on bar (`sh2-jit-m2e.md` section 3) is not met. Condition 2 fails as the design states it; conditions 1, 3 and 4 hold.**

- **Faster (1): holds.** Interpreter / x64 SH-2 total is above 1.0 on all six titles (1.057 to 1.394; Sega Rally, the lowest, 1.057, and 1.088 in a second session) and on the long Burning Rangers run (1.052). No title is within 0.02 of 1.0, so the second-session rule did not apply. The 2E failures are gone: Sega Rally 0.957 -> 1.057, Burning Rangers 1.008 -> 1.099, the long run 0.969 -> 1.052.
- **Smooth (2): fails (a), the design as written (threaded VDP on).** Frames over 16.7 ms: the 1,800-frame windows pass (0 against 0 on all seven), but the long Burning Rangers run has **358 against 0** for the interpreter. Average frame: x64 is above the interpreter's on **Sega Rally (7.697 against 7.296 ms, +5.5%; session 2 +2.8%)**, **Street Fighter Zero 3 (+2.9%; session 2 +2.4%)** and **the long run (9.720 against 9.353, +3.9%)**. Burning Rangers' window passes now (0.965; session 2 0.958).
- **Smooth (2), (b): threaded VDP off, same session, interpreter and x64.** The average-frame part holds on all seven windows (x64 / interpreter 0.870 to 0.980, in both sessions) and on the long run (15.831 against 16.059 ms, 0.986). The frames-over-16.7 ms part stays as the design states it (threaded VDP on) and fails on the long run in (a). Task 4 found that most of the JIT's extra non-SH-2 time is the emulation thread waiting for the VDP2 render thread; (b) removes that wait.
- **Exact (3): holds.** Lockstep is identical for 36,000 frames on x64 (BIOS menu and six titles, with the worker), 1,800 frames on IR, 3,600 frames with `--lockstep-reload 300` on both backends and 1,800 frames on x64 with `--jit-sync-compile`: 35 runs, all exit 0 with `compileFallbacks 0`, `blocksRun` and `interpreted` equal to 2E's on both CPUs (`design/sh2-validation.md`, "Milestone 2E, track F"). `[jit]` (216 cases), the whole suite (343 cases) and `ctest` pass on Windows x64; the Raspberry Pi 3's IR lockstep (7 of 7), its `[jit]` tests and `ctest` pass; CI of `628fa69` (run 39803140783): green: test-windows 6m19s, test-linux-x64 12m44s, test-linux-tsan 16m32s, build-macos-arm64 2m10s (its first attempt was cancelled after 15 minutes in the runner queue and re-run).
- **Memory (4): holds.** The long run has 0 code-cap flushes on both CPUs and a native-code peak of 79.4 / 6.8 MB (master / slave), under 128 MB; the 36,000-frame lockstep runs peak at 113.8 MB (Panzer Dragoon II Zwei, master), 99.7 (Virtua Fighter 2) and 93.5 (Street Fighter Zero 3), all under 128 MB with 0 flushes.

**Step 4 of the plan, the rollout, did not run.** It needs all four conditions with (a), the design as written (controller ruling R4); the bar holds only with threaded VDP off for the average-frame part, and the frames-over part fails either way. The JIT stays off by default, and `brimir_sh2_jit` has no `auto` value.

### Method

The 2C method, as in 2E: `--warmup 2400 --frames 1800` for the games and `--warmup 300 --frames 1800` for the BIOS menu, default settings, a scratch system dir with `brimir_saturn_rtc_us_eu.smpc` also as `brimir_saturn_rtc_jp.smpc`, three rounds, each round running every title in the interpreter, `--sh2-jit --jit-backend ir` and `--sh2-jit --jit-backend x64` (worker) back to back (63 runs), plus one round of `--jit-backend x64 --jit-sync-compile` (7 runs, reference), and the long Burning Rangers run (`--warmup 2400 --frames 33600`, US BIOS) in the interpreter, x64 with the worker and x64 synchronous (one run each). Then, in the same session, the same table runs for the interpreter and x64 only with **`--no-threaded-vdp`** (VDP1 and VDP2 rendered on the emulation thread; 42 runs, a new bench option) and the long run in the interpreter and x64 with it (2 runs): condition 2's average-frame part (b). A second session (after the first, same executable, the three titles whose average frame was in question: Sega Rally, Burning Rangers, Street Fighter Zero 3) ran the interpreter and x64, three rounds, with threaded VDP (a) and without (b): 36 runs. All 153 timed runs of the batches exited 0 (and the step 0 check) and every x64 run reports `compileFallbacks 0` on both CPUs. The table shows the run with the median SH2 total per mode and the three runs' SH2 totals, max frame times and frames over 16.7 ms. Scripts and raw outputs: `.superpowers/sdd/2026-10-21-sh2-jit-m2e-track-f/task-6-data/` (`s1` session 1 (a), `s1/nv` session 1 (b), `s2`, `s2/nv`, lockstep `ls`, `measure.ps1`, `measure-long.ps1`, `gen*.ps1`); the table scripts are those of 2E (`parse.ps1`, `cond2.ps1`) with the same medians.

**Load.** The development PC read 8-13% CPU load before the batch; nothing but the usual desktop applications (Spotify, opencode, a terminal) and the measurement ran, and no lockstep run or build was active. The Raspberry Pi work ran on the Pi during session 1; the only local parts were a `git archive`, an `scp` and an `rsync` at the start of that batch and some `ssh` polling, and a few table scripts ran during it. A 30-second sampler of the whole-machine load reads the benchmark's own threads too (threaded VDP, emulation thread, compile worker): session 1 35.7% on average (2-99%, 229 samples), session 2 18.5% (5-48%, 53 samples); the load at the end of each of the session 1 table runs (`Win32_Processor.LoadPercentage`) is 0-88% (average 12%). The interpreter totals of this session are 1.6-6.7% above 2E's session and the x64 totals 3.9-6.9% below, so ratios are compared only with this session's own runs.

### Results (session 1, threaded VDP on, (a))

| Title | Mode | ms/frame avg | p95 | max | SH2 master ms (%) | SH2 slave ms (%) | SH2 total ms (%) | SH2 total, 3 runs | max frame ms, 3 runs | frames over 16.7 ms, 3 runs |
|---|---|---|---|---|---|---|---|---|---|---|
| BIOS menu (US) | interpreter | 8.044 | 8.527 | 9.4 | 1.753 (21.8%) | 0.000 (0.0%) | 1.753 (21.8%) | 1.753 / 1.733 / 1.781 | 9.4 / 9.3 / 9.6 | 0 / 0 / 0 |
|  | IR | 8.608 | 9.025 | 11.0 | 6.176 (71.8%) | 0.000 (0.0%) | 6.176 (71.8%) | 6.176 / 6.158 / 6.341 | 11.0 / 10.8 / 11.0 | 0 / 0 / 0 |
|  | x64 (worker) | 7.906 | 8.403 | 9.0 | 1.162 (14.7%) | 0.000 (0.0%) | 1.162 (14.7%) | 1.108 / 1.183 / 1.162 | 9.2 / 9.6 / 9.0 | 0 / 0 / 0 |
|  | x64 (sync compile) | 7.881 | 8.613 | 10.2 | 1.273 (16.2%) | 0.000 (0.0%) | 1.273 (16.2%) | 1.273 | 10.2 | 0 |
| Virtua Fighter 2 (JP) | interpreter | 6.360 | 7.707 | 9.8 | 2.874 (45.2%) | 1.687 (26.5%) | 4.561 (71.7%) | 4.521 / 4.561 / 4.663 | 10.0 / 9.8 / 10.4 | 0 / 0 / 0 |
|  | IR | 19.382 | 23.745 | 27.9 | 10.156 (52.4%) | 7.279 (37.6%) | 17.435 (90.0%) | 17.435 / 17.433 / 17.597 | 27.9 / 24.7 / 28.1 | 1404 / 1358 / 1434 |
|  | x64 (worker) | 5.706 | 6.415 | 8.7 | 2.181 (38.2%) | 1.092 (19.1%) | 3.273 (57.4%) | 3.273 / 3.211 / 3.384 | 8.7 / 8.6 / 9.8 | 0 / 0 / 0 |
|  | x64 (sync compile) | 5.742 | 6.579 | 8.9 | 2.266 (39.5%) | 1.092 (19.0%) | 3.358 (58.5%) | 3.358 | 8.9 | 0 |
| Panzer Dragoon II Zwei (US) | interpreter | 6.459 | 7.592 | 10.7 | 3.214 (49.8%) | 1.762 (27.3%) | 4.975 (77.0%) | 4.975 / 5.385 / 4.973 | 10.7 / 10.6 / 8.2 | 0 / 0 / 0 |
|  | IR | 17.637 | 19.539 | 20.9 | 10.417 (59.1%) | 5.276 (29.9%) | 15.694 (89.0%) | 15.694 / 15.936 / 15.683 | 20.9 / 22.9 / 25.1 | 1106 / 1199 / 1120 |
|  | x64 (worker) | 5.835 | 7.223 | 8.2 | 2.692 (46.1%) | 1.370 (23.5%) | 4.062 (69.6%) | 4.044 / 4.062 / 4.162 | 8.2 / 8.2 / 8.6 | 0 / 0 / 0 |
|  | x64 (sync compile) | 6.022 | 7.518 | 9.0 | 2.823 (46.9%) | 1.409 (23.4%) | 4.231 (70.3%) | 4.231 | 9.0 | 0 |
| Sega Rally Championship (US) | interpreter | 7.296 | 8.597 | 10.1 | 2.686 (36.8%) | 2.111 (28.9%) | 4.797 (65.8%) | 4.782 / 4.871 / 4.797 | 12.3 / 11.7 / 10.1 | 0 / 0 / 0 |
|  | IR | 18.098 | 26.213 | 31.8 | 9.444 (52.2%) | 6.588 (36.4%) | 16.032 (88.6%) | 15.955 / 16.032 / 16.058 | 28.2 / 31.8 / 28.8 | 900 / 901 / 902 |
|  | x64 (worker) | 7.697 | 9.356 | 13.0 | 2.327 (30.2%) | 2.211 (28.7%) | 4.538 (59.0%) | 4.554 / 4.538 / 4.461 | 11.6 / 13.0 / 9.8 | 0 / 0 / 0 |
|  | x64 (sync compile) | 7.791 | 9.673 | 11.7 | 2.406 (30.9%) | 2.266 (29.1%) | 4.672 (60.0%) | 4.672 | 11.7 | 0 |
| Burning Rangers (US) | interpreter | 8.956 | 10.100 | 12.5 | 4.420 (49.4%) | 2.143 (23.9%) | 6.563 (73.3%) | 6.527 / 6.594 / 6.563 | 11.3 / 12.7 / 12.5 | 0 / 0 / 0 |
|  | IR | 23.689 | 25.037 | 28.2 | 14.049 (59.3%) | 6.898 (29.1%) | 20.948 (88.4%) | 21.043 / 20.899 / 20.948 | 28.2 / 30.8 / 28.2 | 1800 / 1800 / 1800 |
|  | x64 (worker) | 8.645 | 10.376 | 11.6 | 4.256 (49.2%) | 1.718 (19.9%) | 5.974 (69.1%) | 6.005 / 5.974 / 5.849 | 15.9 / 11.6 / 13.0 | 0 / 0 / 0 |
|  | x64 (sync compile) | 8.701 | 10.495 | 12.4 | 4.299 (49.4%) | 1.719 (19.8%) | 6.018 (69.2%) | 6.018 | 12.4 | 0 |
| Guardian Heroes (US) | interpreter | 6.211 | 7.150 | 8.7 | 2.505 (40.3%) | 1.547 (24.9%) | 4.053 (65.3%) | 4.023 / 4.138 / 4.053 | 9.7 / 9.6 / 8.7 | 0 / 0 / 0 |
|  | IR | 15.701 | 19.369 | 21.2 | 8.370 (53.3%) | 4.930 (31.4%) | 13.299 (84.7%) | 13.299 / 13.384 / 13.299 | 23.7 / 21.4 / 21.2 | 371 / 375 / 370 |
|  | x64 (worker) | 6.096 | 6.777 | 8.1 | 1.927 (31.6%) | 1.376 (22.6%) | 3.303 (54.2%) | 3.309 / 3.303 / 3.199 | 8.5 / 8.1 / 7.8 | 0 / 0 / 0 |
|  | x64 (sync compile) | 6.150 | 6.995 | 8.9 | 2.003 (32.6%) | 1.402 (22.8%) | 3.405 (55.4%) | 3.405 | 8.9 | 0 |
| Street Fighter Zero 3 (JP, 4 MB cart) | interpreter | 9.034 | 9.748 | 11.1 | 2.801 (31.0%) | 1.972 (21.8%) | 4.773 (52.8%) | 4.683 / 4.838 / 4.773 | 11.2 / 11.2 / 11.1 | 0 / 0 / 0 |
|  | IR | 18.279 | 21.849 | 25.8 | 10.181 (55.7%) | 6.008 (32.9%) | 16.189 (88.6%) | 16.511 / 16.189 / 15.791 | 25.3 / 25.8 / 23.6 | 1690 / 1499 / 1257 |
|  | x64 (worker) | 9.296 | 10.210 | 14.4 | 2.756 (29.7%) | 1.374 (14.8%) | 4.130 (44.4%) | 4.137 / 4.130 / 4.105 | 15.5 / 14.4 / 13.9 | 0 / 0 / 0 |
|  | x64 (sync compile) | 9.770 | 11.692 | 24.0 | 3.356 (34.4%) | 1.379 (14.1%) | 4.735 (48.5%) | 4.735 | 24.0 | 19 |

### Ratios (SH2 total, median runs) and 2E

| Title | interpreter / x64 (Track F) | 2E | change | x64 SH2 total Track F / 2E | change | interpreter SH2 total Track F / 2E | change |
|---|---|---|---|---|---|---|---|
| BIOS menu (reference) | **1.509** | 1.359 | +0.150 | 1.162 / 1.209 | -3.9% | 1.753 / 1.643 | +6.7% |
| Virtua Fighter 2 | **1.394** | 1.281 | +0.113 | 3.273 / 3.445 | -5.0% | 4.561 / 4.413 | +3.4% |
| Panzer Dragoon II Zwei | **1.225** | 1.120 | +0.105 | 4.062 / 4.361 | -6.9% | 4.975 / 4.885 | +1.8% |
| Sega Rally Championship | **1.057** | 0.957 | +0.100 | 4.538 / 4.815 | -5.8% | 4.797 / 4.607 | +4.1% |
| Burning Rangers | **1.099** | 1.008 | +0.090 | 5.974 / 6.245 | -4.3% | 6.563 / 6.298 | +4.2% |
| Guardian Heroes | **1.227** | 1.138 | +0.089 | 3.303 / 3.506 | -5.8% | 4.053 / 3.989 | +1.6% |
| Street Fighter Zero 3 | **1.156** | 1.033 | +0.123 | 4.130 / 4.371 | -5.5% | 4.773 / 4.515 | +5.7% |

The BIOS menu is not one of the six titles. The ratios are 0.089-0.150 higher than in the 2E session: the x64 SH-2 totals are 3.9-6.9% lower (tasks 1 and 2: resuming slices and leaner chain entry and exit), and the interpreters' are 1.6-6.7% higher in this session, so some of the difference is the session. The same-session measurements of the tasks are in "Track F progress" (Sega Rally 0.993 -> 1.057 from the base to task 2 in sessions of their own; Burning Rangers 1.038 -> 1.089; task 3 was dropped).

### Compile work, the worker and resumes (session 1, median x64 run)

| Title | interpreter / x64 (worker) | per-round ratios (interp r / x64 r) | IR / x64 | interpreter / IR | interpreter / x64 (sync) | x64 fallbacks m/s | worker ms/frame (med run) | native compiles in window (med run) | emulation-thread compile ms | async / dropped | flushes in window | code-cap flushes m/s (since start) | IR reuses |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| BIOS menu (US) | 1.509 | 1.582 / 1.465 / 1.533 | 5.315 | 0.284 | 1.377 | 0/0 | 0.079 | 473 | 32.7 | 473/0 | 0 | 0/0 | 0 |
| Virtua Fighter 2 (JP) | 1.394 | 1.381 / 1.420 / 1.378 | 5.327 | 0.262 | 1.358 | 0/0 | 0.085 | 959 | 35.8 | 959/0 | 0 | 0/0 | 0 |
| Panzer Dragoon II Zwei (US) | 1.225 | 1.230 / 1.326 / 1.195 | 3.864 | 0.317 | 1.176 | 0/0 | 0.058 | 660 | 26.4 | 660/0 | 0 | 0/0 | 0 |
| Sega Rally Championship (US) | 1.057 | 1.050 / 1.073 / 1.075 | 3.533 | 0.299 | 1.027 | 0/0 | 0.102 | 1056 | 44.3 | 1056/0 | 0 | 0/0 | 0 |
| Burning Rangers (US) | 1.099 | 1.087 / 1.104 / 1.122 | 3.507 | 0.313 | 1.091 | 0/0 | 0.074 | 821 | 34.8 | 821/0 | 0 | 0/0 | 0 |
| Guardian Heroes (US) | 1.227 | 1.216 / 1.253 / 1.267 | 4.026 | 0.305 | 1.190 | 0/0 | 0.066 | 667 | 29.3 | 667/0 | 0 | 0/0 | 0 |
| Street Fighter Zero 3 (JP, 4 MB cart) | 1.156 | 1.132 / 1.171 / 1.163 | 3.920 | 0.295 | 1.008 | 0/0 | 0.596 | 6333 | 253.3 | 6333/0 | 0 | 0/0 | 0 |

Resumes (`resumes / runs`, the share of `Executor::Run` calls that entered native code without `BlockCache::Get`) and chaining, median x64 run:

| Title | Resumes / Run calls, master | slave | chained blocks / native blocks run, master |
|---|---|---|---|
| BIOS menu (reference) | 99.4% | - | 82.2% |
| Virtua Fighter 2 | 91.5% | 99.1% | 77.0% |
| Panzer Dragoon II Zwei | 88.4% | 99.6% | 81.8% |
| Sega Rally Championship | 95.1% | 65.4% | 80.6% |
| Burning Rangers | 92.2% | 77.7% | 76.9% |
| Guardian Heroes | 96.5% | 99.7% | 78.4% |
| Street Fighter Zero 3 | 93.7% | 99.7% | 74.8% |

The worker's CPU time is 0.058-0.102 ms/frame on the BIOS menu and five of the six titles and 0.596 ms/frame on Street Fighter Zero 3; the long run uses 0.404 ms/frame (28,801 native compiles submitted).

### Second session (Sega Rally, Burning Rangers, Street Fighter Zero 3)

Run after the first session's batches, the same executable, interpreter and x64 (worker), three rounds each (36 runs, all exit 0, load 2-21% at the end of each run, sampler average 18.5%).

| Title | Mode | ms/frame avg | p95 | max | SH2 master ms (%) | SH2 slave ms (%) | SH2 total ms (%) | SH2 total, 3 runs | max frame ms, 3 runs | frames over 16.7 ms, 3 runs |
|---|---|---|---|---|---|---|---|---|---|---|
| Sega Rally Championship (US) | interpreter | 7.340 | 8.686 | 10.6 | 2.711 (36.9%) | 2.122 (28.9%) | 4.833 (65.9%) | 4.768 / 4.833 / 4.852 | 10.0 / 10.6 / 9.8 | 0 / 0 / 0 |
|  | x64 (worker) | 7.546 | 9.114 | 10.8 | 2.270 (30.1%) | 2.174 (28.8%) | 4.444 (58.9%) | 4.419 / 4.536 / 4.444 | 10.1 / 10.5 / 10.8 | 0 / 0 / 0 |
| Burning Rangers (US) | interpreter | 9.057 | 10.260 | 15.5 | 4.473 (49.4%) | 2.166 (23.9%) | 6.639 (73.3%) | 6.625 / 6.639 / 6.660 | 11.2 / 15.5 / 13.1 | 0 / 0 / 0 |
|  | x64 (worker) | 8.675 | 10.435 | 13.2 | 4.275 (49.3%) | 1.722 (19.8%) | 5.997 (69.1%) | 5.997 / 6.044 / 5.976 | 13.2 / 13.2 / 16.0 | 0 / 0 / 0 |
| Street Fighter Zero 3 (JP, 4 MB cart) | interpreter | 9.082 | 9.847 | 12.0 | 2.847 (31.3%) | 2.005 (22.1%) | 4.852 (53.4%) | 4.852 / 4.856 / 4.848 | 12.0 / 11.3 / 12.1 | 0 / 0 / 0 |
|  | x64 (worker) | 9.301 | 10.213 | 14.9 | 2.795 (30.1%) | 1.365 (14.7%) | 4.160 (44.7%) | 4.121 / 4.185 / 4.160 | 14.7 / 14.2 / 14.9 | 0 / 0 / 0 |

| Title | interpreter / x64 (worker) | per-round ratios (interp r / x64 r) | IR / x64 | interpreter / IR | interpreter / x64 (sync) | x64 fallbacks m/s | worker ms/frame (med run) | native compiles in window (med run) | emulation-thread compile ms | async / dropped | flushes in window | code-cap flushes m/s (since start) | IR reuses |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Sega Rally Championship (US) | 1.088 | 1.079 / 1.065 / 1.092 | - | - | - | 0/0 | 0.096 | 1056 | 42.8 | 1056/0 | 0 | 0/0 | 0 |
| Burning Rangers (US) | 1.107 | 1.105 / 1.098 / 1.114 | - | - | - | 0/0 | 0.074 | 821 | 34.6 | 821/0 | 0 | 0/0 | 0 |
| Street Fighter Zero 3 (JP, 4 MB cart) | 1.166 | 1.177 / 1.160 / 1.165 | - | - | - | 0/0 | 0.583 | 6333 | 255.8 | 6333/0 | 0 | 0/0 | 0 |

| Title | session 1 | session 2 |
|---|---|---|
| Sega Rally Championship | 1.057 (rounds 1.050 / 1.073 / 1.075) | 1.088 (1.079 / 1.065 / 1.092) |
| Burning Rangers | 1.099 (1.087 / 1.104 / 1.122) | 1.107 (1.105 / 1.098 / 1.114) |
| Street Fighter Zero 3 | 1.156 (1.132 / 1.171 / 1.163) | 1.166 (1.177 / 1.160 / 1.165) |

### Condition 2: frames over 16.7 ms and average frame time, (a) and (b)

**Frames over 16.7 ms** (threaded VDP on, as the design states): the x64 count in each run must be at most the highest count of the interpreter's three runs.

| Title | interpreter: frames over 16.7 ms (3 runs), highest | x64 (3 runs) | rule |
|---|---|---|---|
| BIOS menu (reference) | 0 / 0 / 0, 0 | 0 / 0 / 0 | pass |
| Virtua Fighter 2 | 0 / 0 / 0, 0 | 0 / 0 / 0 | pass |
| Panzer Dragoon II Zwei | 0 / 0 / 0, 0 | 0 / 0 / 0 | pass |
| Sega Rally Championship | 0 / 0 / 0, 0 | 0 / 0 / 0 | pass |
| Burning Rangers | 0 / 0 / 0, 0 | 0 / 0 / 0 | pass |
| Guardian Heroes | 0 / 0 / 0, 0 | 0 / 0 / 0 | pass |
| Street Fighter Zero 3 | 0 / 0 / 0, 0 | 0 / 0 / 0 | pass |

Session 2 (the three titles above): 0 / 0 / 0 on both sides on all three. The long run is in its own table below (358 against 0).

**Average frame time** (median run by SH-2 total; the rule is x64 at most the interpreter's). (a) is the design as written, threaded VDP on; (b) is the same measurement with `--no-threaded-vdp`, interpreter and x64 in the same session. Session 2 cells show interpreter -> x64 (ratio).

| Title | (a) interpreter avg frame ms (median run) | (a) x64 | (a) x64 / interp | (a) session 2 | (b) interpreter avg frame ms | (b) x64 | (b) x64 / interp | (b) session 2 | (b) SH2 total interp / x64 |
|---|---|---|---|---|---|---|---|---|---|
| BIOS menu (reference) | 8.044 | 7.906 | 0.983 | - | 10.116 | 9.405 | 0.930 | - | 1.702 |
| Virtua Fighter 2 | 6.360 | 5.706 | 0.897 | - | 10.644 | 9.257 | 0.870 | - | 1.457 |
| Panzer Dragoon II Zwei | 6.459 | 5.835 | 0.903 | - | 10.275 | 9.269 | 0.902 | - | 1.277 |
| Sega Rally Championship | 7.296 | 7.697 | **1.055** | 7.340 -> 7.546 (1.028) | 14.303 | 13.977 | 0.977 | 14.399 -> 14.112 (0.980) | 1.113 |
| Burning Rangers | 8.956 | 8.645 | 0.965 | 9.057 -> 8.675 (0.958) | 12.067 | 11.528 | 0.955 | 12.125 -> 11.628 (0.959) | 1.137 |
| Guardian Heroes | 6.211 | 6.096 | 0.981 | - | 11.785 | 11.070 | 0.939 | - | 1.301 |
| Street Fighter Zero 3 | 9.034 | 9.296 | **1.029** | 9.082 -> 9.301 (1.024) | 15.010 | 14.326 | 0.954 | 15.096 -> 14.386 (0.953) | 1.218 |

In (a) the average frame fails on Sega Rally (+5.5%, +2.8%) and Street Fighter Zero 3 (+2.9%, +2.4%) in both sessions; Burning Rangers fails in 2E (+2.9%) and passes in both sessions here (0.965, 0.958), with the SH-2 gain now larger (1.099, 1.107). In (b) the rule holds on every title in both sessions (0.870-0.980), and the x64 frame is shorter than the interpreter's by roughly the SH-2 gain: Sega Rally 14.303 -> 13.977 ms against 4.823 -> 4.335 of SH-2 total. The part of `Ymir_RunFrame` that is not SH-2 time changes by -0.02 to +0.22 ms/frame with x64 in (b) (Sega Rally 9.48 -> 9.64, Burning Rangers 5.73 -> 5.95, Guardian Heroes 7.75 -> 7.96, Street Fighter Zero 3 10.14 -> 10.32; the first session's median runs), the order of the real extra work Task 4 found (0.04-0.16 ms/frame there). In (a) it grows by 0.28-0.91 ms/frame (Sega Rally 2.50 -> 3.16, Burning Rangers 2.39 -> 2.67, Street Fighter Zero 3 4.26 -> 5.17; 2E: 0.2-0.46). That is consistent with Task 4's finding that the emulation thread waits for the VDP2 thread (about two thirds of the rise on Sega Rally and Panzer Dragoon II Zwei), which absorbs the SH-2 saving; this session did not instrument the waits.

The frame time with threaded VDP off is much longer for the same SH-2 time (the rendering is on the emulation thread), so the number of frames over 16.7 ms is not meaningful there (Sega Rally 699-767 of 1,800 in both modes) and is not part of (b).

### Long run: Burning Rangers, frames 2,400-36,000

`--warmup 2400 --frames 33600`, one run per mode, default settings ((a): threaded VDP on), interpreter, x64 (worker) and x64 (`--jit-sync-compile`), then the interpreter and x64 with `--no-threaded-vdp` ((b)), after the table runs of the session. All five exited 0.

| Burning Rangers, frames 2,400-36,000 | ms/frame avg | p95 | p99 | max | frames over 16.7 ms | SH2 total ms/frame | interpreter / x64 |
|---|---|---|---|---|---|---|---|
| interpreter | 9.353 | 11.547 | 13.015 | 15.8 | 0 | 6.142 | - |
| x64, compile worker (default) | 9.720 | 14.362 | 16.825 | 48.3 | 358 | 5.839 | 1.052 |
| x64, `--jit-sync-compile` | 10.003 | 16.116 | 21.643 | 96.5 | 1398 | 6.145 | 1.000 |
| (b) interpreter, threaded VDP off | 16.059 | 24.547 | 26.928 | 33.2 | 12507 | 6.004 | - |
| (b) x64, compile worker, threaded VDP off | 15.831 | 25.524 | 29.193 | 57.5 | 13458 | 5.541 | 1.084 |

| Long run, window counters | x64 worker | x64 sync | 2E (5f2df05, worker) |
|---|---|---|---|
| Front-end builds | 312,675 | 305,401 | 306,207 |
| Native compiles in the window | 28,801 submitted (9,044 installed + 19,757 dropped on arrival) | 44,193 | 28,895 submitted (9,691 installed + 19,204 dropped) |
| Compile time on the emulation thread | 5.8 s (0.173 ms/frame) | 17.4 s (0.519 ms/frame) | 5.7 s (0.170 ms/frame) |
| Worker CPU time | 0.404 ms/frame | - | 0.380 ms/frame |
| Frames with compiling | 8,298 | 8,294 | 8,295 |
| Code-cap flushes (master / slave, since start) | 0 / 0 | 0 / 0 | 0 / 0 |
| compileFallbacks (master / slave) | 0 / 0 | 0 / 0 | 0 / 0 |
| Peak native code, master / slave | 79.4 / 6.8 MB | 79.5 / 6.8 MB | 76.9 / 6.5 MB |
| Retired native reuses | 818,703 | 885,900 | 832,749 |
| IR block reuses | 895,814 | 835,891 | 888,236 |
| IR-cap events (window) / IR cap time | 1 / 3.7 ms (409 retired IR blocks freed) | 1 / 3.9 ms | 1 / 4.5 ms (409) |
| Worst frame | 48.3 ms | 96.5 ms | 48.3 ms |

The machine ran at its normal speed in this session (interpreter long run 9.353 ms/frame, 6.142 SH-2, against 9.380 / 5.954 in the 2E session and 12.276 / 7.980 in the slow task 5 session). With the worker the long run now has 358 frames over 16.7 ms against 571 in the 2E session (0 against 1 for the interpreter), the worst frame is the 9,264 burst of new code (48.3 ms, 3,610 front-end builds, 24.6 ms of compiling in it), as in 2E, task 5 and the earlier tracks; the five slowest frames are all in that burst (9,264, 9,270, 9,267, 9,279, 9,309; 48.3, 35.2, 32.0, 31.2 and 30.7 ms, with 12-25 ms of compiling each). With threaded VDP off the non-SH-2 part of the long run's frame is 10.054 ms with the interpreter and 10.289 with x64 (+0.235; the average-frame rule's (b) result is 15.831 against 16.059 ms); with threaded VDP on it is 3.210 and 3.879 (+0.669). The task 5 analysis (what the slow frames' excess is made of, and the ranking of options) was done on the slow machine; this count says only that the slow frames did not go away on a healthy one (358 against 0).

### Default-on bar (design section 3), condition by condition

1. **Faster: holds.** Medians of three, session 1: Virtua Fighter 2 1.394, Panzer Dragoon II Zwei 1.225, Guardian Heroes 1.227, Street Fighter Zero 3 1.156 (session 2 1.166), Burning Rangers 1.099 (1.107), Sega Rally 1.057 (1.088); every round of every title is above 1.0 (lowest: Sega Rally round 1, 1.050). The long Burning Rangers run: 1.052. No title is within 0.02 of 1.0. The smallest margin is Sega Rally's 0.057 (the long run's is 0.052).
2. **Smooth: fails as the design states it (a); (b) is reported beside it.**
   - Frames over 16.7 ms (threaded VDP on): at most the interpreter's highest count in all seven 1,800-frame windows (0 against 0), in both sessions; **fails on the long Burning Rangers run: 358 against 0**.
   - Average frame (a): **fails on Sega Rally** (7.697 against 7.296 ms, session 2 7.546 against 7.340), **Street Fighter Zero 3** (9.296 against 9.034, session 2 9.301 against 9.082) **and the long run** (9.720 against 9.353); holds on the BIOS menu, Virtua Fighter 2, Panzer Dragoon II Zwei, Burning Rangers and Guardian Heroes.
   - Average frame (b), threaded VDP off: **holds on all seven windows** in both sessions (x64 / interpreter 0.870-0.980) **and on the long run** (15.831 against 16.059 ms).
3. **Exact: holds.** All 35 lockstep runs (details in the verdict above and in `design/sh2-validation.md`) are identical with `compileFallbacks 0`; the tests and the Pi pass; CI of the measured code: green: test-windows 6m19s, test-linux-x64 12m44s, test-linux-tsan 16m32s, build-macos-arm64 2m10s (its first attempt was cancelled after 15 minutes in the runner queue and re-run).
4. **Memory: holds.** Long Burning Rangers run: 0 code-cap flushes on both CPUs, peak native code 79.4 / 6.8 MB (master / slave; 76.9 / 6.5 in 2E, +3.3% on the master; task 1's resume tail adds 83-90 bytes per block, +1.7-5.7%), retired pool 3.5 MB at the end. The 36,000-frame lockstep runs peak at 113.8 MB (Panzer Dragoon II Zwei, master), 99.7 (Virtua Fighter 2) and 93.5 (Street Fighter Zero 3): 14.2 MB of headroom under the cap at the tightest, 0 flushes.

Not met: condition 2 (a). The JIT stays off by default; the option text and README, corrected in 2E, still state what is measured.

### What is left

- **The average frame, condition 2 (a).** Task 4's finding stands: with threaded VDP the frame lasts until the VDP2 render thread has rendered the last line, and the SH-2 time the JIT saves cannot shorten it once that thread is behind. On this machine it fails on Sega Rally (+5.5% / +2.8%), Street Fighter Zero 3 and the long run, and holds on the others. Two ways out, neither on the JIT side: speed up or parallelise VDP2 rendering (up to 0.40 / 0.17 ms/frame of waiting on Sega Rally / Panzer Dragoon II Zwei), or change the bar to measure the average frame with threaded VDP off, where it holds on every title. The second is the user's decision: the rule is "my call" in the design, written before the cause was known. This measurement shows what the verdict would be (condition 2 holds with threaded VDP off in the average frame; the frames-over part of it fails in the long run either way).
- **The long run's slow frames, condition 2 (frames over 16.7 ms).** 358 against 0 on a healthy machine. Task 5's ranked options (front end and Prepare off the emulation thread with cold code interpreted; front end only; interpret cold code until its threshold) are the candidates; their estimates were made on the slow machine and should be re-measured on this one before any is chosen. The slow frames are bursts of new code, with 1-5 ms of compile time in most of them against an excess of about 8.8 ms.
- **Speed.** Condition 1 holds with 0.052 or more to spare. The profile's remaining items (the slave's on-chip-register polling loop, 7-17% of the executor; generated-code shape; `RTE` with interrupt entry together, see Task 3) are optional after this.


