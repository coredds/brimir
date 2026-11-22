// Brimir - ST-V cartridge emulation (Sega Titan Video arcade board)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// The game's ROM board in the Saturn's cartridge slot: the cartridge image on the A-Bus (CS0 from 0x02000000; CS1 from
// 0x04000000 shows image offsets 0x02000000-0x02FFFFFF, every 16 MiB), and on Radiant Silvergun the 315-5881 registers
// at 0x04FFFFF0. It owns the cartridge space (BaseCartridge::OwnsCartridgeSpace) and reports CartType::ROM. The IOGA is
// not here: it is on the main bus (STVIOBoard).

#pragma once

#include <ymir/hw/cart/cart_base.hpp>
#include <ymir/util/data_ops.hpp>
#include <ymir/util/size_ops.hpp>

#include "stv_315_5881.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ymir::cart {

inline constexpr size_t kSTVGameROMMaxSize = 48_MiB;

class STVGameROMCartridge final : public BaseCartridge {
public:
    enum class ProtectionMode : uint8 {
        None = 0,
        Chip3155881,
    };

    // The protection hardware's runtime state, for ST-V save states (the ROM and the configuration are not part of it).
    // No padding: every byte is a member.
    struct ProtectionState {
        STV3155881::State crypt;
        uint8 protectionEnabled;
        uint8 reserved[3];
        std::array<uint32, 4> abus; // the four long registers at 0x4FFFFF0 (MAME's m_a_bus)
    };

    STVGameROMCartridge()
        : BaseCartridge(0xFFu, CartType::ROM) {
        SetOwnsCartridgeSpace(true);
    }

    uint8  ReadByte(uint32 address) const override;
    uint16 ReadWord(uint32 address) const override;
    uint32 ReadLong(uint32 address) const override;

    void WriteByte(uint32 address, uint8 value) override;
    void WriteWord(uint32 address, uint16 value) override;
    void WriteLong(uint32 address, uint32 value) override;

    uint8  PeekByte(uint32 address) const override;
    uint16 PeekWord(uint32 address) const override;
    uint32 PeekLong(uint32 address) const override;

    void PokeByte(uint32 address, uint8 value) override;
    void PokeWord(uint32 address, uint16 value) override;
    void PokeLong(uint32 address, uint32 value) override;

    void LoadROM(std::span<const uint8> data);
    void ConfigureProtection(ProtectionMode mode, uint32 key);

    void SaveProtectionState(ProtectionState &state) const;
    // Whether LoadProtectionState would take the state
    bool ValidateProtectionState(const ProtectionState &state) const;
    // Returns false and leaves the cartridge untouched when the state holds values the chip cannot be in.
    bool LoadProtectionState(const ProtectionState &state);

    size_t GetROMSize() const { return m_romSize; }

protected:
    // The 315-5881 registers at 0x4FFFFF0-0x4FFFFFF as MAME's stv_state::common_prot_r/common_prot_w decode them:
    // long registers, byte and word accesses select a lane of the long.
    uint32 ReadProtectionRegister(unsigned index, bool peek) const;
    void WriteProtectionRegister(unsigned index, uint32 data, uint32 mask);
    uint32 ROMLongAt(uint32 address) const;

    size_t m_romSize = 0;
    std::vector<uint8> m_rom;

    ProtectionMode m_protectionMode    = ProtectionMode::None;
    bool           m_protectionEnabled = false;
    std::array<uint32, 4> m_abus{}; // MAME stv_state::m_a_bus; m_abus[0] bit 16 is the chip enable (m_abus_protenable)

    mutable STV3155881 m_crypt3155881;
};

} // namespace ymir::cart
