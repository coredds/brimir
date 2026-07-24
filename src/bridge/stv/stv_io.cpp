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
        // MAME convention: bit set = input, bit clear = output.
        const uint8 bit = static_cast<uint8>(iogaAddr & 0x7);
        if (m_dataDir & (1u << bit)) {
            // Input pin: read the externally applied value.
            data = m_dataIn[bit];
        } else {
            // Output pin: read back the output latch (e.g. PORT-D system output).
            data = m_dataOut[bit];
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
            // PORT-C start bits match MAME/JAMMA: P1 start = bit 4, P2 start = bit 5.
            m_dataIn[0x2] ^= static_cast<uint8>(1u << (4 + p));
        }
    }

    // Coin pulse (active for kCoinPulseFrames). PORT-C coin bits match MAME:
    // coin1 = bit 0, coin2 = bit 1.
    static constexpr int64_t kCoinPulseFrames = 4;
    m_coinActiveCounter--;
    if (m_coinPending > 0 && m_coinActiveCounter <= 0) {
        m_coinActiveCounter = kCoinPulseFrames;
        m_coinPending--;
    }
    if (m_coinActiveCounter > 0) {
        m_dataIn[0x2] ^= 0x03; // assert both coin1 (bit0) and coin2 (bit1)
    }

    // PORT-C test/service bits match MAME: test = bit 2, service = bit 3.
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

    m_serialEEPROM.Load(m_eeprom);
}

// ---------------------------------------------------------------------------
// 93C46 16-bit serial EEPROM (driven by SMPC PDR1/PDR2 on ST-V hardware)
// ---------------------------------------------------------------------------

void STVIOBoard::SerialEEPROM::Load(const std::array<uint8, 128> &src) {
    data = src;
    Reset();
}

void STVIOBoard::SerialEEPROM::Reset() {
    state = State::Reset;
    cs = false;
    clk = false;
    di = false;
    locked = true;
    doOutputHigh = true;
    commandAccum = 0;
    shiftReg = 0;
    bits = 0;
    address = 0;
}

uint16_t STVIOBoard::SerialEEPROM::ReadWord(unsigned addr) const {
    if (addr >= kCells) return 0xFFFFu;
    return static_cast<uint16_t>(data[addr * 2] | (data[addr * 2 + 1] << 8));
}

void STVIOBoard::SerialEEPROM::WriteWord(unsigned addr, uint16_t value) {
    if (addr >= kCells) return;
    data[addr * 2 + 0] = static_cast<uint8_t>(value >> 8);
    data[addr * 2 + 1] = static_cast<uint8_t>(value);
}

void STVIOBoard::SerialEEPROM::SetCS(bool value) {
    if (cs == value) return;
    cs = value;
    if (cs) {
        state = State::WaitStart;
        doOutputHigh = true;
    } else {
        state = State::Reset;
        doOutputHigh = true;
    }
}

void STVIOBoard::SerialEEPROM::SetCLK(bool value) {
    if (clk == value) return;
    clk = value;
    if (!clk) return;
    // Rising edge.
    Step();
}

void STVIOBoard::SerialEEPROM::SetDI(bool value) {
    di = value;
}

void STVIOBoard::SerialEEPROM::Step() {
    switch (state) {
    case State::Reset:
        break;

    case State::WaitStart:
        if (di) {
            commandAccum = 0;
            bits = 0;
            state = State::Command;
        }
        break;

    case State::Command:
        commandAccum = (commandAccum << 1) | (di ? 1u : 0u);
        if (++bits == 2 + kAddressBits) {
            ExecuteCommand();
        }
        break;

    case State::Reading: {
        const unsigned bitIndex = bits++;
        if (bitIndex % kDataBits == 0) {
            const unsigned readAddr = (address + bitIndex / kDataBits) & (kCells - 1);
            shiftReg = static_cast<uint32_t>(ReadWord(readAddr)) << (32 - kDataBits);
        } else {
            shiftReg = (shiftReg << 1) | 1u;
        }
        doOutputHigh = (shiftReg & 0x80000000u) != 0;
        break;
    }

    case State::Writing:
        shiftReg = (shiftReg << 1) | (di ? 1u : 0u);
        if (++bits == kDataBits) {
            if (locked) {
                state = State::Reset;
            } else {
                WriteWord(address, static_cast<uint16_t>(shiftReg));
                state = State::Completion;
            }
            doOutputHigh = true;
        }
        break;

    case State::Completion:
        // Wait for CS fall; output stays high (ready).
        break;
    }
}

void STVIOBoard::SerialEEPROM::ExecuteCommand() {
    address = commandAccum & (kCells - 1);
    const unsigned opcode = commandAccum >> kAddressBits;

    switch (opcode) {
    case 0: {
        const unsigned subcmd = address >> (kAddressBits - 2);
        address = 0;
        switch (subcmd) {
        case 0: // WRDS / lock
            locked = true;
            state = State::Reset;
            break;
        case 1: // WRAL
            state = locked ? State::Reset : State::Writing;
            break;
        case 2: // ERAL
            if (!locked) {
                for (unsigned i = 0; i < kCells; ++i) WriteWord(i, 0xFFFFu);
            }
            state = State::Completion;
            break;
        case 3: // WREN / unlock
            locked = false;
            state = State::Reset;
            break;
        }
        break;
    }
    case 1: // WRITE
        state = locked ? State::Reset : State::Writing;
        break;
    case 2: // READ
        bits = 0;
        shiftReg = 0;
        state = State::Reading;
        break;
    case 3: // ERASE
        if (!locked) WriteWord(address, 0xFFFFu);
        state = State::Completion;
        break;
    }
}

uint8 STVIOBoard::ReadPDR1() const {
    // MAME returns (input port & 0x40) | 0x3f; with inputs idle this is 0x7f.
    return 0x7f;
}

void STVIOBoard::WritePDR1(uint8 data) {
    // PDR1 bit 2 = CS, bit 3 = CLK, bit 4 = DI, bits 0-1 = game select.
    m_serialEEPROM.SetCS((data & 0x04) != 0);
    m_serialEEPROM.SetDI((data & 0x10) != 0);
    m_serialEEPROM.SetCLK((data & 0x08) != 0);
}

uint8 STVIOBoard::ReadPDR2() const {
    // MAME returns (input port & ~0x19) | 0x18 | do_read; with inputs idle this is 0xfe | do.
    return static_cast<uint8>(0xfe | (m_serialEEPROM.GetDO() ? 1u : 0u));
}

void STVIOBoard::WritePDR2(uint8 data) {
    // PDR2 output bit 4 controls the ST-V 68K sound CPU reset (active high).
    // bit 3 may be SCSP reset. Not required for boot, so no action needed here.
    (void)data;
}

} // namespace brimir::stv
