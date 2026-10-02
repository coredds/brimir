# SH-2 x64 backend performance (milestone 2B)

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

## Reproduce

```powershell
brimir_bench --bios <BIOS> --game <game> --system-dir <scratch dir> --warmup 2400 --frames 1800 [--sh2-jit --jit-backend ir|x64]
brimir_bench --bios mpr-17933.bin --warmup 300 --frames 1800 [--sh2-jit --jit-backend ir|x64]    # BIOS menu row
```
