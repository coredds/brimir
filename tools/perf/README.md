# tools/perf - proof and measurement harness (Windows, PowerShell 7)

Three scripts that every speed-up uses: build the base, prove the output is byte-identical, measure the gain. They run
the windows of the performance recon (`docs/superpowers/records/2026-11-11-perf-recon/report.md`, "Content and
windows") with `brimir_bench`.

| window | BIOS | content | warmup |
|---|---|---|---|
| bios | US mpr-17933 | BIOS menu, no disc (`--fixed-rtc` in `prove.ps1` when both benches have it) | 300 |
| rally | US | `states\rally.state` | 1000 |
| vf2 | JP v1.01 | `states\vf2.state` | 120 |
| burning, pdz, megamix, gh, astal | US | `states\<name>.state` | 120 |
| sfz3 | JP v1.01 (4 MB cart) | `states\sfz3.state` | 120 |

Each window runs threaded (`thr`, the default) and with `--no-threaded-vdp` (`unthr`).

## Inputs (not in git)

- `-Records` (default: the recon records folder, or `$env:BRIMIR_PERF_RECORDS`): `states\<window>.state` (bench
  states, BIOS-dependent) and `sys\<window>\brimir_saturn_rtc_*.smpc` (configured RTC files).
- `-BiosDir` (default `F:\OneDrive\Roms\BIOS`, read only): the BIOS images are copied into a fresh scratch system
  directory for every run, with the window's RTC file (the core may write the RTC file back on exit).
- `-Roms` (default `F:\OneDrive\Roms\saturn`): the disc images.
- `tools\perf\vsrun.cmd` (used by `baseline.ps1`): runs a command inside the VS x64 developer environment (vswhere +
  vcvarsall).

## baseline.ps1 - Release+LTO build with provenance

```powershell
tools\perf\baseline.ps1 -Commit f395c6f   # -> %TEMP%\opencode\perf-base-f395c6f\build-lto\bin
tools\perf\baseline.ps1 -Tree .           # -> .\build-lto\bin (the working tree, uncommitted changes recorded)
```

The configuration of `release.yml` (`-G Ninja -DCMAKE_BUILD_TYPE=Release -DBRIMIR_LTO=ON`), targets `brimir_bench`,
`brimir_state_diff`, `brimir_libretro`, `brimir_vkhost` (`-Targets` to change). `-Commit` runs `git worktree prune`,
adds a detached `git worktree` at `%TEMP%\opencode\perf-base-<sha7>` and reuses an existing build whose
`provenance.json` matches the commit and the binaries' SHA-256 (`-Rebuild` to force). `bin\provenance.json` holds the
commit, `git status --porcelain`, the SHA-256 of `git diff HEAD --binary` and of each untracked file (so a `-Tree` build
of uncommitted work is identifiable), the CMake flags, the compiler and each binary's SHA-256. The last output line is
the bin directory. About 1.5 min from scratch on the development machine (16 threads). Remove a base worktree with
`git worktree remove --force <dir>`.

## prove.ps1 - byte-identical output

```powershell
tools\perf\prove.ps1 -Base <bin dir> -New <bin dir> [-States] [-NoAudio] [-Frames 600] [-Windows gh,vf2] [-Modes thr,unthr] [-AllowSame]
```

- Every frame from the state load (or the boot, for the bios window) is checked: `--warmup 0 --frames <the window's
  warmup + Frames>` (rally: 1600 frames, bios 900, the others 720).
- Always: `--hash` (per-frame frame hashes) and `--audio-hash` (per-frame hash of the PCM output) on every window x
  mode. `-NoAudio` for benches older than Task 0.
- `-States`: `--save-state-every 1 -` (hash of the raw core state after every frame, `-StateEvery N` for every Nth) on
  the unthreaded runs. For SH-2, SCSP and save-state tasks. Nothing is written to disk; on a difference at frame i both
  sides are re-run with `--frames i+1 --save-state-every i+1 <dir>` so the first differing state of each side is kept
  in `states\<window>-unthr-{base,new}\` for `brimir_state_diff --strict <base> <new>` (~7 MB each).
- Saving states does not disturb the run: the unthreaded frame and audio hashes are identical with and without
  `--save-state-every` (checked in Task 0 on all 9 windows), so a `-States` run also proves the unthreaded frames.
- Exit 0: identical. Exit 1: a difference; prints `FIRST DIFFERENCE: <window> <mode>: <frame|audio|state> at frame
  <i> from load (window frame <i - warmup>): base '<line>' new '<line>'`. Exit 2: a run failed, a line count was wrong,
  or a script/setup error (the first non-empty stderr line of each side is shown in the `checked` column).
- Base and new must be different benches (SHA-256): comparing a bench with itself aborts with `SELF-COMPARE` unless
  `-AllowSame` (harness self-checks only).
- Base and new run one after the other per window x mode; `-Jobs` (default 6) pairs run at once.
- Output (`-Out`, default `%TEMP%\opencode\perf-prove\<timestamp>`): `prove.md`, `prove.csv`, `provenance.json`,
  `logs\<window>-<mode>-<base|new>.txt`.
- Time (16 threads): frames + audio about 3 min; with `-States` about 10 min.

## measure.ps1 - paired timing

```powershell
tools\perf\measure.ps1 -A <bin dir> -B <bin dir> -SelfCheck                                   # all windows, 6 rounds
tools\perf\measure.ps1 -A <bin dir> -B <bin dir> -Windows vf2,megamix -Modes unthr -Rounds 8  # a task's target
```

- **Isolation.** Each bench is started with `ProcessorAffinity` = one logical CPU per physical core, leaving out
  CPU 0's core (`-Affinity <hex mask>` to override), and `PriorityClass` High. Before each run the script samples the
  whole machine's CPU load for 1 s (`GetSystemTimes`) and waits (up to `-QuietWaitSeconds`, 60) until it is at most
  `-MaxLoad` (5 %); a run started above that is flagged busy (`-RequireQuiet` fails instead). The affinity, priority
  and active power scheme are recorded in `provenance.json`. One discarded warm-up run per build starts the session.
- **Self-check** (`-SelfCheck`): A against A on `-SelfCheckWindows` (gh, vf2) x the modes for `-SelfCheckRounds` (4)
  rounds first; aborts (exit 3) if any paired spread exceeds `-SelfCheckLimit` (3 %).
- **Pairing.** Rounds are the outer loop; inside a round every window x mode runs A then B (B then A in even rounds).
  Default 6 rounds (even, so both orders count equally; use at least 5). For each round the ratio B/A of the p50
  ms/frame is taken (p50, not the average: single slow frames and stalls do not move it); the result is the median
  ratio as a change %, with [min..max] over the rounds. The verdict is `faster`/`SLOWER` only if at least 80 % of the
  rounds have the sign of the median (4 of 5, 5 of 6), else `noise`; `keep` marks faster by 3 % or more.
- **Outliers.** A run whose p50 is more than `-OutlierPercent` (8 %) from its cell's median is re-run (up to
  `-MaxReruns`, 2) and the re-run used in its round; rejected runs are listed in `measure.md` and `rejected.csv`.
- **Stages.** The same paired change, range and verdict for the SH-2 time (master + slave, from the SH-2 host-time
  profiling) and "other" (`Ymir_RunFrame` minus SH-2: VDP, SCSP, SCU, CD block, ...), from the bench's per-run
  averages. In thr mode "other" includes the emulation thread's wait for the VDP threads at the end of the frame: a VDP
  speed-up may show only in unthr, or in thr only as less waiting.
- **Keep/drop decisions:** measure the windows and mode the task targets with `-Windows ... -Modes ... -Rounds 8` or
  more on a quiet machine (nothing else running: no ctest, WSL or builds), after a `-SelfCheck`.
- Output: `measure.md`, `measure-summary.csv`, `measure.csv` (every run with load, wait and status), `rejected.csv`,
  `selfcheck*.csv`, `logs\`, `provenance.json`. Time: about 25 s per run (1200 frames); the full default (6 rounds x 9
  windows x 2 modes x 2 builds = 216 runs) takes about 1.5 h, so split it by `-Windows` to stay under 2 h shells.

## Typical task flow

```powershell
$base = tools\perf\baseline.ps1 -Commit <task base> | Select-Object -Last 1
$new  = tools\perf\baseline.ps1 -Tree . | Select-Object -Last 1
tools\perf\prove.ps1 -Base $base -New $new -States
tools\perf\measure.ps1 -A $base -B $new -SelfCheck -Windows <targets> -Rounds 8
```