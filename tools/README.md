# Brimir tools

## brimir_bench — headless frame benchmark

Runs real content without a frontend and reports host time per frame and the
share of emulation time spent in each SH-2 CPU. Used for the SH-2 JIT baseline
(`design/sh2-baseline.md`) and for before/after comparisons.

Build:

```powershell
cmake --build build --target brimir_bench
```

Run `brimir_bench --help` for all options.

Benchmark a game from boot (no controller input, so it stays on the title or
attract screens):

```powershell
build\bin\brimir_bench.exe --bios system\sega_101.bin --game "D:\Saturn\Game.cue" --system-dir system --frames 1800
```

`--system-dir` must point to a directory that holds a configured
`brimir_saturn_rtc_<jp|us_eu>.smpc` (BIOS language and clock settings, STE=1).
The suffix follows the region of the BIOS image, not the disc, so you need a
configured file for each BIOS region you use. Completing the BIOS
language/clock setup once in RetroArch with Brimir only configures the file for
the region of the BIOS used there. Either repeat the setup with each BIOS
region, or copy the configured file to the other name (for example
`brimir_saturn_rtc_us_eu.smpc` to `brimir_saturn_rtc_jp.smpc`; this is how the
committed baseline was measured). Without a configured file the BIOS stops at
its first-boot setup screen, waits for input, and never boots the disc (the
slave SH-2 then reports 0.000 ms).

The core writes RTC files back into the system directory, as it does under
RetroArch, and may also create or rewrite `brimir_saturn_rtc_none.smpc`. Use a
scratch copy of the system directory rather than the real RetroArch folder.
`--system-dir` has no effect without `--game` (the no-disc path does not load
SMPC settings). If it is omitted, the per-run temp directory is used, which is
always unconfigured.

By default the disc image is preloaded into RAM, as with the `brimir_cd_preload`
core option's default. For a CHD that holds the whole compressed file in memory
(peak RSS 366–489 MB for Panzer Dragoon II Zwei and Street Fighter Zero 3 on a
Raspberry Pi 3, against 45–53 MB without it). `--no-cd-preload` reads the image from the file
instead. It does not change emulation, only host memory and I/O; use it for
`--lockstep` on hosts with little RAM, where two cores would otherwise each hold
a copy. The report prints `cd preload : on|off` when a game is loaded.

Backup RAM (`.srm`) and cartridge RAM (`.cart`) are written to a fresh
directory under `%TEMP%\brimir_bench\run-*` that is removed on exit, so saves
from earlier runs never affect results.

Benchmark gameplay: create a save state at the point of interest, then measure
from it. `--dump-at` runs N frames from the loaded content (and optional
`--state`) and writes the core's raw state data:

```powershell
build\bin\brimir_bench.exe --bios system\sega_101.bin --game Game.cue --system-dir system --dump-at 3600 --dump-state game.bstate
build\bin\brimir_bench.exe --bios system\sega_101.bin --game Game.cue --system-dir system --state game.bstate --frames 1800
```

RetroArch `.state` files are accepted only if they contain the raw core data
(no RetroArch container header, no compression); otherwise loading fails with an
error.

`--sh2-jit` runs both SH-2s through the experimental JIT (`design/sh2-jit.md`)
instead of the interpreter, the same as the `brimir_sh2_jit` core option. Run
the same content with and without it to compare. The report then also prints
the JIT backend (`SH2 JIT backend: ir|x64`) and per-CPU executor totals since
startup (warmup included): `blocksRun` (compiled blocks executed),
`interpreted` (instructions handed to the interpreter), `nativeBlocksRun`
(compiled blocks executed as native code), `compileFallbacks` (blocks the
native backend could not compile; they run on the IR interpreter) and
`staleEntries` (native blocks dropped at entry because their code or a code
page changed since they were compiled; each is recompiled and run) and
`chainedBlocks` (native blocks entered directly from the previous block,
without returning to the executor; `nativeBlocksRun / (nativeBlocksRun -
chainedBlocks)` is the average chain length). A `cache` line per CPU adds the
block cache's totals: `compiles` (blocks built as IR), `nativeCompiles`
(blocks compiled natively: with a native backend a block runs on the IR
interpreter until its 16th run, `kNativeCompileThreshold`), `compileMs` (host
time building blocks and compiling them natively), `nativeBytesPerBlock`,
`invalidations`, `cachedInsts` (IR instructions of IR-only blocks, against the
IR cap), `irEvictions` (times the IR cap was reached, and how many live IR-only
blocks it evicted; it frees the least recently used IR blocks, live and
retired together, until the IR is below 7/8 of the cap), `irReuses` (IR blocks
dropped because their code changed and taken back from the IR pool when the
same code returned, instead of being built again; `retiredIrInsts` is the IR
the pool holds, counted in `cachedInsts`; `irPoolFreed` counts the pool blocks
the IR cap freed, and `irCapMs` the emulation-thread time of the cap events,
not part of `compileMs`), and flushes by trigger (`codeCap`: native code cap, `requested`:
CPU reset, state load, ...). Every 1800 measured frames a `jit progress` line
per CPU shows the cache occupancy. After the
measured frames, `jit window` sums both CPUs' compile work over the measured
frames (`compileMs` is emulation-thread time only: with the compile worker, the
worker's own time is `workerMs` per frame, and `async` and `dropped` count
the background compiles installed and discarded; `irReuses`, `irPoolFreed` and `irCapMs` as above), and five `slow frame` lines list the slowest measured frames (frame
number counted from startup) with their compile work.

`--jit-backend ir|x64` selects the JIT's code backend (`design/sh2-jit-m2.md`):
`ir` runs compiled blocks on the IR interpreter, `x64` compiles them to native
x86-64 code (only in x86-64 builds; elsewhere the option is rejected). The
default is `x64` when it is built, else `ir`. It needs `--sh2-jit` or
`--lockstep` (where it applies to the JIT core); otherwise it is a usage error.
To compare the backends with the interpreter, run the same content three
times: without `--sh2-jit`, with `--sh2-jit --jit-backend ir` and with
`--sh2-jit --jit-backend x64` (`design/sh2-x64-performance.md` uses the
scenes, warmup and frame counts of `design/sh2-baseline.md`).

On x86-64 with the `x64` backend and at least 4 hardware threads, the JIT
compiles native code on one worker thread, like the `brimir_sh2_jit_compile_thread`
core option set to `auto` (`SH2 JIT compile` shows `worker thread` or
`emulation thread`). `--jit-sync-compile` turns it off: compiles run on the
emulation thread, as with the option OFF. It needs `--sh2-jit` or `--lockstep`
(where it applies to the JIT core), and is the baseline for A/B runs of the
worker.

Output:

```
content      : Game.cue
frames       : 1800 (warmup 120)
sh2 jit      : off
ms/frame     : avg 7.912  p50 7.804  p95 9.120  p99 10.301  max 14.022  over16.7 0
fps (host)   : 126.4
Ymir_RunFrame: 7.850 ms/frame
SH2 master   : 3.120 ms/frame (39.7% of Ymir_RunFrame)
SH2 slave    : 1.004 ms/frame (12.8% of Ymir_RunFrame)
SH2 total    : 4.124 ms/frame (52.5% of Ymir_RunFrame)
```

`over16.7` counts the measured frames whose wall time exceeds 16.7 ms (one
60 Hz frame): the stutter count.

SH-2 time is measured inside `SH2::Advance` and includes on-chip peripherals
(DMA, timers) and bus accesses the CPUs make. VDP rendering runs on worker
threads by default, so `Ymir_RunFrame` is the emulation thread's time only.

### Lockstep validation

```powershell
build\bin\brimir_bench.exe --bios <bios> --game <game> --system-dir <dir> --lockstep 36000 [--jit-backend ir|x64] [--jit-sync-compile]
```

`--jit-backend` selects the JIT core's backend (default `x64` when built).
The JIT core compiles on the worker thread where supported (`asyncCompiles` above 0 on the
`cache` lines shows it was used); `--jit-sync-compile` runs it without. The
lockstep summary prints the executor and block-cache counters of both CPUs;
with `x64`, `compileFallbacks 0` means that every block that reached its native
compile threshold compiled; `blocksRun - nativeBlocksRun` are the IR runs before
that threshold.

`--lockstep N` loads the same content (and optional `--state`) into two cores,
one running both SH-2s through the JIT and one through the interpreter, runs
them side by side for N frames, and compares them after every frame (see
`design/sh2-jit.md` section 7.2). Compared after every frame, in this order:

- the audio samples produced during the frame (count and values);
- both SH-2s: registers, pipeline state, cache arrays and the on-chip
  peripherals (FRT, WDT, DMAC, DIVU, BSC, INTC and the pending interrupt), plus
  the slave SH-2 enable flag;
- low and high work RAM;
- the 32 KiB internal backup RAM, and the contents of a backup memory
  cartridge if one is inserted;
- the save state of every other subsystem (scheduler, system, SCU, SMPC, VDP,
  SCSP, the CD block -- HLE, or SH-1/YGR/CD drive/DRAM when LLE -- and the
  spillover cycle counters);
- the output frame.

In this mode threaded VDP rendering is turned off, and the RTC runs on emulated
time (virtual mode) instead of the host clock, so both cores see the same date
and time. Unless `--state` is given (a save state carries its own RTC time),
both cores' RTC starts at 1994-11-22 00:00:00, so runs are reproducible.
Lockstep runs write the virtual RTC timestamp back to the RTC file in
`--system-dir`, so point `--system-dir` at a scratch copy, not your real
RetroArch system folder.

Before the first frame the JIT core is synchronized from the interpreter core's
full state (`brimir::SyncLockstepCores`: save state plus backup RAM), because
some upstream Ymir fields are never initialized and two fresh cores can
otherwise differ in heap garbage that the comparison sees (see
`design/sh2-jit.md` section 7.2). A failed synchronization exits with code 2.

`--lockstep-reload K` (only with `--lockstep`) tests state loads: after every K
compared frames the run saves the interpreter core's state, runs and compares
K / 2 more frames, then loads the saved state into both cores
(`brimir::LoadLockstepState`) and continues; the cores are compared right after
the load too. Replayed frames count toward N. The JIT core keeps its compiled
code across the load, so this checks that cached blocks never run against
memory they were not compiled for.

Progress is printed every 600 frames (with `--lockstep-reload`, after every
reload cycle):

```
lockstep: 600/36000 frames identical
...
lockstep: OK, 36000 frames identical
SH2 JIT backend: x64
jit master   : blocksRun ...  interpreted ...  nativeBlocksRun ...  compileFallbacks ...  staleEntries ...  chainedBlocks ...
cache master : compiles ...  nativeCompiles ...  compileMs ... (build ...  native ...)
               nativeBytesPerBlock ...  invalidations ...  cachedInsts ...
               irEvictions ... (blocks ...)  flushes codeCap ...  requested ...
jit slave    : blocksRun ...  interpreted ...  nativeBlocksRun ...  compileFallbacks ...  staleEntries ...  chainedBlocks ...
cache slave  : ...
```

On the first difference it prints the frame number (0-based) and the first
differing field, for example
`lockstep divergence at frame 1234: master SH-2 R4 differs: a=0x... b=0x...`,
`internal backup RAM[0x0123] differs: a=0x... b=0x...` or
`vdp state differs (byte offset N)` (subsystem save states are compared
bytewise, so only the offset within the state is reported)
(`a` is the JIT core), and exits with code 3. `--lockstep` cannot be combined
with `--dump-at`, `--sh2-jit`, `--frames` or `--warmup` (usage error, exit 1):
it always runs one JIT core and one interpreter core for exactly N frames.

Exit codes: 0 success (including `--help`), 1 usage error (usage is printed to
stderr), 2 load or setup failure (BIOS, game, state, missing `--system-dir`,
temp directory errors), 3 lockstep divergence.

## bench/ — benchmark hosts (Linux devices)

Scripts that run the `design/sh2-baseline.md` method (the BIOS menu and the six
reference titles) on a dedicated Linux device, such as an ARM single-board
computer or handheld. All of them use one data directory (default `/mnt/data`,
or `BRIMIR_BENCH_DATA`):

```
bios/mpr-17933.bin                          US BIOS
bios/Sega Saturn BIOS v1.01 (JAP).bin       JP BIOS
bios/system/brimir_saturn_rtc_us_eu.smpc    configured RTC file (see above),
bios/system/brimir_saturn_rtc_jp.smpc       copied under both names
games/<No-Intro name>.chd                   the six reference titles
build/bench/bin/brimir_bench                default binary (BENCH overrides)
results/<host>/                             output, one folder per device
```

`setup_host.sh [data dir]` prepares a Debian/Ubuntu-based device once: build and
measurement tools (cmake, ninja, clang, perf, hyperfine, stress-ng), a
`cpu-performance` service that sets the `performance` governor at every boot,
and the directory layout. Build on the device with the bench configuration:

```sh
cmake -S <source> -B /mnt/data/build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DBRIMIR_LTO=ON
ninja -C /mnt/data/build/bench -j2 brimir_bench   # LTO peaks at ~520 MB with -j2
```

`run_bench.sh [rounds] [mode...]` runs every title once per round and mode
(`interp`, or a JIT backend name such as `ir` or `x64`), with the warmup and
frame counts of the baseline. Each run starts only when the hottest thermal zone
is at or below `COOL_C` (default 55 °C). Once a second it samples the
temperature, the lowest current/maximum frequency ratio over all CPU policies
and, on a Raspberry Pi, the firmware throttle flags (`vcgencmd`). A run is
marked invalid if it failed, any CPU policy dropped below 97% of its hardware
maximum, or the Pi flags show throttling. `summary.tsv` lists every run,
`medians.tsv` the run with the median average per title and mode (valid runs
only). `ONLY="pd2 sfz3"` restricts the titles; `PIN=4-7` runs `brimir_bench`
under `taskset` (for example on the big cores of a big.LITTLE device). Without
`PIN`, the VDP worker threads run on other cores as they do in the default
configuration.

`run_lockstep.sh [frames] [backend]` runs `--lockstep` on every title (default
1800 frames, backend `ir`) and lists the result line of each. It exits 1 if any
title diverged or failed. It passes `--no-cd-preload` to the game titles
(`EXTRA_ARGS` overrides it): with preloading, a Virtua Fighter 2 lockstep needs
more than the 905 MiB of a Raspberry Pi 3.

Start long runs detached so they survive a dropped SSH session, for example
`setsid -f tools/bench/run_bench.sh 3 interp ir > bench.out 2>&1 < /dev/null`.
