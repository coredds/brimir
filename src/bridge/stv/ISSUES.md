# ST-V Arcade Support — Remaining Issues

Branch: `feature/stv-support`
Last updated: 2026-06-18

---

## Issue 1: Black Screen (no visible graphics)

**Symptom**: Radiant Silvergun boots successfully (no crash, 600+ frames at 320x224), but the framebuffer is all black (0 non-zero pixels in first 1000). Performance: `Ymir_RunFrame avg=0.07ms` — CPU is barely executing, suggesting sleep/wait loop.

**Hypothesis**: The ST-V BIOS jumps to cartridge code (reset vector `0x00200002`), but the game never reaches its main loop. Either:
- RSG encryption returns wrong data → game reads garbage → VDP not configured
- EEPROM settings incorrect → game fails boot check
- SMPC area code mismatch → BIOS hangs before cartridge jump
- CD block status causes BIOS to wait for non-existent drive

**Next steps**:
1. Verify RSG is actually triggered: add `fprintf` to `WriteWord` (0x04FFFFF0) and `ReadWord` (0x04FFFFFC) to confirm game accesses RSG registers
2. Test with an unencrypted ST-V game (e.g. Die Hard Arcade, Baku Baku Animal) if ROMs available
3. Dump first 64 bytes of framebuffer to check if pixels exist but are dark
4. Add SH-2 PC sampling to see where CPU spends time
5. Try different ST-V BIOS region (epr-17952a.ic8, epr-17954a.ic8)
6. Compare EEPROM bytes with Mednafen's init for Radiant Silvergun

---

## Issue 2: `PeekROMByte` Endianness

**File**: `stv_cartridge.cpp:112`
```cpp
return m_rom[address & (m_romSize - 1)];
```

The ROM buffer stores data in big-endian (byte-swapped from LE source). `PeekROMByte(0xF40)` returns `m_rom[0xF40]` — the correct BE byte at that offset. In Mednafen on LE, `PeekROMByte` does `ROM[A ^ 1]` because ROM is stored as `uint16_t[]`. Need to verify our byte-addressable storage matches the expected byte ordering for EEPROM init. If wrong, the game ID and cab type bytes read from ROM will be wrong, causing incorrect EEPROM setup.

**To verify**: Dump `m_rom[0xF40-0xF4F]` and compare with known Radiant Silvergun ROM header bytes.

---

## Issue 3: RSG Counter Behavior Under 32-bit Access

The SCU splits 32-bit A-Bus reads into two 16-bit reads. For a 32-bit read at `0x04FFFFFC`:
- Read 1: `ReadWord(0x04FFFFFC)` → RSG triggers, counter increments
- Read 2: `ReadWord(0x04FFFFFE)` → goes to SCU cartridge ID handler (NOT the cartridge)

RSG counter increments only ONCE per 32-bit access. Verify this matches real hardware. In MAME's ST-V driver, 32-bit reads expect two RSG values. If the game expects two RSG values per 32-bit read instead of one, decryption will fail.

**To verify**: Trace 32-bit reads at `0x04FFFFFC` in Mednafen and compare with our behavior.

---

## Issue 4: RSG Nybble Mask Alternation

The RSG mask alternates every 128 reads:
- Counter 0-127: mask = `0xF0F0` (upper nybbles)
- Counter 128-255: mask = `0x0F0F` (lower nybbles)

Output values cycle through `0x0000, 0x1010, 0x2020, ..., 0xF0F0` (counter 0-127), then `0x0000, 0x0101, 0x0202, ..., 0x0F0F` (counter 128-255). The game uses this sequence as a decryption key. If any step is wrong (initial counter value, increment timing, mask), decryption fails.

**To verify**: Log RSG reads and compares with Mednafen output for the same game.

---

## Issue 5: RSG Activation via Different Write Paths

RSG is activated by writing to `0x04FFFFF0`. Possible write paths:
- 16-bit word write → `WriteWord`
- 8-bit byte write → `WriteByte`
- 32-bit long write → split into two 16-bit writes by SCU
- DMA write to A-Bus → goes through cartridge handlers

Our implementation handles all paths, but verify DMA writes reach the cartridge. In Ymir's SCU, DMA writes to A-Bus addresses go through the bus handlers, which delegate to cartridge. **To verify**: Add logging to all write paths.

---

## Issue 6: EEPROM CRC-16 Validation

The EEPROM has a CRC-16 at offset `0x08-0x09` (computed over `0x0C-0x41`). We set it to `0x0000` (placeholder). If the game validates this CRC, it will fail and the game might refuse to boot or show an error.

**To fix**: Compute CRC-16-CCITT (poly `0x1021`, initial `0x5A81`) over EEPROM bytes `0x0C-0x3F`, XOR with bytes at `0x42-0x43`, store at `0x08-0x09`. Copy `0x08-0x3F` to `0x44-0x7B` (mirrored EEPROM convention).

---

## Issue 7: Control Scheme RSG

Radiant Silvergun uses `STV_CONTROL_RSG` control scheme. Our `UpdateInputs` uses the default 3-button scheme. The RSG scheme might need different button mapping in `m_dataIn[]`. In Mednafen, the RSG scheme handles SW1-SW3 differently:

```c
if(ControlScheme == STV_CONTROL_RSG) {
    if(tmp & 0x1) DataIn[i] &= ~0x3;
    if(tmp & 0x2) DataIn[i] &= ~0x5;
    if(tmp & 0x4) DataIn[i] &= ~0x6;
    if(tmp & 0x8) DataIn[i] &= ~0x7;
}
```

Our `UpdateInputs` doesn't implement this. The game may not receive correct inputs, but this shouldn't cause a black screen (attract mode should show regardless).

---

## Issue 8: EEPROM Slot-Based Access

Currently EEPROM is a flat 128-byte array initialized once at boot. Mednafen uses AK93C45 serial EEPROM emulation with SMPC port shim. The EEPROM is accessed through SMPC port control bits (ECS, EDI, EDO) via the `IODevice_STVSMPC` interface. Some games may read EEPROM during gameplay (not just at boot). If the game needs to read/write EEPROM through the SMPC port, our flat array won't work.

**To fix**: Implement AK93C45 serial protocol and wire through SMPC as an `IODevice`. Reference: Mednafen `ak93c45.c` + `stvio.c` IODevice_STVSMPC.

---

## Issue 9: Multi-file ROMs with Identical Filenames

Some ROM layout entries re-use the same filename at different offsets (e.g. `epr17766.13` appears twice at `0x0000001` and `0x0100001`). In Mednafen, the loader detects `prev_match` and copies from the previous slot instead of re-reading the file. Our loader reads the file again (which works but is inefficient). If the file sizes differ from the layout spec, we might load wrong data.

**To fix**: Detect duplicate filenames and copy from previously loaded data.

---

## Issue 10: `romtwiddle` (Sanjeon) Not Implemented

The "DaeJeon! SanJeon SuJeon" (Final Fight Revenge Korean variant) game uses `STV_ROMTWIDDLE_SANJEON` — a bit-permutation applied to the entire ROM after loading. Not implemented. Affects only this one game.

---

## Issue 11: 315-5881 / 315-5838 Encryption Chips

Many ST-V games use Sega 315-5881 encryption (Challenge-response at `0x04FFFFF0-0x04FFFFFF`). Requires:
- 315-5881 state machine (`stv_5881.c` in Mednafen, ~250 lines)
- Per-game crypt key (e.g. Final Fight Revenge: `0x0524AC01`)
- 315-5838 for Decathlete (separate compression chip)

---

## Issue 12: Screen Rotation (Tate Mode)

EEPROM bit 12 controls screen rotation for vertical shooters. Radiant Silvergun's database entry has `rotate=false`, but the EEPROM init might override this. If rotation is applied, the game's vertical orientation needs 90° rotation in the framebuffer output.

---

## Issue 13: Save State Support

ST-V state (STVGameROMCartridge, STVIOBoard, EEPROM) is not serialized in save states. `retro_serialize`/`retro_unserialize` will lose all ST-V state.

---

## Issue 14: Per-game `.stveep` Persistence

EEPROM state should be saved to/loaded from `system_directory/gamename.stveep` (128 bytes) to persist cabinet settings across sessions. Currently EEPROM is re-initialized every boot from ROM header.

---

## Issue 15: Performance (0.07ms/frame)

If the game IS running but the CPU is mostly idle, this is a symptom of the BIOS/game being stuck in a wait loop. This will resolve once the black screen issue (Issue 1) is fixed — the actual game code will execute millions of cycles per frame.

---

## Issue 16: RetroArch ZIP Extraction

RetroArch intercepts `.zip` files and tries to extract them before passing to the core. Our core handles zips internally but `.zip` had to be removed from `supported_extensions` because RetroArch's VFS extraction fails on multi-file MAME ROM sets. Users must extract zips manually. Long-term: investigate `RETRO_ENVIRONMENT_GET_VFS_INTERFACE` to bypass RetroArch's extraction.

---

## Issue 17: Dev Log Cleanup

Debug `fprintf` calls in cartridge code were removed. The `retro_run` frame counter, framebuffer checker, and "Applying core options" logs should also be cleaned up in `libretro.cpp` before merging to main.

---

## Architecture Notes

- All ST-V code lives in `src/bridge/stv/` — **zero changes to Ymir upstream** (`src/core/`)
- `STVGameROMCartridge` reuses `CartType::ROM` (no new enum value needed)
- `STVIOBoard` maps onto public `mainBus` via `MapNormal` with range-guarded IPL fallback
- ZIP reader uses manual zlib linkage to avoid DLL import conflict with `zlibstatic.lib`
- Game database format matches Mednafen's `db_stv.h` for easy porting of remaining entries
