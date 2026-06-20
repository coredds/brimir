// Brimir - ST-V cartridge implementation
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "stv_cartridge.hpp"

#include <algorithm>
#include <cstring>

namespace ymir::cart {

static constexpr uint32 kCartCS0Base = 0x02000000u;
static constexpr uint32 kCartCS1Base = 0x04000000u;
static constexpr uint32 kCartCS2Base = 0x05800000u;
static constexpr uint32 kCartROMSize = 0x03000000u; // 48MB effective ROM space

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
    m_crypt3155881.SetROM(std::span<const uint8>(m_rom.data(), m_romSize));
    m_crypt3155881.Reset();
}

void STVGameROMCartridge::ConfigureProtection(ProtectionMode mode, uint32 key) {
    m_protectionMode = mode;
    m_protectionKey = key;
    m_protectionEnabled = false;

    m_rsgThingy = false;
    m_rsgCounter = 0;

    m_crypt3155881.Reset();
    m_crypt3155881.SetKey(key);
    m_crypt3155881.SetEnabled(false);
}

void STVGameROMCartridge::SetIOGADispatch(IOGAReadFn readFn, IOGAWriteFn writeFn) {
    m_iogaRead  = std::move(readFn);
    m_iogaWrite = std::move(writeFn);
}

// Resolves ST-V cartridge bus addresses to ROM offsets.
// CS0 maps directly; CS1 mirrors every 16MB into ROM[0x02000000..0x02FFFFFF].
static inline size_t CartOffset(uint32 address, size_t romSize) {
    uint32 romOff;

    if (address >= kCartCS0Base && address < kCartCS1Base) {
        romOff = address - kCartCS0Base;
    } else if (address >= kCartCS1Base && address < kCartCS2Base) {
        romOff = 0x02000000u + ((address - kCartCS1Base) & 0x00FFFFFFu);
    } else {
        return (size_t)-1;
    }

    if (romOff >= kCartROMSize) return (size_t)-1;
    return romOff & (romSize - 1);
}

static constexpr uint32 kIOGABase    = kCartCS1Base;
static constexpr uint32 kIOGAEnd     = kCartCS1Base | 0x7Fu;

static inline bool IsCS1Window(uint32 address) {
    return address >= kCartCS1Base && address < kCartCS2Base;
}

static inline bool IsIOGA(uint32 address) {
    return address >= kIOGABase && address <= kIOGAEnd;
}

uint8 STVGameROMCartridge::ReadByte(uint32 address) const {
    if (IsIOGA(address) && m_iogaRead) {
        return m_iogaRead(address);
    }
    if (m_romSize == 0) return 0xFFu;
    size_t idx = CartOffset(address, m_romSize);
    if (idx == (size_t)-1) return 0xFFu;
    return m_rom[idx];
}

uint16 STVGameROMCartridge::ReadWord(uint32 address) const {
    if (IsIOGA(address) && m_iogaRead) {
        const uint8 data = m_iogaRead(address & ~1u);
        return static_cast<uint16>(0xFF00u | data);
    }
    if (m_romSize == 0) return 0xFFFFu;

    const uint32 reg = address & 0xFu;

    if (IsCS1Window(address)) {
        if (m_protectionMode == ProtectionMode::RSG && m_rsgThingy && (reg == 0xCu || reg == 0xEu)) {
            uint8 lo = (uint8)((((m_rsgCounter & 0x7F) << 1) + 1) & (0xF0 >> ((m_rsgCounter & 0x80) >> 5)));
            uint8 hi = (uint8)((((m_rsgCounter & 0x7F) << 1) + 0) & (0xF0 >> ((m_rsgCounter & 0x80) >> 5)));
            uint16 val = (uint16)((hi << 8) | lo);
            m_rsgCounter++;
            return val;
        }

        // 315-5881 decrypted output is exposed on 32-bit CS1 reads at command 0xC.
        // Word reads return normal ROM data.
    }

    size_t idx = CartOffset(address, m_romSize);
    if (idx == (size_t)-1) return 0xFFFFu;
    return util::ReadBE<uint16>(&m_rom[idx & ~1u]);
}

uint32 STVGameROMCartridge::ReadLong(uint32 address) const {
    if (m_romSize == 0) return 0xFFFFFFFFu;

    if (IsCS1Window(address) && m_protectionMode == ProtectionMode::Chip3155881 && m_protectionEnabled) {
        const uint32 reg = address & 0xFu;
        if (reg == 0xCu) {
            uint32 hi = m_crypt3155881.ReadDecryptedWord();
            hi = bit::byte_swap<uint16>(static_cast<uint16>(hi));
            uint32 lo = m_crypt3155881.ReadDecryptedWord();
            lo = bit::byte_swap<uint16>(static_cast<uint16>(lo));
            return (hi << 16u) | lo;
        }
    }

    const uint32 hi = ReadWord(address + 0u);
    const uint32 lo = ReadWord(address + 2u);
    return (hi << 16u) | lo;
}

void STVGameROMCartridge::WriteByte(uint32 address, uint8 value) {
    const uint32 reg = address & 0xFu;

    if (m_protectionMode == ProtectionMode::RSG) {
        if (IsCS1Window(address) && (reg == 0x0u || reg == 0x1u)) {
            m_rsgThingy = value & 0x1;
            m_rsgCounter = 0;
            return;
        }
    }

    if (m_protectionMode == ProtectionMode::Chip3155881) {
        if (IsCS1Window(address) && reg == 0x1u) {
            m_protectionEnabled = (value & 0x1u) != 0;
            m_crypt3155881.SetEnabled(m_protectionEnabled);
            return;
        }
    }

    if (IsIOGA(address) && m_iogaWrite) {
        m_iogaWrite(address, value);
    }
}

void STVGameROMCartridge::WriteWord(uint32 address, uint16 value) {
    const uint32 reg = address & 0xFu;

    if (m_protectionMode == ProtectionMode::RSG) {
        if (IsCS1Window(address) && (reg == 0x0u || reg == 0x1u)) {
            m_rsgThingy = value & 0x1;
            m_rsgCounter = 0;
            return;
        }
    }

    if (m_protectionMode == ProtectionMode::Chip3155881) {
        if (IsCS1Window(address)) {
            if (reg == 0x1u) {
                m_protectionEnabled = (value & 0x1u) != 0;
                m_crypt3155881.SetEnabled(m_protectionEnabled);
                return;
            }
            if (reg == 0x8u) {
                m_crypt3155881.SetLowAddress(value);
                return;
            }
            if (reg == 0xAu) {
                m_crypt3155881.SetHighAddress(value);
                return;
            }
            if (reg == 0xCu) {
                m_crypt3155881.SetSubKey(value);
                return;
            }
        }
        // Fall through to IOGA for non-protection addresses
    }

    if (IsIOGA(address) && m_iogaWrite) {
        m_iogaWrite(address & ~1u, static_cast<uint8>(value));
    }
}

void STVGameROMCartridge::WriteLong(uint32 address, uint32 value) {
    WriteWord(address + 0u, static_cast<uint16>(value >> 16u));
    WriteWord(address + 2u, static_cast<uint16>(value >> 0u));
}

uint8 STVGameROMCartridge::PeekByte(uint32 address) const {
    if (IsIOGA(address) && m_iogaRead) {
        return m_iogaRead(address);
    }
    if (m_romSize == 0) return 0xFFu;
    size_t idx = CartOffset(address, m_romSize);
    if (idx == (size_t)-1) return 0xFFu;
    return m_rom[idx];
}

uint16 STVGameROMCartridge::PeekWord(uint32 address) const {
    if (m_romSize == 0) return 0xFFFFu;
    size_t idx = CartOffset(address, m_romSize);
    if (idx == (size_t)-1) return 0xFFFFu;
    return util::ReadBE<uint16>(&m_rom[idx & ~1u]);
}

uint32 STVGameROMCartridge::PeekLong(uint32 address) const {
    const uint32 hi = PeekWord(address + 0u);
    const uint32 lo = PeekWord(address + 2u);
    return (hi << 16u) | lo;
}

void STVGameROMCartridge::PokeByte(uint32 address, uint8 value) {
    if (m_romSize == 0) return;
    size_t idx = CartOffset(address, m_romSize);
    if (idx == (size_t)-1) return;
    m_rom[idx] = value;
}

void STVGameROMCartridge::PokeWord(uint32 address, uint16 value) {
    if (m_romSize == 0) return;
    size_t idx = CartOffset(address, m_romSize);
    if (idx == (size_t)-1) return;
    util::WriteBE<uint16>(&m_rom[idx & ~1u], value);
}

void STVGameROMCartridge::PokeLong(uint32 address, uint32 value) {
    PokeWord(address + 0u, static_cast<uint16>(value >> 16u));
    PokeWord(address + 2u, static_cast<uint16>(value >> 0u));
}

// PeekROMByte uses raw buffer offsets (called by our init code, not the bus)
uint8 STVGameROMCartridge::PeekROMByte(uint32 address) const {
    if (m_romSize == 0) return 0xFFu;
    return m_rom[address & (m_romSize - 1)];
}

} // namespace ymir::cart
