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
- Interpreter SH2 totals for five of the games are 26–29% below the baseline table measured on 2026-09-30 on the same machine (for example Panzer Dragoon II Zwei 5.27 vs 7.42 ms). Virtua Fighter 2 and the BIOS menu are within 2% of it. The ratios above only compare runs from the same session.

## Profile (where SH-2 time goes on x64)

A sampling profiler was not usable here: Visual Studio's `VSDiagnostics.exe` collector is installed, but there is no command-line analyzer for its sessions, and `xperf`/WPA are not installed (only `wpr.exe`). The profile therefore comes from a temporary instrumented build (not committed, reverted afterwards): `rdtsc` timers around the parts of `Executor::Run`/`Step` (pre-checks, `BlockCache::Get` including `IsCurrent`, interpreter fallbacks, native block runs) and around every out-of-line trampoline called from generated code (read, write, pipeline refill, access cycles, bus wait, delay-slot setup/end, SR write), plus event counters. Each group of timers could be switched on separately, so every measurement below was taken with only that group on. Counts cover the 1,800 measured frames, both CPUs.

Timer overhead is significant: about 3 ns per timed interval, estimated from how much `Executor::Run` time grows when a timer group is switched on (TSC 3.79 GHz). The numbers are therefore approximate; shares are more reliable than absolute times. The counters alone raised Panzer Dragoon II Zwei's SH2 total from 9.04 to 10.15 ms/frame.

### Panzer Dragoon II Zwei (x64)

Counts: 270.6M executor steps = 266.8M native blocks + 3.8M interpreter fallbacks (3.64M interrupt entries or pending delay slots, 0.13M uncompiled first instructions, 0 fetch-buffer mismatches); 901.8M guest instructions retired in native blocks, **3.4 guest instructions per block**; 204 compiles; 55.6M `Executor::Run` calls (one per `SH2::Advance`).

| Part | Measured | Per frame | Share of `Executor::Run` |
|---|---|---|---|
| `Executor::Run` total (only this timer on) | 16.82 s | 9.35 ms | 100% |
| Native block runs (`X64Backend::Run` incl. generated code and trampolines) | 11.64 s | 6.46 ms | ~63% |
| Dispatch outside native code (`Run` loop, `Step` pre-checks, `Get`/`IsCurrent`, `BlockScope`, `X64Frame` setup, fallbacks) | 6.90 s | 3.83 ms | ~37% |
| of which `BlockCache::Get` (incl. `IsCurrent`), 266.9M calls | 4.58 s | ~17 ns/call | |
| of which `Step` pre-checks (delay slot / interrupt / fetch buffer), 270.6M | 2.21 s | ~8 ns/step | |
| of which interpreter fallbacks, 3.8M | 0.05 s | | |

Slow-path trampolines called from generated code (timed separately; raw times include about 3 ns of timer overhead per call):

| Trampoline | Calls | Time | ns/call |
|---|---|---|---|
| pipeline refill | 622.9M (2.3 per block) | 5.49 s | 8.8 |
| delay-slot setup/end | 141.3M | 1.17 s | 8.3 |
| read (MMIO and other non-array pages) | 85.7M | 0.77 s | 9.0 |
| write (MMIO) | 3.9M | 0.96 s | 250 (device side effects) |
| bus wait | 1.3M | 0.01 s | |
| SR write | 0.9M | 0.01 s | |
| access cycles | 0 (all inlined) | — | |

Corrected for timer overhead, the trampolines take about 5.8 s, roughly half of the native-run time. That leaves about 3 ms/frame for generated code proper. In round numbers, SH-2 executor time on x64 splits into thirds: about one third in generated code, one third in trampolines (two thirds of it pipeline refills) and one third in per-block dispatch. With 3.4 guest instructions per block, the fixed per-block cost (dispatch about 25 ns as measured, timer overhead included, plus `X64Backend::Run` entry/exit) exceeds the ~10.5 ns per instruction that the interpreter needs in this scene (5.27 ms / 501k instructions per frame).

### Street Fighter Zero 3 (x64)

Counts: 214.7M steps, 212.5M native blocks, 875.3M guest instructions (4.1 per block), 78.9M reads, 637.7M pipeline refills, 19.5M delay-slot calls.

- **35,454 compiles** during the 1,800 measured frames (Panzer Dragoon II Zwei: 204). The block cache was flushed 3 times during the measurement (4 times including warmup) with 12,200–14,100 blocks and 32.9–34.0 MB of native code in it. That is below the 64 MB native-code cap, so the flushes came from the IR-instruction cap (`kMaxCachedInsts`, 2^20 IR instructions). No flush came from `Executor::Flush` (resets, state loads) and no compile replaced an invalidated block.
- Each compile costs about 126 µs (4.47 s in the compiling `Get` calls). That is about 2.5 ms/frame on average, spent in a few frames right after each flush: the 256–264 ms max frames and the 44–45 ms p99.
- Without the compile churn, the x64 SH2 total would be about 8.7 ms/frame. The ratio would still be about 0.58.
- Otherwise the split is similar to Panzer Dragoon II Zwei: native block runs took 10.27 s of 21.55 s in `Executor::Run` (~48%) with the native timers on. The rest is dispatch plus the compiles.

## Conclusions for a follow-up plan (2C)

In order of expected gain:

1. **Per-block dispatch** is about a third of SH-2 time, and blocks are short (3.4–4.1 guest instructions). Block linking (`design/sh2-jit-m2.md` §4.6) would chain blocks inside generated code without returning to `Executor::Step` and `BlockCache::Get`. The design already deferred it to 2C. The `IsCurrent` opcode re-check on every lookup should become a write-tracking invalidation (or a per-page generation check) so linked blocks do not need it.
2. **Pipeline refills** are the largest trampoline cost: 623M calls (2.3 per block), about 9 ns each. Inlining the refill for array pages, as reads already are, removes most of them. Delay-slot setup/end (141M calls) can also be inlined as state stores.
3. **Cache capacity**: count native code instead of IR for the flush cap, or drop the IR of natively compiled blocks. That removes Street Fighter Zero 3's recompile stalls (~2.5 ms/frame, 260 ms spikes). Compile time (~126 µs per block with `x86::Compiler`) matters only after a flush.
4. Interrupt entry and pending delay slots go through the interpreter (3.6M steps in Panzer Dragoon II Zwei, more in Sega Rally's slave). That is a small share here.

Even removing all dispatch and refill overhead leaves generated code at about 3 ms/frame for Panzer Dragoon II Zwei vs 5.27 ms for the whole interpreter. Reaching 2x (≤ 2.6 ms) also needs better code inside blocks: keeping guest registers in host registers across a block, and longer blocks.

## Reproduce

```powershell
brimir_bench --bios <BIOS> --game <game> --system-dir <scratch dir> --warmup 2400 --frames 1800 [--sh2-jit --jit-backend ir|x64]
brimir_bench --bios mpr-17933.bin --warmup 300 --frames 1800 [--sh2-jit --jit-backend ir|x64]    # BIOS menu row
```
