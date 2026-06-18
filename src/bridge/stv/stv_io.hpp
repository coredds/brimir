// Brimir - ST-V I/O board emulation (IOGA chip at 0x00400000-0x0040007F)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#pragma once

#include <ymir/core/types.hpp>
#include <ymir/sys/bus.hpp>

#include <array>

namespace brimir::stv {

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

    uint32 m_coinPending = 0;
    int64_t m_coinActiveCounter = 0;

    uint16 m_playerButtons[2] = {};
    bool   m_playerStart[2] = {};
    bool   m_service = false;
    bool   m_test = false;

    std::array<uint8, 128> m_eeprom{};
};

} // namespace brimir::stv
