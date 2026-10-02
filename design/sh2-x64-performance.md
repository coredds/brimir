# SH-2 x64 backend performance (milestones 2B and 2C)

Milestone 2B results and profile first; milestone 2C progress, final results (verdict: **target missed**) and re-profile are in "2C progress" and "Milestone 2C results" at the end.

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
4. **Compile cost** (SFZ3 0.67 ms/frame; asmjit's register allocation is 77% of a native compile): a lighter emitter or cheaper allocation for cold blocks; `kNativeCompileThreshold` = 16 is untested.
5. **Code that is reloaded** (Burning Rangers: 492,237 native compiles in 33,600 frames, SH-2 time 1.8x the interpreter's over that run): reuse native code when the same guest code comes back (key compiled code by its opcodes, not only its PC), raise the native threshold for blocks at PCs that keep going stale, and evict stale and cold native blocks instead of flushing at the code cap (Burning Rangers flushes about once a minute on average, clustered in its code-reload phases).
6. The remaining dispatch (one `Get` and `Step` per `Executor::Run` call, about 31,000 per frame) and interpreter fallbacks are small (under 0.7 ms/frame together).
7. **Save-state loads** (known issue): loading a save state, including RetroArch run-ahead and rewind (which load states continuously), flushes the JIT cache, so native code is recompiled after each load. Follow-up: check whether the flush is still required (check-on-entry may already catch the changed code).

## Reproduce

```powershell
brimir_bench --bios <BIOS> --game <game> --system-dir <scratch dir> --warmup 2400 --frames 1800 [--sh2-jit --jit-backend ir|x64]
brimir_bench --bios mpr-17933.bin --warmup 300 --frames 1800 [--sh2-jit --jit-backend ir|x64]    # BIOS menu row
```
