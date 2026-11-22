// Brimir - ST-V arcade controls on the RetroPad
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "stv_input.hpp"

#include <libretro.h>

namespace brimir::stv {

namespace {

constexpr bool Pressed(uint16_t mask, unsigned id) {
    return (mask & (1u << id)) != 0;
}

} // namespace

STVInputFrame MapSTVInput(uint16_t retropadMask, Controls controls) {
    STVInputFrame frame;

    if (Pressed(retropadMask, RETRO_DEVICE_ID_JOYPAD_DOWN)) frame.directions |= kSTVDown;
    if (Pressed(retropadMask, RETRO_DEVICE_ID_JOYPAD_UP)) frame.directions |= kSTVUp;
    if (Pressed(retropadMask, RETRO_DEVICE_ID_JOYPAD_RIGHT)) frame.directions |= kSTVRight;
    if (Pressed(retropadMask, RETRO_DEVICE_ID_JOYPAD_LEFT)) frame.directions |= kSTVLeft;

    // Buttons 1-3 are the same on every game: PORTA/PORTB bits 0-2 (MAME: BUTTON1..BUTTON3 of `stv`)
    if (Pressed(retropadMask, RETRO_DEVICE_ID_JOYPAD_B)) frame.buttons |= kSTVButton1;
    if (Pressed(retropadMask, RETRO_DEVICE_ID_JOYPAD_A)) frame.buttons |= kSTVButton2;
    if (Pressed(retropadMask, RETRO_DEVICE_ID_JOYPAD_Y)) frame.buttons |= kSTVButton3;

    switch (controls) {
    case Controls::ThreeButtons: break;
    case Controls::SixButtons:
        // MAME's stv6b (Golden Axe: The Duel): PORTA/PORTB bit 3 is unused, buttons 4-6 are PORTF
        if (Pressed(retropadMask, RETRO_DEVICE_ID_JOYPAD_X)) frame.buttons |= kSTVExtraButton4;
        if (Pressed(retropadMask, RETRO_DEVICE_ID_JOYPAD_L)) frame.buttons |= kSTVExtraButton5;
        if (Pressed(retropadMask, RETRO_DEVICE_ID_JOYPAD_R)) frame.buttons |= kSTVExtraButton6;
        break;
    case Controls::RadiantSilvergun:
        // MAME gives rsgun the plain `stv` ports (GAME line: rsgun ... stv, stv_state), BUTTON1..BUTTON4
        if (Pressed(retropadMask, RETRO_DEVICE_ID_JOYPAD_X)) frame.buttons |= kSTVButton4;
        break;
    }

    frame.start = Pressed(retropadMask, RETRO_DEVICE_ID_JOYPAD_START);
    frame.coin = Pressed(retropadMask, RETRO_DEVICE_ID_JOYPAD_SELECT);
    frame.service = Pressed(retropadMask, RETRO_DEVICE_ID_JOYPAD_L3);
    frame.test = Pressed(retropadMask, RETRO_DEVICE_ID_JOYPAD_R3);
    return frame;
}

uint16_t AddStickToDPad(uint16_t retropadMask, int16_t x, int16_t y) {
    if (x >= kSTVStickThreshold) retropadMask |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (x <= -kSTVStickThreshold) retropadMask |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (y >= kSTVStickThreshold) retropadMask |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (y <= -kSTVStickThreshold) retropadMask |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
    return retropadMask;
}

} // namespace brimir::stv
