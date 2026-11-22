# ST-V support: status

Eight arcade games run on the ST-V board emulation in this folder. This file says what was checked, how, and where the
behaviour differs from MAME (the reference). The design is in `docs/superpowers/specs/2026-11-21-stv-support-design.md`
(not tracked); the changes to `src/core/` are listed in `src/core/BRIMIR_FORK.md`.

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
| `gaxeduel` | Golden Axe: The Duel | JUETL | 6 buttons | Japan | full case; boot timing equal to MAME frame for frame; all six buttons of both players proven in the game's own input test |
| `cotton2` | Cotton 2 | JUET | 3 buttons | Japan | full case with continues; visuals match MAME's by eye |
| `grdforce` | Guardian Force | JUET | 3 buttons | Japan | full case with continues; credits (including continues) within a frame of MAME |
| `rsgun` | Radiant Silvergun | JUET | 3 buttons + a 4th (see below) | Japan | full case with continues; the 315-5881 chip's 134 accesses equal to MAME's, the 128 decrypted longs checked in a test |

The BIOS is chosen from the game's regions in the order Japan, USA, Europe (then the game's other regions), or by the
Saturn BIOS core option's region (`jp_*`, `us_*`, `eu_*`) when the game has that region. A game whose regions have no
BIOS in the system folder fails to load with a message that names the accepted file names. The accepted names are MAME's
`stvbios` set and the older MAME names of the same dumps (`stv_bios.cpp`).

Controls follow MAME's input ports: directions, buttons 1-3 on every game, buttons 4-6 on Golden Axe: The Duel, and
button 4 on Radiant Silvergun (MAME gives it the plain `stv` ports; the game itself reads only buttons 1-3, as its input
test shows). Coin, Start per player; Service and Test on port 1 only.

## Known differences from MAME

- **Die Hard Arcade, attract pacing.** The intro shows one more caption than MAME and the title comes about 340 frames
  later. In-game timing after Start lines up with MAME's. The cause was not investigated (the captions may follow the
  voice playback, so SCSP timing is a suspect).
- **Puyo Puyo Sun, game select.** The sprites of the game select screen appear a few frames after MAME's (by about frame
  1990 against 1960).
- **Guardian Force, title after a coin.** The title comes back 3 frames later than in MAME (black for 43 frames, MAME
  40), probably load or decompression time. The demo's player path drifts a little as well.
- **Other attract drift.** Cotton 2's scene changes are a few dozen frames off MAME's in either direction.
- **315-5881 uncompressed path.** Radiant Silvergun uses the chip once, for a boot self-check that decompresses one
  block, so only the compressed path was checked on real data. The uncompressed path (a block header without the
  compression flag) is the port of MAME's code and was exercised on noise ROMs in unit tests only.
- **SMPC settings (SMEM) are not persisted.** MAME keeps the SMPC's five bytes of battery memory for ST-V
  (`smem.nv`); here they start from MAME's defaults at every load. The ST-V BIOS never sends SETSMEM and nothing visible
  depends on it with these games.
- **Slave SH-2 interrupts.** The slave receives VBlank-OUT (MAME's ST-V driver does not raise it for the slave), and the
  SCU's HBlank-OUT lower cancels an HBlank-IN interrupt the slave has not taken yet (MAME's `HOLD_LINE` keeps it
  pending). Details in `src/core/BRIMIR_FORK.md` (the `SetIRLHoldLine` row). No game shows an effect.
- **Boot length.** Brimir's BIOS check ends about 30 frames before MAME's (about frame 1130-1165 against 1191).
- **Cartridge gaps** are filled with 0x00, as MAME 0.289 (`ROMREGION_ERASE00`); later MAME versions went back to 0xFF for
  some sets. No game here reads a gap.

Not supported: ST-V games outside the eight above (the game database holds the ROM file lists of these eight only).

## Saves and states

- `<set>.srm` holds the backup RAM (32 KiB) and the EEPROM (128 bytes), 32,896 bytes. A file of another size (a Saturn
  `.srm`, an older build's) is refused and logged; the cabinet then starts cold. The EEPROM starts blank (0xFF), as MAME's
  (none of these sets has a default image); the BIOS sets it up on first boot.
- Save states carry an ST-V block (cabinet I/O and EEPROM state, protection chip state, scheduler count) after the
  Saturn payload, so run-ahead, rewind and netplay work. The block is not versioned for compatibility yet: the format is
  unreleased. A state from another game or another BIOS is refused.
- Saturn save states are byte-identical to master's.

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

On the development PC (Release, MSVC) the quick tag takes about 100 seconds and the whole `[.stv-rom]` suite (28 cases)
about 11 minutes; under WSL with GCC 14 the whole suite took about 9 minutes and passed.

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
