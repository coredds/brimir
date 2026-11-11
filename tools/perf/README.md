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
- `%TEMP%\opencode\vsrun.cmd` for `baseline.ps1`: runs a command inside the VS x64 developer environment.

## baseline.ps1 - Release+LTO build with provenance

```powershell
tools\perf\baseline.ps1 -Commit f395c6f   # -> %TEMP%\opencode\perf-base-f395c6f\build-lto\bin
tools\perf\baseline.ps1 -Tree .           # -> .\build-lto\bin (the working tree, uncommitted changes recorded)
```

The configuration of `release.yml` (`-G Ninja -DCMAKE_BUILD_TYPE=Release -DBRIMIR_LTO=ON`), targets `brimir_bench`,
`brimir_state_diff`, `brimir_libretro`, `brimir_vkhost` (`-Targets` to change). `-Commit` adds a detached
`git worktree` at `%TEMP%\opencode\perf-base-<sha7>` and reuses an existing build whose `provenance.json` matches the
commit and the binaries' SHA-256 (`-Rebuild` to force). `bin\provenance.json` holds the commit, `git status
--porcelain`, the CMake flags, the compiler and each binary's SHA-256. The last output line is the bin directory. About
1.5 min from scratch on the development machine (16 threads). Remove a base worktree with
`git worktree remove --force <dir>`.

## prove.ps1 - byte-identical output

```powershell
tools\perf\prove.ps1 -Base <bin dir> -New <bin dir> [-Audio] [-States] [-Frames 600] [-Windows gh,vf2] [-Modes thr,unthr]
```

- Always: `--hash` (per-frame frame hashes) on every window x mode.
- `-Audio`: `--audio-hash` (per-frame hash of the PCM output) on the same runs. For SCSP and SH-2 tasks.
- `-States`: `--save-state-every 1` (per-frame raw core state, `-StateEvery N` for every Nth) on the unthreaded runs.
  For SH-2, SCSP and save-state tasks. ~4 GB per run at 600 frames; only the first differing state of each side is
  kept (for `brimir_state_diff --strict <base> <new>`), everything else is deleted after the comparison.
- Exit 0: identical. Exit 1: a difference; prints `FIRST DIFFERENCE: <window> <mode>: <frame|audio|state> at window
  frame <i>: base '<line>' new '<line>'` (window frame = measured frame after the warmup). Exit 2: a run failed or a
  line count was wrong (`logs\*.err`).
- Base and new run one after the other per window x mode; `-Jobs` (default 6, 3 with `-States`) pairs run at once.
- Output (`-Out`, default `%TEMP%\opencode\perf-prove\<timestamp>`): `prove.md`, `prove.csv`, `provenance.json`,
  `logs\<window>-<mode>-<base|new>.txt`.
- `-Audio`/`-States` need Task 0 benches or later (the script checks `--help`).
- Time (16 threads): frames only 2 min; `-Audio -States` 10 min.

## measure.ps1 - interleaved timing

```powershell
tools\perf\measure.ps1 -A <bin dir> -B <bin dir> [-Rounds 3] [-Frames 1200] [-Windows ...] [-Modes thr,unthr] [-Extra '--sh2-jit']
```

The recon method: rounds are the outer loop; inside a round every window x mode runs A then B (B then A in even rounds),
one process at a time, each with a fresh scratch system directory. Per window x mode: median `avg` ms/frame of A and B,
spread = (max - min) / median over the rounds, change % of B against A, p95 medians, and the stage split the bench
reports (SH-2 master + slave from the SH-2 host-time profiling; "other" = `Ymir_RunFrame` minus SH-2: VDP, SCSP, SCU,
CD block and the wait for the VDP threads). Verdict `faster`/`SLOWER` only when |change| > max(3 %, both spreads), else
`noise`. Output: `measure.md`, `measure-summary.csv`, `measure.csv` (every run), `logs\`, `provenance.json`. Time: about
45 min for the default 3 rounds x 9 windows x 2 modes x 2 builds. Close other work while it runs: background load shows
up directly as spread.

## Typical task flow

```powershell
$base = tools\perf\baseline.ps1 -Commit <task base> | Select-Object -Last 1
$new  = tools\perf\baseline.ps1 -Tree . | Select-Object -Last 1
tools\perf\prove.ps1 -Base $base -New $new -Audio -States
tools\perf\measure.ps1 -A $base -B $new
```
