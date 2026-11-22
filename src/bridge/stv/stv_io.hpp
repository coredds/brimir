// Brimir - ST-V I/O board emulation (IOGA chip at 0x00400000-0x0040007F)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0 (the combined work)
//
// STVIOBoard::SerialEEPROM is derived from MAME's eeprom_serial_93cxx_device (src/devices/machine/eepromser.cpp,
// eeprom.cpp; BSD-3-Clause, copyright Aaron Giles). The licence notice is in stv_io.cpp.

#pragma once

#include "stv_input.hpp"

#include <ymir/core/types.hpp>
#include <ymir/sys/bus.hpp>

#include <array>
#include <cstdint>

namespace brimir::stv {

// Everything the I/O board and its 93C46 hold, for ST-V save states. No padding: every byte is a member.
struct STVIOBoardState {
    int64_t coinActiveCounter[2];
    uint64_t eepromBusyNs;    // time left in the 93C46's internal write/erase cycle
    uint32 coinPending[2];
    uint32 eepromCommandAccum;
    uint32 eepromShiftReg;
    uint32 eepromBits;
    uint32 eepromAddress;
    uint16 playerButtons[2];
    uint8 dataOut[8];
    uint8 dataIn[8];
    uint8 eepromData[128];    // cell contents of the 93C46, as the game has written them
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
    uint8 eepromWriteAll;     // the pending write command is WRAL, not WRITE
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

    void SetStart(uint8 player, bool pressed);

    // A player's controls for the frame (player 0 or 1). Player 1's frame also sets the service and test switches,
    // which the cabinet has once; player 2's coin is coin 2. Held coins count once (SetCoin1/SetCoin2).
    void SetPlayerInput(uint8 player, const STVInputFrame &input);

    void UpdateInputs();

    // The 93C46's cells as they are now, including what the game has written through the serial protocol. Words are
    // big-endian: cell n is bytes 2n (high) and 2n+1 (low).
    const std::array<uint8, 128> &GetEEPROM() const { return m_serialEEPROM.data; }

    // Makes an image (a saved one as returned by GetEEPROM, or a blank 0xFF one) the cells. The chip goes back to its
    // reset state (write-locked, idle).
    void LoadEEPROM(const std::array<uint8, 128> &cells);

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

    // Time passing for the 93C46's internal write/erase cycles. While one runs, DO reads low once the chip is selected
    // and a new command is refused. Call it with the emulated time since the last call (any granularity; the cycles
    // last 1 to 8 ms). Without it a write never completes.
    void AdvanceTime(uint64_t nanoseconds);

    void SaveState(STVIOBoardState &state) const;
    // Whether LoadState would take the state
    bool ValidateState(const STVIOBoardState &state) const;
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

    // Pressed bits per player: 0-7 are the PORTA/PORTB byte (buttons 1-4, then down, up, right, left), 8-10 the player's
    // buttons 4-6 of a six-button game (PORTF, bits 0-2 for player 1 and 4-6 for player 2)
    uint16 m_playerButtons[2] = {};
    bool   m_playerStart[2] = {};
    bool   m_service = false;
    bool   m_test = false;

    // 93C46 16-bit serial EEPROM driven by SMPC PDR1/PDR2. Follows MAME's eeprom_serial_93cxx_device
    // (src/devices/machine/eepromser.cpp, eeprom.cpp) in its 16-bit, 64-cell, non-streaming configuration.
    struct SerialEEPROM {
        enum class State : uint8 {
            Reset,      // STATE_IN_RESET
            WaitStart,  // STATE_WAIT_FOR_START_BIT
            Command,    // STATE_WAIT_FOR_COMMAND
            Reading,    // STATE_READING_DATA
            Writing,    // STATE_WAIT_FOR_DATA
            Completion  // STATE_WAIT_FOR_COMPLETION
        };

        State state = State::Reset;
        bool cs = false;
        bool clk = false;
        bool di = false;
        bool locked = true;
        bool writeAll = false; // the command waiting for data is WRAL
        uint32_t commandAccum = 0;
        uint32_t shiftReg = 0;
        unsigned bits = 0;
        unsigned address = 0;
        uint64_t busyNs = 0;
        std::array<uint8, 128> data{};

        static constexpr unsigned kAddressBits = 6;
        static constexpr unsigned kDataBits = 16;
        static constexpr unsigned kCells = 1u << kAddressBits;

        // Operation times from eeprom_base_device's constructor
        static constexpr uint64_t kWriteNs = 1'750'000;
        static constexpr uint64_t kWriteAllNs = 8'000'000;
        static constexpr uint64_t kEraseNs = 1'000'000;
        static constexpr uint64_t kEraseAllNs = 8'000'000;

        void Load(const std::array<uint8, 128> &src);
        void Reset();
        void SetCS(bool value);
        void SetCLK(bool value);
        void SetDI(bool value);
        void AdvanceTime(uint64_t nanoseconds);
        bool Ready() const { return busyNs == 0; }
        // DO as seen through PDR2 (eeprom_serial_93cxx_device::do_read)
        bool GetDO() const;

    private:
        void ClockRising();
        void ExecuteCommand();
        void ExecuteWriteCommand();
        uint16_t ReadWord(unsigned addr) const;
        void WriteWord(unsigned addr, uint16_t value);
    };
    SerialEEPROM m_serialEEPROM;
};

} // namespace brimir::stv
