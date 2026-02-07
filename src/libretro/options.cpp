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
        "brimir_renderer",
        "Renderer",
        nullptr,
        "Choose between software or GPU (Vulkan) rendering. "
        "Software: Accurate, compatible, optimized. "
        "Vulkan: Experimental GPU acceleration with potential for higher performance and upscaling. "
        "Requires Vulkan support. Falls back to software if unavailable. "
        "CHECK LOG to verify GPU is active!",
        nullptr,
        "video",
        {
            { "software", "Software" },
            { "vulkan", "Vulkan (Experimental)" },
            { nullptr, nullptr }
        },
        "software"
    },
    {
        "brimir_internal_resolution",
        "Internal Resolution (GPU Only)",
        nullptr,
        "Render at higher internal resolution for sharper graphics. "
        "GPU ONLY - If this works, GPU rendering is active! "
        "Software renderer will ignore this setting. "
        "Higher values improve quality but reduce performance.",
        nullptr,
        "video",
        {
            { "1", "1x (Native)" },
            { "2", "2x (Sharp)" },
            { "4", "4x (Very Sharp)" },
            { "8", "8x (Maximum)" },
            { nullptr, nullptr }
        },
        "1"
    },
    {
        "brimir_upscale_filter",
        "Upscale Filter (GPU Only)",
        nullptr,
        "Filtering method used when upscaling the software-rendered frame. "
        "Nearest: Sharp pixel edges, no blurring. "
        "Bilinear: Smooth but can look blurry. "
        "Sharp Bilinear: Best of both - smooth with sharp pixel edges. "
        "FSR: AMD FidelityFX Super Resolution 1.0 - edge-adaptive upscaling for highest quality.",
        nullptr,
        "video",
        {
            { "sharp_bilinear", "Sharp Bilinear" },
            { "nearest", "Nearest" },
            { "bilinear", "Bilinear" },
            { "fsr", "FSR 1.0 (Edge Adaptive)" },
            { nullptr, nullptr }
        },
        "sharp_bilinear"
    },
    {
        "brimir_sharpening",
        "Sharpening / Post-Processing (GPU Only)",
        nullptr,
        "Post-processing pass applied after upscaling. "
        "OFF: No post-processing. "
        "FXAA: Fast Approximate Anti-Aliasing - smooths jagged edges. "
        "RCAS: Robust Contrast-Adaptive Sharpening (FSR 1.0) - sharpens edges without artifacts. "
        "Best combined with FSR upscaling for full FSR quality.",
        nullptr,
        "video",
        {
            { "disabled", "OFF" },
            { "fxaa", "FXAA" },
            { "rcas", "RCAS Sharpening (FSR 1.0)" },
            { nullptr, nullptr }
        },
        "disabled"
    },
    {
        "brimir_debanding",
        "Color Debanding (GPU Only)",
        nullptr,
        "Reduce color banding artifacts from Saturn's RGB555 palette (32 shades per channel). "
        "Adds subtle noise to smooth out visible color steps in gradients and shadows. "
        "Most noticeable on sky gradients and dark areas.",
        nullptr,
        "video",
        {
            { "disabled", "OFF" },
            { "enabled", "ON" },
            { nullptr, nullptr }
        },
        "disabled"
    },
    {
        "brimir_brightness",
        "Brightness (GPU Only)",
        nullptr,
        "Adjust output brightness. 1.0 is default.",
        nullptr,
        "video",
        {
            { "0.8", "0.8" },
            { "0.9", "0.9" },
            { "1.0", "1.0 (Default)" },
            { "1.1", "1.1" },
            { "1.2", "1.2" },
            { nullptr, nullptr }
        },
        "1.0"
    },
    {
        "brimir_gamma",
        "Gamma Correction (GPU Only)",
        nullptr,
        "Adjust gamma curve. 1.0 is linear (default). Higher values brighten dark areas.",
        nullptr,
        "video",
        {
            { "0.8", "0.8" },
            { "1.0", "1.0 (Default)" },
            { "1.2", "1.2" },
            { "1.4", "1.4" },
            { "1.8", "1.8" },
            { "2.2", "2.2 (sRGB)" },
            { nullptr, nullptr }
        },
        "1.0"
    },
    {
        "brimir_deinterlacing",
        "Deinterlacing",
        nullptr,
        "Enable deinterlacing for interlaced video modes. Many games use interlaced high-res modes for menus. "
        "Uses optimized single-field rendering for 60 FPS with no scanlines. "
        "Disable only for progressive-only games to skip post-processing.",
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
        "Method for deinterlacing interlaced video modes. "
        "Bob: Duplicates current field to both lines (60 FPS, no scanlines, smooth). "
        "Weave: Shows alternating fields (60 FPS, CRT-style with scanlines). "
        "Blend: Blends both fields (60 FPS, may show ghosting). "
        "Current: Legacy dual-field renderer (more accurate but slower, ~45 FPS). "
        "None: No deinterlacing (native interlaced output).",
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
        "brimir_horizontal_blend",
        "Horizontal Blend (Interlaced)",
        nullptr,
        "Apply horizontal blur filter in high-res interlaced modes (640+ width) to reduce combing artifacts. "
        "Blends adjacent horizontal pixels for smoother perceived image. "
        "Recommended with Bob deinterlacing mode. Minimal performance impact (<1ms).",
        nullptr,
        "video",
        {
            { "disabled", "OFF" },
            { "enabled", "ON" },
            { nullptr, nullptr }
        },
        "disabled"
    },
    {
        "brimir_h_overscan",
        "Horizontal Overscan",
        nullptr,
        "Show horizontal overscan area. Many Saturn games render content in this region. "
        "Disable to crop 8 pixels from each side for cleaner edges.",
        nullptr,
        "video",
        {
            { "disabled", "OFF (Crop 8px)" },
            { "enabled", "ON (Full Width)" },
            { nullptr, nullptr }
        },
        "enabled"
    },
    {
        "brimir_v_overscan",
        "Vertical Overscan",
        nullptr,
        "Show vertical overscan area. Reveals full vertical rendering. "
        "Disable to crop 8 pixels from top/bottom for cleaner edges.",
        nullptr,
        "video",
        {
            { "disabled", "OFF (Crop 8px)" },
            { "enabled", "ON (Full Height)" },
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

