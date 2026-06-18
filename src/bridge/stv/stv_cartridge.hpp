// Brimir - ST-V cartridge emulation (Sega Titan Video arcade board)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Reuses Ymir's CartType::ROM since ST-V games are read-only ROM cartridges.
// The Saturn SCU treats this as a standard ROM cart on the A-Bus CS0/CS1.

#pragma once

#include <ymir/hw/cart/cart_base.hpp>
#include <ymir/util/data_ops.hpp>
#include <ymir/util/size_ops.hpp>

#include <vector>

namespace ymir::cart {

inline constexpr size_t kSTVGameROMMaxSize = 48_MiB;

class STVGameROMCartridge final : public BaseCartridge {
public:
    STVGameROMCartridge()
        : BaseCartridge(0xFFu, CartType::ROM) {}

    uint8  ReadByte(uint32 address) const override;
    uint16 ReadWord(uint32 address) const override;

    void WriteByte(uint32 address, uint8 value) override;
    void WriteWord(uint32 address, uint16 value) override;

    uint8  PeekByte(uint32 address) const override;
    uint16 PeekWord(uint32 address) const override;

    void PokeByte(uint32 address, uint8 value) override;
    void PokeWord(uint32 address, uint16 value) override;

    void LoadROM(std::span<const uint8> data);

    size_t GetROMSize() const { return m_romSize; }

    uint8 PeekROMByte(uint32 address) const;

protected:
    size_t m_romSize = 0;
    std::vector<uint8> m_rom;

    mutable bool  m_rsgThingy  = false;
    mutable uint8 m_rsgCounter = 0;
};

} // namespace ymir::cart
