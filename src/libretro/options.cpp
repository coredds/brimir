// Brimir - Core options implementation
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "options.hpp"
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
    { nullptr, nullptr, nullptr }
};

static struct retro_core_option_v2_definition option_defs[] = {
    {
        "brimir_bios",
        "BIOS Selection",
        nullptr,
        "Select which BIOS file to use. Auto will search in priority order.",
        nullptr,
        "system",
        {
            { "auto", "Auto-detect" },
            { "jp_101", "JP v1.01 (Sega Saturn BIOS v1.01 (JAP).bin)" },
            { "jp_100", "JP v1.00 (Sega Saturn BIOS v1.00 (JAP).bin)" },
            { "us_101", "US v1.01 (sega_101.bin)" },
            { "us_100", "US v1.00 (mpr-17933.bin)" },
            { "eu_100", "EU v1.00 (sega_100.bin)" },
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
        "Software draws and presents every frame on the CPU. Vulkan presents the frames through the frontend's "
        "Vulkan driver (the emulation itself is unchanged for now). Vulkan needs a Vulkan 1.2 GPU and the vulkan "
        "video driver; when the frontend cannot provide it the core falls back to software. Applies when content "
        "is loaded.",
        nullptr,
        "video",
        {
            { "software", "Software" },
            { "vulkan", "Vulkan" },
            { nullptr, nullptr }
        },
        "software"
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

const struct retro_core_options_v2* brimir_get_core_options_v2(void) {
    static struct retro_core_options_v2 options = {
        option_cats,
        option_defs
    };
    return &options;
}
