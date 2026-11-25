// Brimir - ST-V system wiring
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Turns a Saturn into an ST-V board and back: BIOS, game cartridge with its protection chip, the I/O board on the bus,
// the SMPC's ST-V mode and PDR handlers. Detach puts back everything Attach changed, so Saturn content loaded after
// ST-V content in the same core sees none of it.

#pragma once

#include "stv_game_db.hpp"
#include "stv_io.hpp"
#include "stv_state.hpp"

#include <ymir/hw/smpc/smpc_defs.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace ymir {
struct Saturn;
namespace cart {
class STVGameROMCartridge;
}
} // namespace ymir

namespace brimir::stv {

inline constexpr size_t kSTVBiosSize = 512 * 1024;

// The ST-V save RAM (the libretro .srm): the internal backup RAM's data bytes, then the cabinet's 93C46 cells, then the
// SMPC's battery memory as MAME keeps it (smpc_hle_device's m_smem[5], NVRAM "smem", DEFAULT_ALL_0): the four SMEM
// bytes SETSMEM writes, then MAME's validity byte (0xFF once SETSMEM has run, else 0; its bit 7 is the status flag
// INTBACK reports, Ymir's STE). The layout is told by its size: 0.10.0's .srm had no SMEM (kSTVSaveRAMSizeV1) and is
// still accepted, its SMEM then MAME's default (all zero).
inline constexpr size_t kSTVBackupRAMSize = 32 * 1024;
inline constexpr size_t kSTVEEPROMSize = 128;
inline constexpr size_t kSTVSMEMSize = 5;
inline constexpr size_t kSTVSaveRAMSizeV1 = kSTVBackupRAMSize + kSTVEEPROMSize;
inline constexpr size_t kSTVSaveRAMSize = kSTVSaveRAMSizeV1 + kSTVSMEMSize;

// The SMPC area code of the ST-V board, whatever BIOS region is chosen. It follows MAME: src/mame/sega/stv.cpp,
// stv_state::stv (machine config) calls m_smpc_hle->set_region_code(1) ("1 - Japan, configured via jumpers"), and only
// init_stv_us (the CD games, e.g. sfish2) overrides it with 4. All supported sets use init_stv or a wrapper of it
// (init_shienryu adds only DRC hints).
inline constexpr uint8_t kSTVAreaCode = 1;

class STVSystem {
public:
    STVSystem() = default;
    ~STVSystem();
    STVSystem(const STVSystem &) = delete;
    STVSystem &operator=(const STVSystem &) = delete;

    // Makes the Saturn an ST-V board running `game` from cartridge image `cart` (BuildCartImage's) with `bios` (as
    // LoadSTVBiosImage returns it), then hard-resets it and sets the SMPC area code. `eeprom` is the 93C46's contents
    // on a new cabinet (ReadDefaultEEPROM's 128 bytes); empty, every cell is 0xFFFF. The Saturn must outlive this
    // object or Detach must be called first. Returns false with a message in error when the system cannot be built;
    // the Saturn is then back as it was.
    bool Attach(ymir::Saturn &saturn, const GameInfo &game, std::array<uint8_t, kSTVBiosSize> &bios,
                std::span<const uint8_t> cart, uint8_t areaCode, std::string &error,
                std::span<const uint8_t> eeprom = {});

    // Removes the ST-V hooks and the cartridge and restores what Attach found: the page of the bus the I/O board took,
    // the SMPC's mode, handlers, area code and persistent data. The IPL stays the ST-V BIOS and the internal backup
    // RAM stays as the ST-V session left it: the owner reloads or replaces them. No-op when not attached.
    void Detach();

    bool IsAttached() const { return m_saturn != nullptr; }
    const GameInfo *Game() const { return m_game; }
    STVIOBoard &IO() { return m_io; }
    const STVIOBoard &IO() const { return m_io; }
    ymir::cart::STVGameROMCartridge *Cartridge() { return m_cart; }

    // Emulated time starts counting from now (after a save state load or a hard reset moved the scheduler).
    void ResyncTime();

    // The part of an ST-V save state that is not the Saturn's (stv_state.hpp): the game, the I/O board, the protection
    // chip and the time base. ValidateState is whether LoadState would take the block (it is for this game, and the
    // board and the chip can be in the states it holds); LoadState changes nothing when it returns false.
    void SaveState(STVStateBlock &block) const;
    bool ValidateState(const STVStateBlock &block) const;
    bool LoadState(const STVStateBlock &block);

private:
    // Advances the I/O board by the emulated time since the last call (the 93C46's write cycles).
    void AdvanceIOTime();

    ymir::Saturn *m_saturn = nullptr;
    const GameInfo *m_game = nullptr;
    ymir::cart::STVGameROMCartridge *m_cart = nullptr;
    STVIOBoard m_io;

    uint64_t m_lastCount = 0;
    double m_nsRemainder = 0.0; // the fraction of a nanosecond not passed on yet

    uint8_t m_prevAreaCode = 0;
    ymir::smpc::PersistentSMPCData m_prevSMPC{};
};

} // namespace brimir::stv
