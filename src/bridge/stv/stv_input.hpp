// Brimir - ST-V arcade controls on the RetroPad
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#pragma once

#include "stv_game_db.hpp"

#include <cstdint>

namespace brimir::stv {

// One player's controls for a frame, pressed = 1, in the bit layout of MAME's ST-V input ports (src/mame/sega/stv.cpp,
// STV_PLAYER_INPUTS in INPUT_PORTS_START( stv ) and the stv6b modifications). The I/O board turns it into the active-low
// PORTA/PORTB (players 1 and 2), PORTF and PORTC bits.
//
// directions: the PORTA/PORTB bits 4-7: kDown, kUp, kRight, kLeft.
// buttons:    bits 0-3 are the PORTA/PORTB bits 0-3 (BUTTON1..BUTTON4 of `stv`); bits 4-6 are BUTTON4..BUTTON6 of
//             `stv6b`, which MAME takes from PORTF (bits 0-2 for player 1, bits 4-6 for player 2).
struct STVInputFrame {
    uint8_t directions = 0;
    uint8_t buttons = 0;
    bool start = false;   // PORTC bit 4 (player 1), bit 5 (player 2)
    bool coin = false;    // PORTC bit 0 (player 1), bit 1 (player 2); the I/O board counts one coin per press
    bool service = false; // PORTC bit 3 (SERVICE1), player 1 only
    bool test = false;    // PORTC bit 2 (the test switch), player 1 only
};

inline constexpr uint8_t kSTVDown = 0x10;
inline constexpr uint8_t kSTVUp = 0x20;
inline constexpr uint8_t kSTVRight = 0x40;
inline constexpr uint8_t kSTVLeft = 0x80;

inline constexpr uint8_t kSTVButton1 = 0x01;
inline constexpr uint8_t kSTVButton2 = 0x02;
inline constexpr uint8_t kSTVButton3 = 0x04;
inline constexpr uint8_t kSTVButton4 = 0x08;      // `stv`: PORTA bit 3 (Radiant Silvergun)
inline constexpr uint8_t kSTVExtraButton4 = 0x10; // `stv6b`: PORTF bit 0 (Golden Axe: The Duel)
inline constexpr uint8_t kSTVExtraButton5 = 0x20;
inline constexpr uint8_t kSTVExtraButton6 = 0x40;

// `retropadMask` has the RETRO_DEVICE_ID_JOYPAD_* bit positions (bit n = button n pressed), with the left stick already
// folded into the D-pad bits (AddStickToDPad).
//   D-pad          directions
//   B, A, Y        buttons 1-3
//   X, L, R        buttons 4-6 of the six-button games; X is button 4 for Radiant Silvergun, as the `stv` ports
//                  MAME gives it have four buttons
//   Start          Start
//   Select         coin
//   L3, R3         service, test
STVInputFrame MapSTVInput(uint16_t retropadMask, Controls controls);

// The left stick (RETRO_DEVICE_ANALOG, -0x8000..0x7FFF) as D-pad presses: past half of its travel.
inline constexpr int kSTVStickThreshold = 0x4000;
uint16_t AddStickToDPad(uint16_t retropadMask, int16_t x, int16_t y);

} // namespace brimir::stv
