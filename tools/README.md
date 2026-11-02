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
chainedBlocks)` is the average chain length), `resumes` (steps that entered
native code at the PC where the previous slice stopped, without a cache
lookup: the slice ended at a block end or in the middle of a block, at the
cycle limit or for an interrupt; the interrupt, delay-slot and fetch-buffer
gates of a step apply first, so a pending interrupt is still interpreted) and
`runs` (scheduler slices, `Executor::Run` calls; `resumes / runs` is the share
of slices resumed). A `cache` line per CPU adds the block cache's totals: `compiles` (blocks built as IR), `nativeCompiles`
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
With threaded VDP the emulation thread also waits at the end of each frame for
the VDP2 thread, so the frame time can hide a change in SH-2 time.
`--no-threaded-vdp` renders VDP1 and VDP2 on the emulation thread instead (the
report prints `threaded VDP : on|off`); `--lockstep` always runs unthreaded.

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
jit master   : blocksRun ...  interpreted ...  nativeBlocksRun ...  compileFallbacks ...  staleEntries ...  chainedBlocks ...  resumes ...  runs ...
cache master : compiles ...  nativeCompiles ...  compileMs ... (build ...  native ...)
               nativeBytesPerBlock ...  invalidations ...  cachedInsts ...
               irEvictions ... (blocks ...)  flushes codeCap ...  requested ...
jit slave    : blocksRun ...  interpreted ...  nativeBlocksRun ...  compileFallbacks ...  staleEntries ...  chainedBlocks ...  resumes ...  runs ...
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

### Frame hashes

`--hash` prints one line per measured frame, after the warmup and before the report:

```
frame 0 320x224 65d0bc3658c2307f
```

`<i>` counts from 0, then the frame size and a 64-bit FNV-1a hash of the width, the height and the XRGB8888 pixels
(`tools/frame_hash.hpp`, shared with `brimir_vkhost`, so the two tools' lines compare directly). The hash is computed
outside the timed region. It cannot be combined with `--lockstep` or `--dump-at`. The Ymir core library also logs
to stdout, so filter for lines that start with `frame `.

## brimir_vkhost - headless libretro host

A frontend without a window: it loads the built `brimir_libretro` library dynamically (LoadLibrary/dlopen), as
RetroArch does, answers the core's environment calls, runs it and records every frame the core displays. Unlike
`brimir_bench` it goes through the libretro API, so it exercises the core options, geometry and video callback paths
that a frontend sees. It is built only with `BRIMIR_VULKAN` (the Vulkan context provider that hardware frames need is
added to it):

```powershell
cmake --build build --target brimir_vkhost
```

```powershell
build\bin\brimir_vkhost.exe --bios F:\Roms\BIOS\mpr-17933.bin --system-dir %TEMP%\scratch-system --frames 300 --hash
build\bin\brimir_vkhost.exe --bios <bios> --system-dir <dir> --game Game.chd --option brimir_overscan=1 --dump-dir frames
```

| Flag | Meaning |
| --- | --- |
| `--core <library>` | core to load (default: `brimir_libretro.dll` next to the executable, or `brimir_libretro.so` next to it or in `../lib`) |
| `--bios <file>` | BIOS image. It is copied into the system directory under its own file name (the core searches that directory) and selected with the `brimir_bios` option, so other BIOS files in the directory are ignored |
| `--game <file>` | content (`cue`, `chd`, `ccd`, `mds`, `iso`, `m3u`). Omit it to run the BIOS menu on its own |
| `--system-dir <dir>` | system directory; default is a fresh temporary one. As for `brimir_bench`, a game needs a configured `brimir_saturn_rtc_<jp\|us_eu>.smpc` in it, and the BIOS is copied into it, so use a scratch copy. One of `--bios` and `--system-dir` is required |
| `--frames N` | recorded frames (default 300) |
| `--warmup N` | frames run before recording (default 120, the same as `brimir_bench`) |
| `--option key=value` | core option override (repeatable). Every other option answers with the default the core declares through `SET_CORE_OPTIONS_V2` |
| `--hash` | one line per recorded frame, `frame <i> <w>x<h> <hash>`, the same as `brimir_bench --hash` |
| `--dump-dir <dir>` | write each recorded frame as `frame_<i>.ppm` |
| `--verbose` | also log core debug messages, geometry changes and AV info changes |
| `--vulkan` | act as a Vulkan frontend (below) and select `brimir_renderer=vulkan` (an explicit `--option` wins) |
| `--refuse-hw` | refuse `SET_HW_RENDER` and the negotiation interface, as RetroArch does on a gl/d3d driver without driver switching (and in a run-ahead second instance) |
| `--reset-at F` | with `--vulkan`: a video driver reinit before frame F (counted from 0, warmup included): `context_destroy`, the old device destroyed, a new one through the negotiation, `context_reset` |
| `--reset-in-run F` | the same reinit from inside frame F's `retro_run`, at the core's first environment call, the way RetroArch reinits inside `SET_SYSTEM_AV_INFO` (PAL content). A `SET_SYSTEM_AV_INFO` that changes the frame rate or the maximum geometry while a context is live also triggers one |
| `--reset-in-present F` | the same reinit at frame F's `GET_AUDIO_VIDEO_ENABLE`, after the core emulated the frame (its VDP2 frame ended) and before it records and presents it: that frame is lost with the device (a dupe) |
| `--hide-every N` | before every Nth frame (frames N-1, 2N-1, ... counted from 0, warmup included) one extra frame with video disabled, not recorded (fast-forward or run-ahead frames); compare against a software run with the same N |
| `--state-roundtrip N` | `retro_serialize` + `retro_unserialize` after every Nth frame |
| `--save-state-at F <file>` | `retro_serialize` after frame F (counted from 0, warmup included) into `<file>`; with `--compare` each child writes `<file>.software` or `<file>.vulkan` |
| `--load-state <file>` | `retro_unserialize` of `<file>` before the first frame (after `context_reset`, so with the Vulkan renderer installed) |
| `--preferred-hw gl` | with `--vulkan`: `GET_PREFERRED_HW_RENDER` reports GL (RetroArch on the gl driver with driver switching allowed); `SET_HW_RENDER` would still be accepted. The core must not ask |
| `--fake-api X.Y` | with `--vulkan`: `vkGetPhysicalDeviceProperties` (through the core's `get_instance_proc_addr`) caps `apiVersion` at X.Y; with 1.1 the core uses its copy-only output |
| `--grow-mask F` | with `--vulkan`: sync index mask `0b1` until frame F, then `0b111` (after waiting for the device) |
| `--fail-compute-on-grow` | with `--grow-mask`: the core's `vkCreateComputePipelines` fails once the mask has grown (fault injection: the core degrades to copy-only) |
| `--fail-compute-on-reset` | with `--vulkan`: the core's `vkCreateComputePipelines` fails for every context after the first (a reinit then ends in the copy-only output) |
| `--runahead` | run-ahead simulation: before each frame the main instance runs one hidden frame (`GET_AUDIO_VIDEO_ENABLE` without video); a second instance (a copy of the library, refused a hardware context) runs one frame per frame with audio and video disabled. Hidden frames are not rolled back (a pre-existing heap corruption in `retro_unserialize` was observed on BIOS-menu states; it may affect any state), so compare against a software run with `--runahead` |
| `--no-validation` | with `--vulkan` or `--compare`: do not load the Vulkan validation layer (the layer slows the Vulkan run; use it for timing) |
| `--gpu-vdp1` | with `--vulkan` or `--compare`: sets `BRIMIR_VDP1_GPU=1` for the core, whose Vulkan VDP renderer then draws VDP1 on the GPU (GPU stage 2; the default, so this only makes it explicit; the log says `Vulkan VDP1 renderer active`, and `VDP1 on the GPU: N ops-only submits, M sync submits (...)` with the waits by kind and their times when the renderer detaches) |
| `--software-vdp1` | with `--vulkan` or `--compare`: sets `BRIMIR_VDP1_GPU=0` (the core's debug override): VDP1 in software inside the Vulkan VDP renderer, as in stage 1. With `--vulkan` the host also prints `submits: sync S, context C, refused R, ops-only after refused E, outside O; waits: frame F, sync W, serialize Z, context X, outside the allowed list Y`: a submit with the fence the core logs for its VDP1 sync context (`VDP1 sync submit fence 0x...`, debug level) is a sync submit (not one of the run's frame submits); a refused submit (`--fail-submit-at`) is counted apart, and one accepted submit after it in the same run is allowed only with the core's `VDP1 ops-only submit after a refused frame submit` line; submits and waits outside `retro_run`, `retro_serialize` (waits only) and the context callbacks are problems. With `--state-roundtrip` it prints the mean and maximum `retro_serialize` time. |
| `--probe` | not with `--compare`. With `--vulkan`: sets `BRIMIR_VDP2_PROBE=1` for the core, whose Vulkan VDP renderer then logs one `VDP2 probe:` debug line per VDP2 frame (the front half's `Vdp2FeatureCounters` and its CPU time for the frame; with VDP1 on the GPU also, since the previous frame, the VDP1 front half's CPU time `vdp1_front_us`, the CPU time recording the VDP1 work `vdp1_record_us` and its GPU time `vdp1_gpu_us` from timestamp queries, which trails by the frames in flight; 0 where the queue family has no timestamps (`timestampValidBits` 0), logged once as `VDP1 probe: no GPU timestamps on this queue`). The summary's `probe-vdp1:` lines also give each counter's total over the recorded frames. Any renderer: sets `BRIMIR_VDP1_PROBE=1`; a core configured with `-DBRIMIR_VDP1_PROBE=ON` (counters in `src/core/include/ymir/hw/vdp/renderer/vdp1_probe.hpp`; other builds compile them out) logs one `VDP1 probe:` debug line per `retro_run` with the software VDP1 renderer's work: commands, lines, pixel writes, `VDP1SyncFB` calls, those of them after a draw or erase (`dirtysyncs`), CPU framebuffer writes and erases, the GPU VDP1 front half's batches by end reason and the N× VDP1 generator's spans by kind (`nx_rect`, `nx_quad`, `nx_repl`), slots (`nx_slots`), sub-batches (`nx_subb`) and CPU time (`nx_gen_us`; all zero with VDP1 upscaling off). The host collects the recorded frames' lines instead of printing them and prints a summary (below; `probe-vdp1:` lines give median, maximum, mean and frames with a non-zero count) |
| `--compare` | run the content on the software and on the Vulkan renderer in two child processes and compare the frame hashes (below). Exit 3 on any difference or when the Vulkan run was not on the compute output with the Vulkan VDP2 renderer (log line `Vulkan VDP2 renderer active`) for every frame |
| `--compare-vulkan-option key=value` | with `--compare`: a core option for the Vulkan run only (repeatable; to check that a difference is reported) |
| `--compare-state N` | with `--compare`, N at most `--frames` (otherwise a usage error: no state pair would be compared): both children save a state after every Nth recorded frame (`--save-state-every`) and each pair is diffed by `brimir_state_diff` (next to `brimir_vkhost`): the VDP1 framebuffers (`vdp.FBRAM`, `displayFB`) strictly, the rest of the VDP section (VDP1/VDP2 VRAM, CRAM, registers, VDP1 state, phases) strictly, the renderer section (`vdp1State` with `doubleV` and the mesh framebuffers, layer and rotation states, ...) up to its allowlist of line-local fetcher caches; differing work RAM offsets and SMPC output registers are named for information (two runs started at different host times differ there: the RTC follows the host clock). A differing pair fails the compare (exit 3); the first one's diff is printed and both states are copied to `<dump-dir>/states`. The other sections' differing bytes are printed for information (`compare: states:` line) |
| `--save-state-every N <dir>` | `retro_serialize` after every Nth recorded frame (recorded frames N-1, 2N-1, ...) into `<dir>/state_<frame>.bin` (not with `--compare`) |
| `--fixed-date [date]` | pin the RTC for deterministic runs from boot: sets `BRIMIR_RTC_FIXED` for the core (inherited by the `--compare` children, which also get the flag). After loading the content the core switches the RTC to the virtual (emulated-time) clock at that date and logs `RTC fixed to <date> (virtual clock, guest time <date>)`: the guest time is the date read back from the RTC after setting it. Without a date (or `1`): `brimir_bench --lockstep`'s date, 1994-11-22 00:00:00 (`brimir::kDeterministicRtcDate`); otherwise `YYYY-MM-DD` or `YYYY-MM-DDTHH:MM:SS` (an invalid date, or one the host cannot represent, is logged as an error and leaves the RTC alone). **Host time zone:** Ymir converts the date to the RTC's timestamp and back through the host's local time zone (`mktime` with `tm_isdst` 0, `localtime`). The core therefore reads the guest time back and, where daylight saving time would make the guest read another hour, shifts the request once so the guest reads the requested date; a date the zone or C library cannot represent (east of UTC in the first hours of 1970, years beyond 3000 on MSVC) fails with the error above. The stored timestamp still depends on the host zone, so `--fixed-date` runs (and their `rtcTimestamp`) are comparable on one machine, not across machines in different zones; a zone-independent pin would need a fork change (`timegm`/`gmtime` in virtual mode), which has not been made. **Persistence:** on a real system directory the core rewrites `brimir_saturn_rtc_<jp\|us_eu>.smpc` on unload (and when the guest sets the time) with the pinned 1994 timestamp, so use a scratch copy (`--compare` copies the directory per child; a plain run does not). With it two runs of the same content are identical, including the SMPC RTC fields and the work RAM the BIOS keeps the time in. A `--load-state` state carries its own RTC time. With `--compare-state` the pairs are diffed with `brimir_state_diff --strict`: every section (SH-2s, work RAM, SCU, SMPC with its RTC fields, SCSP, CD block, ...) must be identical too, not only the VDP section |
| `--dump-frame I` | with `--dump-dir`: write only recorded frame I |
| `--dump-every N` | with `--dump-dir`: write only recorded frames 0, N, 2N, ... (a usage error together with `--dump-frame`) |
| `--dump-vdp1-ops F <dir>` | with `--vulkan` and VDP1 on the GPU: sets `BRIMIR_VDP1_DUMP_OPS` so the core captures the VDP1 work of its VDP2 frame F (1 or more, counted from the renderer's attach: about frame F counting warmup frames from 0) into `<dir>/vdp1ops_<F>.bin` (`src/gpu/vdp1/vdp1_op_dump.hpp`): the op lists taken during the frame, all eight FBRAM slices before and after them (two Dump syncs), VDP1 VRAM, the VDP2 registers and the CRAM colours. Exit 2 when the file was not written (the core logs why, for example an FBRAM upload during the frame). Games drawing at 30 fps have frames without VDP1 work: capture the next one. Read by the N x VDP1 reference tests (`tests/unit/test_vdp1_nx_*.cpp`) |
| `--internal-resolution N` | the same as `--option brimir_internal_resolution=N` (1-4). With `BRIMIR_VDP1_UPSCALING=1` in the environment (GPU stage 3, until the core option exists) the VDP1 back half also draws the N x sprite surface at N > 1 (log `VDP1 upscaling: sprite surface at Nx (M MiB)`; VDP2 still shows the native sprite layer) |
| `--set-option-at F key=value` | a live option change before frame F (counting warmup frames from 0, as `--reset-at`), reported to the core by the next `GET_VARIABLE_UPDATE` (repeatable); with `--compare` both runs get it |
| `--hash-sample N` | with `--hash`: hash the sample points instead of the frame (below). With `--compare` the software run gets `--hash-sample 1` |
| `--sharpness N` | print, per recorded frame and as a summary, how much the N x frame differs from a nearest upscale (below) |
| `--region x,y,w,h` | with `--sharpness`: only these native output pixels (after crop and rotation; clamped to the frame) |
| `--fail-alloc-above MiB` | with `--vulkan`: the core's `vkAllocateMemory` fails (`VK_ERROR_OUT_OF_DEVICE_MEMORY`) for allocations above MiB (fault injection: a GPU short of memory); the summary counts the refused allocations |
| `--fail-submit-at F` / `--fail-submits N` | with `--vulkan`: the core's command-buffer submit (`vkQueueSubmit`) fails (`VK_ERROR_OUT_OF_DEVICE_MEMORY`, nothing reaches the queue) in runs F .. F+N-1 (frame numbers count warmup frames from 0; N defaults to 1): the core logs the failure and repeats the frontend's image (`video_refresh(NULL)`). With `--defer-image-read` the repeat reads the held image again at its own time |
| `--fail-output-images-above N` | with `--vulkan`: the core's `vkCreateImage` fails (`VK_ERROR_OUT_OF_DEVICE_MEMORY`) for its output images (created `MUTABLE_FORMAT`, which `libretro_vulkan.h` requires for `set_image` images; its internal images do not use it; they are exactly the frame size) wider than 352 x N (N = 1-3; the widest low-res frame at N x): the output images of an internal resolution fail while the renderer's resources do not |
| `--fail-output-images-on-grow` | with `--grow-mask` and `--fail-output-images-above N`: the limit applies only once the sync index mask has grown (the output images re-created for the larger ring fail above N) |
| `--defer-image-read N` | with `--vulkan` (not with `--grow-mask`): read each `set_image` image N (1-2) displayed frames later, from the handles copied at `video_refresh` as RetroArch's threaded video path keeps them (the latest legal read: before the core's next `wait_sync_index` for that sync index on the 3-slot ring). The core destroying such an image or its view before the read (wrapped `vkDestroyImage` / `vkDestroyImageView`) fails the run; reads still pending are done before a reinit, a sync-mask change and at the end. Dupes repeat the frame before them, in order |

The save directory is always a fresh temporary directory that is removed on exit. Core log messages go to stderr,
prefixed `[core <level>]` (`[core#2 ...]` for the run-ahead instance); the Ymir library's own log lines go to stdout,
so filter hash output for lines that start with `frame `. Exit codes: 0 success, 1 usage error, 2 load or setup
failure, 3 a check failed (the `vkhost: FAILED:` lines say which), 4 `--vulkan` without a usable Vulkan device.

### Vulkan host (`--vulkan`)

The host creates its own instance and device (`gpu::CreateHeadlessDevice`, validation layer on when installed;
`BRIMIR_VK_DEVICE` picks the GPU, `BRIMIR_VK_SYNC_VALIDATION=1` adds synchronization validation) before loading the
core, then plays RetroArch's vulkan driver: `GET_PREFERRED_HW_RENDER` answers Vulkan, `SET_HW_RENDER` (Vulkan) and the
negotiation interface (v2) are accepted, and after `retro_load_game` it calls `get_application_info` and
`create_device2` (with a `create_device_wrapper` that creates the device on the host's instance) and then
`context_reset`. `GET_HW_RENDER_INTERFACE` returns a version 5 `retro_hw_render_interface_vulkan` with a 3-slot sync
ring (mask `0b111`, the index advancing each displayed frame, `wait_sync_index` waiting for that slot's fence).
Each `video_refresh(RETRO_HW_FRAME_BUFFER_VALID)` reads back the last `set_image` image (`GENERAL` layout) and
records it as XRGB8888, so `--hash` lines compare directly with a software run.

The host also checks the core's side of the contract and fails the run (exit 3) on: `vkQueueSubmit` (wrapped
through the interface's `get_device_proc_addr`/`get_instance_proc_addr`) without `lock_queue` held, any wait
(`vkWaitForFences`, `vkDeviceWaitIdle`, `vkQueueWaitIdle`, `wait_sync_index`) while holding it, presentation calls
(`set_image`, `get_sync_index`, `wait_sync_index`, `vkQueueWaitIdle`) or hardware frames in frames with video disabled
(submits are allowed there and counted: the Vulkan VDP2 renderer still submits a hidden frame's VDP2 work), more than
one command-buffer submit in one `retro_run`, `set_image` outside `GENERAL`/`VK_QUEUE_FAMILY_IGNORED` or with
semaphores, a hardware frame whose `set_image` image (its extent recorded by the wrapped `vkCreateImage`) is not
exactly the `video_refresh` width x height (RetroArch's filter chain samples the whole image as a frame of that size),
features requested through `VkPhysicalDeviceVulkan1xFeatures`, a `create_device2` refusal (RetroArch may be
left without a GPU after its retry), unreadable frames, failed state saves/loads and validation errors. The
queue-lock check runs a self-test each time a context is created. The summary line reports context resets, the
core's submits and each counter. A second line, `vkhost: vulkan: core device memory: X MiB after the last frame, peak
Y MiB, N allocations`, sums the core's device memory (its `vkAllocateMemory` / `vkFreeMemory` calls, wrapped like
`vkQueueSubmit`; the host's own readback memory is not counted): X is what is live after the last recorded frame, Y
the most that was live at once over the run (every context; a live internal-resolution change briefly holds the old
and the new resources), N the number of successful `vkAllocateMemory` calls over the whole run, freed ones included
(a count of calls, not of live allocations; it grows with every context reset and live resolution change).

The host's volk and the core's are separate copies (the core is its own library with its own globals): the core's
`VulkanDevice::WrapExternal` loads its volk from the host's `vkGetInstanceProcAddr` and instance without touching the
host's function pointers.

The ctest entry `brimir_vkhost_vulkan` (`tests/vkhost_checks.cmake`) runs the BIOS menu (US BIOS from
`tests/fixtures`, 60 + 300 frames) in software, `--vulkan` (the Vulkan VDP2 renderer; also with rotation and
overscan), `--vulkan --refuse-hw`, `--vulkan --reset-at 120`, `--vulkan --reset-in-run 150`,
`--vulkan --reset-in-present 150` (alone and with `--fail-compute-on-reset`: exactly one frame repeats the previous
one, the operational exception "context destroyed during present"), `--vulkan --preferred-hw gl`,
`--vulkan --fake-api 1.1` (software renderer, copy-only output), `--vulkan --grow-mask 100` (also around hidden frames,
and with `--fail-compute-on-grow`: one repeated frame, then software with copy-only), `--hide-every 2` with
deinterlacing on and off, `--state-roundtrip 37`, cross-renderer state loads (a software state loaded by Vulkan and a
Vulkan state loaded by software, compared from recorded frame 2; the two states' renderer sections may differ only in
`brimir_state_diff`'s allowlist of line-local fetcher caches, `tools/state_diff.cpp`) and `--vulkan --runahead`, plus `--compare` (identical runs pass, also with hidden frames and state
round trips; a Vulkan run rotated 180 degrees exits 3 and names the first differing pixel), the internal resolution
(2x from the start, also rotated and cropped, with `--sharpness 2` all-constant on the menu; live changes 1x -> 2x -> 4x
-> 1x with deinterlacing on and off under synchronization validation: SET_GEOMETRY N x native, no SET_SYSTEM_AV_INFO;
`--hide-every 2` and `--reset-in-run 150` at 2x; the copy-only output at 4x stays native; `--fail-alloc-above 64` at 4x
falls back to 1x, a live 2x works, a live 4x stays at 2x; `--fail-output-images-above 3` with a live 2x -> 4x stays at
2x, a live 3x works, and at load 4x with `--fail-output-images-above 2` attaches at 1x; a mask growth at 4x with
`--fail-output-images-above 2 --fail-output-images-on-grow` continues at 2x; `--defer-image-read` with live
1x -> 4x -> 2x, also with hidden frames and weave, and across a reinit;
`--compare --internal-resolution 2 --hash-sample 2`; every
Vulkan run at N compared with software through `--hash-sample`), and requires identical
frame hashes, the Vulkan VDP2 renderer active where the context allows it (and not where it does not), the
validation layer on and zero validation errors, queue-lock violations and hidden-frame presentation calls, and at
most one submit per `retro_run`. It is skipped without the BIOS or a Vulkan device
(`BRIMIR_REQUIRE_VULKAN=1` fails instead; `BRIMIR_VKHOST_ALLOW_NO_VALIDATION=1` only warns without the layer).
A `video_refresh(NULL)` (dupe) is recorded as a repeat of the last frame.

To compare against `brimir_bench` use the same BIOS, game, `--warmup` and `--frames`, and separate scratch system
directories that each hold a copy of the RTC file (the core rewrites it):

```powershell
build\bin\brimir_vkhost.exe --bios <bios> --system-dir <dirA> --warmup 120 --frames 300 --hash
build\bin\brimir_bench.exe  --bios <bios> --system-dir <dirB> --warmup 120 --frames 300 --hash
```

The `frame ...` lines are identical for the software path.

### Software vs Vulkan comparison (`--compare`)

```powershell
build\bin\brimir_vkhost.exe --compare --bios <bios> --system-dir <dir> --game Game.chd --warmup 600 --frames 3600 --option brimir_overscan=2
```

Runs the same content twice, each in its own child process (a copy of this executable, so the Vulkan run's volk and
device state cannot disturb the software run): first the software renderer, then `--vulkan`, both with `--hash`. All
other flags (`--option`, `--warmup`, `--frames`, `--game`, ...) go to both runs; the Vulkan-only switches
(`--reset-at`, `--fake-api`, ...) go to the Vulkan run only, `--compare-vulkan-option` adds core options to the Vulkan
run only. Each child gets its own copy of `--system-dir` (the core rewrites the RTC file), so the directory is never
modified. The children's own summary lines are printed with a `compare: software:` / `compare: vulkan:` prefix; their
full logs are in the work directory (`%TEMP%\brimir_vkhost\compare-*`), which is removed after a successful comparison.

The per-frame hash lines are compared one by one. On the first difference the tool prints the frame index and both
hash lines, re-runs both configurations up to that frame with `--dump-frame`, writes both images as PPM
(`<dump-dir>/software/frame_NNNNN.ppm` and `<dump-dir>/vulkan/...`; `--dump-dir` or else a kept directory inside the
work directory) and prints the first differing pixel and both values:

```
compare: FAILED: 5 of 5 frames differ, first at frame 0
compare:   software: frame 0 320x224 248b219b61e6fcef
compare:   vulkan  : frame 0 320x224 b1da07f6d5775943
compare:   first differing pixel (7,0): software 0x000808, vulkan 0x000008
compare:   67432 of 71680 pixels differ
```

(that output came from `--compare-vulkan-option brimir_rotation=180`). A re-run that does not reproduce the original
hashes is noted. A differing frame count, a failed check of the Vulkan run (queue lock, validation, ... exit 3) or a
software run that fails also fail the comparison. Exit codes: 0 identical (`compare: OK, N frames identical`), 3
differences or a failed check, 2 setup failure, 4 no usable Vulkan device. A Vulkan run that silently stays on software
(or on the copy-only output) would match trivially, so `--compare` also requires: exactly `--frames` frames from both
runs, a `Vulkan frame output ready` log line in the Vulkan run, no fallback or copy-only log line, as many hardware frames
as frames and more than 0 core submits; otherwise it fails with exit 3 and says which. The result line states what was
verified (`compare: OK, 3600 frames identical (software vs vulkan compute output: 3600 hardware frames, 4200 core
submits, validation on, 0 validation errors)`). `--compare` is refused (exit 1) together with `--refuse-hw`,
`--preferred-hw`, `--fake-api`, `--fail-compute-on-grow` and any `brimir_renderer` option. The environment variable
`BRIMIR_VKHOST_COMPARE_VULKAN_ARGS` adds arguments to the Vulkan child only (a test hook: ctest uses it to check that
the vacuous cases fail). The hash covers the width, height and the
XRGB8888 pixels exactly as `--hash` does; the output parity rule (RGB equal at 1x, alpha not compared) is satisfied by
the host converting the Vulkan image to XRGB8888.

Each run also prints `vkhost: timing: X ms/frame (Y without the host readback)`: the wall time of the recorded frames'
`retro_run` calls, minus the time spent hashing or writing PPMs. With `--vulkan`, X includes the host's readback of
every frame (a wait for the GPU, a copy to a host buffer and a conversion; a real frontend presents the image
instead), Y leaves that out. With the software renderer both are the same. Time Vulkan runs with `--no-validation`:
the validation layer is on by default and slows the Vulkan run.

With `--probe` the run ends with `vkhost: probe:` lines over the VDP2 frames that ended while recording: batches,
arena bytes, VRAM/CRAM copy bytes and the front half's CPU time in microseconds per frame (`median`, `max`, `mean`;
the time covers `VDP2BeginFrame`, the lines with their sprite captures and `VDP2EndFrame`, not the register and memory
writes, which only mark state dirty), then per feature of `Vdp2FeatureCounters` (mosaic, vertical cell scroll,
character pattern delay, double-density line scroll, extended colour calculation and shadow lines, mesh slices) the
frames using it, the total and the most per frame, and the CRAM modes used (bit n: mode n). Probe lines that arrive
during warmup or hidden frames are dropped.

### Internal resolution (`--hash-sample`, `--sharpness`)

With `brimir_internal_resolution` N > 1 the Vulkan VDP2 renderer hands over N x the native frame (after crop and
rotation). The core logs `Internal resolution: frames at Mx (requested Nx)` before the first frame and whenever the
scale of the frames it hands over changes (M is 1 for the software renderer and the copy-only output, and after an
allocation fallback the effective scale); the host follows that line, so a frame's scale M is known even across live
changes (`--set-option-at`) and fallbacks. The N given to `--hash-sample` / `--sharpness` is only used until the core
logs a scale (the software renderer without a Vulkan output logs none). Each SET_GEOMETRY and SET_SYSTEM_AV_INFO is
printed (`vkhost: geometry: ...`) and the summary counts them (`vkhost: av: SET_SYSTEM_AV_INFO calls A, SET_GEOMETRY
calls G`): a change of the internal resolution must be a SET_GEOMETRY of M x the native size, never a
SET_SYSTEM_AV_INFO.

The sample point of native output pixel (x, y) of a frame at scale M (native output size W/M x H/M) is the N x pixel
that shows N x source pixel (M px, M py), where (px, py) is the pre-rotation native pixel the presenter reads for (x,
y): rotation 0 (px, py) = (x, y) and output (M px, M py); 90: (y, w-1-x), output (M w-1-M py, M px); 180: (w-1-x,
h-1-y), output (M w-1-M px, M h-1-M py); 270: (pw-1-y, x), output (M py, M pw-1-M px), with w x h the native output
size and pw the pre-rotation native width (`brimir_rotation` is taken from `--option`; a live change of it is not
followed). The overscan crop only offsets the frame by 8 px per step, so it does not change parities.

`--hash-sample`: the hashed image is the W/M x H/M native image of the sample points, with these points set to 0 in
every frame (a conservative host-side form of the brief's excluded lattice, Ruling 2): odd pre-rotation native columns
when the pre-rotation native width is above 400 (the hi-res widths 640/704 minus at most 48 px of crop; low-res is at
most 352), and odd pre-rotation native rows when the height is above 300 (448/480/512 minus the crop; progressive is at
most 256). The line is `frame <i> <W/M>x<H/M> <hash>` (the same FNV-1a as `--hash`). With M = 1 and a low-res
progressive frame nothing is excluded and the hash equals `--hash`'s. A frame whose size is not a multiple of M fails
the run (exit 2). So `--compare --internal-resolution N --hash-sample N` compares the Vulkan run at N with software at 1x
through the sample-point invariant. At the end of the run `vkhost: hash-sample: sampled P of Q points over F frames
(X%)` says how much of the frames the hashes covered: Q is every native pixel of the F frames, P those not in the
excluded lattice (100% on the low-res progressive BIOS menu, 25% on hi-res interlaced screens such as Virtua Fighter 2's
704x448 ones, where only the even columns of even rows are compared).

`--sharpness`: for each recorded frame at scale M (from the log line; frames at 1x give 0 and are counted
separately), over the native output pixels of `--region` (default the whole frame): for each native pixel, its M x M
block of the frame (output pixels [M x, M x + M) x [M y, M y + M)) and its reference, the frame's sample point of that
pixel (above; by the invariant this is the 1x image, so the reference image is nearest(img1) without a second run).
Printed per frame (`vkhost: sharpness: frame i Mx region x,y wxh: blocks B, non-constant K (P%), mean abs diff D`) and
at the end (`vkhost: sharpness: F frames (S at more than 1x), mean non-constant blocks P%, mean abs diff D`, both the
mean over the frames):

- non-constant block fraction K / B: the share of blocks with a pixel whose RGB differs from the reference (a nearest
  upscale has 0);
- mean abs diff D: the mean over every pixel of the region's blocks and its R, G and B channels of |pixel - reference|
  (0-255).

The reference at an excluded lattice point (an odd column of a hi-res RBG, ...) is the N x frame's own pixel there, not
the software 1x pixel, so the metric is relative to the N x frame's sample points, not to a 1x run.

The sample-point mapping and the metric are header-only (`tools/vkhost/sample_points.hpp`) and unit-tested
(`tests/unit/test_vkhost_sample_points.cpp`, tag `[vkhost]`): a nearest upscale gives 0 non-constant blocks and a mean
difference of 0 and samples back to the 1x image in every rotation; detail in one sub-pixel of every 4th block gives
exactly 25% and the expected mean difference; the excluded lattice is 0. On content: the BIOS menu at 2x is 0% (no
sub-pixel features), Virtua Fighter 2's attract fight at 2x about 17%.

Environment calls answered: `GET_CORE_OPTIONS_VERSION` (2), `SET_CORE_OPTIONS_V2`, `GET_VARIABLE`,
`GET_VARIABLE_UPDATE` (true once after a `--set-option-at` change), `SET_PIXEL_FORMAT` (XRGB8888 only), `GET_SYSTEM_DIRECTORY`,
`GET_SAVE_DIRECTORY`, `SET_SYSTEM_AV_INFO`, `SET_GEOMETRY`, `GET_LOG_INTERFACE`, `GET_INPUT_BITMASKS`,
`GET_AUDIO_VIDEO_ENABLE`, `GET/SET_DISK_CONTROL_(EXT_)INTERFACE`, the hardware rendering calls above (with
`--vulkan`; refused otherwise), and `SET_SUPPORT_NO_GAME`,
`SET_INPUT_DESCRIPTORS` and `SET_MEMORY_MAPS` (accepted, ignored). Any other call returns false and is logged once
as `environment call N not supported`. Audio and input are no-ops.

## bench/ - benchmark hosts (Linux devices)

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
