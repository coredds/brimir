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
    m_abus = {};

    m_crypt3155881.Reset();
    m_crypt3155881.SetKey(key);
    m_crypt3155881.SetEnabled(false);
}

void STVGameROMCartridge::SetIOGADispatch(IOGAReadFn readFn, IOGAWriteFn writeFn) {
    m_iogaRead  = std::move(readFn);
    m_iogaWrite = std::move(writeFn);
}

void STVGameROMCartridge::SaveProtectionState(ProtectionState &state) const {
    state = {};
    m_crypt3155881.SaveState(state.crypt);
    state.protectionEnabled = m_protectionEnabled ? 1 : 0;
    state.abus = m_abus;
}

bool STVGameROMCartridge::ValidateProtectionState(const ProtectionState &state) const {
    if (state.protectionEnabled > 1 || state.reserved[0] != 0 || state.reserved[1] != 0 || state.reserved[2] != 0) {
        return false;
    }
    if ((state.protectionEnabled != 0) != (((state.abus[0] >> 16) & 1u) != 0)) return false;
    return m_crypt3155881.ValidateState(state.crypt);
}

bool STVGameROMCartridge::LoadProtectionState(const ProtectionState &state) {
    if (!ValidateProtectionState(state)) return false;
    if (!m_crypt3155881.LoadState(state.crypt)) return false;
    m_protectionEnabled = state.protectionEnabled != 0;
    m_abus = state.abus;
    return true;
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

static inline bool IsIOGA(uint32 address) {
    return address >= kIOGABase && address <= kIOGAEnd;
}

// MAME's install_common_protection(): the handlers cover 0x4FFFFF0-0x4FFFFFF only.
static inline bool IsProtectionRegisters(uint32 address) {
    return (address & 0xFFFFFFF0u) == 0x04FFFFF0u;
}

static inline unsigned ProtectionRegisterIndex(uint32 address) {
    return (address >> 2) & 3u;
}

uint32 STVGameROMCartridge::ROMLongAt(uint32 address) const {
    const size_t idx = CartOffset(address, m_romSize);
    if (idx == (size_t)-1) return 0xFFFFFFFFu;
    const size_t next = CartOffset(address + 2u, m_romSize);
    const uint32 hi = util::ReadBE<uint16>(&m_rom[idx & ~1u]);
    const uint32 lo = next == (size_t)-1 ? 0xFFFFu : util::ReadBE<uint16>(&m_rom[next & ~1u]);
    return (hi << 16u) | lo;
}

// stv_state::common_prot_r. MAME's handler returns the whole long whatever the access size and the access takes the
// lane it needs, so a byte or word read of the decrypted-data register runs the chip twice, like a long read.
uint32 STVGameROMCartridge::ReadProtectionRegister(unsigned index, bool peek) const {
    if ((m_abus[0] >> 16) & 1u) {
        if (index == 3) {
            if (peek) return m_abus[3];
            uint32 hi = m_crypt3155881.ReadDecryptedWord();
            hi = bit::byte_swap<uint16>(static_cast<uint16>(hi));
            uint32 lo = m_crypt3155881.ReadDecryptedWord();
            lo = bit::byte_swap<uint16>(static_cast<uint16>(lo));
            return (hi << 16u) | lo;
        }
        return m_abus[index];
    }
    if (m_abus[index] != 0) return m_abus[index];
    return ROMLongAt(0x04FFFFF0u + index * 4u);
}

// stv_state::common_prot_w: the write is merged into the register under the access's mask, then register 0 sets the
// enable (bit 16), register 2 sets the chip's address (low from the high half if that half is written, high from the
// low half if that half is written) and register 3 the subkey (high half of the merged register).
void STVGameROMCartridge::WriteProtectionRegister(unsigned index, uint32 data, uint32 mask) {
    m_abus[index] = (m_abus[index] & ~mask) | (data & mask);

    if (index == 0) {
        m_protectionEnabled = ((m_abus[0] >> 16) & 1u) != 0;
        m_crypt3155881.SetEnabled(m_protectionEnabled);
    } else if (index == 2) {
        if ((mask & 0xFFFF0000u) != 0) m_crypt3155881.SetLowAddress(static_cast<uint16>(data >> 16u));
        if ((mask & 0x0000FFFFu) != 0) m_crypt3155881.SetHighAddress(static_cast<uint16>(data & 0xFFFFu));
    } else if (index == 3) {
        m_crypt3155881.SetSubKey(static_cast<uint16>(m_abus[3] >> 16u));
    }
}

uint8 STVGameROMCartridge::ReadByte(uint32 address) const {
    if (IsIOGA(address) && m_iogaRead) {
        return m_iogaRead(address);
    }
    if (m_romSize == 0) return 0xFFu;
    if (m_protectionMode == ProtectionMode::Chip3155881 && IsProtectionRegisters(address)) {
        return static_cast<uint8>(ReadProtectionRegister(ProtectionRegisterIndex(address), false) >>
                                  (24u - 8u * (address & 3u)));
    }
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

    if (m_protectionMode == ProtectionMode::Chip3155881 && IsProtectionRegisters(address)) {
        return static_cast<uint16>(ReadProtectionRegister(ProtectionRegisterIndex(address), false) >>
                                   (16u - 16u * ((address >> 1) & 1u)));
    }

    size_t idx = CartOffset(address, m_romSize);
    if (idx == (size_t)-1) return 0xFFFFu;
    return util::ReadBE<uint16>(&m_rom[idx & ~1u]);
}

uint32 STVGameROMCartridge::ReadLong(uint32 address) const {
    if (m_romSize == 0) return 0xFFFFFFFFu;

    if (m_protectionMode == ProtectionMode::Chip3155881 && IsProtectionRegisters(address) && (address & 3u) == 0) {
        return ReadProtectionRegister(ProtectionRegisterIndex(address), false);
    }

    const uint32 hi = ReadWord(address + 0u);
    const uint32 lo = ReadWord(address + 2u);
    return (hi << 16u) | lo;
}

void STVGameROMCartridge::WriteByte(uint32 address, uint8 value) {
    if (m_protectionMode == ProtectionMode::Chip3155881 && IsProtectionRegisters(address)) {
        const unsigned shift = 24u - 8u * (address & 3u);
        WriteProtectionRegister(ProtectionRegisterIndex(address), static_cast<uint32>(value) << shift,
                                0xFFu << shift);
        return;
    }

    if (IsIOGA(address) && m_iogaWrite) {
        m_iogaWrite(address, value);
    }
}

void STVGameROMCartridge::WriteWord(uint32 address, uint16 value) {
    if (m_protectionMode == ProtectionMode::Chip3155881 && IsProtectionRegisters(address)) {
        const unsigned shift = 16u - 16u * ((address >> 1) & 1u);
        WriteProtectionRegister(ProtectionRegisterIndex(address), static_cast<uint32>(value) << shift,
                                0xFFFFu << shift);
        return;
    }

    if (IsIOGA(address) && m_iogaWrite) {
        m_iogaWrite(address & ~1u, static_cast<uint8>(value));
    }
}

void STVGameROMCartridge::WriteLong(uint32 address, uint32 value) {
    if (m_protectionMode == ProtectionMode::Chip3155881 && IsProtectionRegisters(address) && (address & 3u) == 0) {
        WriteProtectionRegister(ProtectionRegisterIndex(address), value, 0xFFFFFFFFu);
        return;
    }
    WriteWord(address + 0u, static_cast<uint16>(value >> 16u));
    WriteWord(address + 2u, static_cast<uint16>(value >> 0u));
}

uint8 STVGameROMCartridge::PeekByte(uint32 address) const {
    if (IsIOGA(address) && m_iogaRead) {
        return m_iogaRead(address);
    }
    if (m_romSize == 0) return 0xFFu;
    if (m_protectionMode == ProtectionMode::Chip3155881 && IsProtectionRegisters(address)) {
        return static_cast<uint8>(ReadProtectionRegister(ProtectionRegisterIndex(address), true) >>
                                  (24u - 8u * (address & 3u)));
    }
    size_t idx = CartOffset(address, m_romSize);
    if (idx == (size_t)-1) return 0xFFu;
    return m_rom[idx];
}

uint16 STVGameROMCartridge::PeekWord(uint32 address) const {
    if (m_romSize == 0) return 0xFFFFu;
    if (m_protectionMode == ProtectionMode::Chip3155881 && IsProtectionRegisters(address)) {
        return static_cast<uint16>(ReadProtectionRegister(ProtectionRegisterIndex(address), true) >>
                                   (16u - 16u * ((address >> 1) & 1u)));
    }
    size_t idx = CartOffset(address, m_romSize);
    if (idx == (size_t)-1) return 0xFFFFu;
    return util::ReadBE<uint16>(&m_rom[idx & ~1u]);
}

uint32 STVGameROMCartridge::PeekLong(uint32 address) const {
    if (m_romSize != 0 && m_protectionMode == ProtectionMode::Chip3155881 && IsProtectionRegisters(address) &&
        (address & 3u) == 0) {
        return ReadProtectionRegister(ProtectionRegisterIndex(address), true);
    }
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
