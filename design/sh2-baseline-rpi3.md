# SH-2 Baseline — Raspberry Pi 3 (ARM64)

**Date**: 2026-10-15
**Machine**: Raspberry Pi 3 Model B Rev 1.2: 4x Cortex-A53 (in-order, ARMv8.0-A; features `fp asimd crc32`, no LSE atomics) at a fixed 1200 MHz (`performance` governor), 32 KiB L1I + 32 KiB L1D per core, 512 KiB shared L2, 905 MiB RAM. Debian 13 (trixie), Linux 6.18.50+rpt-rpi-v8 aarch64. Headless (no desktop), active cooling, 5 V supply without under-voltage.
**Build**: source `7bcae8a`, Release, `BRIMIR_LTO=ON` (core, JIT and bench IPO), Ninja, GCC 14.2.0, default `-O3 -DNDEBUG`, no `-mcpu`/`-mtune` (generic ARMv8-A); the same configuration as the x64 `build-bench`. The x64 JIT backend is not built on ARM; the JIT has only the `ir` backend here.
**Method**: the method of [sh2-baseline.md](sh2-baseline.md): `brimir_bench --warmup 2400 --frames 1800` for the games (`--warmup 300` for the BIOS menu), no save states, no input, default settings (threaded VDP, host RTC), the same BIOS images (MD5 checked) and the configured `brimir_saturn_rtc_us_eu.smpc` copied under both region names into a fresh scratch system dir per run. Three rounds over all titles with `tools/bench/run_bench.sh 3 interp`; the table shows the run with the median average. Not pinned to a core, so the VDP worker threads run on the other cores as by default (load average about 2 during runs).

Each run started at or below 55 °C. Sampled once a second: no throttle flags (`vcgencmd get_throttled` 0x0 before and after every run), ARM clock 1200 MHz throughout, at most 48 °C. All 21 runs exited 0 and were valid.

| Title | ms/frame avg | p95 | p99 | max | SH2 master ms (%) | SH2 slave ms (%) | SH2 total % | avg of the 3 runs |
|---|---|---|---|---|---|---|---|---|
| BIOS menu (US BIOS) | 40.596 | 41.323 | 42.638 | 45.334 | 20.462 (50.4%) | 0.000 (0.0%) | 50.4% | 40.653 / 40.596 / 40.492 |
| Virtua Fighter 2 (Japan) (Rev B), JP BIOS | 60.995 | 79.066 | 80.530 | 82.115 | 26.676 (43.7%) | 18.684 (30.6%) | 74.4% | 61.044 / 60.926 / 60.995 |
| Panzer Dragoon II Zwei (USA), US BIOS | 67.310 | 73.943 | 75.410 | 77.495 | 32.654 (48.5%) | 18.215 (27.1%) | 75.6% | 67.310 / 67.238 / 67.380 |
| Sega Rally Championship (USA), US BIOS | 63.259 | 75.115 | 77.379 | 80.376 | 25.193 (39.8%) | 19.927 (31.5%) | 71.3% | 63.352 / 63.135 / 63.259 |
| Burning Rangers (USA), US BIOS | 82.373 | 95.896 | 97.974 | 101.911 | 36.703 (44.6%) | 21.329 (25.9%) | 70.5% | 82.231 / 82.373 / 82.666 |
| Guardian Heroes (USA), US BIOS | 62.358 | 70.544 | 71.810 | 74.451 | 25.581 (41.0%) | 16.569 (26.6%) | 67.6% | 62.485 / 62.358 / 62.159 |
| Street Fighter Zero 3 (Japan), JP BIOS, 4 MB RAM cart | 63.834 | 69.401 | 71.250 | 88.723 | 26.581 (41.6%) | 20.386 (31.9%) | 73.6% | 63.654 / 64.510 / 63.834 |

Percentages are of `Ymir_RunFrame`, which is within 0.01 ms of the frame average in every run. Raw results: `/mnt/data/results/20261015-071158/` on the device.

## Comparison with the x64 baseline

The x64 numbers are the table of [sh2-baseline.md](sh2-baseline.md) (Ryzen 7 5700G, MSVC, measured 2026-09-30), so the ratios compare different sessions and compilers and are approximate. "Rest" is the frame average minus the SH-2 total.

| Title | SH2 total ms, Pi / x64 | ratio | rest ms, Pi / x64 | ratio |
|---|---|---|---|---|
| BIOS menu | 20.46 / 2.30 | 8.9 | 20.13 / 6.26 | 3.2 |
| Virtua Fighter 2 | 45.36 / 6.71 | 6.8 | 15.64 / 2.70 | 5.8 |
| Panzer Dragoon II Zwei | 50.87 / 7.42 | 6.9 | 16.44 / 2.82 | 5.8 |
| Sega Rally Championship | 45.12 / 7.06 | 6.4 | 18.14 / 3.20 | 5.7 |
| Burning Rangers | 58.03 / 9.20 | 6.3 | 24.34 / 4.37 | 5.6 |
| Guardian Heroes | 42.15 / 6.13 | 6.9 | 20.21 / 3.43 | 5.9 |
| Street Fighter Zero 3 | 46.97 / 6.78 | 6.9 | 16.87 / 4.03 | 4.2 |

## Observations

- Whole frames take 61–82 ms in games (12–16 fps), 3.7–4.9x the 16.7 ms NTSC budget. The SH-2s take 67.6–75.6% of emulation-thread time, slightly more than on x64 (62.7–72.5%).
- The SH-2 interpreter loses more on the A53 than the rest of the emulator does: SH-2 time is 6.3–6.9x the x64 time in games (8.9x on the BIOS menu), the rest of the frame 4.2–5.9x. This fits an in-order core paying for the interpreter's dispatch (indirect branches, table lookups, data-dependent branches) more than an out-of-order x64 core does. A native backend removes that dispatch, so the JIT has more to gain here than on x64.
- The time outside the SH-2s is 15.6–24.3 ms per game frame, about the whole frame budget on its own, so 60 fps is out of reach on this board even with free SH-2 emulation. With SH-2 share *s* and SH-2 speedup *k*, the new frame time is (1 − s + s/k) of the old: at s = 0.72 and k = 3 that is 0.52 (Panzer Dragoon II Zwei from 67 ms to about 35 ms).
- Run-to-run spread of the average is 0.2–0.5% per title (1.3% for Street Fighter Zero 3), much tighter than on the development PC (sh2-baseline.md reports a 5.5% session-to-session difference). Small JIT changes can be measured here.
- Burning Rangers is again the slowest title, with frames up to 102–111 ms on the interpreter.

## Validation on ARM64 (2026-10-15)

Source `7bcae8a` plus the uncommitted `--no-cd-preload` and CHD hunk-cache changes (CHANGELOG, Unreleased).

- **Lockstep, IR backend:** `tools/bench/run_lockstep.sh 1800 ir` (with `--no-cd-preload`): the BIOS menu and all six games `lockstep: OK, 1800 frames identical`, exit 0. The IR backend takes about 3x the interpreter's SH-2 time here (Panzer Dragoon II Zwei, `--warmup 1200 --frames 600`: 151–154 ms/frame against about 51 ms).
- **Test suite** (Release, no LTO, GCC 14.2, BIOS fixtures present): 271 test cases, 236 passed, 35 skipped (all `no x64 backend`: 33 in `test_jit_x64.cpp`, 2 in `test_jit_executor.cpp`), 0 failed; 1,110,891 assertions; 8.5 minutes.
- **Memory:** with the default `brimir_cd_preload`, one core holds the whole CHD (peak RSS 366 MB for Panzer Dragoon II Zwei, 489 MB for Street Fighter Zero 3, about 420 MB for Virtua Fighter 2; 45–53 MB without preloading), so a two-core lockstep does not fit in 905 MiB and must use `--no-cd-preload`. The tests build peaks at 652 MB in one compiler process, so builds must not run beside a preloaded run.

## Profile of the interpreter (perf, Panzer Dragoon II Zwei)

`perf record -F 999` over the full baseline run (`--warmup 2400 --frames 1800`, 68.8 ms/frame, SH-2 75.4%). Function-level, Release + LTO, so small functions are inlined into their callers.

- Threads, share of all samples: emulation thread 54.1%, VDP2 renderer 44.8%, VDP1 renderer 1.1%.
- Emulation thread, share of that thread: `SH2::InterpretNext` (dispatch and inlined handlers) 39%, `SH2::MemRead<uint32, true>` (instruction fetch through the bus) 10%, `SH2::Advance` 6.5%, `SH2::AdvanceFRT` 2.8%, on-chip register reads 2.0%, other SH-2 memory accesses about 3%; SCSP slots and DSP about 12% with the 68000; SCU DMA and DSP about 5%. About 60% of the thread is SH-2 dispatch, fetch and bookkeeping that native code replaces.
- About 95% of `VDP2RenderThread`'s own samples (20.9% of all samples) are one six-instruction loop: the spin phase of the moodycamel semaphore (`ConcQueueTraits::MAX_SEMA_SPINS = 20000`). The VDP2 thread keeps a core busy waiting for events. On this 4-core board it does not take time from the emulation thread directly, but it costs power and heat, and on 2-core devices it would compete with emulation (and with JIT compiles). Upstream Ymir code; not changed.
