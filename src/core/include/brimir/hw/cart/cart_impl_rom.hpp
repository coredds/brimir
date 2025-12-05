#pragma once

#include "rom_cart_defs.hpp"

#include "cart_base.hpp"

#include <brimir/util/data_ops.hpp>

namespace brimir::cart {

class ROMCartridge final : public BaseCartridge {
public:
    ROMCartridge()
        : BaseCartridge(0xFFu, CartType::ROM) {}

    uint8 ReadByte(uint32 address) const override {
        if (util::AddressInRange<0x200'0000, 0x3FF'FFFF>(address)) {
            return m_rom[address & (kROMCartSize - 1)];
        } else {
            return 0xFFu;
        }
    }
    uint16 ReadWord(uint32 address) const override {
        if (util::AddressInRange<0x200'0000, 0x3FF'FFFF>(address)) {
            return util::ReadBE<uint16>(&m_rom[address & (kROMCartSize - 1) & ~1]);
        } else {
            return 0xFFFFu;
        }
    }

    void WriteByte(uint32 address, uint8 value) override {}
    void WriteWord(uint32 address, uint16 value) override {}

    uint8 PeekByte(uint32 address) const override {
        if (util::AddressInRange<0x200'0000, 0x3FF'FFFF>(address)) {
            return m_rom[address & (kROMCartSize - 1)];
        } else {
            return 0xFFu;
        }
    }
    uint16 PeekWord(uint32 address) const override {
        if (util::AddressInRange<0x200'0000, 0x3FF'FFFF>(address)) {
            return util::ReadBE<uint16>(&m_rom[address & (kROMCartSize - 1) & ~1]);
        } else {
            return 0xFFFFu;
        }
    }

    void PokeByte(uint32 address, uint8 value) override {
        if (util::AddressInRange<0x200'0000, 0x3FF'FFFF>(address)) {
            m_rom[address & (kROMCartSize - 1)] = value;
        }
    }
    void PokeWord(uint32 address, uint16 value) override {
        if (util::AddressInRange<0x200'0000, 0x3FF'FFFF>(address)) {
            util::WriteBE<uint16>(&m_rom[address & (kROMCartSize - 1) & ~1], value);
        }
    }

    void LoadROM(std::span<const uint8> out) {
        const size_t size = std::min(out.size(), kROMCartSize);
        std::copy_n(out.begin(), size, m_rom.begin());
    }

    void DumpROM(std::span<uint8, kROMCartSize> out) const {
        std::copy(m_rom.begin(), m_rom.end(), out.begin());
    }

protected:
    std::array<uint8, kROMCartSize> m_rom;
};

} // namespace brimir::cart
