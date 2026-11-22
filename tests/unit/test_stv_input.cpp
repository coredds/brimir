// ST-V controls: the RetroPad to MAME's ST-V input port bits, and what the I/O board makes of them

#include "catch_amalgamated.hpp"

#include <libretro.h>

#include <stv/stv_input.hpp>
#include <stv/stv_io.hpp>

#include <cstdint>

using namespace brimir::stv;

namespace {

constexpr uint16_t Bit(unsigned id) {
    return static_cast<uint16_t>(1u << id);
}

constexpr uint16_t kB = Bit(RETRO_DEVICE_ID_JOYPAD_B);
constexpr uint16_t kA = Bit(RETRO_DEVICE_ID_JOYPAD_A);
constexpr uint16_t kY = Bit(RETRO_DEVICE_ID_JOYPAD_Y);
constexpr uint16_t kX = Bit(RETRO_DEVICE_ID_JOYPAD_X);
constexpr uint16_t kL = Bit(RETRO_DEVICE_ID_JOYPAD_L);
constexpr uint16_t kR = Bit(RETRO_DEVICE_ID_JOYPAD_R);
constexpr uint16_t kL2 = Bit(RETRO_DEVICE_ID_JOYPAD_L2);
constexpr uint16_t kR2 = Bit(RETRO_DEVICE_ID_JOYPAD_R2);
constexpr uint16_t kStart = Bit(RETRO_DEVICE_ID_JOYPAD_START);
constexpr uint16_t kSelect = Bit(RETRO_DEVICE_ID_JOYPAD_SELECT);
constexpr uint16_t kL3 = Bit(RETRO_DEVICE_ID_JOYPAD_L3);
constexpr uint16_t kR3 = Bit(RETRO_DEVICE_ID_JOYPAD_R3);
constexpr uint16_t kUpBit = Bit(RETRO_DEVICE_ID_JOYPAD_UP);
constexpr uint16_t kDownBit = Bit(RETRO_DEVICE_ID_JOYPAD_DOWN);
constexpr uint16_t kLeftBit = Bit(RETRO_DEVICE_ID_JOYPAD_LEFT);
constexpr uint16_t kRightBit = Bit(RETRO_DEVICE_ID_JOYPAD_RIGHT);

// IOGA reads through the I/O board: PORTA, PORTB, PORTC, PORTE, PORTF (active low)
uint8_t ReadPortA(STVIOBoard &io) { return io.ReadIOGAByte(0x400000); }
uint8_t ReadPortB(STVIOBoard &io) { return io.ReadIOGAByte(0x400002); }
uint8_t ReadPortC(STVIOBoard &io) { return io.ReadIOGAByte(0x400004); }
uint8_t ReadPortE(STVIOBoard &io) { return io.ReadIOGAByte(0x400008); }
uint8_t ReadPortF(STVIOBoard &io) { return io.ReadIOGAByte(0x40000A); }

STVIOBoard MakeBoard() {
    STVIOBoard io;
    io.Reset(true);
    io.SetSTVMode(true);
    return io;
}

} // namespace

TEST_CASE("ST-V input: three-button games map B, A, Y to buttons 1-3", "[stv]") {
    for (const Controls controls : {Controls::ThreeButtons, Controls::RadiantSilvergun}) {
        CHECK(MapSTVInput(kB, controls).buttons == kSTVButton1);
        CHECK(MapSTVInput(kA, controls).buttons == kSTVButton2);
        CHECK(MapSTVInput(kY, controls).buttons == kSTVButton3);
        CHECK(MapSTVInput(kB | kA | kY, controls).buttons == (kSTVButton1 | kSTVButton2 | kSTVButton3));
        CHECK(MapSTVInput(0, controls).buttons == 0);
        CHECK(MapSTVInput(0, controls).directions == 0);
    }
    // the cabinet has no more buttons: nothing else reaches the board
    for (const uint16_t other : {kX, kL, kR, kL2, kR2}) {
        CHECK(MapSTVInput(other, Controls::ThreeButtons).buttons == 0);
    }
}

TEST_CASE("ST-V input: six-button games map B, A, Y, X, L, R to buttons 1-6", "[stv]") {
    const auto map = [](uint16_t mask) { return MapSTVInput(mask, Controls::SixButtons).buttons; };
    CHECK(map(kB) == kSTVButton1);
    CHECK(map(kA) == kSTVButton2);
    CHECK(map(kY) == kSTVButton3);
    // MAME's stv6b: PORTA bit 3 is unused, buttons 4-6 come from PORTF
    CHECK(map(kX) == kSTVExtraButton4);
    CHECK(map(kL) == kSTVExtraButton5);
    CHECK(map(kR) == kSTVExtraButton6);
    CHECK(map(kL2 | kR2) == 0);
    CHECK((map(kB | kA | kY | kX | kL | kR) & kSTVButton4) == 0);
}

TEST_CASE("ST-V input: Radiant Silvergun has the four buttons of MAME's stv ports", "[stv]") {
    CHECK(MapSTVInput(kX, Controls::RadiantSilvergun).buttons == kSTVButton4);
    CHECK(MapSTVInput(kL | kR, Controls::RadiantSilvergun).buttons == 0);
}

TEST_CASE("ST-V input: D-pad, Start, coin, service and test", "[stv]") {
    for (const Controls controls : {Controls::ThreeButtons, Controls::SixButtons, Controls::RadiantSilvergun}) {
        // MAME's STV_PLAYER_INPUTS: 0x10 down, 0x20 up, 0x40 right, 0x80 left
        CHECK(MapSTVInput(kDownBit, controls).directions == 0x10);
        CHECK(MapSTVInput(kUpBit, controls).directions == 0x20);
        CHECK(MapSTVInput(kRightBit, controls).directions == 0x40);
        CHECK(MapSTVInput(kLeftBit, controls).directions == 0x80);
        CHECK(MapSTVInput(kUpBit | kLeftBit, controls).directions == 0xA0);

        STVInputFrame f = MapSTVInput(kStart, controls);
        CHECK(f.start);
        CHECK(!f.coin);
        CHECK(!f.service);
        CHECK(!f.test);
        f = MapSTVInput(kSelect, controls);
        CHECK(f.coin);
        CHECK(!f.start);
        f = MapSTVInput(kL3, controls);
        CHECK(f.service);
        CHECK(!f.test);
        f = MapSTVInput(kR3, controls);
        CHECK(f.test);
        CHECK(!f.service);
        f = MapSTVInput(0, controls);
        CHECK(!f.start);
        CHECK(!f.coin);
        CHECK(!f.service);
        CHECK(!f.test);
    }
}

TEST_CASE("ST-V input: the left stick folds into the D-pad past half travel", "[stv]") {
    CHECK(AddStickToDPad(0, 0, 0) == 0);
    CHECK(AddStickToDPad(0, kSTVStickThreshold - 1, -(kSTVStickThreshold - 1)) == 0);
    CHECK(AddStickToDPad(0, 0x7FFF, 0) == kRightBit);
    CHECK(AddStickToDPad(0, -0x8000, 0) == kLeftBit);
    CHECK(AddStickToDPad(0, 0, 0x7FFF) == kDownBit); // libretro's Y grows downwards
    CHECK(AddStickToDPad(0, 0, -0x8000) == kUpBit);
    CHECK(AddStickToDPad(0, 0x7FFF, -0x8000) == (kRightBit | kUpBit));
    CHECK(AddStickToDPad(kB | kLeftBit, 0x7FFF, 0) == (kB | kLeftBit | kRightBit)); // keeps what is pressed
}

TEST_CASE("ST-V input: a player's frame reaches PORTA/PORTB (and PORTF for buttons 4-6) active low", "[stv]") {
    STVIOBoard io = MakeBoard();
    io.UpdateInputs();
    CHECK(ReadPortA(io) == 0xFF);
    CHECK(ReadPortB(io) == 0xFF);
    CHECK(ReadPortE(io) == 0xFF);
    CHECK(ReadPortF(io) == 0xFF);

    io.SetPlayerInput(0, MapSTVInput(kLeftBit | kB | kY, Controls::ThreeButtons));
    io.UpdateInputs();
    CHECK(ReadPortA(io) == static_cast<uint8_t>(~(0x80 | 0x01 | 0x04)));
    CHECK(ReadPortB(io) == 0xFF);

    io.SetPlayerInput(0, {});
    io.SetPlayerInput(1, MapSTVInput(kDownBit | kA, Controls::ThreeButtons));
    io.UpdateInputs();
    CHECK(ReadPortA(io) == 0xFF);
    CHECK(ReadPortB(io) == static_cast<uint8_t>(~(0x10 | 0x02)));

    // buttons 4-6 of a six-button game: PORTF bits 0-2 for player 1, 4-6 for player 2
    io.SetPlayerInput(1, {});
    io.SetPlayerInput(0, MapSTVInput(kX | kR, Controls::SixButtons));
    io.UpdateInputs();
    CHECK(ReadPortA(io) == 0xFF);
    CHECK(ReadPortF(io) == static_cast<uint8_t>(~(0x01 | 0x04)));
    io.SetPlayerInput(0, {});
    io.SetPlayerInput(1, MapSTVInput(kL | kX, Controls::SixButtons));
    io.UpdateInputs();
    CHECK(ReadPortB(io) == 0xFF);
    CHECK(ReadPortF(io) == static_cast<uint8_t>(~(0x20 | 0x10)));

    // Radiant Silvergun's button 4 is PORTA bit 3
    io.SetPlayerInput(1, {});
    io.SetPlayerInput(0, MapSTVInput(kX, Controls::RadiantSilvergun));
    io.UpdateInputs();
    CHECK(ReadPortA(io) == static_cast<uint8_t>(~0x08));
    CHECK(ReadPortF(io) == 0xFF);

    io.SetPlayerInput(0, {});
    io.UpdateInputs();
    CHECK(ReadPortA(io) == 0xFF);
    CHECK(ReadPortF(io) == 0xFF);
}

TEST_CASE("ST-V input: Start, service and test reach PORTC; the switches come from player 1 only", "[stv]") {
    STVIOBoard io = MakeBoard();

    io.SetPlayerInput(0, MapSTVInput(kStart, Controls::ThreeButtons));
    io.UpdateInputs();
    CHECK(ReadPortC(io) == static_cast<uint8_t>(~0x10));
    io.SetPlayerInput(0, {});
    io.SetPlayerInput(1, MapSTVInput(kStart, Controls::ThreeButtons));
    io.UpdateInputs();
    CHECK(ReadPortC(io) == static_cast<uint8_t>(~0x20));

    // MAME's PORTC: bit 2 is the test switch (PORT_SERVICE_NO_TOGGLE), bit 3 SERVICE1
    io.SetPlayerInput(1, {});
    io.SetPlayerInput(0, MapSTVInput(kR3, Controls::ThreeButtons));
    io.UpdateInputs();
    CHECK(ReadPortC(io) == static_cast<uint8_t>(~0x04));
    io.SetPlayerInput(0, MapSTVInput(kL3, Controls::ThreeButtons));
    io.UpdateInputs();
    CHECK(ReadPortC(io) == static_cast<uint8_t>(~0x08));

    // player 2's L3/R3 do nothing, and do not release player 1's
    io.SetPlayerInput(0, MapSTVInput(kL3, Controls::ThreeButtons));
    io.SetPlayerInput(1, MapSTVInput(kR3, Controls::ThreeButtons));
    io.UpdateInputs();
    CHECK(ReadPortC(io) == static_cast<uint8_t>(~0x08));

    io.SetPlayerInput(0, {});
    io.SetPlayerInput(1, MapSTVInput(kL3 | kR3, Controls::ThreeButtons));
    io.UpdateInputs();
    CHECK(ReadPortC(io) == 0xFF);
}

TEST_CASE("ST-V input: Select held for many frames is one coin", "[stv]") {
    STVIOBoard io = MakeBoard();
    const STVInputFrame held = MapSTVInput(kSelect, Controls::ThreeButtons);

    const auto coinsOver = [&](const STVInputFrame &frame, unsigned player, unsigned frames, unsigned mask) {
        unsigned edges = 0;
        bool wasActive = false;
        for (unsigned i = 0; i < frames; ++i) {
            io.SetPlayerInput(player, frame);
            io.UpdateInputs();
            const bool active = (ReadPortC(io) & mask) != mask;
            edges += active && !wasActive ? 1 : 0;
            wasActive = active;
        }
        return edges;
    };

    CHECK(coinsOver(held, 0, 40, 0x01) == 1);
    CHECK(coinsOver({}, 0, 10, 0x01) == 0);
    CHECK(coinsOver(held, 0, 40, 0x01) == 1); // a new press is a new coin
    CHECK(coinsOver({}, 0, 10, 0x01) == 0);
    // player 2's coin is PORTC bit 1
    CHECK(coinsOver(held, 1, 40, 0x02) == 1);
    CHECK((ReadPortC(io) & 0x01) != 0);
}
