# SH-2 baseline (interpreter)

**Date**: 2026-09-30
**Commit**: b2733c0 (the measured code: the parent of the commit that adds this report)
**Machine**: AMD Ryzen 7 5700G, 8 cores / 16 threads, 32 GB RAM, Windows (Microsoft Windows NT 10.0.26300.0)
**Build**: Release, LTO + IPO, MSVC 19.44.35229 (Visual Studio 2022, toolset 14.44), `CMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG`
**Method**: `brimir_bench --frames 1800 --warmup 300` for the BIOS menu and `--frames 1800 --warmup 2400` for games (no save states, no controller input, so games are measured on their title/attract screens), median of 3 runs (the run with the median `avg`). SH-2 time is host time inside `SH2::Advance` (includes SH-2 DMA/timers and bus access). VDP rendering is threaded (defaults). Percentages are of `Ymir_RunFrame`, the emulation thread's time per frame, which is within 0.01 ms of the frame average in every run.

| Title | Scene | ms/frame avg | p95 | SH2 master ms (%) | SH2 slave ms (%) | SH2 total % |
|---|---|---|---|---|---|---|
| BIOS menu (US BIOS) | boot +5 s, no disc, no input (SMPC settings unset, so the BIOS first-boot setup screen is expected) | 8.559 | 9.401 | 2.301 (26.9%) | 0.000 (0.0%) | 26.9% |
| Virtua Fighter 2 (Japan) (Rev B), JP BIOS | boot +40–70 s, attract/title (no input); switches to 704x448 double-density interlace | 9.411 | 12.258 | 4.155 (44.2%) | 2.554 (27.1%) | 71.3% |
| Panzer Dragoon II Zwei (USA), US BIOS | boot +40–70 s, attract/title (no input) | 10.246 | 11.787 | 4.691 (45.8%) | 2.733 (26.7%) | 72.5% |
| Sega Rally Championship (USA), US BIOS | boot +40–70 s, attract/title (no input) | 10.259 | 12.684 | 3.872 (37.7%) | 3.187 (31.1%) | 68.8% |
| Burning Rangers (USA), US BIOS | boot +40–70 s, attract/title (no input); uses 704x448 interlace at points | 13.567 | 15.611 | 6.040 (44.5%) | 3.161 (23.3%) | 67.8% |
| Guardian Heroes (USA), US BIOS | boot +40–70 s, attract/title (no input) | 9.563 | 11.390 | 3.713 (38.8%) | 2.417 (25.3%) | 64.1% |
| Street Fighter Zero 3 (Japan), JP BIOS, 4 MB RAM cart | boot +40–70 s, attract/title (no input) | 10.810 | 11.816 | 3.887 (36.0%) | 2.890 (26.7%) | 62.7% |

Content: US BIOS is `mpr-17933.bin` (US/EU v1.00, MD5 `3240872c70984b6cbfda1586cab68dbe`), JP BIOS is `Sega Saturn BIOS v1.01 (JAP).bin` (JP v1.01, MD5 `85ec9ca47d8f6807718151cbcca8b964`). The file named `sega_101.bin` in the same BIOS folder is the same JP v1.01 image (identical MD5), not a US BIOS. The 4 MB cart for Street Fighter Zero 3 is inserted automatically from Ymir's game database. Scenes were not checked visually; the benchmark has no video output. The descriptions come from elapsed time and the VDP2 resolution changes in the log.

To reproduce a row:

```powershell
brimir_bench --bios <BIOS> --game <game> --system-dir <configured system dir> --warmup 2400 --frames 1800
brimir_bench --bios mpr-17933.bin --warmup 300 --frames 1800    # BIOS menu row
```

The system directory must hold a configured `brimir_saturn_rtc_<jp|us_eu>.smpc` (the suffix follows the BIOS region). To get one, run any game once in RetroArch with Brimir and complete the BIOS language/clock setup. The file in the RetroArch system folder then has STE=1, and that folder can be passed as `--system-dir`. Without it (STE=0) the BIOS stops at its first-boot language/clock screen and never boots the disc. That screen costs 8.1–9.1 ms/frame at 24–28% SH-2 share, the same as the no-disc row, and the slave SH-2 reports 0.000 ms. With `--system-dir` the core may write updated RTC settings back into that directory, as it does under RetroArch. `brimir_bench` puts backup and cart RAM (`.srm`, `.cart`) in a fresh per-run temp directory and deletes it on exit, so every run starts from the same state. The no-disc path does not load SMPC settings, so the BIOS menu row always measures the setup screen.

The committed numbers were measured before `--system-dir` existed, with an equivalent manual setup: the configured `brimir_saturn_rtc_us_eu.smpc` was copied into the bench's temp directory under both the `jp` and `us_eu` names, and `.srm`/`.cart` files were deleted before every run. The JP titles (Virtua Fighter 2, Street Fighter Zero 3) therefore ran with settings copied from the us_eu file. A check run of Sega Rally with `--system-dir` and no manual copying (3 runs: 9.202, 10.311, 9.693 ms avg; SH-2 total 67.7%, 68.8%, 68.1%) matched the SH-2 share within 1.1 points. Its median avg (9.693 ms) was 5.5% below the table's 10.259 ms, and the three check runs spread 9.20–10.31 ms, so compare ms/frame only between runs made in the same session.

## Observations

- Every game is SH-2 bound. The two SH-2s take 62.7–72.5% of emulation-thread time. The 3D titles (Panzer Dragoon II Zwei, Virtua Fighter 2, Sega Rally, Burning Rangers) are at 67.8–72.5%. The 2D titles (Guardian Heroes, Street Fighter Zero 3) are lower at 62.7–64.1%, but the SH-2s are still the largest cost. VDP rendering runs on worker threads, so it is not part of these numbers. The remaining 27–37% of emulation-thread time is presumably the SCU, SCSP/68000, CD block, VDP register and timing emulation, and the scheduler (not profiled). Any time the emulation thread spends waiting on the VDP worker threads would also land there.
- The BIOS with no disc uses only the master SH-2 (slave 0.000 ms) and spends only 26.9% of its time in SH-2 code.
- All six games start the slave SH-2 (`SSHON`) and keep it busy on their title/attract screens. It takes 23–31% of emulation-thread time, 2.4–3.2 ms/frame, which is 0.52–0.82x the master's time. Sega Rally is the most balanced (slave 3.19 ms vs master 3.87 ms). Burning Rangers has the heaviest master (6.04 ms/frame) and is the slowest title overall (13.57 ms avg, 15.61 ms p95). It is still inside the 16.7 ms NTSC frame budget on this machine, but with little margin: Burning Rangers run 2 had p95 16.667 ms and p99 19.6 ms. VDP1 "possible infinite loop" warnings (emulator-side) appeared in this title's logs in every run.
- Run-to-run spread was small. Median-to-extreme `avg` difference was at most 5.4% (Burning Rangers run 2, which had one 76 ms outlier frame), and the SH-2 shares varied by less than 1 point across runs.

## Implications for the JIT

- Expected ceiling: the SH-2s take 62.7–72.5% of frame time in games, so removing all SH-2 cost would save at most that much. For example, Panzer Dragoon II Zwei would go from 10.25 ms to about 2.8 ms, and Burning Rangers from 13.57 ms to about 4.4 ms. `SH2::Advance` also includes bus accesses, on-chip DMA and timers, which a JIT keeps, so the real ceiling is lower. With share *s* and SH-2 speedup *k*, the new frame time is (1 − s + s/k) of the old. At s = 0.70 and k = 3–5 that is 0.44–0.53 of the current frame time, or about 1.9–2.3x overall.
- Milestone 1 validation (10-minute runs): use Virtua Fighter 2 (interlaced high-res, both SH-2s), Sega Rally Championship (most slave SH-2 work), Burning Rangers (heaviest master SH-2 load, slowest title) and Guardian Heroes (2D). For later speed comparisons, use all six games plus the BIOS row, with the same `brimir_bench` flags and a configured `--system-dir`. Add gameplay save states (`--dump-at` / `--state`) when gameplay scenes are needed, since attract screens may under-represent in-game load.
