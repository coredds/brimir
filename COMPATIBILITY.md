# Saturn compatibility

**46 of 46 titles (48 discs) in the developer's collection work at the level each row below states.** The collection is
small (46 titles) and was chosen by the developer, not sampled from the whole Saturn library, so this is not a
compatibility percentage for all Saturn games.

Results of an automated sweep on 2026-11-27, after the sync of upstream Ymir `71f2968d` (the final run; the first run
of the same day is in the history note below). Brimir was built from the code of `c101560` (the emulation core at
upstream Ymir `71f2968d` and `02d295a7`; the tree of master `9e8d375` plus the sweep tools), with the core's developer
log on, as in a test build. That log made one game, Albert Odyssey, run so slowly that the driver stopped it as a log
flood (see below), so Albert Odyssey was rerun with the log off, the way release cores are built since `285563c`. Each
disc ran for 10,800 frames (3 minutes of Saturn time) with a scripted controller and a screenshot every 600 frames; the
screenshots were checked by eye. "How far" is how far the script got, not how far the game can be played: nobody played
these games to the end.

## Summary

| Status | Discs |
|---|---|
| Works (reaches gameplay or the attract mode) | 44 |
| Intro only (boots and plays its opening movie or narration; the script did not get further) | 2 |
| Expected screen (disc 2 started on its own shows the screen it is supposed to) | 2 |
| Minor issue (cosmetic, audio or speed) | 0 |
| Major issue (hang, black or garbled screen, wrong behaviour, unplayable speed) | 0 |
| Does not boot | 0 |

In titles: all 46 boot and run; for the two-disc games Grandia and Sakura Wars only disc 1 has to start the game, and
both do.

### History: the first sweep and what changed

The first sweep (earlier on 2026-11-27, with the emulation core at upstream Ymir `783035ee`) found three problems and
44 titles that worked. All three are gone:

- **Shinrei Jusatsushi Taroumaru and Baroque** broke in Brimir's 2026-11-26 sync, which took upstream's change
  `fd3e4398` (VBLANK forced to 1 while the display is off). Taroumaru stayed black and silent from power-on; Baroque
  froze black after its opening movie. Upstream replaced the change the next day with `71f2968d` ("Fixes 100+
  regressions"); Brimir took that, and the post-sync sweep shows both games in gameplay (Taroumaru: title, character
  select, stage 1; Baroque: first dungeon). Upstream Ymir at `783035ee` shows the same black screens, so this was not a
  Brimir-only bug, but Brimir had taken the faulty change.
- **Albert Odyssey** was the only Brimir-only problem, and it was not the emulation: the game sends the VDP1 an
  invalid command thousands of times per frame, and Brimir's build of that time printed a developer log line for each
  one (Ymir's release builds have the log off). The game ran at about 4 frames per second from about frame 2,400 and
  wrote gigabytes of log. Since `285563c` the developer log is off in release builds. The rerun with a log-off build
  runs all 10,800 frames in 40 seconds (3.4 ms per frame; 62 ms per frame with the log on, with identical frames), and
  its screenshots up to frame 4,799 are byte for byte those of the first run.

### Differences between the two sweeps

The post-sync sweep was compared with the first one (same input, frames and `--fixed-rtc`): 38 games identical, 10
different. Two are the fixed games and one is Albert Odyssey (above). The other differences came with the VBLANK
change and look like run-to-run content changes, not problems; they were only checked by eye:

- **King of Fighters '97:** the same flow, but a different CPU opponent and stage from frame 599 on (probably a timing
  or random-number difference); the fights play fine.
- **Daytona USA Circuit Edition:** screenshots differ from the second one on (the attract or title fade is in another
  phase); the contact sheets look the same.
- Daytona USA Ultimate, Grandia discs 1 and 2, J. League Victory Goal '96 and Magic Knight Rayearth: identical
  screenshots, only the audio peak or the final program counters in the summary differ.

No new failure appeared in any title.

## Titles

Status: **works**, **intro only**, **expected screen**. For two-disc games only disc 1 has to boot the game; disc 2 is
"expected screen" when it shows the screen it is supposed to show when started on its own.

| Title | Status | How far the sweep got | Notes |
|---|---|---|---|
| Albert Odyssey - Legend of Eldean (USA) | works | Title, intro, first town, village scenes with fights (all 10,800 frames) | Rerun with a log-off build: the sweep's own build had the developer log on, and the game then ran at about 4 fps (see the history above). |
| Astal (USA) | works | Title, stage 1 | |
| Baroque (Japan, English patch) | works | Title, menus, new save, name entry, opening movie, first dungeon with HUD (to the end) | Broken in the first sweep, fixed by `71f2968d` (see the history above). |
| Batsugun (Japan) | works | Gameplay, continue screens | |
| Blast Wind (Japan) | works | Gameplay to game over, high scores, back to title | |
| Bomberman Wars (Japan) | works | Quest mode, battle map | |
| Bulk Slash (English v1.001) | works | Stage select, mission briefing, gameplay | |
| Burning Rangers (USA) | works | Story mode, first mission | |
| Daytona USA - Circuit Edition (Japan) | works | Race | The attract fade is in another phase than in the first sweep; not a problem. |
| Daytona USA Ultimate | works | Race | |
| Dead or Alive (Japan) | works | Arcade mode fights | |
| DecAthlete (USA) | works | 100 m dash | |
| Dragon Force (USA) | works | New game, story dialogue | |
| Fighters Megamix (USA) | works | 1P mode fight | |
| Gale Racer (Japan) | works | Save file, name entry, race | |
| Golden Axe - The Duel (USA) | works | Fights | |
| Grandia (English v1.1.1), disc 1 | intro only | Title, new game, opening movie (to the end of the run) | The script did not get past the opening movie; the game boots and plays it. |
| Grandia (English v1.1.1), disc 2 | expected screen | "This is DISC 2. There is no Save Data present for this disc. Please insert DISC 1." | Expected without a save. The screen is silent; the same in Ymir. |
| Guardian Force (Japan) | works | Gameplay, continues | |
| Guardian Heroes (USA) | works | Stage 1 fights | With the default input the first story dialogue never advances (it wants B or C), the same in Ymir; a rerun with `a:10/60,c:10/90,start:20/1800` reached Stage 1. |
| Highway 2000 (USA) | works | Race to time up | |
| J. League Jikkyou Honoo no Striker (Japan) | works | Match | |
| J. League Victory Goal '96 (Japan) | works | Exhibition match | |
| King of Fighters '96, The (Japan) | works | Fights (1 MB RAM cartridge inserted from the game database) | |
| King of Fighters '97, The (Japan) | works | Fights (1 MB RAM cartridge) | A different CPU opponent and stage than in the first sweep; not a problem. |
| Last Bronx (USA) | works | Arcade mode fight | |
| Legend of Oasis, The (USA) | works | New game, opening dialogue | |
| Lunar - Silver Star Story (English v1.5.1a dub) | works | New game, opening movie, first scene | |
| Magic Knight Rayearth (undub) | works | In game, save menu | |
| Mega Man 8 (USA) | works | Intro stage | |
| Panzer Dragoon (USA) | works | Episode 1 | |
| Panzer Dragoon II Zwei (USA) | works | Episode 1 | |
| Radiant Silvergun (Japan) | works | Stage play to game over, back to title | |
| Sakura Wars (English patch), disc 1 | works | Title, intro, Episode 1 opening scene | |
| Sakura Wars (English patch), disc 2 | expected screen | Title, then "Please change discs ... Replace Disc 2 with Disc 1" from frame 2,781 to the end | Expected without a save. The same in Ymir. |
| Saturn Bomberman (Japan) | works | Normal game, stage 1 | |
| Sega Ages - Power Drift (Japan) | works | Race | |
| Sega Rally Championship (USA) | works | Race (Desert) | With the default input the game stays in its attract loop: the Start presses never come at a point the title takes them, the same in Ymir. A rerun with `start:10/120,a:10/90` reached a race. |
| Shining Force III - Scenario 1 (English patch) | works | Opening dialogue | |
| Shinobi Legions (USA) | works | Stage 1 | |
| Shinrei Jusatsushi Taroumaru (Japan) | works | Logo, credits, title, mode menu, character select, stage 1 with gameplay | Did not boot in the first sweep, fixed by `71f2968d` (see the history above). |
| Sonic R (USA) | works | Load data, character and course select, race | |
| Street Fighter Zero 3 (Japan) | works | Fights (4 MB RAM cartridge) | |
| Virtua Fighter 2 (Japan, Rev B) | works | Arcade mode fights | |
| VR Virtua Racing (USA) | works | Arcade race | |
| Wachenroeder (English) | intro only | Title, start, opening narration (still running at the end) | The script did not get past the narration; the game boots and plays it. |
| Winter Heat (Japan) | works | Speed skiing | |
| Yakyuuken Special, The (Japan) | works | Game sequences and movies | |

## Comparison with upstream Ymir

For the first sweep, each title that did not simply work was also run in upstream Ymir at `783035ee` (the same core
version) and at upstream's then-current `1526ade4`, with the same BIOS, settings, input and frame count, using a small
headless Ymir runner. The other 43 titles ran there too. Their timelines matched Brimir's: the same screens at the same
points, and nearly the same runs of identical frames. Brimir and Ymir show the same screens; in 33 of the 48 discs
some screenshots differ by about one frame of animation (the runs of identical frames start one frame apart), the
cause was not investigated, and the audio levels the two tools report differ slightly. This page does not count those
differences as compatibility problems. Plain Ymir `783035ee` also crashes at a leftover debug break in two games
(Baroque and J. League Victory Goal '96); upstream removed it in `02d295a7` and Brimir never had it. The post-sync
sweep was not repeated in Ymir.

## How to reproduce

Use a build with the tests off, so the core's developer log is off (configure once, from a shell with the MSVC
environment loaded for Ninja on Windows):

```powershell
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target brimir_bench
```

Then run the sweep with that bench and a new output folder, so the records in `docs\superpowers\records` are not
overwritten:

```powershell
$bench = 'build-release\bin\brimir_bench.exe'
pwsh tools\compat_sweep.ps1 -Bench $bench -OutDir C:\Temp\sweep-new     # all top-level *.chd in F:\OneDrive\Roms\saturn
pwsh -Command "& tools\compat_sweep.ps1 -Bench $bench -OutDir C:\Temp\sweep-rally -Only 'Sega Rally*' -InputScript 'start:10/120,a:10/90'"
pwsh -Command "& tools\compat_sweep.ps1 -Bench $bench -OutDir C:\Temp\sweep-gh -Only 'Guardian Heroes*' -InputScript 'a:10/60,c:10/90,start:20/1800'"
```

The last two are the reruns whose inputs got Sega Rally into a race and Guardian Heroes to Stage 1 (the same
`a:10/60,c:10/90,start:20/1800` also ran Lunar and Legend of Oasis). An existing build directory keeps the developer log
setting it was first configured with; use a fresh directory or pass `-DBrimir_ENABLE_DEVLOG=OFF`.

Defaults: 10,800 frames, input `start:120/600,a:30/900`, screenshot every 600 frames, `--fixed-rtc`, BIOS Auto,
cartridge from the game database, software renderer, default core settings. See `tools/README.md` (`compat_sweep.ps1`).
The screenshots and contact sheets are not in the repository.
