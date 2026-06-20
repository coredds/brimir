// Brimir - ST-V I/O board implementation
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "stv_io.hpp"

#include <ymir/util/data_ops.hpp>

#include <algorithm>
#include <cstring>

namespace brimir::stv {

static uint16 ComputeSTVEEPROMCRC(const std::array<uint8, 128>& eeprom) {
    uint16 crc = 0x5A81;
    for (uint32 i = 0x0C; i <= 0x3F; i++) {
        crc ^= static_cast<uint16>(eeprom[i]) << 8;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000u) {
                crc = static_cast<uint16>((crc << 1) ^ 0x1021u);
            } else {
                crc = static_cast<uint16>(crc << 1);
            }
        }
    }

    const uint16 xorWord = static_cast<uint16>((static_cast<uint16>(eeprom[0x42]) << 8) | eeprom[0x43]);
    return static_cast<uint16>(crc ^ xorWord);
}

void STVIOBoard::Reset(bool hard) {
    if (hard) {
        m_coinPending = 0;
        m_coinActiveCounter = 0;
    }
    m_dataDir = 0xFF;
    m_dataOut.fill(0xFF);
}

void STVIOBoard::MapMemory(ymir::sys::SH2Bus &bus) {
    static constexpr auto cast = [](void *ctx) -> STVIOBoard & {
        return *static_cast<STVIOBoard *>(ctx);
    };

    bus.MapNormal(
        0x0400000, 0x040007F, this,
        [](uint32 address, void *ctx) -> uint8 {
            return cast(ctx).ReadIOGA<uint8, false>(address & ~1u);
        },
        [](uint32 address, void *ctx) -> uint16 {
            return cast(ctx).ReadIOGA<uint16, false>(address & ~1u);
        },
        [](uint32 address, void *ctx) -> uint32 {
            return cast(ctx).ReadIOGA<uint32, false>(address & ~1u);
        },
        [](uint32 address, uint8 value, void *ctx) {
            cast(ctx).WriteIOGA<uint8, false>(address & ~1u, value);
        },
        [](uint32 address, uint16 value, void *ctx) {
            cast(ctx).WriteIOGA<uint16, false>(address & ~1u, value);
        },
        [](uint32 address, uint32 value, void *ctx) {
            cast(ctx).WriteIOGA<uint32, false>(address & ~1u, value);
        });

    bus.MapSideEffectFree(
        0x0400000, 0x040007F, this,
        [](uint32 address, void *ctx) -> uint8 {
            return cast(ctx).ReadIOGA<uint8, true>(address & ~1u);
        },
        [](uint32 address, void *ctx) -> uint16 {
            return cast(ctx).ReadIOGA<uint16, true>(address & ~1u);
        },
        [](uint32 address, void *ctx) -> uint32 {
            return cast(ctx).ReadIOGA<uint32, true>(address & ~1u);
        },
        [](uint32 address, uint8 value, void *ctx) {
            cast(ctx).WriteIOGA<uint8, true>(address & ~1u, value);
        },
        [](uint32 address, uint16 value, void *ctx) {
            cast(ctx).WriteIOGA<uint16, true>(address & ~1u, value);
        },
        [](uint32 address, uint32 value, void *ctx) {
            cast(ctx).WriteIOGA<uint32, true>(address & ~1u, value);
        });
}

template <typename T, bool peek>
T STVIOBoard::ReadIOGA(uint32 address) {
    if constexpr (std::is_same_v<T, uint32>) {
        uint32 val = ReadIOGA<uint16, peek>(address) << 16;
        val |= ReadIOGA<uint16, peek>(address + 2);
        return val;
    }

    if (!m_stvModeActive) {
        if constexpr (std::is_same_v<T, uint16>) {
            if (m_iplROM) {
                uint32 off = address & 0x7FFFF;
                return static_cast<uint16>((static_cast<uint16>(m_iplROM[off]) << 8) | m_iplROM[off ^ 1]);
            }
            return 0xFFFFu;
        } else {
            return m_iplROM ? m_iplROM[address & 0x7FFFF] : 0xFFu;
        }
    }

    // Only handle IOGA register range 0x0400000-0x040007F
    // Outside this range, fall back to IPL ROM for BIOS code execution
    if (address < 0x0400000 || address > 0x040007F) {
        if constexpr (std::is_same_v<T, uint16>) {
            if (m_iplROM) {
                uint32 off = address & 0x7FFFF;
                return static_cast<uint16>((static_cast<uint16>(m_iplROM[off]) << 8) | m_iplROM[off ^ 1]);
            }
            return 0xFFFFu;
        } else {
            return m_iplROM ? m_iplROM[address & 0x7FFFF] : 0xFFu;
        }
    }

    const uint8 iogaAddr = static_cast<uint8>((address >> 1) & 0x3F);
    uint8 data;

    if (iogaAddr == 0x8) {
        data = m_dataDir;
    } else {
        data = m_dataIn[iogaAddr & 0x7];
        if (!(m_dataDir & (1u << (iogaAddr & 0x7)))) {
            data &= m_dataOut[iogaAddr & 0x7];
        }
    }

    if constexpr (std::is_same_v<T, uint16>) {
        return static_cast<uint16>(0xFF00u | data);
    } else {
        return data;
    }
}

template <typename T, bool poke>
void STVIOBoard::WriteIOGA(uint32 address, T value) {
    if constexpr (std::is_same_v<T, uint32>) {
        WriteIOGA<uint16, poke>(address, static_cast<uint16>(value >> 16));
        WriteIOGA<uint16, poke>(address + 2, static_cast<uint16>(value));
        return;
    }

    if (!m_stvModeActive) return;

    // Only handle IOGA register range
    if (address < 0x0400000 || address > 0x040007F) return;

    const uint8 iogaAddr = static_cast<uint8>((address >> 1) & 0x3F);
    const uint8 byteVal = static_cast<uint8>(value);

    if (iogaAddr == 0x8) {
        m_dataDir = byteVal;
    } else if (iogaAddr < 0x8) {
        m_dataOut[iogaAddr] = byteVal;
    }
}

void STVIOBoard::SetCoin1(bool pressed) {
    if (pressed) m_coinPending++;
}

void STVIOBoard::SetCoin2(bool pressed) {
    if (pressed) m_coinPending++;
}

void STVIOBoard::SetService(bool pressed) { m_service = pressed; }
void STVIOBoard::SetTest(bool pressed)   { m_test = pressed; }

void STVIOBoard::SetButton(uint8 player, uint16 buttons) {
    if (player < 2) m_playerButtons[player] = buttons;
}

void STVIOBoard::SetStart(uint8 player, bool pressed) {
    if (player < 2) m_playerStart[player] = pressed;
}

void STVIOBoard::UpdateInputs() {
    m_dataIn.fill(0xFF);

    for (unsigned p = 0; p < 2; p++) {
        uint16 tmp = m_playerButtons[p];
        m_dataIn[p] ^= static_cast<uint8>(((tmp & 0xA0) >> 1) | ((tmp & 0x50) << 1) |
                                          ((tmp >> 10) & 0x01) | ((tmp >> 7) & 0x06));
        m_dataIn[0x5] ^= static_cast<uint8>(
            (((tmp >> 2) & 0x01) | (tmp & 0x02) | ((tmp << 2) & 0x04)) << (p << 2));

        if (m_playerStart[p]) {
            m_dataIn[0x2] ^= static_cast<uint8>(1u << (7 - p));
        }
    }

    // Coin pulse (active for kCoinPulseFrames)
    static constexpr int64_t kCoinPulseFrames = 4;
    m_coinActiveCounter--;
    if (m_coinPending > 0 && m_coinActiveCounter <= 0) {
        m_coinActiveCounter = kCoinPulseFrames;
        m_coinPending--;
    }
    if (m_coinActiveCounter > 0) {
        m_dataIn[0x2] ^= 0x10;
    }

    if (m_test)    m_dataIn[0x2] ^= 0x04;
    if (m_service) m_dataIn[0x2] ^= 0x08;

    m_dataIn[0x3] = 0x00;
}

void STVIOBoard::InitEEPROM(const uint8 *romHeader, const uint8 *gameSettings, uint8 cabType) {
    m_eeprom.fill(0xFF);

    m_eeprom[0x00] = 'S';
    m_eeprom[0x01] = 'E';
    m_eeprom[0x02] = 'G';
    m_eeprom[0x03] = 'A';

    m_eeprom[0x0C] = 0x00; m_eeprom[0x0D] = 0x00;
    m_eeprom[0x0E] = 0x00; m_eeprom[0x0F] = 0x01;
    m_eeprom[0x10] = 0x01; m_eeprom[0x11] = 0x00;
    m_eeprom[0x12] = 0x01; m_eeprom[0x13] = 0x01;
    m_eeprom[0x14] = 0x00; m_eeprom[0x15] = 0x00;
    m_eeprom[0x16] = 0x00; m_eeprom[0x17] = 0x00;
    m_eeprom[0x18] = 0x00; m_eeprom[0x19] = 0x08;

    // Determine cabinet type from ROM byte at 0xF46
    unsigned cab_players = 2;
    switch (cabType) {
        case 0x01: case 0x03: cab_players = 1; break;
        case 0x10:             cab_players = 3; break;
        default:               cab_players = 2; break;
    }

    // Settings: matches Mednafen 0x089C base + cab_type + alone + advertise
    uint16 settings = 0x089C | ((cab_players - 1) & 0x3) | (1U << 5) | (1U << 6);
    m_eeprom[0x1A] = static_cast<uint8>(settings >> 8);
    m_eeprom[0x1B] = static_cast<uint8>(settings);

    if (romHeader) {
        m_eeprom[0x1C] = romHeader[0x00];
        m_eeprom[0x1D] = romHeader[0x01];
    }
    if (gameSettings) {
        std::memcpy(&m_eeprom[0x1E], gameSettings, 8);
    }

    const uint16 crc = ComputeSTVEEPROMCRC(m_eeprom);
    m_eeprom[0x08] = static_cast<uint8>(crc >> 8);
    m_eeprom[0x09] = static_cast<uint8>(crc);

    // Mirror block, matches cabinet EEPROM layout used by ST-V BIOS/game code.
    std::copy(m_eeprom.begin() + 0x08, m_eeprom.begin() + 0x40, m_eeprom.begin() + 0x44);
}

} // namespace brimir::stv
