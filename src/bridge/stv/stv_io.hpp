// Brimir - ST-V I/O board emulation (IOGA chip at 0x00400000-0x0040007F)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#pragma once

#include <ymir/core/types.hpp>
#include <ymir/sys/bus.hpp>

#include <array>
#include <cstdint>

namespace brimir::stv {

// Everything the I/O board and its 93C46 hold, for ST-V save states. No padding: every byte is a member.
struct STVIOBoardState {
    int64_t coinActiveCounter[2];
    uint32 coinPending[2];
    uint32 eepromCommandAccum;
    uint32 eepromShiftReg;
    uint32 eepromBits;
    uint32 eepromAddress;
    uint16 playerButtons[2];
    uint8 dataOut[8];
    uint8 dataIn[8];
    uint8 eeprom[128];        // image set by InitEEPROM
    uint8 eepromData[128];    // live cell contents of the 93C46
    uint8 dataDir;
    uint8 stvModeActive;
    uint8 playerStart[2];
    uint8 coinHeld[2];
    uint8 service;
    uint8 test;
    uint8 eepromState;        // SerialEEPROM::State
    uint8 eepromCS;
    uint8 eepromCLK;
    uint8 eepromDI;
    uint8 eepromLocked;
    uint8 eepromDOHigh;
    uint8 reserved[6];
};

class STVIOBoard {
public:
    STVIOBoard() = default;

    void Reset(bool hard);
    void MapMemory(ymir::sys::SH2Bus &bus);
    void SetIPLPointer(const uint8 *iplROM) { m_iplROM = iplROM; }
    void SetSTVMode(bool active) { m_stvModeActive = active; }

    void SetCoin1(bool pressed);
    void SetCoin2(bool pressed);
    void SetService(bool pressed);
    void SetTest(bool pressed);

    void SetButton(uint8 player, uint16 buttons);
    void SetStart(uint8 player, bool pressed);

    void UpdateInputs();

    std::array<uint8, 128> &GetEEPROM() { return m_eeprom; }
    const std::array<uint8, 128> &GetEEPROM() const { return m_eeprom; }

    void InitEEPROM(const uint8 *romHeader, const uint8 *gameSettings, uint8 cabType);

    uint8 ReadIOGAByte(uint32 address) {
        return ReadIOGA<uint8, false>(address);
    }
    void WriteIOGAByte(uint32 address, uint8 value) {
        WriteIOGA<uint8, false>(address, value);
    }

    // SMPC PDR1/PDR2 hooks for ST-V cabinet EEPROM bit-banging.
    // Matches MAME: PDR1 drives 93C46 CLK/DI/CS, PDR2 reads DO.
    uint8 ReadPDR1() const;
    void WritePDR1(uint8 data);
    uint8 ReadPDR2() const;
    void WritePDR2(uint8 data);

    void SaveState(STVIOBoardState &state) const;
    // Returns false and leaves the board untouched when the state holds values the board cannot be in.
    bool LoadState(const STVIOBoardState &state);

private:
    template <typename T, bool peek>
    T ReadIOGA(uint32 address);

    template <typename T, bool poke>
    void WriteIOGA(uint32 address, T value);

    bool m_stvModeActive = false;
    const uint8 *m_iplROM = nullptr;

    uint8 m_dataDir = 0xFF;
    std::array<uint8, 8> m_dataOut{};
    std::array<uint8, 8> m_dataIn{};

    // Per coin input: presses waiting to be pulsed, frames left in the current pulse, and whether the button is held.
    uint32 m_coinPending[2] = {};
    int64_t m_coinActiveCounter[2] = {};
    bool m_coinHeld[2] = {};

    uint16 m_playerButtons[2] = {};
    bool   m_playerStart[2] = {};
    bool   m_service = false;
    bool   m_test = false;

    std::array<uint8, 128> m_eeprom{};

    // 93C46 16-bit serial EEPROM state machine driven by SMPC PDR1/PDR2.
    struct SerialEEPROM {
        enum class State {
            Reset,
            WaitStart,
            Command,
            Reading,
            Writing,
            Completion
        };

        State state = State::Reset;
        bool cs = false;
        bool clk = false;
        bool di = false;
        bool locked = true;
        bool doOutputHigh = true; // high-impedance DO pulled up
        uint32_t commandAccum = 0;
        uint32_t shiftReg = 0;
        unsigned bits = 0;
        unsigned address = 0;
        std::array<uint8, 128> data{};

        static constexpr unsigned kAddressBits = 6;
        static constexpr unsigned kDataBits = 16;
        static constexpr unsigned kCells = 1u << kAddressBits;

        void Load(const std::array<uint8, 128> &src);
        void Reset();
        void SetCS(bool value);
        void SetCLK(bool value);
        void SetDI(bool value);
        bool GetDO() const { return doOutputHigh; }

    private:
        void Step();
        void ExecuteCommand();
        uint16_t ReadWord(unsigned addr) const;
        void WriteWord(unsigned addr, uint16_t value);
    };

    SerialEEPROM m_serialEEPROM;
};

} // namespace brimir::stv
