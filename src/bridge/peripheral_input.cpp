// Brimir - libretro input to Saturn peripheral reports
// Licensed under GPL-3.0

#include <brimir/peripheral_input.hpp>

#include <algorithm>

namespace brimir {

namespace {

using ymir::peripheral::Button;

constexpr bool Pressed(uint16_t joypad, unsigned id) {
    return (joypad & (1u << id)) != 0;
}

// Saturn buttons (1 = released) with the listed RetroPad buttons pressed.
struct ButtonMap {
    unsigned id;
    Button button;
};

template <size_t N>
Button MapButtons(uint16_t joypad, const ButtonMap (&map)[N]) {
    Button buttons = Button::Default;
    for (const ButtonMap& m : map) {
        if (Pressed(joypad, m.id)) {
            buttons &= ~m.button;
        }
    }
    return buttons;
}

// Brimir's original Control Pad layout (unchanged since the first release).
constexpr ButtonMap kControlPadMap[] = {
    {RETRO_DEVICE_ID_JOYPAD_UP, Button::Up},       {RETRO_DEVICE_ID_JOYPAD_DOWN, Button::Down},
    {RETRO_DEVICE_ID_JOYPAD_LEFT, Button::Left},   {RETRO_DEVICE_ID_JOYPAD_RIGHT, Button::Right},
    {RETRO_DEVICE_ID_JOYPAD_B, Button::A},         {RETRO_DEVICE_ID_JOYPAD_A, Button::B},
    {RETRO_DEVICE_ID_JOYPAD_Y, Button::C},         {RETRO_DEVICE_ID_JOYPAD_X, Button::X},
    {RETRO_DEVICE_ID_JOYPAD_L2, Button::Y},        {RETRO_DEVICE_ID_JOYPAD_R2, Button::Z},
    {RETRO_DEVICE_ID_JOYPAD_L, Button::L},         {RETRO_DEVICE_ID_JOYPAD_R, Button::R},
    {RETRO_DEVICE_ID_JOYPAD_START, Button::Start},
};

// Beetle Saturn's face buttons, shared by the 3D Control Pad, Arcade Racer and Mission Stick.
constexpr ButtonMap kAnalogPadMap[] = {
    {RETRO_DEVICE_ID_JOYPAD_UP, Button::Up},       {RETRO_DEVICE_ID_JOYPAD_DOWN, Button::Down},
    {RETRO_DEVICE_ID_JOYPAD_LEFT, Button::Left},   {RETRO_DEVICE_ID_JOYPAD_RIGHT, Button::Right},
    {RETRO_DEVICE_ID_JOYPAD_B, Button::A},         {RETRO_DEVICE_ID_JOYPAD_A, Button::B},
    {RETRO_DEVICE_ID_JOYPAD_R, Button::C},         {RETRO_DEVICE_ID_JOYPAD_Y, Button::X},
    {RETRO_DEVICE_ID_JOYPAD_X, Button::Y},         {RETRO_DEVICE_ID_JOYPAD_L, Button::Z},
    {RETRO_DEVICE_ID_JOYPAD_L2, Button::L},        {RETRO_DEVICE_ID_JOYPAD_R2, Button::R},
    {RETRO_DEVICE_ID_JOYPAD_START, Button::Start},
};

constexpr ButtonMap kArcadeRacerMap[] = {
    {RETRO_DEVICE_ID_JOYPAD_B, Button::A},         {RETRO_DEVICE_ID_JOYPAD_A, Button::B},
    {RETRO_DEVICE_ID_JOYPAD_R, Button::C},         {RETRO_DEVICE_ID_JOYPAD_Y, Button::X},
    {RETRO_DEVICE_ID_JOYPAD_X, Button::Y},         {RETRO_DEVICE_ID_JOYPAD_L, Button::Z},
    {RETRO_DEVICE_ID_JOYPAD_L2, Button::Up},       {RETRO_DEVICE_ID_JOYPAD_R2, Button::Down},
    {RETRO_DEVICE_ID_JOYPAD_UP, Button::Up},       {RETRO_DEVICE_ID_JOYPAD_DOWN, Button::Down},
    {RETRO_DEVICE_ID_JOYPAD_START, Button::Start},
};

constexpr ButtonMap kMissionStickMap[] = {
    {RETRO_DEVICE_ID_JOYPAD_B, Button::A},         {RETRO_DEVICE_ID_JOYPAD_A, Button::B},
    {RETRO_DEVICE_ID_JOYPAD_R, Button::C},         {RETRO_DEVICE_ID_JOYPAD_Y, Button::X},
    {RETRO_DEVICE_ID_JOYPAD_X, Button::Y},         {RETRO_DEVICE_ID_JOYPAD_L, Button::Z},
    {RETRO_DEVICE_ID_JOYPAD_L2, Button::L},        {RETRO_DEVICE_ID_JOYPAD_R2, Button::R},
    {RETRO_DEVICE_ID_JOYPAD_START, Button::Start},
};

// libretro axis (-0x8000 .. 0x7FFF, 0 = centre) to a Saturn axis byte (0x00 .. 0xFF, `center` at 0): both ends reach
// the extremes.
uint8_t AxisToByte(int32_t v, int32_t center) {
    v = std::clamp<int32_t>(v, -0x8000, 0x7FFF);
    if (v < 0) {
        return static_cast<uint8_t>(center - ((-v) * center + 0x4000) / 0x8000);
    }
    return static_cast<uint8_t>(center + (v * (255 - center) + 0x3FFF) / 0x7FFF);
}

// Analog trigger (0 .. 0x7FFF) to 0x00 .. 0xFF; a frontend without analog triggers reports 0, so the digital button
// then gives a full press.
uint8_t TriggerToByte(int16_t analog, bool digital) {
    if (analog > 0) {
        return static_cast<uint8_t>(std::min<int32_t>(255, (int32_t{analog} * 255 + 0x3FFF) / 0x7FFF));
    }
    return digital ? 0xFF : 0x00;
}

// Mission Stick throttle from the right stick's Y axis: up is more (0xFF), centre 0x7F.
uint8_t StickThrottle(const InputSnapshot& in) {
    return AxisToByte(std::min<int32_t>(0x7FFF, -int32_t{in.rightY}), 0x7F);
}

// libretro screen coordinate (-0x7FFF .. 0x7FFF across the image) to a pixel of an image `size` pixels across.
uint32_t ScreenToPixel(int16_t v, uint32_t size) {
    const int64_t c = std::clamp<int32_t>(v, -0x7FFF, 0x7FFF) + 0x7FFF; // 0 .. 0xFFFE
    return std::min<uint32_t>(static_cast<uint32_t>(c * size / 0xFFFF), size == 0 ? 0 : size - 1);
}

void AddClipped(std::array<OverlayRect, kMaxCrosshairRects>& rects, size_t& count, int32_t x, int32_t y, int32_t w,
                int32_t h, uint32_t imageW, uint32_t imageH, uint32_t color) {
    const int32_t x0 = std::max<int32_t>(x, 0);
    const int32_t y0 = std::max<int32_t>(y, 0);
    const int32_t x1 = std::min<int32_t>(x + w, static_cast<int32_t>(imageW));
    const int32_t y1 = std::min<int32_t>(y + h, static_cast<int32_t>(imageH));
    if (x1 <= x0 || y1 <= y0 || count >= rects.size()) {
        return;
    }
    rects[count++] = {static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), static_cast<uint32_t>(x1 - x0),
                      static_cast<uint32_t>(y1 - y0), color};
}

} // namespace

PeripheralKind PeripheralKindForDevice(unsigned device) {
    switch (device) {
    case RETRO_DEVICE_NONE: return PeripheralKind::None;
    case RETRO_DEVICE_SATURN_PAD: return PeripheralKind::ControlPad;
    case RETRO_DEVICE_SATURN_3D_PAD: return PeripheralKind::AnalogPad;
    case RETRO_DEVICE_SATURN_WHEEL: return PeripheralKind::ArcadeRacer;
    case RETRO_DEVICE_SATURN_MISSION_STICK: return PeripheralKind::MissionStick;
    case RETRO_DEVICE_SATURN_MOUSE: return PeripheralKind::ShuttleMouse;
    case RETRO_DEVICE_SATURN_VIRTUA_GUN: return PeripheralKind::VirtuaGun;
    default: break;
    }
    switch (device & RETRO_DEVICE_MASK) {
    case RETRO_DEVICE_ANALOG: return PeripheralKind::AnalogPad;
    case RETRO_DEVICE_MOUSE: return PeripheralKind::ShuttleMouse;
    case RETRO_DEVICE_LIGHTGUN: return PeripheralKind::VirtuaGun;
    default: return PeripheralKind::ControlPad;
    }
}

const char* PeripheralKindName(PeripheralKind kind) {
    switch (kind) {
    case PeripheralKind::None: return "None";
    case PeripheralKind::ControlPad: return "Control Pad";
    case PeripheralKind::AnalogPad: return "3D Control Pad";
    case PeripheralKind::ArcadeRacer: return "Arcade Racer";
    case PeripheralKind::MissionStick: return "Mission Stick";
    case PeripheralKind::ShuttleMouse: return "Shuttle Mouse";
    case PeripheralKind::VirtuaGun: return "Virtua Gun";
    }
    return "?";
}

void AdvancePortInput(PeripheralKind kind, const InputSnapshot& in, PortInputState& state) {
    const uint16_t pressed = static_cast<uint16_t>(in.joypad & ~state.prevButtons);
    if (kind == PeripheralKind::AnalogPad && Pressed(pressed, RETRO_DEVICE_ID_JOYPAD_SELECT)) {
        state.analogMode = !state.analogMode;
    }
    if (kind == PeripheralKind::MissionStick && Pressed(pressed, RETRO_DEVICE_ID_JOYPAD_R3)) {
        if (state.throttleLatched) {
            state.throttleLatched = false;
        } else {
            state.throttleLatched = true;
            state.throttle = StickThrottle(in);
        }
    }
    state.prevButtons = in.joypad;
}

ymir::peripheral::ControlPadReport MapControlPad(const InputSnapshot& in) {
    return {.buttons = MapButtons(in.joypad, kControlPadMap)};
}

ymir::peripheral::AnalogPadReport MapAnalogPad(const InputSnapshot& in, const PortInputState& state) {
    return {
        .buttons = MapButtons(in.joypad, kAnalogPadMap),
        .analog = state.analogMode,
        .x = AxisToByte(in.leftX, 0x80),
        .y = AxisToByte(in.leftY, 0x80),
        .l = TriggerToByte(in.l2, Pressed(in.joypad, RETRO_DEVICE_ID_JOYPAD_L2)),
        .r = TriggerToByte(in.r2, Pressed(in.joypad, RETRO_DEVICE_ID_JOYPAD_R2)),
    };
}

ymir::peripheral::ArcadeRacerReport MapArcadeRacer(const InputSnapshot& in) {
    return {.buttons = MapButtons(in.joypad, kArcadeRacerMap), .wheel = AxisToByte(in.leftX, 0x7F)};
}

ymir::peripheral::MissionStickReport MapMissionStick(const InputSnapshot& in, const PortInputState& state) {
    return {
        .buttons = MapButtons(in.joypad, kMissionStickMap),
        .sixAxis = false,
        .x1 = AxisToByte(in.leftX, 0x7F),
        .y1 = AxisToByte(in.leftY, 0x7F),
        .z1 = state.throttleLatched ? state.throttle : StickThrottle(in),
        .x2 = 0x7F,
        .y2 = 0x7F,
        .z2 = 0x00,
    };
}

ymir::peripheral::ShuttleMouseReport MapShuttleMouse(const InputSnapshot& in) {
    using S = InputSnapshot;
    return {
        .start = (in.mouseButtons & (S::kMouseButton4 | S::kMouseButton5)) != 0,
        .left = (in.mouseButtons & S::kMouseLeft) != 0,
        .middle = (in.mouseButtons & S::kMouseMiddle) != 0,
        .right = (in.mouseButtons & S::kMouseRight) != 0,
        .x = in.mouseX,
        .y = in.mouseY,
    };
}

ymir::peripheral::VirtuaGunReport MapVirtuaGun(const InputSnapshot& in, const GunScreen& screen) {
    ymir::peripheral::VirtuaGunReport report{
        .start = in.gunStart,
        .trigger = in.gunTrigger && !in.gunOffscreen,
        .reload = in.gunReload || (in.gunTrigger && in.gunOffscreen),
        .x = 0xFFFF,
        .y = 0xFFFF,
    };
    if (in.gunOffscreen || screen.width == 0 || screen.height == 0) {
        return report;
    }
    // The shown frame: the overscan crop (with the bridge's guard), then the rotation.
    const bool crop = screen.width > screen.cropH + 32 && screen.height > screen.cropV + 32;
    const uint32_t cropW = crop ? screen.width - screen.cropH : screen.width;
    const uint32_t cropH = crop ? screen.height - screen.cropV : screen.height;
    const uint32_t cropLeft = crop ? screen.cropH / 2 : 0;
    const uint32_t cropTop = crop ? screen.cropV / 2 : 0;
    const bool swap = screen.rotation == 90 || screen.rotation == 270;
    const uint32_t shownW = swap ? cropH : cropW;
    const uint32_t shownH = swap ? cropW : cropH;
    const uint32_t dx = ScreenToPixel(in.gunX, shownW);
    const uint32_t dy = ScreenToPixel(in.gunY, shownH);
    // Shown pixel -> cropped frame pixel: the inverse of the rotation (as the GPU frame output computes it).
    uint32_t sx = dx;
    uint32_t sy = dy;
    if (screen.rotation == 90) {
        sx = dy;
        sy = shownW - 1 - dx;
    } else if (screen.rotation == 180) {
        sx = shownW - 1 - dx;
        sy = shownH - 1 - dy;
    } else if (screen.rotation == 270) {
        sx = cropW - 1 - dy;
        sy = dx;
    }
    report.x = static_cast<uint16_t>(sx + cropLeft);
    report.y = static_cast<uint16_t>(sy + cropTop);
    return report;
}

size_t CrosshairRects(const Crosshair& crosshair, uint32_t w, uint32_t h,
                      std::array<OverlayRect, kMaxCrosshairRects>& rects) {
    size_t count = 0;
    if (crosshair.style == CrosshairStyle::Off || w == 0 || h == 0) {
        return 0;
    }
    // One crosshair pixel per 240 lines (2 at 480 lines, 4 at 2x internal resolution of an interlaced mode), at most 8.
    const int32_t t = std::clamp<int32_t>(static_cast<int32_t>((h + 120) / 240), 1, 8);
    const int32_t off = t / 2;
    const int32_t cx = static_cast<int32_t>(ScreenToPixel(crosshair.x, w));
    const int32_t cy = static_cast<int32_t>(ScreenToPixel(crosshair.y, h));
    if (crosshair.style == CrosshairStyle::Cross) {
        const int32_t half = 6 * t;
        AddClipped(rects, count, cx - half - off - t, cy - off - t, 2 * half + 3 * t, 3 * t, w, h, kCrosshairOutline);
        AddClipped(rects, count, cx - off - t, cy - half - off - t, 3 * t, 2 * half + 3 * t, w, h, kCrosshairOutline);
        AddClipped(rects, count, cx - half - off, cy - off, 2 * half + t, t, w, h, crosshair.color);
        AddClipped(rects, count, cx - off, cy - half - off, t, 2 * half + t, w, h, crosshair.color);
    } else {
        AddClipped(rects, count, cx - off - 2 * t, cy - off - 2 * t, 5 * t, 5 * t, w, h, kCrosshairOutline);
        AddClipped(rects, count, cx - off - t, cy - off - t, 3 * t, 3 * t, w, h, crosshair.color);
    }
    return count;
}

void DrawCrosshair(const Crosshair& crosshair, uint32_t* xrgb, uint32_t w, uint32_t h, size_t pitchPixels) {
    std::array<OverlayRect, kMaxCrosshairRects> rects;
    const size_t count = CrosshairRects(crosshair, w, h, rects);
    for (size_t i = 0; i < count; ++i) {
        const OverlayRect& r = rects[i];
        for (uint32_t y = r.y; y < r.y + r.h; ++y) {
            std::fill_n(xrgb + y * pitchPixels + r.x, r.w, r.color);
        }
    }
}

} // namespace brimir
