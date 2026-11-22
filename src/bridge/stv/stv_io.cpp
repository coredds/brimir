// Brimir - ST-V I/O board implementation
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "stv_io.hpp"

#include <ymir/util/data_ops.hpp>

#include <algorithm>
#include <cstring>

namespace brimir::stv {

static constexpr int64_t kCoinPulseFrames = 4;

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
        for (unsigned coin = 0; coin < 2; coin++) {
            m_coinPending[coin] = 0;
            m_coinActiveCounter[coin] = 0;
            m_coinHeld[coin] = false;
        }
    }
    m_dataDir = 0xFF;
    m_dataOut.fill(0xFF);
    m_dataIn.fill(0xFF);
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

// A coin is counted when the button goes down; holding it does not insert more.
void STVIOBoard::SetCoin1(bool pressed) {
    if (pressed && !m_coinHeld[0]) m_coinPending[0]++;
    m_coinHeld[0] = pressed;
}

void STVIOBoard::SetCoin2(bool pressed) {
    if (pressed && !m_coinHeld[1]) m_coinPending[1]++;
    m_coinHeld[1] = pressed;
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
    for (unsigned coin = 0; coin < 2; coin++) {
        if (m_coinActiveCounter[coin] > 0) m_coinActiveCounter[coin]--;
        if (m_coinPending[coin] > 0 && m_coinActiveCounter[coin] <= 0) {
            m_coinActiveCounter[coin] = kCoinPulseFrames;
            m_coinPending[coin]--;
        }
        if (m_coinActiveCounter[coin] > 0) {
            m_dataIn[0x2] ^= static_cast<uint8>(1u << coin);
        }
    }

    // PORT-C test/service bits match MAME: test = bit 2, service = bit 3.
    if (m_test)    m_dataIn[0x2] ^= 0x04;
    if (m_service) m_dataIn[0x2] ^= 0x08;

    m_dataIn[0x3] = 0x00;
}

void STVIOBoard::InitEEPROM(const uint8 *romHeader, const uint8 *gameSettings, uint8 cabType) {
    std::array<uint8, 128> image;
    image.fill(0xFF);

    image[0x00] = 'S';
    image[0x01] = 'E';
    image[0x02] = 'G';
    image[0x03] = 'A';

    image[0x0C] = 0x00; image[0x0D] = 0x00;
    image[0x0E] = 0x00; image[0x0F] = 0x01;
    image[0x10] = 0x01; image[0x11] = 0x00;
    image[0x12] = 0x01; image[0x13] = 0x01;
    image[0x14] = 0x00; image[0x15] = 0x00;
    image[0x16] = 0x00; image[0x17] = 0x00;
    image[0x18] = 0x00; image[0x19] = 0x08;

    // Determine cabinet type from ROM byte at 0xF46
    unsigned cab_players = 2;
    switch (cabType) {
        case 0x01: case 0x03: cab_players = 1; break;
        case 0x10:             cab_players = 3; break;
        default:               cab_players = 2; break;
    }

    // Settings: matches Mednafen 0x089C base + cab_type + alone + advertise
    uint16 settings = 0x089C | ((cab_players - 1) & 0x3) | (1U << 5) | (1U << 6);
    image[0x1A] = static_cast<uint8>(settings >> 8);
    image[0x1B] = static_cast<uint8>(settings);

    if (romHeader) {
        image[0x1C] = romHeader[0x00];
        image[0x1D] = romHeader[0x01];
    }
    if (gameSettings) {
        std::memcpy(&image[0x1E], gameSettings, 8);
    }

    const uint16 crc = ComputeSTVEEPROMCRC(image);
    image[0x08] = static_cast<uint8>(crc >> 8);
    image[0x09] = static_cast<uint8>(crc);

    // Mirror block, matches cabinet EEPROM layout used by ST-V BIOS/game code.
    std::copy(image.begin() + 0x08, image.begin() + 0x40, image.begin() + 0x44);

    m_serialEEPROM.Load(image);
}

void STVIOBoard::LoadEEPROM(const std::array<uint8, 128> &cells) {
    m_serialEEPROM.Load(cells);
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
    return static_cast<uint16_t>((data[addr * 2] << 8) | data[addr * 2 + 1]);
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
            bits = 0;
            shiftReg = 0;
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
        bits = 0;
        shiftReg = 0;
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

void STVIOBoard::SaveState(STVIOBoardState &state) const {
    state = {};
    for (unsigned i = 0; i < 2; i++) {
        state.coinActiveCounter[i] = m_coinActiveCounter[i];
        state.coinPending[i] = m_coinPending[i];
        state.playerButtons[i] = m_playerButtons[i];
        state.playerStart[i] = m_playerStart[i] ? 1 : 0;
        state.coinHeld[i] = m_coinHeld[i] ? 1 : 0;
    }
    std::copy(m_dataOut.begin(), m_dataOut.end(), state.dataOut);
    std::copy(m_dataIn.begin(), m_dataIn.end(), state.dataIn);
    state.dataDir = m_dataDir;
    state.stvModeActive = m_stvModeActive ? 1 : 0;
    state.service = m_service ? 1 : 0;
    state.test = m_test ? 1 : 0;

    const SerialEEPROM &ee = m_serialEEPROM;
    state.eepromCommandAccum = ee.commandAccum;
    state.eepromShiftReg = ee.shiftReg;
    state.eepromBits = ee.bits;
    state.eepromAddress = ee.address;
    std::copy(ee.data.begin(), ee.data.end(), state.eepromData);
    state.eepromState = static_cast<uint8>(ee.state);
    state.eepromCS = ee.cs ? 1 : 0;
    state.eepromCLK = ee.clk ? 1 : 0;
    state.eepromDI = ee.di ? 1 : 0;
    state.eepromLocked = ee.locked ? 1 : 0;
    state.eepromDOHigh = ee.doOutputHigh ? 1 : 0;
}

bool STVIOBoard::LoadState(const STVIOBoardState &state) {
    using EEState = SerialEEPROM::State;

    if (state.eepromState > static_cast<uint8>(EEState::Completion)) return false;
    if (state.eepromAddress >= SerialEEPROM::kCells) return false;
    if (state.eepromState == static_cast<uint8>(EEState::Command) && state.eepromBits >= 2 + SerialEEPROM::kAddressBits) {
        return false;
    }
    if (state.eepromState == static_cast<uint8>(EEState::Writing) && state.eepromBits >= SerialEEPROM::kDataBits) {
        return false;
    }
    for (unsigned i = 0; i < 2; i++) {
        if (state.coinActiveCounter[i] < 0 || state.coinActiveCounter[i] > kCoinPulseFrames) return false;
        if (state.playerStart[i] > 1 || state.coinHeld[i] > 1) return false;
    }
    if (state.stvModeActive > 1 || state.service > 1 || state.test > 1 || state.eepromCS > 1 || state.eepromCLK > 1 ||
        state.eepromDI > 1 || state.eepromLocked > 1 || state.eepromDOHigh > 1) {
        return false;
    }

    for (unsigned i = 0; i < 2; i++) {
        m_coinActiveCounter[i] = state.coinActiveCounter[i];
        m_coinPending[i] = state.coinPending[i];
        m_playerButtons[i] = state.playerButtons[i];
        m_playerStart[i] = state.playerStart[i] != 0;
        m_coinHeld[i] = state.coinHeld[i] != 0;
    }
    std::copy(std::begin(state.dataOut), std::end(state.dataOut), m_dataOut.begin());
    std::copy(std::begin(state.dataIn), std::end(state.dataIn), m_dataIn.begin());
    m_dataDir = state.dataDir;
    m_stvModeActive = state.stvModeActive != 0;
    m_service = state.service != 0;
    m_test = state.test != 0;

    SerialEEPROM &ee = m_serialEEPROM;
    ee.commandAccum = state.eepromCommandAccum;
    ee.shiftReg = state.eepromShiftReg;
    ee.bits = state.eepromBits;
    ee.address = state.eepromAddress;
    std::copy(std::begin(state.eepromData), std::end(state.eepromData), ee.data.begin());
    ee.state = static_cast<EEState>(state.eepromState);
    ee.cs = state.eepromCS != 0;
    ee.clk = state.eepromCLK != 0;
    ee.di = state.eepromDI != 0;
    ee.locked = state.eepromLocked != 0;
    ee.doOutputHigh = state.eepromDOHigh != 0;
    return true;
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
