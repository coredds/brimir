// Brimir - Core options implementation
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "libretro.h"
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
    {
        "brimir_region",
        "Console Region",
        nullptr,
        "Force a specific region. Auto will detect from the disc.",
        nullptr,
        "system",
        {
            { "auto", "Auto-detect" },
            { "us", "North America" },
            { "eu", "Europe" },
            { "jp", "Japan" },
            { nullptr, nullptr }
        },
        "auto"
    },
    {
        "brimir_video_standard",
        "Video Standard",
        nullptr,
        "Force NTSC (60Hz) or PAL (50Hz). Auto will use region default.",
        nullptr,
        "video",
        {
            { "auto", "Auto" },
            { "ntsc", "NTSC (60Hz)" },
            { "pal", "PAL (50Hz)" },
            { nullptr, nullptr }
        },
        "auto"
    },
    {
        "brimir_deinterlacing",
        "Deinterlacing",
        nullptr,
        "Enable deinterlacing for interlaced video modes. Many games use interlaced high-res modes for menus (Panzer Dragoon Zwei, Grandia, etc). "
        "RECOMMENDED: Keep enabled. Uses optimized single-field rendering (Bob mode) for 60 FPS with no scanlines. "
        "Disable only for progressive-only games to skip post-processing.",
        nullptr,
        "video",
        {
            { "enabled", "Enabled (Recommended)" },
            { "disabled", "Disabled" },
            { nullptr, nullptr }
        },
        "enabled"
    },
    {
        "brimir_deinterlace_mode",
        "Deinterlacing Mode",
        nullptr,
        "Method for deinterlacing interlaced video modes (menus in Panzer Dragoon, Grandia, etc). "
        "Bob (RECOMMENDED): Duplicates current field to both lines - 60 FPS, no scanlines, smooth. Best for libretro. "
        "Weave: Shows alternating fields - 60 FPS, authentic CRT look with scanlines. "
        "Blend: Blends both fields - 60 FPS, may show ghosting. "
        "Current: Legacy dual-field threaded renderer - more accurate but slower (~45 FPS).",
        nullptr,
        "video",
        {
            { "bob", "Bob (Recommended - No Scanlines, 60 FPS)" },
            { "weave", "Weave (CRT Authentic, Scanlines)" },
            { "blend", "Blend (Ghosting Effect)" },
            { "current", "Current (Accurate, Slower)" },
            { "none", "None (Native Interlacing)" },
            { nullptr, nullptr }
        },
        "bob"
    },
    {
        "brimir_horizontal_blend",
        "Horizontal Blend (Interlaced)",
        nullptr,
        "Apply horizontal blur filter in high-res interlaced modes (>=640 width) to reduce combing artifacts. "
        "Inspired by Mednafen's ss.h_blend. Blends adjacent horizontal pixels for sharper perceived image. "
        "RECOMMENDED: Enable with Bob mode for smoothest interlaced output. "
        "Minimal performance impact (<1ms) on modern hardware.",
        nullptr,
        "video",
        {
            { "enabled", "Enabled (Recommended)" },
            { "disabled", "Disabled" },
            { nullptr, nullptr }
        },
        "enabled"
    },
    {
        "brimir_h_overscan",
        "Horizontal Overscan",
        nullptr,
        "Show horizontal overscan area. Many Saturn games render content in this region. "
        "Mednafen default: Enabled. Disable to crop ~8 pixels from each side for cleaner edges.",
        nullptr,
        "video",
        {
            { "enabled", "Enabled (Mednafen Default)" },
            { "disabled", "Disabled (Crop Edges)" },
            { nullptr, nullptr }
        },
        "enabled"
    },
    {
        "brimir_v_overscan",
        "Vertical Overscan",
        nullptr,
        "Show vertical overscan area. Reveals full vertical rendering. "
        "Mednafen default: Enabled. Disable to crop ~8 pixels from top/bottom for cleaner edges.",
        nullptr,
        "video",
        {
            { "enabled", "Enabled (Mednafen Default)" },
            { "disabled", "Disabled (Crop Edges)" },
            { nullptr, nullptr }
        },
        "enabled"
    },
    {
        "brimir_frameskip",
        "Frameskip",
        nullptr,
        "Skip frames to improve performance. 0 = disabled.",
        nullptr,
        "video",
        {
            { "0", "Disabled" },
            { "1", "Skip 1 frame" },
            { "2", "Skip 2 frames" },
            { "3", "Skip 3 frames" },
            { nullptr, nullptr }
        },
        "0"
    },
    {
        "brimir_autodetect_region",
        "Auto-Detect Region from Disc",
        nullptr,
        "Automatically set console region based on loaded game disc. When enabled, the Console Region setting is overridden by the disc's region code.",
        nullptr,
        "system",
        {
            { "enabled", "Enabled" },
            { "disabled", "Disabled" },
            { nullptr, nullptr }
        },
        "enabled"
    },
    {
        "brimir_audio_interpolation",
        "Audio Interpolation",
        nullptr,
        "Sample interpolation method. Linear is hardware accurate. Nearest Neighbor is faster but introduces aliasing.",
        nullptr,
        "audio",
        {
            { "linear", "Linear (Accurate)" },
            { "nearest", "Nearest Neighbor (Fast)" },
            { nullptr, nullptr }
        },
        "linear"
    },
    {
        "brimir_cd_speed",
        "CD Read Speed",
        nullptr,
        "CD-ROM read speed multiplier. Higher values reduce loading times but may break some games. 2x matches real hardware.",
        nullptr,
        "media",
        {
            { "2", "2x (Accurate)" },
            { "4", "4x" },
            { "6", "6x" },
            { "8", "8x" },
            { "12", "12x" },
            { "16", "16x" },
            { nullptr, nullptr }
        },
        "2"
    },
    { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {{nullptr, nullptr}}, nullptr }
};

const struct retro_core_options_v2* brimir_get_core_options_v2(void) {
    static struct retro_core_options_v2 options = {
        option_cats,
        option_defs
    };
    return &options;
}

