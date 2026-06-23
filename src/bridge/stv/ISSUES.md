# ST-V Arcade Support — Remaining Issues

Branch: `feature/stv-support`
Last updated: 2026-06-21 (session 3)

---

## Current Status

- ST-V BIOS boots and executes properly (processes SMPC commands, configures VDP).
- Cartridge ROM loads correctly at all expected address ranges (verified via bus peek).
- EEPROM initializes from ROM header.
- IOGA responds to BIOS reads (no more "Unhandled" bus messages).
- CKCHG352 sends NMI to master + stops slave (matching Kronos).
- **Blocker**: BIOS falls into CD block service menu loop at `0x060152BC`. Game never boots.
- Slave SH-2 stays at `0x20000200`; SSHON never issued.

---

## This Session's Changes (2026-06-21 session 3)

### 1. CKCHG352/CKCHG320 NMI + Slave Stop (`smpc.cpp:716-729`)

Kronos `SmpcCKCHG352` (`smpc.c:219-239`) stops the slave SH-2 and sends NMI to the master. Brimir previously only changed clock speed.

**Fix**: `CKCHG352` and `CKCHG320` now call `m_smpcOps.DisableSlaveSH2()` before the clock change and `m_smpcOps.RaiseNMI()` after.

### 2. IOGA Bus Mapping at Page 0x40 (`core_wrapper.cpp:548-556`)

IOGA address `0x0400000` maps to page 0x40, which falls in the **unmapped gap** between WRAMLow mirror (pages 0x30-0x3F) and cartridge CS0 (page 0x200+). The cartridge dispatch only covers pages 0x200-0x57F, so IOGA accesses never reached the cartridge handler.

Page 0x40 does NOT conflict with cartridge CS1 (page 0x400+).

**Fix**: Re-added `m_stvIO->MapMemory(m_saturn->mainBus)` in `LoadSTVGame`, registering IOGA directly on the bus at page 0x40. The bus "Unhandled" messages for IOGA addresses are now gone.

### 3. ST-V ROM Mapping Rewrite (`stv_loader.cpp`, `stv_game_db.hpp`, `stv_game_db.cpp`)

Comparison with Kronos `stv.c` revealed critical mapping differences:

| Aspect | Kronos | Brimir (before) | Brimir (after) |
|--------|--------|----------------|----------------|
| Header bytes | `HEADER_BLOB`: interleaved (`offset + 2*j`) | `STV_MAP_BYTE`: interleaved | `STV_MAP_HEADER`: interleaved |
| Program bytes | `GAME_BYTE_BLOB`: contiguous (`offset + j`) | N/A (missing) | `STV_MAP_BYTE`: contiguous |
| ROM fill value | `0x00` (via `T1MemoryInit`/calloc) | `0xFF` | `0x00` |
| Program code offsets | `0x0200000`, `0x0300000` | missing entirely | `0x0200000`, `0x0300000` |

**Key insight**: Kronos maps the same byte ROM file (`fpr17969.13` for Baku Baku) **three times**:
- Once at offset 0x1 as interleaved header (standard Sega header at standard location)
- Twice at offsets 0x0200000 and 0x0300000 as contiguous program code
Brimir was only mapping it once at offset 0x1, so no program code was available.

**Fixes**:
- Added `STV_MAP_HEADER` enum value for interleaved header mapping
- Changed `STV_MAP_BYTE` to contiguous (matching Kronos `GAME_BYTE_BLOB`)
- Updated all 5 byte-mapped games (Baku Baku, Die Hard, Golden Axe, Puyo Puyo, Columns 97) to use HEADER + 2x BYTE layout
- Changed ROM buffer fill from `0xFF` to `0x00` (matching Kronos)

### 4. Libretro Info Update (`resources/info/brimir_libretro.info`)

Added `zip` to `supported_extensions` to allow RetroArch to pass ST-V ROM zips directly to the core.

### 5. Test Infrastructure (`tests/unit/test_stv_boot.cpp`)

Created comprehensive ST-V boot test that:
- Loads ST-V game via `CoreWrapper::LoadSTVGame`
- Dumps cartridge CS0, BIOS vectors, CD block area, program code areas before frame 1
- Tracks master/slave SH-2 PCs across 180 frames
- Dumps CD block contents at frame 60 when BIOS transitions there

---

## Verified Non-Blockers (Kronos Comparison)

| Issue | Kronos | Brimir | Status |
|-------|--------|--------|--------|
| Cartridge ID | `0xFF` (with "I have no idea" comment) | `0xFF` | Not a blocker — same value |
| MSHON | Explicitly not implemented (no-op) | No-op with TODO comment | Not a blocker |
| VRAM clear in CKCHG352 | Comment exists but no clearing code | N/A | Not a blocker |
| `g_cpu_ready` semaphore | Kronos threading artifact | Not needed | Not a blocker |
| IOGA emulation | Minimal (peripheral port-G only) | Full IOGA at page 0x40 | Brimir is more complete |

---

## Confirmed Correct After Session 3

Cartridge data verified via bus peek:
- **CS0 at 0x02000000**: Interleaved header: `00 53, 00 45, 00 47...` = "SEGA..." (correct)
- **CS0 at 0x02200000**: Contiguous program code from byte file mirror
- **CS0 at 0x02400000**: 16LE word ROM data (`mpr17970.2`) loaded correctly
- **CS1 at 0x04000000**: All 0x00 (expected — no CS1 data for this game)
- **BIOS vectors at 0x00000000**: Reset PC=0x20000200, SP=0x06100000 (correct ST-V BIOS)
- **EEPROM init data at 0x00000F40**: readable (used for cart header init)

Boot sequence (confirmed via devlog):
```
Frame 1-6:  mPC in BIOS ROM area (0x00000D06-0x00000D18)  — BIOS init loop
Frame 7:    SMPC: SSHOFF, RESDISA, CKCHG352, MSHON         — clock change + NMI
Frame 7-30: mPC in BIOS ROM area                            — BIOS continues
Frame 60:   mPC=0x06024A0E                                  — jumps to WRAMHigh
Frame 90+:  mPC=0x060152BC (stuck)                          — service menu loop
```

The master SH-2 starts at reset vector 0x20000200 (cartridge CS0). The cartridge contains header text at that address (not executable code), causing an illegal instruction exception. The SH-2 exception handler routes to the BIOS boot code, which completes init but chooses the service menu path instead of booting the game.

---

## Remaining Suspects

### Suspect 1: BIOS Cartridge Validation
The BIOS reads cartridge header fields and makes a decision to boot the game or enter the service menu. Something about the hardware state makes the BIOS choose the service menu path.
- SMPC INTBACK OREG[8] (cartridge code): both Kronos and Brimir hardcode to 0x00
- Possible unemulated hardware register the BIOS checks

### Suspect 2: Deeper Hardware Emulation Differences
- SCU register readback values may differ from Kronos
- VDP1/VDP2 reset during CKCHG352 may differ: Kronos calls `Vdp1Reset()` + `Vdp2Reset()` separately; Brimir calls `VDP.Reset(false)`
- Interrupt controller timing/behavior during NMI delivery
- CD block status register values (BIOS runs in CD block area)

### Suspect 3: BIOS Needs SSHON But Some Condition Blocks It
The slave SH-2 stays at 0x20000200 throughout. SSHON is never issued. The BIOS may be waiting for a hardware condition that never occurs.

### Next Investigation Steps
1. Run Kronos with same BIOS/ROM and diff the SMPC command sequence after RESENAB
2. Add bus trace logging to compare SH-2 memory access patterns during boot
3. Check SCU register readback values (especially interrupt status/mask registers)
4. Try setting OREG[8] to different values to see if BIOS behavior changes
5. Check if CD block HLE code is interfering with ST-V mode (the BIOS jumps to CD block area)

---

## Files Modified This Session

| File | Change |
|------|--------|
| `src/core/src/ymir/hw/smpc/smpc.cpp` | CKCHG352/CKCHG320: added `DisableSlaveSH2()` + `RaiseNMI()` |
| `src/bridge/core_wrapper.cpp` | Added `m_stvIO->MapMemory()` back for IOGA at page 0x40 |
| `src/bridge/stv/stv_loader.cpp` | `STV_MAP_BYTE` → contiguous; added `STV_MAP_HEADER` handling; ROM fill → 0x00 |
| `src/bridge/stv/stv_game_db.hpp` | Added `STV_MAP_HEADER` enum value |
| `src/bridge/stv/stv_game_db.cpp` | Updated all 5 byte-mapped games to HEADER + 2x BYTE layout; fixed program code offsets |
| `resources/info/brimir_libretro.info` | Added `zip` to `supported_extensions` |
| `tests/unit/test_stv_boot.cpp` | New file: ST-V boot test with cartridge/BIOS diagnostics |
| `tests/CMakeLists.txt` | Added `test_stv_boot.cpp` |

---

## Build & Test

```bash
# Build tests (requires BRIMIR_BUILD_TESTS=ON)
cmake -S . -B build -DBRIMIR_BUILD_TESTS=ON
cmake --build build --config Release --target brimir_tests

# Run ST-V test
./build/bin/Release/brimir_tests.exe "[stv]"
```

Test result: `CHECK( pcLeftCDBlock )` → **FAILED** (master PC still in CD block area after 180 frames).
