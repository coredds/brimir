# Saturn compatibility

Results of an automated sweep over 46 Saturn titles (48 discs) on 2026-11-27, with Brimir built from commit `f4b7e07`
(the emulation core at upstream Ymir `783035ee`). Each disc ran for 10,800 frames (3 minutes of Saturn time) with a
scripted controller and a screenshot every 600 frames; the screenshots were checked by eye. "How far" is how far the
script got, not how far the game can be played: nobody played these games to the end.

Each title that did not simply work was also run in upstream Ymir at `783035ee` (the same core version) and at
upstream's current `1526ade4`, with the same BIOS, settings, input and frame count, using a small headless Ymir runner.
"Ymir too" means upstream Ymir shows the same problem; "Brimir only" means it does not.

## Summary

| Status | Titles |
|---|---|
| Works (reaches gameplay or the attract mode) | 43 |
| Minor issue (cosmetic, audio or speed) | 0 |
| Major issue (hang, black or garbled screen, wrong behaviour, unplayable speed) | 2 |
| Does not boot | 1 |

The three problems have two causes:

- **Shinrei Jusatsushi Taroumaru, Baroque:** Ymir too. Both are broken by an upstream change that Brimir took in its
  2026-11-26 sync (`fd3e4398`, VBLANK reads as 1 while the display is off). Upstream replaced it the next day with
  `71f2968d` ("Fixes 100+ regressions"), which fixes both games in the Ymir runs. Brimir needs that sync.
- **Albert Odyssey:** Brimir only. The emulation matches Ymir frame for frame, but the game sends the VDP1 an invalid
  command thousands of times per frame, and Brimir's release build prints a debug log line for each one (Brimir
  builds with the developer log on; Ymir's release builds have it off). The game slows to about 4 frames per second
  and writes gigabytes of log.

## Titles

Status: **works**, **minor**, **major**, **does not boot**. For two-disc games only disc 1 has to boot the game;
disc 2 is listed as "expected" when it shows the screen it is supposed to show when started on its own.

| Title | Status | How far the sweep got | Notes |
|---|---|---|---|
| Albert Odyssey - Legend of Eldean (USA) | major | Title, intro, first town (frame 4,799 of 10,800 before the 15-minute limit) | Brimir only. From about frame 2,400 it runs at about 4 fps: each frame logs thousands of `VDP1-Command \| Invalid command F` debug lines (13 GB of log in 15 minutes). Screens match Ymir `783035ee`, which runs all 10,800 frames at full speed with its log off. |
| Astal (USA) | works | Title, stage 1 | |
| Baroque (Japan, English patch) | major | Title, menus, new save, name entry, opening movie; then black and frozen from frame 4,696 to the end | Ymir too at `783035ee` (same black screen, bit for bit, once a leftover upstream debug break that stops Ymir there is removed). Fixed upstream in `71f2968d`: upstream's current version reaches the first dungeon. |
| Batsugun (Japan) | works | Gameplay, continue screens | |
| Blast Wind (Japan) | works | Gameplay to game over, high scores, back to title | |
| Bomberman Wars (Japan) | works | Quest mode, battle map | |
| Bulk Slash (English v1.001) | works | Stage select, mission briefing, gameplay | |
| Burning Rangers (USA) | works | Story mode, first mission | |
| Daytona USA - Circuit Edition (Japan) | works | Race | |
| Daytona USA Ultimate | works | Race | |
| Dead or Alive (Japan) | works | Arcade mode fights | |
| DecAthlete (USA) | works | 100 m dash | |
| Dragon Force (USA) | works | New game, story dialogue | |
| Fighters Megamix (USA) | works | 1P mode fight | |
| Gale Racer (Japan) | works | Save file, name entry, race | |
| Golden Axe - The Duel (USA) | works | Fights | |
| Grandia (English v1.1.1), disc 1 | works | Title, new game, opening movie (to the end of the run) | |
| Grandia (English v1.1.1), disc 2 | works (expected screen) | "This is DISC 2. There is no Save Data present for this disc. Please insert DISC 1." | Expected without a save. The screen is silent; the same in Ymir. |
| Guardian Force (Japan) | works | Gameplay, continues | |
| Guardian Heroes (USA) | works | Stage 1 fights | With the default input the first story dialogue never advances (it wants B or C), the same in Ymir; a rerun pressing A and C reached Stage 1. |
| Highway 2000 (USA) | works | Race to time up | |
| J. League Jikkyou Honoo no Striker (Japan) | works | Match | |
| J. League Victory Goal '96 (Japan) | works | Exhibition match | |
| King of Fighters '96, The (Japan) | works | Fights (1 MB RAM cartridge inserted from the game database) | |
| King of Fighters '97, The (Japan) | works | Fights (1 MB RAM cartridge) | |
| Last Bronx (USA) | works | Arcade mode fight | |
| Legend of Oasis, The (USA) | works | New game, opening dialogue | |
| Lunar - Silver Star Story (English v1.5.1a dub) | works | New game, opening movie, first scene | |
| Magic Knight Rayearth (undub) | works | In game, save menu | |
| Mega Man 8 (USA) | works | Intro stage | |
| Panzer Dragoon (USA) | works | Episode 1 | |
| Panzer Dragoon II Zwei (USA) | works | Episode 1 | |
| Radiant Silvergun (Japan) | works | Stage play to game over, back to title | |
| Sakura Wars (English patch), disc 1 | works | Title, intro, Episode 1 opening scene | |
| Sakura Wars (English patch), disc 2 | works (expected screen) | Title, then "Please change discs ... Replace Disc 2 with Disc 1" from frame 2,781 to the end | Expected without a save. The same in Ymir. |
| Saturn Bomberman (Japan) | works | Normal game, stage 1 | |
| Sega Ages - Power Drift (Japan) | works | Race | |
| Sega Rally Championship (USA) | works | Race (Desert) | With the default input the game stays in its attract loop: the Start presses never come at a point the title takes them, the same in Ymir. A rerun pressing Start for 10 frames every 120 reached a race. |
| Shining Force III - Scenario 1 (English patch) | works | Opening dialogue | |
| Shinobi Legions (USA) | works | Stage 1 | |
| Shinrei Jusatsushi Taroumaru (Japan) | does not boot | Black and silent from power-on (the whole run) | Ymir too at `783035ee` (bit for bit). Fixed upstream in `71f2968d`: upstream's current version reaches the title, character select and stage 1. |
| Sonic R (USA) | works | Load data, character and course select, race | |
| Street Fighter Zero 3 (Japan) | works | Fights (4 MB RAM cartridge) | |
| Virtua Fighter 2 (Japan, Rev B) | works | Arcade mode fights | |
| VR Virtua Racing (USA) | works | Arcade race | |
| Wachenroeder (English) | works | Title, start, opening narration (still running at the end) | |
| Winter Heat (Japan) | works | Speed skiing | |
| Yakyuuken Special, The (Japan) | works | Game sequences and movies | |

The other 43 titles also ran in Ymir `783035ee` and in upstream's current version. Their timelines match Brimir's
(the same screens at the same points and nearly the same runs of identical frames). Some screenshots differ by about one
frame of animation, and the audio levels the two tools report differ slightly. This page does not count those
differences as compatibility problems.

## How to reproduce

```powershell
pwsh tools\compat_sweep.ps1                       # all top-level *.chd in F:\OneDrive\Roms\saturn, defaults as above
pwsh -Command "& tools\compat_sweep.ps1 -Only 'Sega Rally*' -InputScript 'start:10/120,a:10/90' -OutDir <dir>"
```

Defaults: 10,800 frames, input `start:120/600,a:30/900`, screenshot every 600 frames, `--fixed-rtc`, BIOS Auto,
cartridge from the game database, software renderer, default core settings. See `tools/README.md` (`compat_sweep.ps1`).
The screenshots and contact sheets are not in the repository.
