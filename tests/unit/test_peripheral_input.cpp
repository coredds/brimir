// Brimir - controller peripherals: libretro input mappings, device changes and save states
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"

#include <brimir/core_wrapper.hpp>
#include <brimir/peripheral_input.hpp>
#include <brimir/savestate_codec.hpp>
#include <lz4.h>
#include <ymir/hw/smpc/peripheral/peripheral_impl_virtua_gun.hpp>
#include <ymir/sys/saturn.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace brimir;
using ymir::peripheral::Button;
using ymir::peripheral::PeripheralType;

namespace {

constexpr uint16_t Bit(unsigned id) {
    return static_cast<uint16_t>(1u << id);
}

// Brimir's Control Pad conversion before this change (CoreWrapper::ConvertLibretroButtons, v0.6.1), copied verbatim
// in behaviour: the reference the new mapping must match bit for bit.
Button OriginalControlPad(uint16_t retroButtons) {
    Button b = Button::Default;
    if (retroButtons & (1 << 4)) b &= ~Button::Up;      // UP
    if (retroButtons & (1 << 5)) b &= ~Button::Down;    // DOWN
    if (retroButtons & (1 << 6)) b &= ~Button::Left;    // LEFT
    if (retroButtons & (1 << 7)) b &= ~Button::Right;   // RIGHT
    if (retroButtons & (1 << 0)) b &= ~Button::A;       // B -> A
    if (retroButtons & (1 << 8)) b &= ~Button::B;       // A -> B
    if (retroButtons & (1 << 1)) b &= ~Button::C;       // Y -> C
    if (retroButtons & (1 << 9)) b &= ~Button::X;       // X -> X
    if (retroButtons & (1 << 12)) b &= ~Button::Y;      // L2 -> Y
    if (retroButtons & (1 << 13)) b &= ~Button::Z;      // R2 -> Z
    if (retroButtons & (1 << 10)) b &= ~Button::L;      // L
    if (retroButtons & (1 << 11)) b &= ~Button::R;      // R
    if (retroButtons & (1 << 3)) b &= ~Button::Start;   // START
    return b;
}

bool IsPressed(Button buttons, Button button) {
    return (buttons & button) == Button::None;
}

InputSnapshot Pad(uint16_t joypad) {
    InputSnapshot in;
    in.joypad = joypad;
    return in;
}

// Brimir 0.7.0's axis and trigger conversions (peripheral_input.cpp AxisToByte, TriggerToByte), copied verbatim: the
// reference a 0 % deadzone must match exactly.
uint8_t AxisToByte070(int32_t v, int32_t center) {
    v = std::clamp<int32_t>(v, -0x8000, 0x7FFF);
    if (v < 0) {
        return static_cast<uint8_t>(center - ((-v) * center + 0x4000) / 0x8000);
    }
    return static_cast<uint8_t>(center + (v * (255 - center) + 0x3FFF) / 0x7FFF);
}

uint8_t TriggerToByte070(int16_t analog, bool digital) {
    if (analog > 0) {
        return static_cast<uint8_t>(std::min<int32_t>(255, (int32_t{analog} * 255 + 0x3FFF) / 0x7FFF));
    }
    return digital ? 0xFF : 0x00;
}

InputSnapshot Mouse(int16_t x, int16_t y, uint8_t buttons = 0) {
    InputSnapshot in;
    in.mouseX = x;
    in.mouseY = y;
    in.mouseButtons = buttons;
    return in;
}

// One read of the mouse at the bridge level: this frame's buttons and the movement it takes.
ymir::peripheral::ShuttleMouseReport ReadMouse(const InputSnapshot& in, PortInputState& state) {
    auto report = MapShuttleMouse(in);
    TakeMouseMovement(state, report);
    return report;
}

// One INTBACK-style read of port 1's Shuttle Mouse through the core (UpdateInputs then Read): the movement as the
// Saturn gets it (x right, y up), clamped to the report as ShuttleMouse::Read does, with the overflow flags.
struct MouseRead {
    int x;
    int y;
    bool xOver;
    bool yOver;
};

MouseRead ReadPort1Mouse(CoreWrapper& core) {
    auto& peripheral = core.GetSaturn()->SMPC.GetPeripheralPort1().GetPeripheral();
    peripheral.UpdateInputs();
    std::array<uint8_t, 3> report{};
    peripheral.Read(report);
    const bool xSign = (report[0] & 0x10) != 0;
    const bool ySign = (report[0] & 0x20) != 0;
    return {xSign ? report[1] - 256 : report[1], ySign ? report[2] - 256 : report[2], (report[0] & 0x40) != 0,
            (report[0] & 0x80) != 0};
}

} // namespace

TEST_CASE("peripherals: device types and kinds", "[peripherals]") {
    CHECK(RETRO_DEVICE_SATURN_PAD == RETRO_DEVICE_JOYPAD);
    CHECK(RETRO_DEVICE_SATURN_3D_PAD == RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0));
    CHECK(RETRO_DEVICE_SATURN_WHEEL == RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1));
    CHECK(RETRO_DEVICE_SATURN_MISSION_STICK == RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 2));
    CHECK(RETRO_DEVICE_SATURN_MOUSE == RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 0));
    CHECK(RETRO_DEVICE_SATURN_VIRTUA_GUN == RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 0));

    CHECK(PeripheralKindForDevice(RETRO_DEVICE_NONE) == PeripheralKind::None);
    CHECK(PeripheralKindForDevice(RETRO_DEVICE_SATURN_PAD) == PeripheralKind::ControlPad);
    CHECK(PeripheralKindForDevice(RETRO_DEVICE_SATURN_3D_PAD) == PeripheralKind::AnalogPad);
    CHECK(PeripheralKindForDevice(RETRO_DEVICE_SATURN_WHEEL) == PeripheralKind::ArcadeRacer);
    CHECK(PeripheralKindForDevice(RETRO_DEVICE_SATURN_MISSION_STICK) == PeripheralKind::MissionStick);
    CHECK(PeripheralKindForDevice(RETRO_DEVICE_SATURN_MOUSE) == PeripheralKind::ShuttleMouse);
    CHECK(PeripheralKindForDevice(RETRO_DEVICE_SATURN_VIRTUA_GUN) == PeripheralKind::VirtuaGun);
    // Other types by their base class
    // Plain RETRO_DEVICE_ANALOG ("RetroPad w/ Analog", saved by configurations of v0.6.x) stays the Control Pad
    CHECK(PeripheralKindForDevice(RETRO_DEVICE_ANALOG) == PeripheralKind::ControlPad);
    CHECK(PeripheralKindForDevice(RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 7)) == PeripheralKind::ControlPad);
    CHECK(PeripheralKindForDevice(RETRO_DEVICE_MOUSE) == PeripheralKind::ShuttleMouse);
    CHECK(PeripheralKindForDevice(RETRO_DEVICE_LIGHTGUN) == PeripheralKind::VirtuaGun);
    CHECK(PeripheralKindForDevice(RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 5)) == PeripheralKind::ControlPad);
    CHECK(PeripheralKindForDevice(RETRO_DEVICE_KEYBOARD) == PeripheralKind::ControlPad);
}

TEST_CASE("peripherals: Control Pad mapping equals the original table bit for bit", "[peripherals]") {
    for (uint32_t mask = 0; mask <= 0xFFFF; ++mask) {
        const auto report = MapControlPad(Pad(static_cast<uint16_t>(mask)));
        if (report.buttons != OriginalControlPad(static_cast<uint16_t>(mask))) {
            FAIL("mask " << mask);
        }
    }
    // Spot checks of the layout
    CHECK(IsPressed(MapControlPad(Pad(Bit(RETRO_DEVICE_ID_JOYPAD_B))).buttons, Button::A));
    CHECK(IsPressed(MapControlPad(Pad(Bit(RETRO_DEVICE_ID_JOYPAD_L2))).buttons, Button::Y));
    CHECK(MapControlPad(Pad(Bit(RETRO_DEVICE_ID_JOYPAD_SELECT))).buttons == Button::Default);
}

TEST_CASE("peripherals: 3D Control Pad mapping", "[peripherals]") {
    PortInputState state;
    SECTION("neutral") {
        const auto r = MapAnalogPad(InputSnapshot{}, state);
        CHECK(r.buttons == Button::Default);
        CHECK(r.analog);
        CHECK(r.x == 0x80);
        CHECK(r.y == 0x80);
        CHECK(r.l == 0x00);
        CHECK(r.r == 0x00);
    }
    SECTION("stick and triggers scaled to Ymir's ranges") {
        InputSnapshot in;
        in.leftX = -0x8000;
        in.leftY = 0x7FFF;
        in.l2 = 0x7FFF;
        in.r2 = 0x4000;
        auto r = MapAnalogPad(in, state);
        CHECK(r.x == 0x00);
        CHECK(r.y == 0xFF);
        CHECK(r.l == 0xFF);
        CHECK(r.r == 0x80);
        in.leftX = 0x7FFF;
        in.leftY = -0x8000;
        in.l2 = 0;
        in.r2 = 1;
        r = MapAnalogPad(in, state);
        CHECK(r.x == 0xFF);
        CHECK(r.y == 0x00);
        CHECK(r.l == 0x00);
        CHECK(r.r == 0x00);
        // A frontend without analog triggers: the digital button is a full press
        in.r2 = 0;
        in.joypad = Bit(RETRO_DEVICE_ID_JOYPAD_R2);
        CHECK(MapAnalogPad(in, state).r == 0xFF);
    }
    SECTION("buttons follow Beetle Saturn's layout") {
        const struct {
            unsigned id;
            Button button;
        } layout[] = {
            {RETRO_DEVICE_ID_JOYPAD_B, Button::A},     {RETRO_DEVICE_ID_JOYPAD_A, Button::B},
            {RETRO_DEVICE_ID_JOYPAD_R, Button::C},     {RETRO_DEVICE_ID_JOYPAD_Y, Button::X},
            {RETRO_DEVICE_ID_JOYPAD_X, Button::Y},     {RETRO_DEVICE_ID_JOYPAD_L, Button::Z},
            {RETRO_DEVICE_ID_JOYPAD_START, Button::Start}, {RETRO_DEVICE_ID_JOYPAD_UP, Button::Up},
            {RETRO_DEVICE_ID_JOYPAD_DOWN, Button::Down},   {RETRO_DEVICE_ID_JOYPAD_LEFT, Button::Left},
            {RETRO_DEVICE_ID_JOYPAD_RIGHT, Button::Right},
        };
        for (const auto& e : layout) {
            CAPTURE(e.id);
            CHECK(MapAnalogPad(Pad(Bit(e.id)), state).buttons == (Button::Default & ~e.button));
        }
    }
    SECTION("Select switches the mode on each press") {
        AdvancePortInput(PeripheralKind::AnalogPad, Pad(Bit(RETRO_DEVICE_ID_JOYPAD_SELECT)), state);
        CHECK_FALSE(state.analogMode);
        CHECK_FALSE(MapAnalogPad(Pad(Bit(RETRO_DEVICE_ID_JOYPAD_SELECT)), state).analog);
        AdvancePortInput(PeripheralKind::AnalogPad, Pad(Bit(RETRO_DEVICE_ID_JOYPAD_SELECT)), state); // held
        CHECK_FALSE(state.analogMode);
        AdvancePortInput(PeripheralKind::AnalogPad, Pad(0), state);
        AdvancePortInput(PeripheralKind::AnalogPad, Pad(Bit(RETRO_DEVICE_ID_JOYPAD_SELECT)), state);
        CHECK(state.analogMode);
        // Not on other devices
        PortInputState pad;
        AdvancePortInput(PeripheralKind::ControlPad, Pad(Bit(RETRO_DEVICE_ID_JOYPAD_SELECT)), pad);
        CHECK(pad.analogMode);
    }
}

TEST_CASE("peripherals: Arcade Racer mapping", "[peripherals]") {
    InputSnapshot in;
    CHECK(MapArcadeRacer(in).wheel == 0x7F);
    CHECK(MapArcadeRacer(in).buttons == Button::Default);
    in.leftX = -0x8000;
    CHECK(MapArcadeRacer(in).wheel == 0x00);
    in.leftX = 0x7FFF;
    CHECK(MapArcadeRacer(in).wheel == 0xFF);
    in.leftX = 0x4000;
    CHECK(MapArcadeRacer(in).wheel == 0xBF);
    // Shift paddles on L2/R2 (and the D-pad's up/down): the shifter is Up/Down
    CHECK(IsPressed(MapArcadeRacer(Pad(Bit(RETRO_DEVICE_ID_JOYPAD_L2))).buttons, Button::Up));
    CHECK(IsPressed(MapArcadeRacer(Pad(Bit(RETRO_DEVICE_ID_JOYPAD_R2))).buttons, Button::Down));
    CHECK(IsPressed(MapArcadeRacer(Pad(Bit(RETRO_DEVICE_ID_JOYPAD_DOWN))).buttons, Button::Down));
    CHECK(MapArcadeRacer(Pad(Bit(RETRO_DEVICE_ID_JOYPAD_LEFT))).buttons == Button::Default);
    CHECK(IsPressed(MapArcadeRacer(Pad(Bit(RETRO_DEVICE_ID_JOYPAD_B))).buttons, Button::A));
    CHECK(IsPressed(MapArcadeRacer(Pad(Bit(RETRO_DEVICE_ID_JOYPAD_R))).buttons, Button::C));
    CHECK(IsPressed(MapArcadeRacer(Pad(Bit(RETRO_DEVICE_ID_JOYPAD_L))).buttons, Button::Z));
}

TEST_CASE("peripherals: Mission Stick mapping", "[peripherals]") {
    PortInputState state;
    InputSnapshot in;
    auto r = MapMissionStick(in, state);
    CHECK_FALSE(r.sixAxis);
    CHECK(r.x1 == 0x7F);
    CHECK(r.y1 == 0x7F);
    CHECK(r.z1 == 0x7F);
    in.leftX = 0x7FFF;
    in.leftY = -0x8000;
    in.rightY = -0x8000; // right stick up: full throttle
    r = MapMissionStick(in, state);
    CHECK(r.x1 == 0xFF);
    CHECK(r.y1 == 0x00);
    CHECK(r.z1 == 0xFF);
    in.rightY = 0x7FFF;
    CHECK(MapMissionStick(in, state).z1 == 0x00);
    CHECK(IsPressed(MapMissionStick(Pad(Bit(RETRO_DEVICE_ID_JOYPAD_L2)), state).buttons, Button::L));
    CHECK(IsPressed(MapMissionStick(Pad(Bit(RETRO_DEVICE_ID_JOYPAD_R2)), state).buttons, Button::R));

    SECTION("R3 latches the throttle, a second press releases it") {
        InputSnapshot latch;
        latch.rightY = -0x4000;
        latch.joypad = Bit(RETRO_DEVICE_ID_JOYPAD_R3);
        AdvancePortInput(PeripheralKind::MissionStick, latch, state);
        REQUIRE(state.throttleLatched);
        const uint8_t held = MapMissionStick(latch, state).z1;
        CHECK(held == 0xBF);
        InputSnapshot moved;
        moved.rightY = 0x7FFF;
        AdvancePortInput(PeripheralKind::MissionStick, moved, state);
        CHECK(MapMissionStick(moved, state).z1 == held);
        moved.joypad = Bit(RETRO_DEVICE_ID_JOYPAD_R3);
        AdvancePortInput(PeripheralKind::MissionStick, moved, state);
        CHECK_FALSE(state.throttleLatched);
        CHECK(MapMissionStick(moved, state).z1 == 0x00);
    }
}

TEST_CASE("peripherals: Shuttle Mouse mapping", "[peripherals]") {
    PortInputState state;
    InputSnapshot in = Mouse(-200, 12, InputSnapshot::kMouseLeft | InputSnapshot::kMouseMiddle);
    AdvancePortInput(PeripheralKind::ShuttleMouse, in, state);
    auto r = MapShuttleMouse(in);
    CHECK(r.x == 0); // the movement comes with each read
    CHECK(r.y == 0);
    TakeMouseMovement(state, r);
    CHECK(r.x == -200);
    CHECK(r.y == 12); // libretro: down is positive, as Ymir's report (which inverts it for the Saturn)
    CHECK(r.left);
    CHECK(r.middle);
    CHECK_FALSE(r.right);
    CHECK_FALSE(r.start);
    in.mouseButtons = InputSnapshot::kMouseRight | InputSnapshot::kMouseButton5;
    r = MapShuttleMouse(in);
    CHECK(r.right);
    CHECK(r.start);
    CHECK_FALSE(r.left);
    // Other devices build up no movement
    PortInputState pad;
    AdvancePortInput(PeripheralKind::ControlPad, Mouse(50, 50), pad);
    CHECK(pad.mouseX == 0);
    CHECK(pad.mouseY == 0);
}

TEST_CASE("mouse: two reads in one frame don't double movement", "[peripherals][mouse]") {
    SECTION("bridge") {
        PortInputState state;
        AdvancePortInput(PeripheralKind::ShuttleMouse, Mouse(10, -7), state);
        auto r = ReadMouse(Mouse(10, -7), state);
        CHECK(r.x == 10);
        CHECK(r.y == -7);
        r = ReadMouse(Mouse(10, -7), state);
        CHECK(r.x == 0);
        CHECK(r.y == 0);
    }
    SECTION("INTBACK reads through the core") {
        CoreWrapper core;
        REQUIRE(core.Initialize());
        core.SetPeripheral(0, PeripheralKind::ShuttleMouse);
        core.SetPeripheralInput(0, Mouse(10, -7));
        auto r = ReadPort1Mouse(core);
        CHECK(r.x == 10);
        CHECK(r.y == 7); // the Saturn's y is up
        r = ReadPort1Mouse(core);
        CHECK(r.x == 0);
        CHECK(r.y == 0);
    }
}

TEST_CASE("mouse: unread movement carries to the next read", "[peripherals][mouse]") {
    PortInputState state;
    AdvancePortInput(PeripheralKind::ShuttleMouse, Mouse(10, 3), state);
    AdvancePortInput(PeripheralKind::ShuttleMouse, Mouse(-4, 3), state); // a frame without a read
    AdvancePortInput(PeripheralKind::ShuttleMouse, Mouse(20, 3), state);
    auto r = ReadMouse(Mouse(20, 3), state);
    CHECK(r.x == 26);
    CHECK(r.y == 9);

    CoreWrapper core;
    REQUIRE(core.Initialize());
    core.SetPeripheral(0, PeripheralKind::ShuttleMouse);
    core.SetPeripheralInput(0, Mouse(10, 3));
    core.SetPeripheralInput(0, Mouse(20, 3));
    const auto read = ReadPort1Mouse(core);
    CHECK(read.x == 30);
    CHECK(read.y == -6);
}

TEST_CASE("mouse: remainder beyond the report range carries over", "[peripherals][mouse]") {
    PortInputState state;
    AdvancePortInput(PeripheralKind::ShuttleMouse, Mouse(400, -300), state);
    auto r = ReadMouse(InputSnapshot{}, state);
    CHECK(r.x == 255);  // the report's largest right movement
    CHECK(r.y == -255); // up 255: the Saturn's +255
    r = ReadMouse(InputSnapshot{}, state);
    CHECK(r.x == 145);
    CHECK(r.y == -45);
    r = ReadMouse(InputSnapshot{}, state);
    CHECK(r.x == 0);
    CHECK(r.y == 0);
    // The other direction reaches -256 (left) and +256 (down: the Saturn's -256)
    AdvancePortInput(PeripheralKind::ShuttleMouse, Mouse(-400, 300), state);
    r = ReadMouse(InputSnapshot{}, state);
    CHECK(r.x == -256);
    CHECK(r.y == 256);
    r = ReadMouse(InputSnapshot{}, state);
    CHECK(r.x == -144);
    CHECK(r.y == 44);
    // Through the core: in range, so no overflow flag; the remainder comes with the next read
    CoreWrapper core;
    REQUIRE(core.Initialize());
    core.SetPeripheral(0, PeripheralKind::ShuttleMouse);
    core.SetPeripheralInput(0, Mouse(400, 300));
    auto read = ReadPort1Mouse(core);
    CHECK(read.x == 255);
    CHECK(read.y == -256);
    CHECK_FALSE(read.xOver);
    CHECK_FALSE(read.yOver);
    read = ReadPort1Mouse(core);
    CHECK(read.x == 145);
    CHECK(read.y == -44);
    // The movement kept between reads is bounded
    PortInputState bounded;
    for (int i = 0; i < 200; ++i) {
        AdvancePortInput(PeripheralKind::ShuttleMouse, Mouse(0x7FFF, -0x8000), bounded, InputSettings{0, 0, 200});
    }
    CHECK(bounded.mouseX == kMouseMovementLimit);
    CHECK(bounded.mouseY == -kMouseMovementLimit);
}

TEST_CASE("mouse: accumulation survives rollback and resets on device change", "[peripherals][mouse][savestate]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());
    core.SetPeripheral(0, PeripheralKind::ShuttleMouse);
    core.SetPeripheralInput(0, Mouse(400, -10));
    REQUIRE(ReadPort1Mouse(core).x == 255);
    std::vector<uint8_t> state(core.GetStateSize());
    REQUIRE(core.SaveState(state.data(), state.size()));
    REQUIRE(core.GetPortInputState(0).mouseX == 145 * kMouseMovementUnit);
    REQUIRE(ReadPort1Mouse(core).x == 145);

    SECTION("in place (run-ahead, rewind)") {
        core.SetPeripheralInput(0, Mouse(33, 0)); // movement of the frame run ahead
        REQUIRE(core.LoadState(state.data(), state.size()));
        CHECK(core.GetPortInputState(0).mouseX == 145 * kMouseMovementUnit);
        const auto r = ReadPort1Mouse(core);
        CHECK(r.x == 145);
        CHECK(r.y == 0);
        CHECK(ReadPort1Mouse(core).x == 0);
    }
    SECTION("into a fresh instance (netplay)") {
        CoreWrapper fresh;
        REQUIRE(fresh.Initialize());
        fresh.SetPeripheral(0, PeripheralKind::ShuttleMouse);
        fresh.SetPeripheralInput(0, Mouse(33, 21));
        REQUIRE(fresh.LoadState(state.data(), state.size()));
        CHECK(ReadPort1Mouse(fresh).x == 145);
    }
    SECTION("a device change clears it") {
        core.SetPeripheralInput(0, Mouse(77, 0));
        core.SetPeripheral(0, PeripheralKind::ControlPad);
        core.SetPeripheral(0, PeripheralKind::ShuttleMouse);
        CHECK(core.GetPortInputState(0).mouseX == 0);
        CHECK(ReadPort1Mouse(core).x == 0);
        // A state saved with the mouse loads into a port with another device: that device's state (none built up)
        core.SetPeripheral(0, PeripheralKind::AnalogPad);
        REQUIRE(core.LoadState(state.data(), state.size()));
        CHECK(core.GetPortInputState(0).mouseX == 0);
        core.SetPeripheral(0, PeripheralKind::ShuttleMouse);
        CHECK(ReadPort1Mouse(core).x == 0);
    }
    SECTION("a direct (TH/TR) read in progress keeps the movement it took") {
        // The first step of the handshake takes the report; a state saved in the middle of it continues with the
        // same movement, in place and in a fresh instance
        core.SetPeripheralInput(0, Mouse(0x5A, 0));
        ymir::Saturn& saturn = *core.GetSaturn();
        saturn.mainBus.Write<uint8>(0x010'0079, 0x60); // DDR1: TH and TR are outputs
        saturn.mainBus.Write<uint8>(0x010'0075, 0x60); // PDR1: TH high, the handshake starts over
        const auto step = [](ymir::Saturn& s, bool tr) {
            s.mainBus.Write<uint8>(0x010'0075, tr ? 0x20 : 0x00);
            return static_cast<uint8_t>(s.mainBus.Peek<uint8>(0x010'0075) & 0x0F);
        };
        bool tr = true;
        for (int i = 0; i < 3; ++i) {
            step(saturn, tr = !tr);
        }
        std::vector<uint8_t> mid(core.GetStateSize());
        REQUIRE(core.SaveState(mid.data(), mid.size()));
        const bool trAtSave = tr;
        const auto rest = [&](ymir::Saturn& s) {
            std::vector<uint8_t> nibbles;
            bool t = trAtSave;
            for (int i = 0; i < 6; ++i) {
                nibbles.push_back(step(s, t = !t));
            }
            return nibbles;
        };
        const auto expected = rest(saturn);
        CHECK(expected[2] == 0x5); // X high nibble
        CHECK(expected[3] == 0xA); // X low nibble
        core.SetPeripheralInput(0, Mouse(9, 9));
        REQUIRE(core.LoadState(mid.data(), mid.size()));
        CHECK(rest(saturn) == expected);
        CoreWrapper fresh;
        REQUIRE(fresh.Initialize());
        fresh.SetPeripheral(0, PeripheralKind::ShuttleMouse);
        fresh.SetPeripheralInput(0, Mouse(9, 9));
        REQUIRE(fresh.LoadState(mid.data(), mid.size()));
        CHECK(rest(*fresh.GetSaturn()) == expected);
        // The next handshake gets only what was built up since: nothing
        saturn.mainBus.Write<uint8>(0x010'0075, 0x60);
        REQUIRE(core.LoadState(mid.data(), mid.size()));
        (void)rest(saturn);
        saturn.mainBus.Write<uint8>(0x010'0075, 0x60);
        tr = true;
        std::vector<uint8_t> next;
        for (int i = 0; i < 9; ++i) {
            next.push_back(step(saturn, tr = !tr));
        }
        CHECK(next[5] == 0);
        CHECK(next[6] == 0);
    }
}

TEST_CASE("mouse: sensitivity scales movement", "[peripherals][mouse]") {
    const struct {
        unsigned percent;
        int x;
        int y;
    } cases[] = {{5, 5, -2}, {100, 100, -40}, {200, 200, -80}};
    for (const auto& c : cases) {
        CAPTURE(c.percent);
        PortInputState state;
        AdvancePortInput(PeripheralKind::ShuttleMouse, Mouse(100, -40), state, InputSettings{0, 0, c.percent});
        const auto r = ReadMouse(InputSnapshot{}, state);
        CHECK(r.x == c.x);
        CHECK(r.y == c.y);
    }
    // At 5 % small movements still add up (the fraction of a count is kept)
    PortInputState slow;
    for (int i = 0; i < 20; ++i) {
        AdvancePortInput(PeripheralKind::ShuttleMouse, Mouse(1, -1), slow, InputSettings{0, 0, 5});
        const auto r = ReadMouse(InputSnapshot{}, slow);
        CHECK(r.x == (i == 19 ? 1 : 0));
        CHECK(r.y == (i == 19 ? -1 : 0));
    }
    // Through the core (the option)
    CoreWrapper core;
    REQUIRE(core.Initialize());
    core.SetPeripheral(0, PeripheralKind::ShuttleMouse);
    core.SetInputSettings(InputSettings{0, 0, 200});
    core.SetPeripheralInput(0, Mouse(50, 0));
    CHECK(ReadPort1Mouse(core).x == 100);
    CHECK(core.GetInputSettings().mouseSensitivity == 200);
}

TEST_CASE("deadzone: zero equals previous mapping; continuous above threshold", "[peripherals][deadzone]") {
    PortInputState state;
    SECTION("0 %: exactly 0.7.0's mapping") {
        const InputSettings zero{0, 0, 100};
        for (int32_t v = -0x8000; v <= 0x7FFF; v += 7) {
            for (const int16_t other : {int16_t{-0x8000}, int16_t{-12345}, int16_t{0}, int16_t{77}, int16_t{0x7FFF}}) {
                InputSnapshot in;
                in.leftX = static_cast<int16_t>(v);
                in.leftY = other;
                in.rightY = other;
                const auto pad = MapAnalogPad(in, state, zero);
                const auto wheel = MapArcadeRacer(in, zero);
                const auto stick = MapMissionStick(in, state, zero);
                if (pad.x != AxisToByte070(v, 0x80) || pad.y != AxisToByte070(other, 0x80) ||
                    wheel.wheel != AxisToByte070(v, 0x7F) || stick.x1 != AxisToByte070(v, 0x7F) ||
                    stick.y1 != AxisToByte070(other, 0x7F) ||
                    stick.z1 != AxisToByte070(std::min<int32_t>(0x7FFF, -int32_t{other}), 0x7F)) {
                    FAIL("x " << v << " y " << other);
                }
                // The defaults are the same mapping
                const auto plain = MapAnalogPad(in, state);
                if (plain.x != pad.x || plain.y != pad.y) {
                    FAIL("default settings, x " << v << " y " << other);
                }
            }
        }
        for (int32_t t = 0; t <= 0x7FFF; t += 3) {
            for (const bool digital : {false, true}) {
                InputSnapshot in;
                in.l2 = static_cast<int16_t>(t);
                in.r2 = static_cast<int16_t>(0x7FFF - t);
                in.joypad = digital ? static_cast<uint16_t>(Bit(RETRO_DEVICE_ID_JOYPAD_L2) | Bit(RETRO_DEVICE_ID_JOYPAD_R2))
                                    : uint16_t{0};
                const auto pad = MapAnalogPad(in, state, zero);
                if (pad.l != TriggerToByte070(in.l2, digital) || pad.r != TriggerToByte070(in.r2, digital)) {
                    FAIL("trigger " << t << " digital " << digital);
                }
            }
        }
    }
    for (const unsigned percent : {15u, 30u}) {
        CAPTURE(percent);
        const InputSettings settings{percent, percent, 100};
        const int32_t stickEdge = 0x8000 * static_cast<int32_t>(percent) / 100;
        const int32_t triggerEdge = 0x7FFF * static_cast<int32_t>(percent) / 100;
        SECTION("stick: centred inside, rescaled continuously to the full range outside (" +
                std::to_string(percent) + " %)") {
            int last = -1;
            for (int32_t v = 0; v <= 0x7FFF; v += 16) {
                InputSnapshot in;
                in.leftX = static_cast<int16_t>(v);
                const int x = MapAnalogPad(in, state, settings).x;
                if (v <= stickEdge) {
                    if (x != 0x80) {
                        FAIL("inside the deadzone at " << v << ": " << x);
                    }
                } else if (x < last || x - last > 1) {
                    FAIL("jump at " << v << ": " << last << " -> " << x);
                }
                last = x;
            }
            InputSnapshot in;
            in.leftX = 0x7FFF;
            CHECK(MapAnalogPad(in, state, settings).x == 0xFF);
            CHECK(MapArcadeRacer(in, settings).wheel == 0xFF);
            in.leftX = -0x8000;
            in.leftY = -0x8000; // a corner: beyond the range, clamped
            CHECK(MapAnalogPad(in, state, settings).x == 0x00);
            CHECK(MapAnalogPad(in, state, settings).y == 0x00);
            CHECK(MapMissionStick(in, state, settings).x1 == 0x00);
            // Just past the edge: one step from the centre
            in = {};
            in.leftX = static_cast<int16_t>(-(stickEdge + 64));
            CHECK(MapAnalogPad(in, state, settings).x >= 0x7F);
            CHECK(MapArcadeRacer(in, settings).wheel >= 0x7E);
            // Radial: each axis inside alone, but the diagonal outside moves both axes
            const int16_t inside = static_cast<int16_t>(stickEdge * 8 / 10);
            in = {};
            in.leftX = inside;
            CHECK(MapAnalogPad(in, state, settings).x == 0x80);
            in.leftY = inside;
            const auto diagonal = MapAnalogPad(in, state, settings);
            CHECK(diagonal.x > 0x80);
            CHECK(diagonal.y > 0x80);
            const auto missionDiagonal = MapMissionStick(in, state, settings);
            CHECK(missionDiagonal.x1 > 0x7F);
            CHECK(missionDiagonal.y1 > 0x7F);
            // The throttle has no deadzone
            in = {};
            in.rightY = static_cast<int16_t>(-inside);
            CHECK(MapMissionStick(in, state, settings).z1 == AxisToByte070(inside, 0x7F));
        }
        SECTION("trigger: released inside, rescaled continuously outside (" + std::to_string(percent) + " %)") {
            int last = -1;
            for (int32_t t = 8; t <= 0x7FFF; t += 8) {
                InputSnapshot in;
                in.l2 = static_cast<int16_t>(t);
                in.joypad = Bit(RETRO_DEVICE_ID_JOYPAD_L2); // the frontend's digital L2 may already be on
                const int l = MapAnalogPad(in, state, settings).l;
                if (t <= triggerEdge) {
                    if (l != 0) {
                        FAIL("inside the deadzone at " << t << ": " << l);
                    }
                } else if (l < last || l - last > 1) {
                    FAIL("jump at " << t << ": " << last << " -> " << l);
                }
                last = l;
            }
            InputSnapshot in;
            in.r2 = 0x7FFF;
            CHECK(MapAnalogPad(in, state, settings).r == 0xFF);
            // Without analog triggers the digital button is still a full press
            in.r2 = 0;
            in.joypad = Bit(RETRO_DEVICE_ID_JOYPAD_R2);
            CHECK(MapAnalogPad(in, state, settings).r == 0xFF);
        }
    }
}

TEST_CASE("peripherals: Virtua Gun coordinates", "[peripherals]") {
    InputSnapshot in;
    in.gunOffscreen = false;
    const auto at = [&](int16_t x, int16_t y, const GunScreen& screen) {
        in.gunX = x;
        in.gunY = y;
        const auto r = MapVirtuaGun(in, screen);
        return std::array<uint32_t, 2>{r.x, r.y};
    };
    SECTION("the active Saturn resolution") {
        const GunScreen lowRes{320, 224};
        CHECK(at(-0x7FFF, -0x7FFF, lowRes) == std::array<uint32_t, 2>{0, 0});
        CHECK(at(-0x8000, -0x8000, lowRes) == std::array<uint32_t, 2>{0, 0});
        CHECK(at(0x7FFF, 0x7FFF, lowRes) == std::array<uint32_t, 2>{319, 223});
        CHECK(at(0, 0, lowRes) == std::array<uint32_t, 2>{159, 111});
        const GunScreen hiResInterlace{704, 480};
        CHECK(at(0x7FFF, 0x7FFF, hiResInterlace) == std::array<uint32_t, 2>{703, 479});
        CHECK(at(0, 0, hiResInterlace) == std::array<uint32_t, 2>{351, 239});
        const GunScreen pal{352, 256};
        CHECK(at(0x7FFF, -0x7FFF, pal) == std::array<uint32_t, 2>{351, 0});
    }
    SECTION("overscan crop and rotation") {
        const GunScreen cropped{320, 224, 32, 32};
        CHECK(at(-0x7FFF, -0x7FFF, cropped) == std::array<uint32_t, 2>{16, 16});
        CHECK(at(0x7FFF, 0x7FFF, cropped) == std::array<uint32_t, 2>{303, 207});
        // Crop guard: a frame not more than 32 pixels larger than the crop is shown whole
        const GunScreen guarded{320, 224, 300, 300};
        CHECK(at(0x7FFF, 0x7FFF, guarded) == std::array<uint32_t, 2>{319, 223});
        const GunScreen r90{320, 224, 0, 0, 90};   // shown 224 x 320
        CHECK(at(-0x7FFF, -0x7FFF, r90) == std::array<uint32_t, 2>{0, 223});
        const GunScreen r180{320, 224, 0, 0, 180};
        CHECK(at(-0x7FFF, -0x7FFF, r180) == std::array<uint32_t, 2>{319, 223});
        const GunScreen r270{320, 224, 0, 0, 270};
        CHECK(at(-0x7FFF, -0x7FFF, r270) == std::array<uint32_t, 2>{319, 0});
    }
    SECTION("off-screen: no coordinates, the trigger reloads") {
        in.gunOffscreen = true;
        in.gunTrigger = true;
        auto r = MapVirtuaGun(in, GunScreen{});
        CHECK(r.x == 0xFFFF);
        CHECK(r.y == 0xFFFF);
        CHECK(r.reload);
        CHECK_FALSE(r.trigger);
        in.gunOffscreen = false;
        r = MapVirtuaGun(in, GunScreen{});
        CHECK(r.trigger);
        CHECK_FALSE(r.reload);
        in.gunTrigger = false;
        in.gunReload = true;
        in.gunStart = true;
        r = MapVirtuaGun(in, GunScreen{});
        CHECK(r.reload);
        CHECK(r.start);
        CHECK(r.x != 0xFFFF);
    }
}

TEST_CASE("peripherals: the gun hits the pixel shown under it, for every crop and rotation", "[peripherals]") {
    // Each source pixel holds its coordinates; the bridge crops and rotates the frame as it is shown, and the gun
    // aimed at a shown pixel must map back to that source pixel.
    constexpr uint32_t kW = 320;
    constexpr uint32_t kH = 224;
    std::vector<uint32_t> frame(kW * kH);
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            frame[y * kW + x] = (y << 12) | x; // XBGR: the bridge swaps bytes 0 and 2
        }
    }
    CoreWrapper core;
    REQUIRE(core.Initialize());
    for (const int crop : {0, 16, 32}) {
        for (const int rotation : {0, 90, 180, 270}) {
            CAPTURE(crop, rotation);
            core.SetOverscanCrop(crop, crop);
            core.SetRotation(rotation);
            core.TestInjectFrame(frame.data(), kW, kH);
            const auto* shown = static_cast<const uint32_t*>(core.GetFramebuffer());
            const uint32_t w = core.GetFramebufferWidth();
            const uint32_t h = core.GetFramebufferHeight();
            const GunScreen screen{kW, kH, static_cast<uint32_t>(crop), static_cast<uint32_t>(crop),
                                   static_cast<uint32_t>(rotation)};
            for (const int16_t gx : {int16_t{-0x7FFF}, int16_t{-12345}, int16_t{0}, int16_t{777}, int16_t{0x7FFF}}) {
                for (const int16_t gy : {int16_t{-0x7FFF}, int16_t{-3000}, int16_t{1}, int16_t{20000}, int16_t{0x7FFF}}) {
                    InputSnapshot in;
                    in.gunOffscreen = false;
                    in.gunX = gx;
                    in.gunY = gy;
                    const auto r = MapVirtuaGun(in, screen);
                    const uint32_t dx = static_cast<uint32_t>((int64_t{gx} + 0x7FFF) * w / 0xFFFF);
                    const uint32_t dy = static_cast<uint32_t>((int64_t{gy} + 0x7FFF) * h / 0xFFFF);
                    const uint32_t xrgb = shown[dy * w + dx];
                    const uint32_t xbgr = (xrgb & 0x00FF00) | ((xrgb & 0xFF) << 16) | ((xrgb >> 16) & 0xFF);
                    CAPTURE(gx, gy);
                    CHECK(r.x == (xbgr & 0xFFF));
                    CHECK(r.y == (xbgr >> 12));
                }
            }
        }
    }
}

TEST_CASE("peripherals: crosshair rectangles", "[peripherals]") {
    std::array<OverlayRect, kMaxCrosshairRects> rects;
    Crosshair cross{CrosshairStyle::Cross, 0, 0, kCrosshairColors[0]};
    REQUIRE(CrosshairRects(cross, 320, 224, rects) == 4);
    // Outline first, then the colour, centred on the shown pixel under the gun (159, 111)
    CHECK(rects[0].color == kCrosshairOutline);
    CHECK(rects[2].color == kCrosshairColors[0]);
    CHECK(rects[2].x == 159 - 6);
    CHECK(rects[2].y == 111);
    CHECK(rects[2].w == 13);
    CHECK(rects[2].h == 1);
    CHECK(rects[3].x == 159);
    CHECK(rects[3].h == 13);

    Crosshair dot{CrosshairStyle::Dot, 0, 0, kCrosshairColors[1]};
    REQUIRE(CrosshairRects(dot, 320, 224, rects) == 2);
    CHECK(rects[1].w == 3);
    CHECK(rects[1].color == kCrosshairColors[1]);

    Crosshair off{CrosshairStyle::Off, 0, 0, 0};
    CHECK(CrosshairRects(off, 320, 224, rects) == 0);

    // Clipped at the corner, scaled with the height, never larger than the Vulkan path's colour regions
    Crosshair corner{CrosshairStyle::Cross, -0x7FFF, -0x7FFF, kCrosshairColors[0]};
    const size_t n = CrosshairRects(corner, 320, 224, rects);
    REQUIRE(n == 4);
    for (size_t i = 0; i < n; ++i) {
        CHECK(rects[i].x + rects[i].w <= 320);
        CHECK(rects[i].y + rects[i].h <= 224);
    }
    for (const uint32_t h : {224u, 480u, 896u, 2048u, 2816u}) {
        REQUIRE(CrosshairRects(cross, h, h, rects) == 4);
        for (size_t i = 0; i < 4; ++i) {
            CHECK(size_t{rects[i].w} * rects[i].h <= kMaxCrosshairRectPixels);
        }
    }
    REQUIRE(CrosshairRects(cross, 704, 480, rects) == 4);
    CHECK(rects[2].h == 2);

    // Drawn on an image: only around the centre
    std::vector<uint32_t> image(320 * 224, 0x123456);
    DrawCrosshair(cross, image.data(), 320, 224, 320);
    CHECK(image[111 * 320 + 159] == kCrosshairColors[0]);
    CHECK(image[111 * 320 + 159 - 7] == kCrosshairOutline);
    CHECK(image[0] == 0x123456);
    CHECK(image[111 * 320 + 159 - 9] == 0x123456);
}

TEST_CASE("peripherals: CoreWrapper connects the chosen peripheral and hands it the mapped report", "[peripherals]") {
    CoreWrapper core;
    CHECK(core.GetPeripheral(0) == PeripheralKind::ControlPad);
    core.SetPeripheral(1, PeripheralKind::MissionStick); // before Initialize: kept
    REQUIRE(core.Initialize());
    auto& port1 = core.GetSaturn()->SMPC.GetPeripheralPort1();
    auto& port2 = core.GetSaturn()->SMPC.GetPeripheralPort2();
    CHECK(port1.GetPeripheral().GetType() == PeripheralType::ControlPad);
    CHECK(port2.GetPeripheral().GetType() == PeripheralType::MissionStick);

    const struct {
        PeripheralKind kind;
        PeripheralType type;
    } kinds[] = {
        {PeripheralKind::AnalogPad, PeripheralType::AnalogPad},
        {PeripheralKind::ArcadeRacer, PeripheralType::ArcadeRacer},
        {PeripheralKind::MissionStick, PeripheralType::MissionStick},
        {PeripheralKind::ShuttleMouse, PeripheralType::ShuttleMouse},
        {PeripheralKind::VirtuaGun, PeripheralType::VirtuaGun},
        {PeripheralKind::None, PeripheralType::None},
        {PeripheralKind::ControlPad, PeripheralType::ControlPad},
    };
    for (const auto& k : kinds) {
        core.SetPeripheral(0, k.kind);
        CHECK(core.GetPeripheral(0) == k.kind);
        CHECK(port1.GetPeripheral().GetType() == k.type);
    }

    // The 3D pad reads the report of this frame's input (UpdateInputs runs before every read)
    core.SetPeripheral(0, PeripheralKind::AnalogPad);
    InputSnapshot in;
    in.leftX = 0x7FFF;
    in.joypad = Bit(RETRO_DEVICE_ID_JOYPAD_B);
    core.SetPeripheralInput(0, in);
    auto& peripheral = port1.GetPeripheral();
    peripheral.UpdateInputs();
    REQUIRE(peripheral.GetReportLength() == 6);
    std::array<uint8_t, 6> report{};
    peripheral.Read(report);
    CHECK(report[2] == 0xFF);                // AX
    CHECK(report[3] == 0x80);                // AY
    CHECK((report[0] & 0x04) == 0);          // A pressed (bit 10 of the buttons: byte 0 bit 2)
    // Select switches the pad to digital mode (2-byte report, type code 0)
    core.SetPeripheralInput(0, Pad(Bit(RETRO_DEVICE_ID_JOYPAD_SELECT)));
    peripheral.UpdateInputs();
    CHECK(peripheral.GetReportLength() == 2);
    CHECK_FALSE(core.GetPortInputState(0).analogMode);
}

TEST_CASE("peripherals: device change between frames and across state load", "[peripherals]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());
    core.SetPeripheral(0, PeripheralKind::AnalogPad);
    core.SetPeripheralInput(0, Pad(Bit(RETRO_DEVICE_ID_JOYPAD_SELECT))); // digital mode
    auto& pad = core.GetSaturn()->SMPC.GetPeripheralPort1().GetPeripheral();
    pad.UpdateInputs(); // as a game's read would: the pad takes the mode
    REQUIRE(pad.GetReportLength() == 2);
    core.RunFrame();
    std::vector<uint8_t> state(core.GetStateSize());
    REQUIRE(core.SaveState(state.data(), state.size()));

    SECTION("a state saved with the 3D pad loads with the Control Pad connected") {
        core.SetPeripheral(0, PeripheralKind::ControlPad);
        core.SetPeripheralInput(0, Pad(Bit(RETRO_DEVICE_ID_JOYPAD_START)));
        REQUIRE(core.LoadState(state.data(), state.size()));
        CHECK(core.GetPeripheral(0) == PeripheralKind::ControlPad);
        CHECK(core.GetSaturn()->SMPC.GetPeripheralPort1().GetPeripheral().GetType() == PeripheralType::ControlPad);
        // The Control Pad keeps its own input state, not the 3D pad's
        CHECK(core.GetPortInputState(0).prevButtons == Bit(RETRO_DEVICE_ID_JOYPAD_START));
        core.SetPeripheralInput(0, Pad(0));
        core.RunFrame();
        core.RunFrame();
        REQUIRE(core.SaveState(state.data(), state.size()));
    }
    SECTION("the same device gets its state back") {
        core.SetPeripheralInput(0, Pad(0));
        core.SetPeripheralInput(0, Pad(Bit(RETRO_DEVICE_ID_JOYPAD_SELECT))); // analog again
        REQUIRE(core.GetPortInputState(0).analogMode);
        pad.UpdateInputs();
        REQUIRE(pad.GetReportLength() == 6);
        REQUIRE(core.LoadState(state.data(), state.size()));
        CHECK_FALSE(core.GetPortInputState(0).analogMode);
        CHECK(core.GetPortInputState(0).prevButtons == Bit(RETRO_DEVICE_ID_JOYPAD_SELECT));
        CHECK(core.GetSaturn()->SMPC.GetPeripheralPort1().GetPeripheral().GetReportLength() == 2);
    }
    SECTION("a Control Pad state loads with every other device, both ports") {
        CoreWrapper other;
        REQUIRE(other.Initialize());
        std::vector<uint8_t> padState(other.GetStateSize());
        REQUIRE(other.SaveState(padState.data(), padState.size()));
        for (const PeripheralKind kind : {PeripheralKind::AnalogPad, PeripheralKind::ArcadeRacer,
                                          PeripheralKind::MissionStick, PeripheralKind::ShuttleMouse,
                                          PeripheralKind::VirtuaGun, PeripheralKind::None}) {
            other.SetPeripheral(0, kind);
            other.SetPeripheral(1, kind);
            REQUIRE(other.LoadState(padState.data(), padState.size()));
            CHECK(other.GetPeripheral(0) == kind);
            CHECK(other.GetPeripheral(1) == kind);
            other.SetPeripheralInput(0, InputSnapshot{});
            other.RunFrame();
        }
    }
}

namespace {

constexpr uint32_t kPDR1 = 0x010'0075;
constexpr uint32_t kDDR1 = 0x010'0079;

// One TH/TR handshake step on port 1 (TH low, TR as given): what the peripheral answers.
uint8_t Step(ymir::Saturn& saturn, bool tr) {
    saturn.mainBus.Write<uint8>(kPDR1, tr ? 0x20 : 0x00);
    return saturn.mainBus.Peek<uint8>(kPDR1);
}

// Starts a TH/TR read (TH high resets the handshake) and takes `steps` answers.
std::vector<uint8_t> Handshake(ymir::Saturn& saturn, int steps, bool& tr) {
    std::vector<uint8_t> answers;
    for (int i = 0; i < steps; ++i) {
        tr = !tr;
        answers.push_back(Step(saturn, tr));
    }
    return answers;
}

} // namespace

TEST_CASE("peripherals: a TH/TR read in progress continues the same after a state load", "[peripherals][savestate]") {
    // The 3D pad, Arcade Racer, Mission Stick and Shuttle Mouse keep their position in the Saturn ID handshake between
    // port accesses; save states hold it (version 9), so run-ahead and netplay replay a game's direct read exactly.
    for (const PeripheralKind kind : {PeripheralKind::AnalogPad, PeripheralKind::ArcadeRacer,
                                      PeripheralKind::MissionStick, PeripheralKind::ShuttleMouse}) {
        CAPTURE(PeripheralKindName(kind));
        CoreWrapper core;
        REQUIRE(core.Initialize());
        core.SetPeripheral(0, kind);
        InputSnapshot in;
        in.leftX = 0x1234;
        in.mouseX = 77;
        in.joypad = Bit(RETRO_DEVICE_ID_JOYPAD_B);
        core.SetPeripheralInput(0, in);
        ymir::Saturn& saturn = *core.GetSaturn();
        saturn.mainBus.Write<uint8>(kDDR1, 0x60); // TH and TR are outputs
        saturn.mainBus.Write<uint8>(kPDR1, 0x60); // TH high: the handshake starts over
        bool tr = true;
        Handshake(saturn, 3, tr);
        std::vector<uint8_t> state(core.GetStateSize());
        REQUIRE(core.SaveState(state.data(), state.size()));
        const bool trAtSave = tr;
        const auto expected = Handshake(saturn, 5, tr);

        // In place (run-ahead) and in a fresh core (netplay, a state from another session)
        REQUIRE(core.LoadState(state.data(), state.size()));
        tr = trAtSave;
        CHECK(Handshake(saturn, 5, tr) == expected);

        CoreWrapper fresh;
        REQUIRE(fresh.Initialize());
        fresh.SetPeripheral(0, kind);
        fresh.SetPeripheralInput(0, in);
        REQUIRE(fresh.LoadState(state.data(), state.size()));
        tr = trAtSave;
        CHECK(Handshake(*fresh.GetSaturn(), 5, tr) == expected);
    }
}

// Diagnostics for the determinism evidence (not run by default): the peripherals' state in every state_*.bin of
// $BRIMIR_STATE_DIR (vkhost --save-state-every), to see whether a run exercised the TH/TR handshake (direct port reads).
TEST_CASE("peripherals: summary of the peripherals' state in saved states", "[.][peripherals-state]") {
    const char* dir = std::getenv("BRIMIR_STATE_DIR");
    if (dir == nullptr) {
        SKIP("BRIMIR_STATE_DIR not set");
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".bin") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    auto state = std::make_unique<ymir::savestate::SaveState>();
    size_t handshake[2] = {};
    size_t modeChanges[2] = {};
    bool lastMode[2] = {};
    for (size_t f = 0; f < files.size(); ++f) {
        std::ifstream in(files[f], std::ios::binary);
        std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        REQUIRE(buf.size() > 16);
        uint32_t version = 0, payloadSize = 0, compressedSize = 0;
        std::memcpy(&version, buf.data() + 4, 4);
        std::memcpy(&payloadSize, buf.data() + 8, 4);
        std::memcpy(&compressedSize, buf.data() + 12, 4);
        std::vector<uint8_t> payload(payloadSize);
        REQUIRE(LZ4_decompress_safe(reinterpret_cast<const char*>(buf.data() + 16),
                                    reinterpret_cast<char*>(payload.data()), static_cast<int>(compressedSize),
                                    static_cast<int>(payloadSize)) == static_cast<int>(payloadSize));
        REQUIRE(DecodeSaveState(payload, *state, version));
        for (int p = 0; p < 2; ++p) {
            const auto& port = state->brimir.peripherals.ports[p];
            handshake[p] += (port.reportPos != 0 || port.tl) ? 1 : 0;
            if (f > 0 && port.analogMode != lastMode[p]) {
                ++modeChanges[p];
            }
            lastMode[p] = port.analogMode;
        }
    }
    for (int p = 0; p < 2; ++p) {
        std::printf("port %d: %zu of %zu states inside a TH/TR handshake (reportPos or TL set), %zu 3D pad mode "
                    "changes, type %u\n",
                    p + 1, handshake[p], files.size(), modeChanges[p],
                    unsigned{state->brimir.peripherals.ports[p].type});
    }
}

TEST_CASE("peripherals: the Virtua Gun's latched reload is saved", "[peripherals][savestate]") {
    ymir::peripheral::VirtuaGunReport given{};
    given.reload = true;
    given.x = 10;
    given.y = 20;
    struct Ctx {
        ymir::peripheral::VirtuaGunReport report;
    } ctx{given};
    const auto callback = ymir::peripheral::CBPeripheralReport{&ctx, [](ymir::peripheral::PeripheralReport& r, void* c) {
                                                                   r.report.virtuaGun = static_cast<Ctx*>(c)->report;
                                                               }};
    ymir::peripheral::VirtuaGun gun(callback);
    gun.UpdateInputs();
    (void)gun.WritePDR(0x00, 0x00, true); // read mode with the external latch enabled: the reload is latched
    ymir::savestate::BrimirSaveState::Peripherals::Port saved{};
    gun.SaveBrimirState(saved);
    CHECK(saved.gunReload);

    ctx.report.reload = false;
    ymir::peripheral::VirtuaGun fresh(callback);
    fresh.UpdateInputs();
    fresh.LoadBrimirState(saved);
    uint16_t x = 0;
    uint16_t y = 0;
    REQUIRE(fresh.GetExternalLatchCoordinates(x, y));
    CHECK(x == 0xFFFF); // reloading: no coordinates
    CHECK(y == 0xFFFF);
}
