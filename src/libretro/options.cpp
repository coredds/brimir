// Brimir - Core options implementation
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "options.hpp"
#include <brimir/core_wrapper.hpp>
#include <cstring>

// Core option definitions using libretro API v2
static struct retro_core_option_v2_category option_cats[] = {
    {
        "system",
        "System Settings",
        "Configure BIOS and region settings"
    },
    {
        "video",
        "Video Settings",
        "Configure video output options"
    },
    {
        "audio",
        "Audio Settings",
        "Configure audio quality and emulation"
    },
    {
        "media",
        "Media Settings",
        "Configure CD-ROM and disc loading"
    },
    {
        "input",
        "Input Settings",
        "Configure controllers and the light gun"
    },
    { nullptr, nullptr, nullptr }
};

static struct retro_core_option_v2_definition option_defs[] = {
    {
        "brimir_bios",
        "BIOS Selection",
        nullptr,
        "Select which BIOS file to use. Auto uses a BIOS of the disc's region (discs for Japan/Asia only: Japanese; others: US/EU), else any BIOS found.",
        nullptr,
        "system",
        {
            { "auto", "Auto-detect" },
            { "jp_101", "JP v1.01 (Sega Saturn BIOS v1.01 (JAP).bin)" },
            { "jp_100", "JP v1.00 (Sega Saturn BIOS v1.00 (JAP).bin)" },
            { "us_101", "JP v1.01 (sega_101.bin)" },
            { "us_100", "US v1.00 (mpr-17933.bin)" },
            { "eu_100", "JP v1.00 (sega_100.bin)" },
            { "eu_alt", "EU (Sega Saturn BIOS (EUR).bin)" },
            { nullptr, nullptr }
        },
        "auto"
    },
#ifdef BRIMIR_VULKAN
    {
        // Read once at content load (retro_load_game), like brimir_bios.
        "brimir_renderer",
        "Renderer (restart required)",
        nullptr,
        "Software draws every frame on the CPU. Vulkan draws VDP1 (polygons and sprites) and VDP2 (backgrounds) on "
        "the GPU and enables Internal Resolution. Needs the frontend's vulkan video driver (the core never switches "
        "drivers; otherwise it uses software) and a Vulkan 1.2 GPU (older ones get software frames through a plain "
        "copy). Applies when content is loaded.",
        nullptr,
        "video",
        {
            { "software", "Software" },
            { "vulkan", "Vulkan" },
            { nullptr, nullptr }
        },
        "software"
    },
    {
        // Live: applies at the next frame (libretro.cpp, VulkanOutput::SetInternalScale).
        "brimir_internal_resolution",
        "Internal Resolution (Vulkan)",
        nullptr,
        "Renders at a multiple of the Saturn resolution: backgrounds (VDP2) always, rotating and zoomed ones getting "
        "sharper, and polygons and sprites (VDP1) when VDP1 Upscaling is on. Only used with Renderer = Vulkan on the "
        "GPU renderer; ignored by the software renderer and the copy-only output. Applies immediately. 3x and 4x of "
        "high-resolution modes exceed 1080p.",
        nullptr,
        "video",
        {
            { "1", "1x (Native)" },
            { "2", "2x" },
            { "3", "3x" },
            { "4", "4x" },
            { nullptr, nullptr }
        },
        "1"
    },
    {
        // Live: applies at the next frame, like brimir_internal_resolution (VulkanOutput::SetVdp1Upscaling).
        "brimir_vdp1_upscaling",
        "VDP1 Upscaling (Vulkan)",
        nullptr,
        "Draws polygons and sprites (VDP1) at the internal resolution: smoother 3D edges and distant/shrunk "
        "textures (magnified textures stay blocky and unscaled 2D sprites look as at native resolution). "
        "Disable it for a game that shows artefacts (gaps, seams or misplaced lines) at higher internal resolutions; "
        "sprites then stay at native resolution. Only used with Renderer = Vulkan at Internal Resolution 2x or more. "
        "Applies immediately.",
        nullptr,
        "video",
        {
            { "enabled", "Enabled" },
            { "disabled", "Disabled" },
            { nullptr, nullptr }
        },
        "enabled"
    },
#endif
    {
        "brimir_deinterlacing",
        "Deinterlacing",
        nullptr,
        "Enable deinterlacing for interlaced video modes. Many games use interlaced high-res modes for menus. "
        "Uses optimized single-field rendering for 60 FPS with no scanlines. "
        "Disable only for progressive-only games to skip post-processing. "
        "OFF turns deinterlacing off whatever Deinterlacing Mode is set to.",
        nullptr,
        "video",
        {
            { "disabled", "OFF" },
            { "enabled", "ON" },
            { nullptr, nullptr }
        },
        "enabled"
    },
    {
        "brimir_deinterlace_mode",
        "Deinterlacing Mode",
        nullptr,
        "Bob, Weave, Blend and Current currently behave the same: the frame is fully deinterlaced. "
        "None: No deinterlacing (native interlaced output), same as turning Deinterlacing OFF. "
        "Only used while Deinterlacing is ON.",
        nullptr,
        "video",
        {
            { "bob", "Bob" },
            { "weave", "Weave" },
            { "blend", "Blend" },
            { "current", "Current" },
            { "none", "None" },
            { nullptr, nullptr }
        },
        "bob"
    },
    {
        "brimir_threaded_vdp1",
        "Threaded VDP1 Rendering",
        nullptr,
        "Run VDP1 rendering on a separate worker thread. "
        "Disabling can improve stability with save states at the cost of performance.",
        nullptr,
        "video",
        {
            { "enabled", "ON" },
            { "disabled", "OFF" },
            { nullptr, nullptr }
        },
        "enabled"
    },
    {
        "brimir_threaded_vdp2",
        "Threaded VDP2 Rendering",
        nullptr,
        "Run VDP2 rendering on a separate worker thread. "
        "Disabling can improve stability with save states at the cost of performance.",
        nullptr,
        "video",
        {
            { "enabled", "ON" },
            { "disabled", "OFF" },
            { nullptr, nullptr }
        },
        "enabled"
    },
    {
        "brimir_autodetect_region",
        "Auto-Detect Region from Disc",
        nullptr,
        "Automatically set console region based on loaded game disc. "
        "When enabled, the Console Region setting is overridden by the disc's region code.",
        nullptr,
        "system",
        {
            { "disabled", "OFF" },
            { "enabled", "ON" },
            { nullptr, nullptr }
        },
        "enabled"
    },
    {
        // Read once at content load (retro_load_game), like brimir_bios.
        "brimir_rtc_mode",
        "RTC Mode",
        nullptr,
        "How the Saturn's clock keeps time. Virtual: the clock runs with the emulated time only, so save states, "
        "run-ahead, rewind and netplay stay exact; it starts at your computer's local time and continues from the "
        "saved clock (one per BIOS region, in the system folder) on later sessions. Host: the clock follows your "
        "computer's clock. Applies when content is loaded.",
        nullptr,
        "system",
        {
            { "virtual", "Virtual" },
            { "host", "Host" },
            { nullptr, nullptr }
        },
        "virtual"
    },
    {
        // Read once at content load (retro_load_game), like brimir_rtc_mode.
        "brimir_cartridge",
        "Cartridge",
        nullptr,
        "The cartridge in the Saturn's cartridge slot. Auto: the one the game needs (1 MB or 4 MB RAM for the games "
        "that use it, the ROM cartridge for The King of Fighters '95 and Ultraman, backup memory for the games that "
        "save to it), none for other games. Backup memory is a 4 Mbit cartridge saved to <game>.bcr in the save "
        "folder (the same file as Beetle Saturn's). The ROM cartridges need their dumps in the system folder: "
        "mpr-18811-mx.ic1 (The King of Fighters '95) and mpr-19367-mx.ic1 (Ultraman); without one, the game starts "
        "without the cartridge. Applies when content is loaded.",
        nullptr,
        "system",
        {
            { "auto", "Auto" },
            { "none", "None" },
            { "dram_1mb", "1 MB RAM" },
            { "dram_4mb", "4 MB RAM" },
            { "backup", "Backup Memory" },
            { "rom_kof95", "The King of Fighters '95 ROM" },
            { "rom_ultraman", "Ultraman ROM" },
            { nullptr, nullptr }
        },
        "auto"
    },
    {
        // Read once at content load (retro_load_game), like brimir_cartridge.
        "brimir_sh2_cache",
        "SH-2 Cache Emulation",
        nullptr,
        "Emulates the SH-2 CPUs' caches. Most games do not need it; it costs some speed. Games known to need it "
        "(Astal, Dark Savior, Waku Waku 7 and others in Ymir's game database) always get it. The SH-2 JIT is not "
        "used while it is active. Applies when content is loaded.",
        nullptr,
        "system",
        {
            { "disabled", "OFF" },
            { "enabled", "ON" },
            { nullptr, nullptr }
        },
        "disabled"
    },
    {
        // Read once at content load (retro_load_game), like brimir_cartridge.
        "brimir_cdblock_lle",
        "CD Block Low-Level Emulation",
        nullptr,
        "Emulates the CD Block's own SH-1 processor running its firmware instead of emulating its commands. Slower, "
        "and a work in progress in Ymir. Needs the CD Block ROM (64 KiB) in the system folder as cdb106.bin, "
        "cdb105.bin or cdb104.bin; without it the CD Block stays high-level. Save states record the mode: a "
        "high-level state switches the session to high-level, a low-level state needs this option and the same ROM. "
        "Applies when content is loaded.",
        nullptr,
        "media",
        {
            { "disabled", "OFF" },
            { "enabled", "ON" },
            { nullptr, nullptr }
        },
        "disabled"
    },
    {
        // Live: SCSP::SetStepGranularity switches between steps while running.
        "brimir_scsp_granularity",
        "SCSP Step Granularity",
        nullptr,
        "How finely the sound processor (SCSP) is stepped against the CPUs: 0 steps one sample (32 slots) at a time, "
        "5 one slot at a time. Higher values are more accurate for games that time the sound hardware closely, and "
        "slower. Applies immediately.",
        nullptr,
        "audio",
        {
            { "0", "0 (32 slots, default)" },
            { "1", "1 (16 slots)" },
            { "2", "2 (8 slots)" },
            { "3", "3 (4 slots)" },
            { "4", "4 (2 slots)" },
            { "5", "5 (1 slot)" },
            { nullptr, nullptr }
        },
        "0"
    },
    {
        "brimir_audio_interpolation",
        "Audio Interpolation",
        nullptr,
        "Sample interpolation method. Linear is hardware accurate. "
        "Nearest Neighbor is faster but introduces aliasing.",
        nullptr,
        "audio",
        {
            { "linear", "Linear" },
            { "nearest", "Nearest" },
            { nullptr, nullptr }
        },
        "linear"
    },
    {
        "brimir_audio_volume",
        "Audio Volume",
        nullptr,
        "Audio output volume as a percentage. 100% is unchanged, lower values reduce volume, higher values amplify.",
        nullptr,
        "audio",
        {
            { "0", "0%" },
            { "25", "25%" },
            { "50", "50%" },
            { "75", "75%" },
            { "100", "100%" },
            { "125", "125%" },
            { "150", "150%" },
            { "175", "175%" },
            { "200", "200%" },
            { nullptr, nullptr }
        },
        "100"
    },
    {
        "brimir_rotation",
        "Screen Rotation (TATE)",
        nullptr,
        "Rotate the screen for vertical (TATE) arcade games. "
        "90° and 270° are for vertical shooters like Battle Garegga, Dodonpachi, Radiant Silvergun.",
        nullptr,
        "video",
        {
            { "0", "None" },
            { "90", "90\u00B0" },
            { "180", "180\u00B0" },
            { "270", "270\u00B0" },
            { nullptr, nullptr }
        },
        "0"
    },
    {
        "brimir_overscan",
        "Overscan Crop",
        nullptr,
        "Crop overscan edges from the video output. "
        "Small cuts ~16px, Medium ~32px, Large ~48px from each edge.",
        nullptr,
        "video",
        {
            { "0", "None" },
            { "1", "Small (~16px)" },
            { "2", "Medium (~32px)" },
            { "3", "Large (~48px)" },
            { nullptr, nullptr }
        },
        "0"
    },
    {
        "brimir_cd_speed",
        "CD Read Speed",
        nullptr,
        "CD-ROM read speed multiplier. Higher values reduce loading times but may break some games. "
        "2x matches real hardware.",
        nullptr,
        "media",
        {
            { "2", "2x" },
            { "4", "4x" },
            { "6", "6x" },
            { "8", "8x" },
            { "12", "12x" },
            { "16", "16x" },
            { "24", "24x" },
            { "32", "32x" },
            { "200", "Max (200x)" },
            { nullptr, nullptr }
        },
        "2"
    },
    {
        "brimir_cd_preload",
        "Preload Disc to RAM",
        nullptr,
        "Load the entire disc image into memory when the game starts. "
        "This avoids the brief hitches that can occur when streaming CD-DA audio or seeking from compressed CHD files. "
        "Disable only on systems with very limited RAM; the next disc load will use the new setting.",
        nullptr,
        "media",
        {
            { "enabled", "ON" },
            { "disabled", "OFF" },
            { nullptr, nullptr }
        },
        "enabled"
    },
    {
        "brimir_sh2_overclock",
        "SH-2 CPU Overclock",
        nullptr,
        "Overclock the dual SH-2 CPUs to reduce internal slowdown in demanding games. "
        "100% matches real hardware. Higher values may cause issues in some titles. "
        "Effective in: Panzer Dragoon Saga, Burning Rangers, Sonic R, Tomb Raider.",
        nullptr,
        "system",
        {
            { "100", "100% (Stock)" },
            { "125", "125%" },
            { "150", "150%" },
            { "175", "175%" },
            { "200", "200%" },
            { "250", "250%" },
            { "300", "300%" },
            { nullptr, nullptr }
        },
        "100"
    },
    {
        "brimir_sh2_jit",
        "SH-2 JIT (Experimental)",
        nullptr,
        "Experimental. Run the SH-2 CPUs through the JIT. It compiles all SH-2 instructions except "
        "exception entry/return and SLEEP to native code on x86-64 (through an IR interpreter on "
        "other CPUs) and is validated to produce exactly the interpreter's results. On x86-64 its "
        "SH-2 time is between about 5% and 28% below the interpreter's in the games tested, "
        "but with threaded VDP rendering (the default) the average frame can still be slightly "
        "longer than the interpreter's on some games, and it can still stutter (frame spikes) "
        "while a game loads a lot of new code, over long sessions; on other CPUs it is much slower than the interpreter. For testing only. Not "
        "used while SH-2 cache emulation is active.",
        nullptr,
        "system",
        {
            { "disabled", "OFF" },
            { "enabled", "ON" },
            { nullptr, nullptr }
        },
        "disabled"
    },
    {
        "brimir_sh2_jit_compile_thread",
        "SH-2 JIT Compile Thread (Experimental)",
        nullptr,
        "Compile new SH-2 JIT code on one extra thread when the CPU has at least 4 threads, "
        "instead of on the emulation thread. This removes most of the stutter caused by "
        "compiling, but not all: a game that loads a lot of new code at once can still drop "
        "frames while the JIT decodes it. OFF compiles on the emulation thread. Only used by the "
        "x86-64 JIT, and only while SH-2 JIT is ON.",
        nullptr,
        "system",
        {
            { "auto", "Auto" },
            { "disabled", "OFF" },
            { nullptr, nullptr }
        },
        "auto"
    },
    {
        // Live: the next shown frame (libretro.cpp, update_crosshairs).
        "brimir_gun_crosshair",
        "Gun Crosshair",
        nullptr,
        "The crosshair shown where a Virtua Gun aims (red for port 1, blue for port 2). Drawn on the shown frame "
        "only: the game does not see it. Off hides it (with a real light gun, or for games that draw their own). "
        "Applies immediately.",
        nullptr,
        "input",
        {
            { "cross", "Cross" },
            { "dot", "Dot" },
            { "off", "Off" },
            { nullptr, nullptr }
        },
        "cross"
    },
    {
        // Live: the next frame's input (libretro.cpp, CoreWrapper::SetInputSettings).
        "brimir_analog_deadzone",
        "Analog Stick Deadzone",
        nullptr,
        "Inner deadzone of the analog stick of the 3D Control Pad, Arcade Racer and Mission Stick device types, as a "
        "share of the stick's range: inside it the stick reads centred, beyond it the rest of the range is stretched "
        "to the full range (no jump at the edge). It adds to any deadzone set in the frontend's input settings. 0% "
        "reads the stick as reported. Applies immediately.",
        nullptr,
        "input",
        {
            { "0", "0%" },
            { "5", "5%" },
            { "10", "10%" },
            { "15", "15%" },
            { "20", "20%" },
            { "25", "25%" },
            { "30", "30%" },
            { nullptr, nullptr }
        },
        "15"
    },
    {
        // Live, like brimir_analog_deadzone.
        "brimir_trigger_deadzone",
        "Trigger Deadzone",
        nullptr,
        "Inner deadzone of the analog L and R triggers of the 3D Control Pad device type, as a share of their travel: "
        "a trigger pulled less reads released, the rest of the travel is stretched to the full range. It adds to any "
        "deadzone set in the frontend's input settings. 0% reads the triggers as reported. Applies immediately.",
        nullptr,
        "input",
        {
            { "0", "0%" },
            { "5", "5%" },
            { "10", "10%" },
            { "15", "15%" },
            { "20", "20%" },
            { "25", "25%" },
            { "30", "30%" },
            { nullptr, nullptr }
        },
        "15"
    },
    {
        // Live, like brimir_analog_deadzone.
        "brimir_mouse_sensitivity",
        "Mouse Sensitivity",
        nullptr,
        "Scales the movement of the Shuttle Mouse device type. Movement is kept until the game reads the mouse, so "
        "none is counted twice. Applies immediately.",
        nullptr,
        "input",
        {
            { "5", "5%" },     { "10", "10%" },   { "15", "15%" },   { "20", "20%" },   { "25", "25%" },
            { "30", "30%" },   { "35", "35%" },   { "40", "40%" },   { "45", "45%" },   { "50", "50%" },
            { "55", "55%" },   { "60", "60%" },   { "65", "65%" },   { "70", "70%" },   { "75", "75%" },
            { "80", "80%" },   { "85", "85%" },   { "90", "90%" },   { "95", "95%" },   { "100", "100%" },
            { "105", "105%" }, { "110", "110%" }, { "115", "115%" }, { "120", "120%" }, { "125", "125%" },
            { "130", "130%" }, { "135", "135%" }, { "140", "140%" }, { "145", "145%" }, { "150", "150%" },
            { "155", "155%" }, { "160", "160%" }, { "165", "165%" }, { "170", "170%" }, { "175", "175%" },
            { "180", "180%" }, { "185", "185%" }, { "190", "190%" }, { "195", "195%" }, { "200", "200%" },
            { nullptr, nullptr }
        },
        "100"
    },
    {
        "brimir_profiling",
        "Performance Profiling",
        nullptr,
        "Log a performance breakdown every 300 frames. Enabled can be noisy.",
        nullptr,
        "system",
        {
            { "disabled", "OFF" },
            { "enabled", "ON" },
            { nullptr, nullptr }
        },
        "disabled"
    },
    { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {{nullptr, nullptr}}, nullptr }
};

bool brimir_deinterlace_effective(const char* deinterlacing, const char* mode) {
    const bool enabled = deinterlacing && std::strcmp(deinterlacing, "enabled") == 0;
    const bool modeOff = mode && std::strcmp(mode, "none") == 0;
    return enabled && !modeOff;
}

bool brimir_update_deinterlace(BrimirDeinterlaceOptions& cache, const char* deinterlacing, const char* mode,
                               bool force, bool& effective) {
    const char* newDeinterlacing = deinterlacing ? deinterlacing : cache.deinterlacing.c_str();
    const char* newMode = mode ? mode : cache.mode.c_str();
    const bool changed = cache.deinterlacing != newDeinterlacing || cache.mode != newMode;
    cache.deinterlacing = newDeinterlacing;
    cache.mode = newMode;
    effective = brimir_deinterlace_effective(cache.deinterlacing.c_str(), cache.mode.c_str());
    return force || changed;
}

brimir::CartridgeChoice brimir_cartridge_choice(const char* value) {
    using brimir::CartridgeChoice;
    static const struct {
        const char* value;
        CartridgeChoice choice;
    } kChoices[] = {
        { "none", CartridgeChoice::None },
        { "dram_1mb", CartridgeChoice::DRAM8Mbit },
        { "dram_4mb", CartridgeChoice::DRAM32Mbit },
        { "backup", CartridgeChoice::Backup },
        { "rom_kof95", CartridgeChoice::ROM_KOF95 },
        { "rom_ultraman", CartridgeChoice::ROM_Ultraman },
    };
    for (const auto& c : kChoices) {
        if (value && std::strcmp(value, c.value) == 0) {
            return c.choice;
        }
    }
    return CartridgeChoice::Auto;
}

namespace {

// A percentage option value: a multiple of 5 within [min, max], else the fallback (the option's default).
unsigned percent_value(const char* value, unsigned min, unsigned max, unsigned fallback) {
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    unsigned n = 0;
    for (const char* c = value; *c != '\0'; ++c) {
        if (*c < '0' || *c > '9' || n > max) {
            return fallback;
        }
        n = n * 10 + static_cast<unsigned>(*c - '0');
    }
    return n >= min && n <= max && n % 5 == 0 ? n : fallback;
}

} // namespace

brimir::InputSettings brimir_input_settings(const char* analog_deadzone, const char* trigger_deadzone,
                                            const char* mouse_sensitivity) {
    brimir::InputSettings settings;
    settings.analogDeadzone = percent_value(analog_deadzone, 0, 30, 15);
    settings.triggerDeadzone = percent_value(trigger_deadzone, 0, 30, 15);
    settings.mouseSensitivity = percent_value(mouse_sensitivity, 5, 200, 100);
    return settings;
}

const struct retro_core_options_v2* brimir_get_core_options_v2(void) {
    static struct retro_core_options_v2 options = {
        option_cats,
        option_defs
    };
    return &options;
}
