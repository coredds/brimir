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
| `--probe` | with `--vulkan` (not `--compare`): sets `BRIMIR_VDP2_PROBE=1` for the core, whose Vulkan VDP renderer then logs one `VDP2 probe:` debug line per VDP2 frame (the front half's `Vdp2FeatureCounters` and its CPU time for the frame); the host collects the recorded frames' lines instead of printing them and prints a summary (below) |
| `--compare` | run the content on the software and on the Vulkan renderer in two child processes and compare the frame hashes (below). Exit 3 on any difference or when the Vulkan run was not on the compute output with the Vulkan VDP2 renderer (log line `Vulkan VDP2 renderer active`) for every frame |
| `--compare-vulkan-option key=value` | with `--compare`: a core option for the Vulkan run only (repeatable; to check that a difference is reported) |
| `--dump-frame I` | with `--dump-dir`: write only recorded frame I |

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
semaphores, features requested through `VkPhysicalDeviceVulkan1xFeatures`, a `create_device2` refusal (RetroArch may be
left without a GPU after its retry), unreadable frames, failed state saves/loads and validation errors. The
queue-lock check runs a self-test each time a context is created. The summary line reports context resets, the
core's submits and each counter.

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
round trips; a Vulkan run rotated 180 degrees exits 3 and names the first differing pixel), and requires identical
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

Environment calls answered: `GET_CORE_OPTIONS_VERSION` (2), `SET_CORE_OPTIONS_V2`, `GET_VARIABLE`,
`GET_VARIABLE_UPDATE` (always false), `SET_PIXEL_FORMAT` (XRGB8888 only), `GET_SYSTEM_DIRECTORY`,
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
