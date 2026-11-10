// Brimir - libretro input to Saturn peripheral reports
// Licensed under GPL-3.0
//
// The libretro layer polls one InputSnapshot per port and frame (what the port's device type needs) and the bridge
// (CoreWrapper::SetPeripheralInput) turns it into the report of the Ymir peripheral connected to the port with the
// pure functions below. Edge-triggered controls (the 3D Control Pad's mode switch, the Mission Stick's throttle latch)
// and the Shuttle Mouse's movement built up between reads keep their state in PortInputState, which CoreWrapper holds
// per port and puts in save states.
//
// RetroPad layouts (the Control Pad keeps Brimir's original layout; the other devices follow Beetle Saturn's, whose
// analog L/R triggers and shift paddles need L2/R2):
//
//   RetroPad   Control Pad  3D Control Pad    Arcade Racer        Mission Stick
//   B          A            A                 A                   A
//   A          B            B                 B                   B
//   Y          C            X                 X                   X
//   X          X            Y                 Y                   Y
//   L          L            Z                 Z                   Z
//   R          R            C                 C                   C
//   L2         Y            L (analog)        shift up (Up)       L
//   R2         Z            R (analog)        shift down (Down)   R
//   Start      Start        Start             Start               Start
//   Select     -            mode switch       -                   -
//   D-Pad      D-Pad        D-Pad             Up/Down (shifter)   -
//   L stick    -            analog stick      wheel (X)           stick
//   R stick Y  -            -                 -                   throttle (up = more)
//   R3         -            -                 -                   throttle latch
//
//   Shuttle Mouse: mouse movement, left / right / middle buttons, buttons 4 and 5 = Start.
//   Virtua Gun: light gun screen position, trigger, reload (also the trigger aimed off-screen), Start.

#pragma once

#include <libretro.h>

#include <ymir/hw/smpc/peripheral/peripheral_report.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace brimir {

/// @brief The peripheral connected to a controller port (retro_set_controller_port_device)
enum class PeripheralKind : uint8_t { None, ControlPad, AnalogPad, ArcadeRacer, MissionStick, ShuttleMouse, VirtuaGun };

/// @brief libretro device types offered on both ports (RETRO_ENVIRONMENT_SET_CONTROLLER_INFO)
inline constexpr unsigned RETRO_DEVICE_SATURN_PAD = RETRO_DEVICE_JOYPAD;
inline constexpr unsigned RETRO_DEVICE_SATURN_3D_PAD = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0);
inline constexpr unsigned RETRO_DEVICE_SATURN_WHEEL = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1);
inline constexpr unsigned RETRO_DEVICE_SATURN_MISSION_STICK = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 2);
inline constexpr unsigned RETRO_DEVICE_SATURN_MOUSE = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 0);
inline constexpr unsigned RETRO_DEVICE_SATURN_VIRTUA_GUN = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 0);

/// @brief The peripheral for a libretro device type: the types above, RETRO_DEVICE_NONE; any other type by its base
/// class (mouse: Shuttle Mouse, light gun: Virtua Gun), else the Control Pad (plain RETRO_DEVICE_ANALOG, "RetroPad w/
/// Analog" in configurations saved with earlier versions, included)
PeripheralKind PeripheralKindForDevice(unsigned device);

/// @brief Display name of a peripheral (logs)
const char* PeripheralKindName(PeripheralKind kind);

/// @brief One frame of libretro input for a port, as polled for its device type (unpolled fields stay zero)
struct InputSnapshot {
    uint16_t joypad = 0; ///< bit n: RETRO_DEVICE_ID_JOYPAD_n pressed
    int16_t leftX = 0, leftY = 0, rightX = 0, rightY = 0; ///< RETRO_DEVICE_ANALOG sticks, -0x8000 .. 0x7FFF
    int16_t l2 = 0, r2 = 0; ///< analog L2 / R2 (RETRO_DEVICE_INDEX_ANALOG_BUTTON), 0 .. 0x7FFF; 0 if unsupported
    int16_t mouseX = 0, mouseY = 0; ///< RETRO_DEVICE_MOUSE movement since the last frame
    uint8_t mouseButtons = 0;       ///< kMouse* bits
    int16_t gunX = 0, gunY = 0;     ///< RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X/Y, -0x7FFF .. 0x7FFF across the shown frame
    bool gunOffscreen = true;
    bool gunTrigger = false, gunReload = false, gunStart = false;

    static constexpr uint8_t kMouseLeft = 1u << 0;
    static constexpr uint8_t kMouseRight = 1u << 1;
    static constexpr uint8_t kMouseMiddle = 1u << 2;
    static constexpr uint8_t kMouseButton4 = 1u << 3;
    static constexpr uint8_t kMouseButton5 = 1u << 4;
};

/// @brief Controller options in percent (brimir_analog_deadzone, brimir_trigger_deadzone, brimir_mouse_sensitivity;
/// the libretro layer applies them). These defaults map exactly as 0.7.0 did.
struct InputSettings {
    /// Sticks of the 3D Control Pad, Arcade Racer (wheel) and Mission Stick: a radial deadzone of 0-30 % of the
    /// stick's range, the rest rescaled to the full range (no jump at its edge). The Mission Stick throttle has none.
    unsigned analogDeadzone = 0;
    /// Analog L/R of the 3D Control Pad: a linear deadzone of 0-30 % of the travel, the rest rescaled
    unsigned triggerDeadzone = 0;
    /// Shuttle Mouse movement scale, 5-200 %
    unsigned mouseSensitivity = 100;
};

/// @brief Shuttle Mouse movement is kept in hundredths of a count (a low sensitivity loses no movement)
inline constexpr int32_t kMouseMovementUnit = 100;
/// @brief Bound of the movement a port keeps between reads, either axis, in kMouseMovementUnit (whole counts fit an
/// int16 report, negated too)
inline constexpr int32_t kMouseMovementLimit = 32767 * kMouseMovementUnit;

/// @brief Input state a port keeps across frames (held by CoreWrapper, saved in states)
struct PortInputState {
    uint16_t prevButtons = 0;     ///< RetroPad buttons of the previous frame (edges)
    bool analogMode = true;       ///< 3D Control Pad: analog mode (the pad's power-on mode); Select toggles it
    bool throttleLatched = false; ///< Mission Stick: R3 holds the throttle where it was
    uint8_t throttle = 0;         ///< Mission Stick: the latched throttle value
    /// Shuttle Mouse: the movement built up since the last read (libretro axes: right and down positive, in
    /// kMouseMovementUnit, sensitivity applied); each read takes what fits in its report (TakeMouseMovement)
    int32_t mouseX = 0;
    int32_t mouseY = 0;
};

/// @brief Applies this frame's edges to the port's state (before mapping), then remembers the buttons; adds a Shuttle
/// Mouse's movement to what it has built up since the last read
void AdvancePortInput(PeripheralKind kind, const InputSnapshot& in, PortInputState& state,
                      const InputSettings& settings = {});

/// @brief Reports per peripheral (Ymir conventions: buttons 0 = pressed)
ymir::peripheral::ControlPadReport MapControlPad(const InputSnapshot& in);
ymir::peripheral::AnalogPadReport MapAnalogPad(const InputSnapshot& in, const PortInputState& state,
                                               const InputSettings& settings = {});
ymir::peripheral::ArcadeRacerReport MapArcadeRacer(const InputSnapshot& in, const InputSettings& settings = {});
ymir::peripheral::MissionStickReport MapMissionStick(const InputSnapshot& in, const PortInputState& state,
                                                     const InputSettings& settings = {});
/// @brief The Shuttle Mouse's buttons; the movement (zero here) is taken at each read with TakeMouseMovement
ymir::peripheral::ShuttleMouseReport MapShuttleMouse(const InputSnapshot& in);

/// @brief A read of the Shuttle Mouse (Mednafen / Beetle Saturn behaviour): it takes every whole count built up since
/// the last read and removes it from the port's state; the mouse clamps the report to -256 .. 255 per axis and sets
/// that axis's overflow bit beyond, so the excess is dropped (no carry to later reads). Only the fraction of a count
/// stays (a low sensitivity loses no slow movement). A game reading twice in one frame gets the movement once; a frame
/// without a read loses none.
void TakeMouseMovement(PortInputState& state, ymir::peripheral::ShuttleMouseReport& report);

/// @brief The emulated screen the light gun aims at: the Saturn resolution (VDP2, including hi-res and double
/// interlace) and the output transforms between it and the frame the frontend shows (CoreWrapper::OnFrameComplete)
struct GunScreen {
    uint32_t width = 320;
    uint32_t height = 224;
    uint32_t cropH = 0; ///< total overscan crop (applied only when the frame is more than 32 pixels larger)
    uint32_t cropV = 0;
    uint32_t rotation = 0; ///< 0, 90, 180, 270
};

/// @brief Virtua Gun report: the shown frame's position mapped back to Saturn screen coordinates; off-screen gives no
/// coordinates (0xFFFF), and the trigger pulled off-screen is a reload
ymir::peripheral::VirtuaGunReport MapVirtuaGun(const InputSnapshot& in, const GunScreen& screen);

/// @brief Light gun crosshair (option brimir_gun_crosshair), drawn on the shown frame only (never emulated)
enum class CrosshairStyle : uint8_t { Off, Cross, Dot };

struct Crosshair {
    CrosshairStyle style = CrosshairStyle::Off;
    int16_t x = 0, y = 0; ///< libretro light gun screen coordinates
    uint32_t color = 0;   ///< 0x00RRGGBB
};

/// @brief Crosshair colour per port (0x00RRGGBB) and its outline
inline constexpr uint32_t kCrosshairColors[2] = {0xFF2020, 0x2080FF};
inline constexpr uint32_t kCrosshairOutline = 0x000000;

/// @brief A filled rectangle of an output image, already clipped to it
struct OverlayRect {
    uint32_t x = 0, y = 0, w = 0, h = 0;
    uint32_t color = 0; ///< 0x00RRGGBB
};

inline constexpr size_t kMaxCrosshairRects = 4;
/// @brief Largest rectangle CrosshairRects returns, in pixels (any output size)
inline constexpr size_t kMaxCrosshairRectPixels = 8192;

/// @brief The crosshair as rectangles (outline first) on an output image of w x h pixels, whose scale follows the
/// image height; returns how many (0 when off or wholly outside)
size_t CrosshairRects(const Crosshair& crosshair, uint32_t w, uint32_t h,
                      std::array<OverlayRect, kMaxCrosshairRects>& rects);

/// @brief Draws the crosshair into an XRGB8888 image
void DrawCrosshair(const Crosshair& crosshair, uint32_t* xrgb, uint32_t w, uint32_t h, size_t pitchPixels);

} // namespace brimir
