# ST-V Arcade Support — Remaining Issues

Branch: `feature/stv-support`
Last updated: 2026-06-20 (session 2)

---

## Current Status

- ST-V BIOS boots and reaches factory/service menu.
- Game ROM loads, EEPROM initializes, cartridge inserted with protection config.
- **Blocker**: Game does not boot past BIOS factory menu. Display stays OFF.
- **Root cause identified for RSG**: 315-5881 decrypted data had reversed byte order (`ReadLong` missing byte-swap per word). FIXED.
- **New blocker**: IOGA bus mapping at `0x0400000` shadows cartridge CS1 page — 315-5881 command writes/reads never reach cartridge. FIXED (IOGA moved into cartridge dispatch).
- **Current state**: BIOS factory menu appears but game doesn't boot. `Unhandled 8-bit main bus read from 0400001` messages suggest the cartridge IOGA dispatch may not be reached by the bus.
- **Next investigation**: Verify why the bus fallback fires for page 0x40 instead of SCU cartridge handler.

---

## This Session's Changes (2026-06-20 session 2)

### 1. 315-5881 Byte-Swap Fix (`stv_cartridge.cpp:135-139`)
- `ReadLong` now byte-swaps each decrypted word via `bit::byte_swap<uint16>` before assembling the 32-bit result.
- Matches Kronos `ROMSTVCs1ReadLong` behavior (`cs0.c:1270-1273`).

### 2. Byte Write Interception Fix (`stv_cartridge.cpp`)
- `WriteByte` no longer intercepts 0x8/0xA/0xC for 315-5881.
- Kronos only intercepts 0x1 for byte writes; the rest fall through to ROM.

### 3. IOGA/CS1 Page Conflict Resolution
- **Problem**: `STVIOBoard::MapMemory` registered IOGA at `0x0400000-0x040007F` on the bus, overriding the SCU cartridge handler for page 0x40 (64KB page). 315-5881 command registers at `0x04000000-0x0400000F` were intercepted by IOGA instead of reaching the cartridge.
- **Fix**: Removed IOGA bus registration. IOGA now delegates through the cartridge mapper via `SetIOGADispatch` callbacks.
- Cartridge inserted at init time with IOGA dispatch; reloaded in `LoadSTVGame`.
- Added public `ReadIOGAByte`/`WriteIOGAByte` to `STVIOBoard`.
- `STVGameROMCartridge::ReadByte`/`ReadWord`/`WriteByte`/`WriteWord`/`PeekByte` check `IsIOGA` and delegate.
- Word reads from IOGA return `0xFF00 | data` (matching original IOGA mapper behavior).

### 4. Cartridge Long-Word Infrastructure
- Added `ReadLong`/`WriteLong`/`PeekLong`/`PokeLong` to `BaseCartridge` and `CartridgeSlot`.
- `STVGameROMCartridge` overrides all long-word ops; 315-5881 decrypted output only exposed on 32-bit CS1 reads at command 0xC.
- SCU `ReadCartridge`/`WriteCartridge` now use `m_cartSlot.ReadLong`/`WriteLong` instead of splitting into two 16-bit accesses.

### 5. CS1 Handler Rename
- `IsRSGWindow` → `IsCS1Window` throughout `stv_cartridge.cpp`.

---

## Remaining Issues (from previous session)

### Issue 1: BIOS Factory Menu — Game Not Booting
**Symptom**: BIOS reaches factory/service menu, game does not execute.
**Suspected causes**:
- Cartridge ID value (0xFF) may not match expected ST-V cartridge ID.
- SMPC CKCHG352 handling differs from Kronos (no VRAM clear, no `g_cpu_ready` semaphore, MSHON is no-op).
- Bus fallback firing for IOGA addresses (see new blocker below).

**New blocker**: `debug | Bus | Unhandled 8-bit main bus read from 0400001` — bus fallback at `saturn.cpp:62-65` fires instead of SCU cartridge handler for page 0x40.
**Need to verify**: Does the SCU `MapNormal` for `0x2000000-0x57FFFFF` correctly register page 0x40? Is an earlier mapper (e.g., `MemoryMap::MapMemory` calling `MapArray`) shadowing the page?

### Issue 2: `PeekROMByte` Endianness
(same as before — `stv_cartridge.cpp:242`)

### Issue 3-17: Same as previous session
(RSG counter, EEPROM CRC, control scheme, save states, etc.)

---

## Files Modified This Session

| File | Change |
|------|--------|
| `src/core/include/ymir/hw/cart/cart_base.hpp` | Added `ReadLong`/`WriteLong`/`PeekLong`/`PokeLong` virtuals |
| `src/core/include/ymir/hw/cart/cart_slot.hpp` | Added `ReadLong`/`WriteLong` templates |
| `src/core/src/ymir/hw/scu/scu.cpp` | `ReadCartridge`/`WriteCartridge` use slot long-word ops |
| `src/bridge/stv/stv_cartridge.hpp` | Added `SetIOGADispatch`, IOGA fn types, `m_iogaRead`/`m_iogaWrite` members, long-word overrides |
| `src/bridge/stv/stv_cartridge.cpp` | IOGA dispatch in all accessors, 315-5881 byte-swap fix, `WriteByte` 0x8/0xA/0xC removal, `IsCS1Window` rename |
| `src/bridge/stv/stv_io.hpp` | Added public `ReadIOGAByte`/`WriteIOGAByte` |
| `src/bridge/core_wrapper.cpp` | Removed `m_stvIO->MapMemory`, added init-time cartridge with IOGA dispatch, `LoadSTVGame` resets IOGA dispatch |
| `include/brimir/core_wrapper.hpp` | Added `m_stvCartridge` member, forward decl for `STVGameROMCartridge` |
