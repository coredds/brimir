# SH-2 JIT validation

Milestone 1 first; milestones 2A, 2B and 2C are in their own sections at the end.

## Milestone 1

**Date**: 2026-10-01
**Commit**: cc3b35f (the validated code: the parent of the commit that adds this report)
**Machine**: AMD Ryzen 7 5700G, 8 cores / 16 threads, 32 GB RAM, Microsoft Windows 11 Pro 10.0.26300
**Build**: `build-bench`, Ninja, Release, `BRIMIR_LTO=ON`, `Brimir_ENABLE_IPO=ON`, MSVC 19.44.35229 (Visual Studio 2022, toolset 14.44), `CMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG`

### Method

- **Lockstep**: `brimir_bench --bios <bios> [--game <game>] --system-dir <scratch dir> --lockstep 36000` (10 minutes of emulated time). One JIT core and one interpreter core run side by side and are compared after every frame: both SH-2s, work RAM, the save state of every other subsystem, audio and the output frame (`design/sh2-jit.md` section 7.2). Lockstep turns threaded VDP rendering off and runs the RTC in virtual mode.
- **Smoke run** (default user settings: threaded VDP, host RTC): `brimir_bench --bios <bios> --game <game> --system-dir <scratch dir> [--sh2-jit] --warmup 600 --frames 3600`, once with the JIT and once with the interpreter, back to back in the same session. No input, so games run on their title/attract screens.
- **Content**: US BIOS `mpr-17933.bin` (US/EU v1.00), JP BIOS `Sega Saturn BIOS v1.01 (JAP).bin` (JP v1.01). The scratch system dir held the configured `brimir_saturn_rtc_us_eu.smpc`, copied also as `brimir_saturn_rtc_jp.smpc` (same setup as `design/sh2-baseline.md`). Street Fighter Zero 3 gets the 4 MB RAM cart from Ymir's game database.

### Results

| Title | BIOS | Lockstep frames | Result | jit master blocksRun | jit master interpreted | Lockstep wall time | Smoke ms/frame JIT | Smoke ms/frame interpreter | Smoke SH2 total % (JIT / interp.) |
|---|---|---|---|---|---|---|---|---|---|
| BIOS menu (no disc) | US | 36000 | OK, identical | 2,953,113,195 | 3,168,974 | 15:41 | — | — | — |
| Virtua Fighter 2 (Japan) (Rev B) | JP | 36000 | OK, identical | 2,616,782,606 | 293,303,544 | 38:42 | 24.353 | 12.084 | 85.7% / 55.8% |
| Panzer Dragoon II Zwei (USA) | US | 36000 | OK, identical | 3,347,722,435 | 200,688,020 | 28:14 | 21.700 | 10.328 | 85.2% / 73.1% |
| Sega Rally Championship (USA) | US | 36000 | OK, identical | 3,114,465,703 | 218,025,966 | 23:13 | 22.668 | 10.134 | 84.6% / 68.9% |
| Burning Rangers (USA) | US | 36000 | OK, identical | 2,936,067,078 | 802,975,371 | 28:38 | 26.195 | 13.497 | 84.4% / 61.6% |
| Guardian Heroes (USA) | US | 36000 | OK, identical | 2,532,479,487 | 59,806,609 | 20:07 | 20.488 | 9.448 | 81.1% / 64.7% |
| Street Fighter Zero 3 (Japan) | JP | 36000 | OK, identical | 2,320,551,274 | 194,386,819 | ~32 min (re-run, see Findings) | 22.119 | 10.185 | 85.4% / 68.4% |

All lockstep runs ended with `lockstep: OK, 36000 frames identical` (exit 0; the exit code of the Street Fighter Zero 3 re-run was not captured to a file, its log ends with the OK line). The command lines of the BIOS-menu and Virtua Fighter 2 runs were not recorded; they used the US and JP BIOS respectively, as in the baseline and the plan. The lockstep summary line reports only the master executor's counters, so slave counters are not available for the lockstep runs. All twelve smoke runs exited 0.

Smoke-run JIT executor stats (3600 measured frames, after warmup):

| Title | jit master blocksRun | jit master interpreted | jit slave blocksRun | jit slave interpreted |
|---|---|---|---|---|
| Virtua Fighter 2 | 270,611,866 | 27,387,919 | 431,463,632 | 369,706 |
| Panzer Dragoon II Zwei | 361,450,330 | 16,245,590 | 205,691,329 | 140,456 |
| Sega Rally Championship | 343,195,268 | 29,403,048 | 167,374,639 | 125,452,455 |
| Burning Rangers | 331,979,950 | 81,278,838 | 219,628,549 | 11,240,666 |
| Guardian Heroes | 289,585,948 | 10,078,770 | 210,982,027 | 136,527 |
| Street Fighter Zero 3 | 267,657,925 | 30,697,242 | 188,697,803 | 32,107 |

As expected, the IR-interpreter backend is slower than Ymir's interpreter: frame time is 1.9–2.2x the interpreter's and the SH-2 share rises to 81–86%. Milestone 1 targets exactness, not speed; native backends are milestones 2 and 3.

### Opcodes still interpreted

These use the interpreter fallback (one instruction at a time through the fork's interpreter):

- Multiply and MAC: `MUL.L`, `MULS.W`, `MULU.W`, `DMULS.L`, `DMULU.L`, `MAC.W`, `MAC.L`
- Divide step: `DIV0S`, `DIV0U`, `DIV1`
- `TAS.B`
- Exceptions and control: `TRAPA`, `RTE`, `SLEEP`
- Memory forms of the system-register transfers: `LDC.L @Rm+`, `LDS.L @Rm+`, `STC.L …,@-Rn`, `STS.L …,@-Rn`
- Illegal opcodes (exception path)

Everything else is compiled.

### Findings

- No JIT bugs were found during validation; no fixes were needed.
- Environment issue (not a JIT issue): the first Street Fighter Zero 3 lockstep run stalled because the host barely scheduled the process overnight (about 242 s of CPU time in 3 hours) and was stopped. The re-run completed in about 32 minutes with no divergence.

## Milestone 2A

**Date**: 2026-10-01
**Commit**: 58f05e7 (the validated code: the parent of the commit that adds this section)
**Machine and build**: as above (`build-bench`, Release, LTO/IPO, `/O2 /Ob2 /DNDEBUG`). Same BIOS files, scratch system dir and commands as the milestone 1 method.

Plan 2A compiles the multiplies (`MUL.L`, `MULS.W`, `MULU.W`, `DMULS.L`, `DMULU.L`), `MAC.W`/`MAC.L`, the divide step (`DIV0S`, `DIV0U`, `DIV1`), `TAS.B` and the memory forms of `LDC`/`LDS`/`STC`/`STS`. The random-program fuzz test (`test_jit_diff.cpp`) now also draws `MAC.W`/`MAC.L` (as `mov Rbase,Rd ; mac.x @Rd+,@Rd+`) and restricts `LDC.L @Rm+,GBR/SR/VBR` and `LDS.L @Rm+,PR` to values stored right before them, so the GBR-address and PR-return-target invariants hold. Measured: steps=15284 blocksRun=15142 interpreted=142 compiles=1048 (about 1 s in Release).

### Lockstep

| Title | BIOS | Lockstep frames | Result | jit master blocksRun | jit master interpreted (milestone 1) | Wall time |
|---|---|---|---|---|---|---|
| Sega Rally Championship (USA) | US | 36000 | OK, identical (exit 0) | 2,983,278,830 | 19,740,973 (218,025,966) | 24:56 |
| Burning Rangers (USA) | US | 36000 | OK, identical (exit 0) | 2,348,616,009 | 33,446,500 (802,975,371) | 29:56 |
| Virtua Fighter 2 (Japan) (Rev B) | JP | 36000 | OK, identical (exit 0) | 2,417,548,180 | 30,837,846 (293,303,544) | 40:48 |
| Panzer Dragoon II Zwei (USA) | US | 36000 | OK, identical (exit 0) | 3,266,028,823 | 73,852,344 (200,688,020) | 28:40 |
| BIOS menu (no disc) | US | 1800 | OK, identical (exit 0) | 147,582,397 | 119,591 | 0:52 |
| Guardian Heroes (USA) | US | 1800 | OK, identical (exit 0) | 116,053,331 | 713,444 | 0:59 |
| Street Fighter Zero 3 (Japan) | JP | 1800 | OK, identical (exit 0) | 103,429,336 | 1,846,153 | 0:58 |

The milestone 1 interpreted counts are from 36,000-frame runs, so they compare only for the first four rows. Fewer interpreted instructions also merge blocks, which lowers `blocksRun` (Burning Rangers: 2.94 to 2.35 billion).

### Smoke runs

Default settings (threaded VDP, host RTC), `--warmup 600 --frames 3600`, JIT and interpreter back to back. All twelve runs exited 0.

| Title | ms/frame JIT | ms/frame interpreter | SH2 total % (JIT / interp.) | jit master blocksRun | jit master interpreted (milestone 1) | jit slave blocksRun | jit slave interpreted (milestone 1) |
|---|---|---|---|---|---|---|---|
| Virtua Fighter 2 | 18.665 | 9.126 | 84.7% / 52.6% | 251,304,014 | 4,400,051 (27,387,919) | 431,463,496 | 368,372 (369,706) |
| Panzer Dragoon II Zwei | 18.259 | 7.749 | 84.8% / 71.7% | 355,340,100 | 7,523,301 (16,245,590) | 205,691,327 | 140,454 (140,456) |
| Sega Rally Championship | 17.951 | 7.721 | 85.3% / 66.5% | 324,860,971 | 2,633,273 (29,403,048) | 157,265,066 | 110,244,782 (125,452,455) |
| Burning Rangers | 21.444 | 10.049 | 85.4% / 61.1% | 266,233,763 | 4,067,199 (81,278,838) | 219,628,549 | 11,240,666 (11,240,666) |
| Guardian Heroes | 15.786 | 7.014 | 81.5% / 64.2% | 282,781,655 | 1,584,196 (10,078,770) | 210,939,477 | 63,211 (136,527) |
| Street Fighter Zero 3 | 19.203 | 8.296 | 85.0% / 60.9% | 249,823,349 | 4,975,828 (30,697,242) | 188,696,964 | 30,787 (32,107) |

Master `interpreted` counts drop 2x to 20x. Most slave counts barely move because the slaves already ran almost no newly compiled instructions. The JIT remains about 2.0–2.4x slower than the interpreter (IR-interpreter backend; the x64 backend is plan 2B). Interpreter ms/frame is lower than in the milestone 1 table on the same machine; treat cross-session comparisons as approximate.

What `interpreted` still counts: a temporary instrumentation build (not committed) classified every executor fallback in the Sega Rally and Panzer Dragoon II Zwei JIT smoke runs (both CPUs together). Sega Rally: 112.9M fallbacks = 36.4M interrupt entries + 36.4M uncompiled opcodes (all but 10 of them `RTE`) + 40.0M delay slots (the slots of those `RTE`s, mostly `NOP`, plus slots left pending when a block stopped between a branch and its slot). Panzer Dragoon II Zwei: 7.66M = 0.24M interrupt entries + 0.24M uncompiled opcodes (all but 10 `RTE`) + 7.18M delay slots. No fallback came from a fetch-buffer mismatch. The interrupt-heavy Sega Rally slave (about 10,000 interrupts per frame) accounts for its high slave count.

### Opcodes still interpreted (after milestone 2A)

- Exceptions and control: `TRAPA`, `RTE`, `SLEEP`
- Illegal opcodes (exception path)

The executor also falls back to the interpreter for interrupt entry, for a delay slot left pending outside a block (after `RTE`, or when a block stopped between a branch and its slot) and when the fetch buffer at PC & 2 disagrees with memory. Everything else is compiled.

### Findings

- No JIT bugs were found; no fixes were needed. Every lockstep run ended with `lockstep: OK, N frames identical` and exit 0 on the first attempt; no run stalled.

## Milestone 2B

**Date**: 2026-10-01
**Commit**: a86fae7 (the validated code: the parent of the commit that adds this section)
**Machine and build**: as above (`build-bench`, Ninja, Release, LTO/IPO, MSVC 19.44.35229, `/O2 /Ob2 /DNDEBUG`). Same BIOS files, scratch system dir and commands as the milestone 1 method, plus `--jit-backend x64`.

Plan 2B adds the x64 backend: the executor compiles each block to native x86-64 code with asmjit and runs it instead of the IR interpreter (`RunBlock`), which stays the fallback (`design/sh2-jit-m2.md`).

### Fuzz

The random-program fuzz test ("JIT matches the interpreter on random programs", `test_jit_diff.cpp`) now runs three rigs when the x64 backend is available: the interpreter, an IR executor and an x64 executor run the same 300 programs. After every step the IR rig is compared with the interpreter (as before) and the x64 rig with the IR rig: identical `ExitInfo` (cycles, retired, busWait, aborted, boundary) and `DiffRigs` with peripherals. The test requires `compileFallbacks == 0` and the same `blocksRun`/`interpreted` as the IR rig, with every block run natively. Measured:

- IR rig: steps=15284 blocksRun=15142 interpreted=142 compiles=1048 (unchanged from milestone 2A)
- x64 rig: blocksRun=15142 nativeBlocksRun=15142 compileFallbacks=0 interpreted=142

### Lockstep (x64 backend)

`brimir_bench --bios <bios> [--game <game> --system-dir <scratch dir>] --lockstep 36000 --jit-backend x64`, run sequentially. All seven runs ended with `lockstep: OK, 36000 frames identical` and exit 0 on the first attempt; no run stalled. Every block on both CPUs ran as native code (`nativeBlocksRun == blocksRun`, `compileFallbacks 0`).

| Title | BIOS | Frames | Result | jit master blocksRun / interpreted | jit slave blocksRun / interpreted | nativeBlocksRun (master / slave) | compileFallbacks | Wall time |
|---|---|---|---|---|---|---|---|---|
| BIOS menu (no disc) | US | 36000 | OK, identical (exit 0) | 2,952,071,540 / 894,693 | 0 / 0 | 2,952,071,540 / 0 | 0 / 0 | 14:38 |
| Virtua Fighter 2 (Japan) (Rev B) | JP | 36000 | OK, identical (exit 0) | 2,417,548,180 / 30,837,846 | 4,074,892,394 / 7,440,566 | 2,417,548,180 / 4,074,892,394 | 0 / 0 | 42:26 |
| Panzer Dragoon II Zwei (USA) | US | 36000 | OK, identical (exit 0) | 3,266,028,823 / 73,852,344 | 2,069,237,372 / 5,586,014 | 3,266,028,823 / 2,069,237,372 | 0 / 0 | 31:09 |
| Sega Rally Championship (USA) | US | 36000 | OK, identical (exit 0) | 2,983,278,830 / 19,740,973 | 1,582,039,497 / 1,104,116,598 | 2,983,278,830 / 1,582,039,497 | 0 / 0 | 19:39 |
| Burning Rangers (USA) | US | 36000 | OK, identical (exit 0) | 2,348,616,009 / 33,446,500 | 2,232,901,296 / 102,482,762 | 2,348,616,009 / 2,232,901,296 | 0 / 0 | 38:29 |
| Guardian Heroes (USA) | US | 36000 | OK, identical (exit 0) | 2,493,469,510 / 11,301,264 | 2,102,857,379 / 1,170,391 | 2,493,469,510 / 2,102,857,379 | 0 / 0 | 17:15 |
| Street Fighter Zero 3 (Japan) | JP | 36000 | OK, identical (exit 0) | 2,233,386,250 / 43,262,494 | 1,923,173,206 / 123,208 | 2,233,386,250 / 1,923,173,206 | 0 / 0 | 24:33 |

The lockstep summary now prints both CPUs and the native counters. The master counts of the four titles also run for 36,000 frames in milestone 2A are identical to that table (the backend does not change what is compiled or interpreted). The BIOS menu, Guardian Heroes and Street Fighter Zero 3 ran 1,800 frames in milestone 2A and 36,000 here. The Sega Rally slave's high `interpreted` count is its interrupt load (see the milestone 2A smoke-run notes).

### Performance

Measured separately in `design/sh2-x64-performance.md`: the x64 backend is 1.3–1.7x faster than the IR backend in SH-2 time but still 1.7–2.2x slower than the interpreter, so the 2x target is missed.

### Findings

- No JIT bugs were found; no fixes were needed.

## Milestone 2C

**Date**: 2026-10-02
**Commit**: 3aa3090 (the validated code: the parent of the commit that adds this section)
**Machine and build**: as above (`build-bench`, Ninja, Release, LTO/IPO, MSVC 19.44.35229, `/O2 /Ob2 /DNDEBUG`). Same BIOS files, scratch system dir and commands as the milestone 1 method.

Plan 2C (`design/plans/2026-10-02-sh2-jit-m2c-performance.md`) changes the x64 backend (inline known refills and delay slots, block chaining with in-block validation, guest registers in host registers, tiered native compilation, native-only cache accounting, IR-only eviction) and adds an IR pass (`OptimizeBlock`) that both backends run, so both were validated.

### Lockstep, x64 backend (36,000 frames)

`brimir_bench --bios <bios> [--game <game> --system-dir <scratch dir>] --lockstep 36000 --jit-backend x64`, run sequentially. All seven runs ended with `lockstep: OK, 36000 frames identical` and exit 0 on the first attempt; no run stalled and nothing was written to stderr. `compileFallbacks` is 0 on both CPUs in every run.

| Title | BIOS | Result | jit master blocksRun / interpreted | jit slave blocksRun / interpreted | nativeBlocksRun (master / slave) | IR-cap evictions (blocks), master / slave | Code-cap flushes, master / slave | Wall time |
|---|---|---|---|---|---|---|---|---|
| BIOS menu (no disc) | US | OK, identical (exit 0) | 2,952,071,540 / 894,693 | 0 / 0 | 2,952,050,815 / 0 | 0 / 0 | 0 / 0 | 13:13 |
| Virtua Fighter 2 (Japan) (Rev B) | JP | OK, identical (exit 0) | 2,417,548,180 / 30,837,846 | 4,074,892,394 / 7,440,566 | 2,417,209,193 / 4,074,825,128 | 1 (13,950) / 0 | 0 / 0 | 32:20 |
| Panzer Dragoon II Zwei (USA) | US | OK, identical (exit 0) | 3,266,028,823 / 73,852,344 | 2,069,237,372 / 5,586,014 | 3,265,702,492 / 2,069,216,967 | 1 (12,358) / 0 | 0 / 0 | 23:18 |
| Sega Rally Championship (USA) | US | OK, identical (exit 0) | 2,983,278,830 / 19,740,973 | 1,582,039,497 / 1,104,116,598 | 2,983,164,468 / 1,582,017,801 | 0 / 0 | 0 / 0 | 18:39 |
| Burning Rangers (USA) | US | OK, identical (exit 0) | 2,348,616,009 / 33,446,500 | 2,232,901,296 / 102,482,762 | 2,344,517,907 / 2,229,410,657 | 0 / 0 | **10 / 9** | 24:19 |
| Guardian Heroes (USA) | US | OK, identical (exit 0) | 2,493,469,510 / 11,301,264 | 2,102,857,379 / 1,170,391 | 2,493,249,100 / 2,102,757,314 | 0 / 0 | 0 / 0 | 15:55 |
| Street Fighter Zero 3 (Japan) | JP | OK, identical (exit 0) | 2,233,386,250 / 43,262,494 | 1,923,173,206 / 123,208 | 2,232,972,953 / 1,923,163,709 | 4 (56,713) / 0 | 0 / 0 | 20:28 |

- `blocksRun` and `interpreted` are identical to the milestone 2B table on both CPUs for all seven runs: none of the 2C changes alters which instructions are compiled or where blocks stop. `nativeBlocksRun` is now below `blocksRun` by the IR runs before each block reaches `kNativeCompileThreshold` (8) runs.
- Long-session cache events (the bench prints both counters): the IR cap was reached once in Virtua Fighter 2 and Panzer Dragoon II Zwei and four times in Street Fighter Zero 3, each time evicting only IR-only blocks. Burning Rangers is the only title that reached the 128 MB native-code cap: 10 full flushes on the master and 9 on the slave in 36,000 frames. Its attract sequence keeps loading new code over old code: 1,125,913 blocks built and 307,507 compiled natively on the master (883,698 invalidations, 142,387 stale native blocks found by their prologue), 1,137,419 and 196,276 on the slave, 89.4 s and 83.4 s of compiling. Stale native code stays allocated until a flush, so the cap fills in about 3,600 frames. See `design/sh2-x64-performance.md`, "Milestone 2C results".
- Other titles' code churn over 36,000 frames (master): Virtua Fighter 2 65,131 builds / 38,746 native compiles, Panzer Dragoon II Zwei 62,061 / 37,870, Sega Rally 20,185 / 14,294, Guardian Heroes 40,359 / 26,719, Street Fighter Zero 3 103,184 / 36,601, BIOS menu 4,222 / 2,491.

### Lockstep, IR backend (1,800 frames)

`brimir_bench ... --lockstep 1800 --jit-backend ir`, because `OptimizeBlock` also changed the blocks the IR backend runs. All seven runs ended with `lockstep: OK, 1800 frames identical` and exit 0.

| Title | jit master blocksRun / interpreted | jit slave blocksRun / interpreted | IR-cap evictions (master) | Wall time |
|---|---|---|---|---|
| BIOS menu (no disc) | 147,582,397 / 119,591 | 0 / 0 | 0 | 0:49 |
| Virtua Fighter 2 (Japan) (Rev B) | 121,164,082 / 970,802 | 139,022,597 / 130,532 | 1 | 1:36 |
| Panzer Dragoon II Zwei (USA) | 139,379,992 / 2,554,481 | 64,432,951 / 43,524 | 1 | 0:59 |
| Sega Rally Championship (USA) | 119,482,068 / 1,311,301 | 49,740,102 / 35,054,995 | 1 | 1:06 |
| Burning Rangers (USA) | 128,046,327 / 772,328 | 68,833,193 / 3,504,627 | 1 | 1:21 |
| Guardian Heroes (USA) | 116,053,331 / 713,444 | 66,995,037 / 18,947 | 1 | 0:59 |
| Street Fighter Zero 3 (Japan) | 103,429,336 / 1,846,153 | 57,967,706 / 30,735 | 1 | 0:56 |

The BIOS menu, Guardian Heroes and Street Fighter Zero 3 master counts equal the milestone 2A 1,800-frame runs. With the IR backend every block is IR-only, so an IR-cap eviction empties the cache, as the earlier whole-cache flush did.

### Performance

Measured separately in `design/sh2-x64-performance.md` ("Milestone 2C results"): interpreter SH-2 total / x64 SH-2 total is 0.91–1.25 on the six titles, so the 2x target is missed and the JIT stays off by default.

### Findings

- No JIT bugs were found; no fixes were needed.
- Burning Rangers fills the 128 MB native-code cap every ~3,600 frames through stale native code (above); this is a performance issue (full flush and recompile), not a correctness one: its lockstep run is identical.
