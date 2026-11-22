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

// The SMPC area code of the board for a BIOS region (the board's region jumpers, which match its BIOS): the codes the
// Saturn uses for the same regions. MAME fixes the code at 1 (Japan) for the supported sets whatever the BIOS
// (stv_state::stv); a US-region init (init_stv_us, CD games only) uses 4.
uint8_t AreaCodeForBiosRegion(Region region);

class STVSystem {
public:
    STVSystem() = default;
    ~STVSystem();
    STVSystem(const STVSystem &) = delete;
    STVSystem &operator=(const STVSystem &) = delete;

    // Makes the Saturn an ST-V board running `game` from cartridge image `cart` (BuildCartImage's) with `bios` (as
    // LoadSTVBiosImage returns it), then hard-resets it and sets the SMPC area code. The Saturn must outlive this
    // object or Detach must be called first. Returns false with a message in error when the system cannot be built;
    // the Saturn is then back as it was.
    bool Attach(ymir::Saturn &saturn, const GameInfo &game, std::array<uint8_t, kSTVBiosSize> &bios,
                std::span<const uint8_t> cart, uint8_t areaCode, std::string &error);

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
