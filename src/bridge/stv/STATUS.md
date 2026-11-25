# ST-V support: status

The thirteen arcade games in the table below run on the ST-V board emulation in this folder. This file says what was
checked, how, and where the behaviour differs from MAME (the reference). The design is in
`docs/superpowers/specs/2026-11-21-stv-support-design.md` (not tracked); the changes to `src/core/` are listed in
`src/core/BRIMIR_FORK.md`.

## Games

Every game was run on Brimir through `CoreWrapper` as the libretro layer drives it (the real ROM set from a MAME zip,
the real BIOS) and compared with MAME 0.289 (`-bios jp1`, or `-bios us` for Die Hard Arcade) on the same schedule: boot,
attract mode, one coin, Start, a second player's coin, then a minute of scripted play for both players. The BIOS credit
count (work RAM `0x06000740`) was logged in both emulators and changes on the same frames within two to four. Screens
were compared by eye (attract, credit, game select, play); audio is only checked to be present (peak above 16), not
compared with MAME's.

| MAME set | Title | Regions | Controls | BIOS used by default | Verified |
|----------|-------|---------|----------|----------------------|----------|
| `bakubaku` | Baku Baku Animal | J | 3 buttons | Japan (`epr-20091.ic8`) | boot, attract, coin, Start; EEPROM first-boot image equal to MAME's; saves persist |
| `colmns97` | Columns '97 | JET | 3 buttons | Japan | full case; credits within 2 frames of MAME |
| `puyosun` | Puyo Puyo Sun | J | 3 buttons | Japan | full case; credits within 3 frames of MAME |
| `diehard` | Die Hard Arcade | UET | 3 buttons | USA (`epr-17952a.ic8`; no Japan region) | full case; needed the slave SH-2 hold-line interrupt fix |
| `gaxeduel` | Golden Axe: The Duel | JUETL | 6 buttons | Japan | full case; boot timing not measured against MAME (it runs the same BIOS check as the other games, see "Boot length"); all six buttons of both players proven in the game's own input test |
| `cotton2` | Cotton 2 | JUET | 3 buttons | Japan | full case with continues; visuals match MAME's by eye |
| `grdforce` | Guardian Force | JUET | 3 buttons | Japan | full case with continues; credits (including continues) within a frame of MAME |
| `rsgun` | Radiant Silvergun | JUET | 3 buttons + a 4th (see below) | Japan | full case with continues; the 315-5881 chip's 134 accesses equal to MAME's, the 128 decrypted longs checked in a test |
| `shienryu` | Shienryu | JUET | 3 buttons | Japan | full case with continues; vertical (MAME's ROT270): upright under Screen Rotation Auto, BIOS screens included (the set's default EEPROM); screens match MAME's by eye |
| `ffreveng` | Final Fight Revenge | JUET | 6 buttons (the game reads 5) | Japan | full case; the 315-5881's uncompressed path: the game's 16 decrypted longs equal to MAME's in every round (403 in MAME, 401 in Brimir over 7600 frames), checked in a test; buttons 1-5 of both players proven in the game's input test |
| `sss` | Steep Slope Sliders | JUET | 3 buttons (the game reads 2), one player | Japan | full case for one player (see below); the 315-5881's compressed path over a whole block: the 7344 decompressed longs equal to MAME's, checked in a test; the play minute shows the same timer and points as MAME's at three frames; buttons 1-2 proven in the game's input test |
| `sanjeon` | DaeJeon! SanJeon SuJeon | AJTUE | 3 buttons | Japan | full case; the cartridge's scramble (see below); credits within 4 frames of MAME; from Start on, the screens match MAME's at every compared frame (the minigames, their timers and the money) |
| `decathlt` | Decathlete (V1.001) | JUET | 3 buttons | Japan | full case, compared with libretro-stv (MAME does not run it, see below); the 315-5838's data stream equal to libretro-stv's (209,536 words over the first 1800 frames, byte for byte); a state saved inside one of the game's real blocks (the test starts the block itself, through the bank window with the game's uploaded tables, since Brimir finishes every block inside one frame) continues identically in a fresh core |

The BIOS is chosen from the game's regions in the order Japan, USA, Europe (then the game's other regions), or by the
Saturn BIOS core option's region (`jp_*`, `us_*`, `eu_*`) when the game has that region. A game whose regions have no
BIOS in the system folder fails to load with a message that names the accepted file names. The accepted names are MAME's
`stvbios` set and the older MAME names of the same dumps (`stv_bios.cpp`).

Within a region the core tries the verified dumps first, then the accepted but unverified ones in MAME's order (Ruling
R23). "BIOS used by default" in the table above is therefore the file the core picks from the development PC's folder,
not MAME's default: MAME's default Japan BIOS is its newest dump, `epr-23603.ic8` (`-bios jp`), which was not available
and was never run here; MAME was run with `-bios jp1` (`epr-20091.ic8`) to match.

- Verified (on the development PC, CRC-32 equal to MAME's): Japan `epr-20091.ic8` (= `20091.bin`), `epr-19730.ic8`
  (= `epr19730.ic8`), `epr-17951a.ic8` (= `mp17951a.s`); USA `epr-17952a.ic8` (= `mp17952a.s`); Europe `epr-17954a.ic8`
  (= `mp17954a.s`); Taiwan `epr-17953a.ic8` (= `mp17953a.ic8`). The games were run with `epr-20091.ic8`, Die Hard Arcade
  with `epr-17952a.ic8`; the others were only identified.
- Accepted, not verified (no dump to run): `epr-23603.ic8`, `epr-17740a.ic8`, `epr-17740.ic8` (MAME's `jp5`),
  `epr-17741a.ic8`, `epr-19854.ic8`, `epr-17742a.ic8`. Current MAME master fills the cartridge gaps with 0xFF and says
  `jp5` needs that, while this core fills them with 0x00 as MAME 0.289 does (see "Cartridge gaps" below), so `epr-17740`
  may not work.

A `.zip` the reader cannot parse (damaged, truncated, zip64) is refused as unreadable rather than tried as a Saturn disc,
and every ROM file's data is checked against the CRC-32 in the archive when the cartridge image is built. Load failures
of zip content are shown on screen through `RETRO_ENVIRONMENT_SET_MESSAGE` as well as logged. The disk control interface
refuses eject, insert and image changes while an ST-V game runs (image 0 answers with an empty path). File names are
handled as UTF-8, so non-ASCII zip names and folders work on Windows.

Controls follow MAME's input ports: directions, buttons 1-3 on every game, buttons 4-6 on Golden Axe: The Duel and Final
Fight Revenge (MAME's `stv6b`; Final Fight Revenge reads only buttons 1-5, as its input test shows in MAME and here), and
button 4 on Radiant Silvergun (MAME gives it the plain `stv` ports; the game itself reads only buttons 1-3, as its input
test shows). Coin, Start per player; Service and Test on port 1 only.

Steep Slope Sliders is a one-player game. Its set's default EEPROM (`sss.nv`, MAME's "preconfigured to 1 player")
holds the system assignment it needs: with the BIOS's own two-player assignment (a blank EEPROM) the BIOS refuses the
cartridge ("OPERATION OF THIS CARTRIDGE IS UNAVAILABLE WITH SYSTEM ASSIGNMENTS"), in MAME too. Its input test shows
player 1 with SW 1 and SW 2 only; player 2's coin is a credit, but player 2's Start does nothing, in MAME as here. Its
full case therefore checks that instead of a second player joining.

DaeJeon! SanJeon SuJeon's cartridge is scrambled: every byte is stored inverted with its bits shuffled. The core undoes
it at load with MAME's `init_sanjeon` transform (XOR 0xFF, then five bit swaps), over the whole cartridge image, gaps
included (so the SH-2 reads the gaps as 0xFF inside the 32 MiB image, as in MAME; past the image's end Brimir mirrors the
image where MAME reads 0xFF, as it does for every set since 0.10.0). libretro-stv's COMPATIBILITY.md lists the game as a 315-5838
game, but its own database gives it no chip and the same transform; MAME's transform alone runs the game here, to the
same screens as MAME's, so the set needs no protection chip.

Decathlete unpacks its graphics through the 315-5838 compression chip (`stv_315_5838.cpp`, from MAME's
`315-5838_317-0229_comp.cpp`). MAME 0.289 marks the game not working (its title comes up with wrong colours), so the
reference for this game is libretro-stv (Mednafen-based), which runs it: its trace of the chip's register writes and
data words is identical to Brimir's over the attract mode and the start of play (the first 766,813 accesses of a 7200
frame run; after that the two runs' games ask for different blocks as their timing drifts apart), and the screens match
at every compared frame (title, character select with two players, the 100-metre dash). The boot itself (to the SEGA
logo and the title fade) shows the same screens as MAME's at snapshots 50 frames apart (the SEGA logo at 1250, the title
fading in at 1850); the offset was not measured more finely. Only `decathlt` (V1.001) is supported; `decathlto` (V1.000) is not.

## Known differences from MAME

- **Die Hard Arcade, attract timing.** The attract mode runs 23 frames ahead of MAME's from the FBI screen to the
  logos (FBI screen 505 against 528, title close-up 1835 against 1858, white flash 2315 against 2338), frame for frame
  otherwise: the same captions in the same order. The whole lead is made at boot, by the BIOS's CD-block check (see
  "Boot length"). The 0.10.0 note of "one more caption, title about 340 frames later" was a misreading: single
  snapshots near the test's coin at frame 1850 caught the two runs 23 frames apart; MAME shows that caption as well.
- **Puyo Puyo Sun, game select.** The sprites of the game select screen appear a few frames after MAME's (by about frame
  1990 against 1960).
- **Guardian Force, title after a coin.** The title comes back 3 frames later than in MAME (black for 43 frames, MAME
  40), probably load or decompression time. The demo's player path drifts a little as well.
- **Other attract drift.** Cotton 2's scene changes are a few dozen frames off MAME's in either direction. DaeJeon!
  SanJeon SuJeon's intro matches MAME's at frames 2100-2200, but some scene cuts come earlier (by frame 2300 the kart
  race demo shows where MAME still shows the caption card before it, up to about 100 frames ahead); after Start the
  game is in step with MAME's.
- **315-5881 paths on real data.** Radiant Silvergun uses the chip once, for a boot self-check that decompresses one
  block (the compressed path). Final Fight Revenge reads only the uncompressed path: from the end of the BIOS check
  to the end of play, the same 16 longs of one uncompressed block (header 0x0001FFFF), over and over; no compressed
  block. Steep Slope Sliders also uses it once, at the end of the BIOS check: one compressed block (header 0x0002D787,
  136 lines of 216 bytes) read to its last line, 7344 longs, and no next header. All three games' values equal MAME's.
  No game here reads past a block's end into the next header, so the switch from one block to the next is checked on
  synthetic data only.
- **Steep Slope Sliders, the haze over the scenery.** Brimir draws the far scenery and the sky pale, as in a haze; MAME
  draws them dark (a black sky in daylight). The game turns on the VDP2 line colour screen for RBG0 and the sprites
  (LNCLEN 0x0030, colour calculation on for both), which MAME 0.289's VDP2 does not draw (`saturn_v.cpp` calls
  its line colour pass only under `TEST_FUNCTIONS`, defined 0, with a TODO). The VDP2 code is shared with the Saturn
  and not changed for ST-V; Brimir's picture is probably the more accurate one.
- **Final Fight Revenge, HUD on some stages.** On the Chinatown stage MAME draws the top health bars half transparent;
  Brimir draws them solid, as both draw them on the other stages seen. Not investigated (MAME marks the game's graphics
  imperfect; the VDP code is shared with the Saturn and not changed for ST-V).
- **Decathlete, transfer speed and attract timing (against libretro-stv).** Brimir finishes each of the game's
  315-5838 blocks inside one frame; libretro-stv spreads one over several (187,712 words over three frames at its
  1639-1641). The screens are the same; the loading pauses are shorter. libretro-stv also boots later than Brimir and
  MAME (its first chip read at frame 1335, Brimir's at 1195), so its attract mode runs about 100 frames behind;
  after the coin and Start the two are within about 25 frames.
- **SMPC settings (SMEM)** are kept as MAME keeps them: five bytes (`smpc_hle_device::m_smem[5]`, NVRAM `smem`, default
  all zero; four set by SETSMEM and a validity byte SETSMEM sets to 0xFF), saved in the tail of `<set>.srm`. The clock is
  not kept (MAME does not keep it either). The ST-V BIOS never sends SETSMEM and nothing visible depends on it with
  these games.
- **Slave SH-2 interrupts.** The slave receives VBlank-OUT (MAME's ST-V driver does not raise it for the slave), and the
  SCU's HBlank-OUT lower cancels an HBlank-IN interrupt the slave has not taken yet (MAME's `HOLD_LINE` keeps it
  pending). Details in `src/core/BRIMIR_FORK.md` (the `SetIRLHoldLine` row). No game shows an effect: with either
  changed alone, Die Hard Arcade's first 2350 frames are identical.
- **Boot length.** Brimir's BIOS check ends before MAME's. Measured on Die Hard Arcade: 23 frames ahead from the FBI
  screen on (the slave SH-2 starts at frame 495 against 521), and on Steep Slope Sliders: its 315-5881 is first read, at
  the end of the BIOS check, at frame 1164 against MAME's 1192 (28 frames). The other games were not measured one by
  one (Golden Axe: The Duel and Decathlete included; Decathlete's boot matches MAME's screens at 50-frame snapshots
  only); they run the same BIOS check, so they should boot about as far ahead, but the lead is not known for them. Cause: the BIOS (every dump here) looks
  for a CD block, polling CR1-CR4 at 0x25890018 for "\0CDBLOCK" up to 200,001 times behind a "SYSTEM CHECKING, PLEASE
  WAIT A FEW SECONDS..." screen. MAME maps nothing at A-bus CS2 (reads 0), so the poll runs out (about 32 frames with
  Die Hard Arcade); Brimir keeps the Saturn's CD block mapped in ST-V mode, it answers, and the check passes at once.
  Unmapping it brings the screen back, but with Brimir's bus and SH-2 timing the poll is slower and Die Hard Arcade's
  boot then ends 18 to 85 frames after MAME's (CS2 access 2 to 40 cycles; MAME runs the poll at about one cycle per
  instruction with no bus wait), so it stays mapped: matching MAME would mean changing shared timing. **Decided (the
  developer's ruling): ST-V mode keeps the Saturn's CD block mapped, and the shorter boot is a known difference from
  MAME and from real hardware, not a bug to fix.** A real ST-V has no CD block, so unmapping was considered and not
  done: with Brimir's timing it would lengthen every game's boot by 44-111 frames (the boots would end after MAME's, not
  before), the real wait length is unknown, so the result would be no more certain to be right, and it would move every
  `[.stv-rom]` schedule.
- **Cartridge gaps** are filled with 0x00, as MAME 0.289 (`ROMREGION_ERASE00`); current MAME master fills them with 0xFF
  and notes that the `jp5` BIOS (`epr-17740.ic8`) needs it. No game here reads a gap with the verified BIOS dumps.
  (DaeJeon! SanJeon SuJeon's gaps read 0xFF: its scramble turns the 0x00 fill into 0xFF, as in MAME 0.289.)
- **The IOGA's page.** Only the IOGA registers answer in the 64 KiB page at 0x00400000; the rest of the page reads 0 and
  ignores writes, as MAME's unmapped space. (The registers themselves are decoded over 0x00400000-0x0040007F; MAME maps
  0x00400000-0x0040003F.)

Not supported: ST-V games outside the table above (the game database holds the ROM file lists of these games only).

## Saves and states

- `<set>.srm` holds the backup RAM (32 KiB), the EEPROM (128 bytes) and the SMPC's five SMEM bytes, 32,901 bytes. A
  0.10.0 file (32,896 bytes, no SMEM) is accepted, its SMEM at MAME's default, and written in the new size at unload. A
  file of another size (a Saturn `.srm`, an older build's) is refused and logged; the cabinet then starts cold. The
  EEPROM starts as MAME's: the set's default image when its ROM_START has one (Shienryu's `eeprom-shienryu.bin`, which
  sets the cabinet up for the vertical monitor, and Steep Slope Sliders' `sss.nv`, its one-player assignment; both are
  part of the set), else blank (0xFF), and the BIOS sets it up on first boot.
- Save states carry an ST-V block (cabinet I/O and EEPROM state, the protection chips' state, scheduler count) after
  the Saturn payload. It is meant to make run-ahead, rewind and netplay exact; run-ahead and save/load were checked in
  RetroArch on Baku Baku Animal (and save/load on Decathlete), rewind and netplay were not (see "Frontend and platform
  checks"). The block is versioned: version 1 is 0.10.0's, version 2 adds the 315-5838's state; a version 1 block
  still loads (the chip at its reset value; unit tests, and a Baku Baku Animal state saved by a 0.10.0 build continued
  for 300 identical frames) and a block is always saved in version 2. A state from another game or another BIOS is
  refused.
- Saturn save states are byte-identical to master's.

## Frontend and platform checks

Run with RetroArch 1.22.2 (a scratch copy, never the developer's install) and the Android emulator; the tests above load
through `CoreWrapper` and cannot see frontend behaviour. The first eight games and Android were run for 0.10.0; the five
newer games were run for the release after it (Windows only).

Verified:
- All eight 0.10.0 games reach their attract mode in RetroArch on Windows, with the software and the Vulkan renderer
  (16 runs, clean exits, the BIOS chosen as in the table above).
- The five newer games (Shienryu, Final Fight Revenge, Steep Slope Sliders, DaeJeon! SanJeon SuJeon, Decathlete) reach
  their attract mode the same way (10 runs, software and Vulkan, screenshots at 30, 50 and 70 s, clean exits, no
  error or warning in the logs, a 32,901-byte `.srm` after each). Shienryu is upright and 3:4 with Screen Rotation at
  Auto; with the option at None the same run shows it lying on its side at 4:3 (what a user who ran 0.10.0 sees until
  they choose Auto).
- Decathlete, save state and load state in RetroArch (software renderer): a state taken on the title screen, then
  another coin and Start (the game moved on to its first event's screen), and the load brought the title back, with
  one credit (the credit line blinks with "PRESS START BUTTON" on this screen, so the second coin's count was not
  caught on a screenshot).
- Coin, Start and play through RetroArch's own input path (its network RetroPad), on Baku Baku Animal with both renderers:
  coin gives a credit, Start and B start the game.
- Baku Baku Animal: save state and load state (the credit count goes back), run-ahead in all three modes (single instance;
  second instance with the software renderer; second instance with the Vulkan renderer, where the core declines the second
  copy and RetroArch falls back to one instance), and `bakubaku.srm` of 32,896 bytes kept across restarts (credits and
  bookkeeping counters continue).
- Android: the arm64 and x86_64 cores pass the ELF checks (`check_elf`); Baku Baku Animal reaches its attract mode in the
  x86_64 emulator (Android 14) with the software and the Vulkan renderer, about 105 MB PSS; the `[stv]` tests pass in the
  emulator.
- Two defects found there and fixed: RetroArch could not load an ST-V zip at all (it tried to extract it; the core now sets
  `block_extract`), and every load raised a disc-index error notification (the disk control now answers image 0 with an
  empty path).

**Consequence of `block_extract` (Ruling R22):** RetroArch no longer unpacks any archive for this core, so a Saturn disc
image inside a `.7z` or `.zip` no longer loads (a zip was refused by the core already, and `.7z` was unpacked by
RetroArch before). libretro has no per-extension setting for it. Disc images in archives are unsupported: use CHD or
extract them. README and CHANGELOG say so.

Not verified, left for the developer's manual test:
- Rewind and netplay with an ST-V game (the state block is in the state and the stream is deterministic, but neither was
  run).
- A physical Android phone (the arm64 core passed the ELF checks only) and Android's 16 KB page images.
- Save states and run-ahead on the games other than Baku Baku Animal (and, for save states, Decathlete), and `.srm`
  persistence beyond the sizes written; rewind and netplay on any of them; the five newer games on Android.
- Sound and gamepad feel on a real pad; Golden Axe: The Duel's six buttons and two players through RetroArch (the six
  buttons are proven in the game's own input test through `CoreWrapper`); Radiant Silvergun and Die Hard Arcade by hand.
- RetroArch's frame-related network commands are ignored with single-instance run-ahead (a RetroArch limitation seen
  with Saturn and NES content too), so save states under single-instance run-ahead were not exercised through RetroArch.

## Running the tests

The unit tests with the `[stv]` tag need no ROMs and are part of the default suite. The tests that boot real games are
hidden (`[.stv-rom]`) and read the local folders from the environment; they SKIP when a folder or a zip is missing.
BIOS and ROM files are never in the repository, in CI or in a release.

```powershell
$env:BRIMIR_STV_BIOS_DIR = "F:\OneDrive\Roms\BIOS"           # the ST-V BIOS files (MAME names)
$env:BRIMIR_STV_ROM_DIR  = "F:\OneDrive\Roms\saturn\stv roms" # <set>.zip, MAME's non-merged or split sets
build\bin\brimir_tests.exe "[.stv-rom-quick]"                 # every game boots to the attract mode and takes a coin
build\bin\brimir_tests.exe "[.stv-rom][rsgun]"                # everything about one game (tag = the MAME set name)
build\bin\brimir_tests.exe "[.stv-rom]"                       # all of it
```

On the development PC (Release, MSVC) the quick tag takes about two minutes and the whole `[.stv-rom]` suite (43 cases)
about 14 minutes; under WSL with GCC 14 the whole suite took about 9 minutes and passed.

Each game has a tag with its set name; `[all-games]` is on the cases that walk the whole database. Set
`BRIMIR_STV_SCREENS_DIR` to a folder to get PNG screenshots of the key frames (`<folder>/<set>/`) for comparison with
MAME's. The cases are in `tests/unit/test_stv_games.cpp` (one `GameSession` per game, shared helpers for the coin, the
second player, the scripted minute and the input test), the ROM-set loading in `test_stv_loader.cpp` and
`test_stv_detect.cpp`, saves in `test_stv_sram.cpp`, states in `test_stv_state.cpp`.

## Traces against MAME

Test builds (`-DBRIMIR_BUILD_TESTS=ON`) and `-DBRIMIR_TRACE=ON` builds give `brimir_bench` two diagnostics (not in
release builds, see `tools/README.md`):

```powershell
brimir_bench --game "<set>.zip" --system-dir <scratch folder with the BIOS> --frames 1600 --warmup 0 `
             --trace-io brimir_io.txt                    # SMPC, SCU, IOGA and cartridge accesses of the master SH-2
brimir_bench ... --trace-msh2 pc.txt --trace-msh2-frames 1100-1200    # the master's PCs (about 2 MB per frame)
```

The `--trace-io` lines (`<frame> <PC> <R|W><size> <address> <value>`) have the fields MAME's debugger prints from a
watchpoint action, so an access sequence can be compared line by line with `mame <set> -bios jp1 -debug`. This is how
the 315-5881 accesses of Radiant Silvergun were shown identical (134 of 134), and how the phantom credits at boot were
traced to the sound RAM clear in the clock change.
