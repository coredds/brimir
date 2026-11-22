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

#include "stv_315_5881.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace ymir::cart {

inline constexpr size_t kSTVGameROMMaxSize = 48_MiB;

class STVGameROMCartridge final : public BaseCartridge {
public:
    enum class ProtectionMode : uint8 {
        None = 0,
        RSG,
        Chip3155881,
    };

    // The protection hardware's runtime state, for ST-V save states (the ROM and the configuration are not part of it).
    // No padding: every byte is a member.
    struct ProtectionState {
        STV3155881::State crypt;
        uint8 rsgThingy;
        uint8 rsgCounter;
        uint8 protectionEnabled;
        uint8 reserved;
    };

    using IOGAReadFn  = std::function<uint8(uint32)>;
    using IOGAWriteFn = std::function<void(uint32, uint8)>;

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
    void ConfigureProtection(ProtectionMode mode, uint32 key);

    void SetIOGADispatch(IOGAReadFn readFn, IOGAWriteFn writeFn);

    void SaveProtectionState(ProtectionState &state) const;
    // Returns false and leaves the cartridge untouched when the state holds values the chip cannot be in.
    bool LoadProtectionState(const ProtectionState &state);

    size_t GetROMSize() const { return m_romSize; }

    uint8 PeekROMByte(uint32 address) const;

protected:
    size_t m_romSize = 0;
    std::vector<uint8> m_rom;

    mutable bool  m_rsgThingy  = false;
    mutable uint8 m_rsgCounter = 0;

    ProtectionMode m_protectionMode    = ProtectionMode::None;
    uint32         m_protectionKey     = 0;
    bool           m_protectionEnabled = false;

    mutable STV3155881 m_crypt3155881;

    IOGAReadFn  m_iogaRead  = nullptr;
    IOGAWriteFn m_iogaWrite = nullptr;
};

} // namespace ymir::cart
