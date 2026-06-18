// Brimir - ST-V cartridge implementation
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "stv_cartridge.hpp"

#include <algorithm>
#include <cstring>

namespace ymir::cart {

static size_t NextPow2(size_t v) {
    if (v == 0) return 0;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    if constexpr (sizeof(size_t) >= 8) {
        v |= v >> 32;
    }
    return v + 1;
}

void STVGameROMCartridge::LoadROM(std::span<const uint8> data) {
    m_rom.clear();
    m_romSize = 0;
    if (data.empty()) return;

    size_t allocSize = NextPow2(data.size());
    if (allocSize < data.size()) allocSize = data.size();

    m_rom.assign(allocSize, 0xFFu);
    std::memcpy(m_rom.data(), data.data(), data.size());
    m_romSize = allocSize;
}

uint8 STVGameROMCartridge::ReadByte(uint32 address) const {
    if (m_romSize == 0) return 0xFFu;
    if (address >= 0x2000000 && address <= 0x4FFFFFD) {
        return m_rom[address & (m_romSize - 1)];
    }
    return 0xFFu;
}

uint16 STVGameROMCartridge::ReadWord(uint32 address) const {
    if (m_romSize == 0) return 0xFFFFu;
    if (address >= 0x04FFFFFC && address < 0x05000000 && m_rsgThingy) {
        uint8 lo = (uint8)((((m_rsgCounter & 0x7F) << 1) + 1) & (0xF0 >> ((m_rsgCounter & 0x80) >> 5)));
        uint8 hi = (uint8)((((m_rsgCounter & 0x7F) << 1) + 0) & (0xF0 >> ((m_rsgCounter & 0x80) >> 5)));
        m_rsgCounter++;
        return (uint16)((hi << 8) | lo);
    }
    if (address >= 0x2000000 && address <= 0x4FFFFFC) {
        return util::ReadBE<uint16>(&m_rom[address & (m_romSize - 1) & ~1]);
    }
    return 0xFFFFu;
}

void STVGameROMCartridge::WriteByte(uint32 address, uint8 value) {
    if (address >= 0x04FFFFF0 && address < 0x05000000) {
        if (address == 0x04FFFFF0 || address == 0x04FFFFF1) {
            m_rsgThingy = value & 0x1;
            m_rsgCounter = 0;
        }
    }
}

void STVGameROMCartridge::WriteWord(uint32 address, uint16 value) {
    if (address >= 0x04FFFFF0 && address < 0x05000000) {
        if ((address & ~1) == 0x04FFFFF0) {
            m_rsgThingy = value & 0x1;
            m_rsgCounter = 0;
        }
    }
}

uint8 STVGameROMCartridge::PeekByte(uint32 address) const {
    if (m_romSize == 0) return 0xFFu;
    if (address >= 0x2000000 && address <= 0x4FFFFFD) {
        return m_rom[address & (m_romSize - 1)];
    }
    return 0xFFu;
}

uint16 STVGameROMCartridge::PeekWord(uint32 address) const {
    if (m_romSize == 0) return 0xFFFFu;
    if (address >= 0x2000000 && address <= 0x4FFFFFC) {
        return util::ReadBE<uint16>(&m_rom[address & (m_romSize - 1) & ~1]);
    }
    return 0xFFFFu;
}

void STVGameROMCartridge::PokeByte(uint32 address, uint8 value) {
    if (m_romSize == 0) return;
    if (address >= 0x2000000 && address <= 0x4FFFFFD) {
        m_rom[address & (m_romSize - 1)] = value;
    }
}

void STVGameROMCartridge::PokeWord(uint32 address, uint16 value) {
    if (m_romSize == 0) return;
    if (address >= 0x2000000 && address <= 0x4FFFFFC) {
        util::WriteBE<uint16>(&m_rom[address & (m_romSize - 1) & ~1], value);
    }
}

uint8 STVGameROMCartridge::PeekROMByte(uint32 address) const {
    if (m_romSize == 0) return 0xFFu;
    return m_rom[address & (m_romSize - 1)];
}

} // namespace ymir::cart
