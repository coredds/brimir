// Brimir - Core options header
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#pragma once

#include "libretro.h"
#include <brimir/peripheral_input.hpp>
#include <string>

namespace brimir {
enum class CartridgeChoice; // core_wrapper.hpp
}

// Get core options v2 structure
const struct retro_core_options_v2* brimir_get_core_options_v2(void);

// The controller options (brimir_analog_deadzone, brimir_trigger_deadzone, brimir_mouse_sensitivity) as settings; a
// missing or unknown value is the option's default (15, 15, 100).
brimir::InputSettings brimir_input_settings(const char* analog_deadzone, const char* trigger_deadzone,
                                            const char* mouse_sensitivity);

// The cartridge a brimir_cartridge value chooses (Auto for nullptr or an unknown value).
brimir::CartridgeChoice brimir_cartridge_choice(const char* value);

// Last seen values of the two deinterlacing options. Defaults must match options.cpp.
struct BrimirDeinterlaceOptions {
    std::string deinterlacing = "enabled";
    std::string mode = "bob";
};

// Whether deinterlacing is on: brimir_deinterlacing is "enabled" and brimir_deinterlace_mode is not "none".
bool brimir_deinterlace_effective(const char* deinterlacing, const char* mode);

// Stores the new option values in `cache`. Returns true when the effective flag, written to `effective`,
// has to be applied: on `force` or when either option changed. The result depends only on the two values,
// not on which option changed first.
bool brimir_update_deinterlace(BrimirDeinterlaceOptions& cache, const char* deinterlacing, const char* mode,
                               bool force, bool& effective);

// Whether this is an Android build: internal resolution above 1x is not offered there (it hangs the GPU of some
// phones, e.g. Samsung Xclipse), whatever the option file or a core-option override says.
#if defined(__ANDROID__)
inline constexpr bool kBrimirAndroid = true;
#else
inline constexpr bool kBrimirAndroid = false;
#endif

// The internal resolution (1-4) a brimir_internal_resolution value asks for (1 for nullptr or an unknown value), and
// the one that applies: with `android` the highest is 1x. `requested` (optional) receives the 1-4 value asked for.
unsigned brimir_internal_resolution_scale(const char* value, bool android, unsigned* requested = nullptr);

// The one-line log text for a cap: the requested value and why it was lowered to 1x.
std::string brimir_internal_resolution_cap_note(unsigned requested);

// The on-screen hint for a vertical ST-V game shown with a Screen Rotation that is not Auto: `rotationOption` is the
// brimir_rotation value (nullptr or "auto" is Auto), `stv` whether ST-V content is loaded and `gameRotation` the game's
// database rotation in degrees. Returns nullptr (no hint) under Auto, for Saturn content, for an upright game, and when
// the chosen rotation equals the game's.
const char* brimir_rotation_hint(const char* rotationOption, bool stv, int gameRotation);
