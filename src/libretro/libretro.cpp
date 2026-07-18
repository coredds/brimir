// Brimir - Sega Saturn libretro core based on Ymir
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "libretro.h"
#include "options.hpp"
#include "brimir/core_wrapper.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <memory>
#include <string>

// Libretro callbacks
static retro_log_printf_t log_cb = nullptr;
static retro_video_refresh_t video_cb = nullptr;
static retro_audio_sample_t audio_cb = nullptr;
static retro_audio_sample_batch_t audio_batch_cb = nullptr;
static retro_input_poll_t input_poll_cb = nullptr;
static retro_input_state_t input_state_cb = nullptr;
static retro_environment_t environ_cb = nullptr;

// Core instance
static std::unique_ptr<brimir::CoreWrapper> g_core;

// Memory descriptors for RetroArch's memory viewer / cheat search
// ptr fields are updated on each game load; struct layout must match
// declaration order in libretro.h (flags, ptr, offset, start, select, disconnect, len, addrspace)
static struct retro_memory_descriptor g_memdesc[] = {
    { RETRO_MEMDESC_SYSTEM_RAM, nullptr, 0, 0x00200000, 0, 0, 1024 * 1024, "SYSARAM" },
    { RETRO_MEMDESC_SAVE_RAM,   nullptr, 0, 0x00100000, 0, 0, 32 * 1024,   "SYSARAM" },
    { RETRO_MEMDESC_SYSTEM_RAM, nullptr, 0, 0x06000000, 0, 0, 1024 * 1024, "SYSARAM" },
};
static const unsigned g_memdesc_count = sizeof(g_memdesc) / sizeof(g_memdesc[0]);

// Whether the frontend supports RETRO_DEVICE_ID_JOYPAD_MASK (single-call input read)
static bool g_input_bitmask_supported = false;

// Helper function for logging
static void brimir_log(retro_log_level level, const char* fmt, ...) {
    if (!log_cb) return;
    
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    log_cb(level, "[Brimir] %s\n", buffer);
}

// Disk control callback forward declarations
static bool disk_set_eject_state(bool ejected);
static bool disk_get_eject_state(void);
static unsigned disk_get_image_index(void);
static bool disk_set_image_index(unsigned index);
static unsigned disk_get_num_images(void);
static bool disk_replace_image_index(unsigned index, const struct retro_game_info* info);
static bool disk_add_image_index(void);
static bool disk_set_initial_image(unsigned index, const char* path);
static bool disk_get_image_path(unsigned index, char* s, size_t len);
static bool disk_get_image_label(unsigned index, char* s, size_t len);


// Helper function to get option value
static const char* get_option_value(const char* key, const char* default_value = nullptr) {
    if (!environ_cb) return default_value;
    
    struct retro_variable var = { key, nullptr };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        return var.value;
    }
    return default_value;
}

// Cached core option values so Quick Menu changes can be applied live each frame.
// Defaults here must stay in sync with src/libretro/options.cpp for each key.
struct OptionCache {
    std::string audio_interp = "linear";
    std::string cd_speed = "2";
    std::string sh2_overclock = "100";
    std::string autodetect_region = "enabled";
    std::string deinterlacing = "enabled";
    std::string deinterlace_mode = "bob";
    std::string audio_volume = "100";
    std::string rotation = "0";
    std::string overscan = "0";
    std::string profiling = "disabled";
} g_options;

static void apply_core_options(bool force) {
    if (!g_core) return;

    auto apply = [force](const char* key, std::string& cached, auto setter) {
        const char* value = get_option_value(key, cached.c_str());
        if (force || cached != value) {
            cached = std::string(value);
            setter(value);
        }
    };

    apply("brimir_audio_interpolation",     g_options.audio_interp,     [](const char* v){ g_core->SetAudioInterpolation(v); });
    apply("brimir_cd_speed",                g_options.cd_speed,         [](const char* v){ g_core->SetCDReadSpeed(static_cast<uint8_t>(atoi(v))); });
    apply("brimir_sh2_overclock",           g_options.sh2_overclock,    [](const char* v){ g_core->SetSH2OverclockFactor(static_cast<uint32_t>(atoi(v))); });
    apply("brimir_autodetect_region",       g_options.autodetect_region,[](const char* v){ g_core->SetAutodetectRegion(strcmp(v, "enabled") == 0); });
    apply("brimir_deinterlacing",           g_options.deinterlacing,    [](const char* v){ g_core->SetDeinterlacing(strcmp(v, "enabled") == 0); });
    apply("brimir_deinterlace_mode",        g_options.deinterlace_mode, [](const char* v){ g_core->SetDeinterlacingMode(v); });
    apply("brimir_audio_volume",            g_options.audio_volume,     [](const char* v){ g_core->SetAudioVolume(atoi(v)); });
    apply("brimir_rotation",                g_options.rotation,         [](const char* v){ g_core->SetRotation(atoi(v)); });
    apply("brimir_overscan",                g_options.overscan,         [](const char* v){
        int overscan = atoi(v);
        g_core->SetOverscanCrop(overscan * 16, overscan * 16);
    });
    apply("brimir_profiling",               g_options.profiling,        [](const char* /*v*/){});
}

// Libretro API implementation

RETRO_API void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    
    // Set core options v2
    unsigned version = 0;
    if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
        const struct retro_core_options_v2* options_v2 = brimir_get_core_options_v2();
        cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, (void*)options_v2);
    }
    
    // Set core options
    bool no_content = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);
    
    // Set input descriptors for controller remapping
    // Sega Saturn Digital Pad layout:
    // - D-Pad (Up, Down, Left, Right)
    // - 6 face buttons: A, B, C (bottom row) and X, Y, Z (top row)
    // - L and R shoulder buttons
    // - Start button
    //
    // Button mapping (from bridge/core_wrapper.cpp):
    // RetroPad B  -> Saturn A
    // RetroPad A  -> Saturn B
    // RetroPad Y  -> Saturn C
    // RetroPad X  -> Saturn X
    // RetroPad L2 -> Saturn Y
    // RetroPad R2 -> Saturn Z
    // RetroPad L  -> Saturn L (shoulder)
    // RetroPad R  -> Saturn R (shoulder)
    static const struct retro_input_descriptor input_desc[] = {
        // Player 1
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "B" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "C" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "X" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Y" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Z" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
        
        // Player 2
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "B" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "C" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "X" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Y" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Z" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
        
        // Terminator
        { 0 },
    };
    
    cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)input_desc);
    
    // Request log interface
    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
        log_cb = logging.log;
    }

    // Check for bitmask input support (avoids 14 separate calls per player per frame)
    g_input_bitmask_supported = cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, nullptr);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) {
    video_cb = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) {
    audio_cb = cb;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    audio_batch_cb = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb) {
    input_poll_cb = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb) {
    input_state_cb = cb;
}

RETRO_API void retro_init(void) {
    brimir_log(RETRO_LOG_INFO, "Brimir initializing...");
    brimir_log(RETRO_LOG_INFO, "Based on Ymir emulator by StrikerX3");
    
    // Create core instance
    g_core = std::make_unique<brimir::CoreWrapper>();
    
    if (!g_core->Initialize()) {
        brimir_log(RETRO_LOG_ERROR, "Failed to initialize core");
        g_core.reset();
        return;
    }

    // Register disk control interface for multi-disc games
    unsigned dc_version = 0;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION, &dc_version) && dc_version >= 1) {
        static struct retro_disk_control_ext_callback disk_cb = {
            disk_set_eject_state,
            disk_get_eject_state,
            disk_get_image_index,
            disk_set_image_index,
            disk_get_num_images,
            disk_replace_image_index,
            disk_add_image_index,
            disk_set_initial_image,
            disk_get_image_path,
            disk_get_image_label,
        };
        environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &disk_cb);
        brimir_log(RETRO_LOG_INFO, "Disk control ext interface registered");
    }
}

RETRO_API void retro_deinit(void) {
    brimir_log(RETRO_LOG_INFO, "Brimir shutting down");
    
    if (g_core) {
        g_core->Shutdown();
        g_core.reset();
    }
}

RETRO_API unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info* info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "Brimir";
    info->library_version = "0.4.6";
    info->need_fullpath = true;
    info->valid_extensions = "chd|cue|bin|iso|ccd|img|mds|mdf|m3u";
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
    memset(info, 0, sizeof(*info));
    
    // Base resolution for Saturn (varies between games)
    info->geometry.base_width = 320;
    info->geometry.base_height = 224;
    
    // Saturn max native resolution
    info->geometry.max_width = 704;
    info->geometry.max_height = 512;
    info->geometry.aspect_ratio = 4.0f / 3.0f;

    // Fallback timing. The actual regional framerate (NTSC/PAL) is set once the
    // disc region is known via update_system_av_info_for_region() in retro_load_game.
    info->timing.fps = 59.94;
    info->timing.sample_rate = 44100.0;
}

static void update_system_av_info_for_region(void) {
    if (!environ_cb || !g_core) return;

    struct retro_system_av_info avi;
    retro_get_system_av_info(&avi);

    if (g_core->GetConsoleRegion() == brimir::ConsoleRegion::PAL) {
        avi.timing.fps = 50.0;
    } else {
        avi.timing.fps = 59.94;
    }

    environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &avi);
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
    brimir_log(RETRO_LOG_INFO, "Controller port %u set to device %u", port, device);
}

RETRO_API void retro_reset(void) {
    brimir_log(RETRO_LOG_INFO, "Reset requested");
    
    if (g_core) {
        g_core->Reset();
    }
}

RETRO_API void retro_run(void) {
    if (!g_core) {
        return;
    }

    // Only re-query core options when the frontend reports a change. This avoids
    // string comparisons and setter calls on every frame when nothing changed.
    bool updated = false;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        apply_core_options(false);
    }

    // Performance profiling: dump report every 300 frames if enabled
    if (g_options.profiling == "enabled") {
        static size_t frame_count = 0;
        frame_count++;
        if (frame_count == 300) {
            brimir_log(RETRO_LOG_INFO, "=== Performance Profile (300 frames) ===");
            std::string report = g_core->GetProfilingReport();
            size_t pos = 0;
            while (pos < report.size()) {
                size_t end = report.find('\n', pos);
                if (end == std::string::npos) end = report.size();
                std::string line = report.substr(pos, end - pos);
                if (!line.empty()) {
                    brimir_log(RETRO_LOG_INFO, "%s", line.c_str());
                }
                pos = end + 1;
            }
            g_core->ResetProfiling();
            frame_count = 0;
        }
    }

    // Poll input
    if (input_poll_cb) {
        input_poll_cb();
    }
    
    // Read and update controller input for both players
    if (input_state_cb) {
        uint16_t buttons_p1 = 0;
        uint16_t buttons_p2 = 0;

        if (g_input_bitmask_supported) {
            // Single call returns all buttons as a bitmask - bits match RETRO_DEVICE_ID_JOYPAD_* values
            buttons_p1 = static_cast<uint16_t>(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK));
            buttons_p2 = static_cast<uint16_t>(input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK));
        } else {
            // Fallback: query each button individually
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))      buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_B);
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))      buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_Y);
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT)) buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_SELECT);
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))  buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_START);
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))     buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_UP);
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))   buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_DOWN);
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))   buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))  buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT);
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))      buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_A);
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X))      buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_X);
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L))      buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_L);
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R))      buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_R);
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2))     buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_L2);
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2))     buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_R2);

            if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))      buttons_p2 |= (1 << RETRO_DEVICE_ID_JOYPAD_B);
            if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))      buttons_p2 |= (1 << RETRO_DEVICE_ID_JOYPAD_Y);
            if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT)) buttons_p2 |= (1 << RETRO_DEVICE_ID_JOYPAD_SELECT);
            if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))  buttons_p2 |= (1 << RETRO_DEVICE_ID_JOYPAD_START);
            if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))     buttons_p2 |= (1 << RETRO_DEVICE_ID_JOYPAD_UP);
            if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))   buttons_p2 |= (1 << RETRO_DEVICE_ID_JOYPAD_DOWN);
            if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))   buttons_p2 |= (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
            if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))  buttons_p2 |= (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT);
            if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))      buttons_p2 |= (1 << RETRO_DEVICE_ID_JOYPAD_A);
            if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X))      buttons_p2 |= (1 << RETRO_DEVICE_ID_JOYPAD_X);
            if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L))      buttons_p2 |= (1 << RETRO_DEVICE_ID_JOYPAD_L);
            if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R))      buttons_p2 |= (1 << RETRO_DEVICE_ID_JOYPAD_R);
            if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2))     buttons_p2 |= (1 << RETRO_DEVICE_ID_JOYPAD_L2);
            if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2))     buttons_p2 |= (1 << RETRO_DEVICE_ID_JOYPAD_R2);
        }

        g_core->SetControllerState(0, buttons_p1);
        g_core->SetControllerState(1, buttons_p2);
    }
    
    // Run one frame of emulation
    g_core->RunFrame();
    
    // Output video
    if (video_cb) {
        const void* fb = g_core->GetFramebuffer();
        unsigned int width = g_core->GetFramebufferWidth();
        unsigned int height = g_core->GetFramebufferHeight();
        unsigned int pitch = g_core->GetFramebufferPitch();
        
        // Update geometry whenever dimensions change, including the first frame.
        // retro_get_system_av_info() reports a fixed fallback base size, so we
        // correct it here as soon as the running content produces its first frame.
        static unsigned int s_lastWidth = 0, s_lastHeight = 0;
        if (width != s_lastWidth || height != s_lastHeight) {
            struct retro_game_geometry geo = {};
            geo.base_width = width;
            geo.base_height = height;
            geo.max_width = 704;
            geo.max_height = 512;
            geo.aspect_ratio = 4.0f / 3.0f;
            environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geo);
            s_lastWidth = width;
            s_lastHeight = height;
        }
        
        video_cb(fb, width, height, pitch);
    }
    
    // Output audio
    if (audio_batch_cb) {
        int16_t audio_buffer[2048 * 2]; // Stereo buffer
        size_t samples = g_core->GetAudioSamples(audio_buffer, 2048);
        if (samples > 0) {
            audio_batch_cb(audio_buffer, samples);
        }
    }
}

RETRO_API size_t retro_serialize_size(void) {
    if (!g_core) {
        return 0;
    }
    
    return g_core->GetStateSize();
}

RETRO_API bool retro_serialize(void* data, size_t size) {
    if (!g_core) {
        return false;
    }
    
    return g_core->SaveState(data, size);
}

RETRO_API bool retro_unserialize(const void* data, size_t size) {
    if (!g_core) {
        return false;
    }
    
    return g_core->LoadState(data, size);
}

RETRO_API void retro_cheat_reset(void) {
    // TODO: Implement cheat support
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char* code) {
    // TODO: Implement cheat support
    (void)index;
    (void)enabled;
    (void)code;
}

RETRO_API bool retro_load_game(const struct retro_game_info* game) {
    if (!game) {
        brimir_log(RETRO_LOG_ERROR, "No game provided");
        return false;
    }
    
    if (!g_core) {
        brimir_log(RETRO_LOG_ERROR, "Core not initialized");
        return false;
    }
    
    brimir_log(RETRO_LOG_INFO, "Loading game: %s", game->path ? game->path : "unknown");
    
    // Set pixel format - XRGB8888 for full quality (lossless from VDP output)
    // This is a lossless channel swap from VDP's native XBGR8888, replacing the old
    // lossy RGB565 conversion.
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        brimir_log(RETRO_LOG_ERROR, "XRGB8888 pixel format is not supported by this frontend");
        return false;
    }
    
    // Get system directory from RetroArch (needed for BIOS and RTC)
    const char* system_dir = nullptr;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) || !system_dir) {
        brimir_log(RETRO_LOG_ERROR, "Could not get system directory");
        return false;
    }
    
    // Get save directory from RetroArch for persistent backup RAM
    const char* save_dir = nullptr;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) || !save_dir) {
        brimir_log(RETRO_LOG_WARN, "Could not get save directory, using temp directory");
    } else {
        brimir_log(RETRO_LOG_INFO, "Save directory: %s", save_dir);
    }
    
    // Load BIOS if not already loaded
    if (!g_core->IsIPLLoaded()) {
        brimir_log(RETRO_LOG_INFO, "Loading BIOS...");
        
        // Get BIOS preference from core options
        const char* bios_pref = get_option_value("brimir_bios", "auto");
        
        // Map of BIOS option keys to filenames
        struct BIOSEntry {
            const char* key;
            const char* filename;
        };
        
        static const BIOSEntry bios_map[] = {
            { "jp_101", "Sega Saturn BIOS v1.01 (JAP).bin" },
            { "jp_100", "Sega Saturn BIOS v1.00 (JAP).bin" },
            { "us_101", "sega_101.bin" },
            { "us_100", "mpr-17933.bin" },
            { "eu_100", "sega_100.bin" },
            { "eu_alt", "Sega Saturn BIOS (EUR).bin" },
        };
        static const int bios_count = sizeof(bios_map) / sizeof(bios_map[0]);
        
        bool bios_loaded = false;
        
        // If user selected a specific BIOS, try only that one
        if (strcmp(bios_pref, "auto") != 0) {
            for (int i = 0; i < bios_count; i++) {
                if (strcmp(bios_pref, bios_map[i].key) == 0) {
                    char bios_path[4096];
                    snprintf(bios_path, sizeof(bios_path), "%s%c%s", system_dir,
#ifdef _WIN32
                        '\\',
#else
                        '/',
#endif
                        bios_map[i].filename);
                    
                    if (g_core->LoadIPLFromFile(bios_path)) {
                        brimir_log(RETRO_LOG_INFO, "Loaded BIOS: %s", bios_map[i].filename);
                        bios_loaded = true;
                    } else {
                        brimir_log(RETRO_LOG_ERROR, "Selected BIOS not found: %s", bios_map[i].filename);
                    }
                    break;
                }
            }
        } else {
            // Auto-detect: try Japanese BIOS first for better compatibility
            // Japanese games work better with Japanese BIOS
            // Note: sega1003.bin (JP v1.003) is NOT included - it has compatibility issues
            const char* preferred_order[] = {
                "Sega Saturn BIOS v1.01 (JAP).bin", // JP v1.01 (try first)
                "Sega Saturn BIOS v1.00 (JAP).bin", // JP v1.00
                "sega_101.bin",  // US v1.01
                "mpr-17933.bin", // US v1.00
                "sega_100.bin",  // EU v1.00
                "Sega Saturn BIOS (EUR).bin"
            };
            
            for (const char* filename : preferred_order) {
                char bios_path[4096];
                snprintf(bios_path, sizeof(bios_path), "%s%c%s", system_dir,
#ifdef _WIN32
                    '\\',
#else
                    '/',
#endif
                    filename);
                
                if (g_core->LoadIPLFromFile(bios_path)) {
                    brimir_log(RETRO_LOG_INFO, "Auto-detected and loaded BIOS: %s", filename);
                    bios_loaded = true;
                    break;
                }
            }
        }
        
        if (!bios_loaded) {
            brimir_log(RETRO_LOG_ERROR, "Failed to load BIOS. Place one of the following in system directory: %s", system_dir);
            brimir_log(RETRO_LOG_ERROR, "  - Sega Saturn BIOS v1.01 (JAP).bin (JP v1.01 - recommended)");
            brimir_log(RETRO_LOG_ERROR, "  - Sega Saturn BIOS v1.00 (JAP).bin (JP v1.00)");
            brimir_log(RETRO_LOG_ERROR, "  - sega_101.bin (US v1.01)");
            brimir_log(RETRO_LOG_ERROR, "  - mpr-17933.bin (US v1.00)");
            brimir_log(RETRO_LOG_ERROR, "  - sega_100.bin (EU v1.00)");
            brimir_log(RETRO_LOG_ERROR, "  - Sega Saturn BIOS (EUR).bin");
            brimir_log(RETRO_LOG_ERROR, "Note: sega1003.bin (JP v1.003) is NOT supported due to compatibility issues");
            return false;
        }
    }
    
    brimir_log(RETRO_LOG_INFO, "Loading game: %s", game->path);

    if (!g_core->LoadGame(game->path, save_dir, system_dir)) {
        const std::string& error = g_core->GetLastError();
        brimir_log(RETRO_LOG_ERROR, "Failed to load game: %s",
            error.empty() ? "(no error message)" : error.c_str());
        return false;
    }

    brimir_log(RETRO_LOG_INFO, "Game loaded successfully");

    // Apply core options now and cache them for live updates each frame
    apply_core_options(true);

    update_system_av_info_for_region();

    g_core->SetRenderer("software");

    // Register memory descriptors for RetroArch's memory viewer / cheat search
    // Use raw pointer getters to avoid triggering .srm sync side effects
    g_memdesc[0].ptr = g_core->GetSystemRAMRawPointer();
    g_memdesc[1].ptr = g_core->GetSRAMRawPointer();
    g_memdesc[2].ptr = g_core->GetSystemRAMHighRawPointer();

    static struct retro_memory_map mmap = {
        .descriptors     = g_memdesc,
        .num_descriptors = g_memdesc_count,
    };
    environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmap);

    return true;
}

RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info) {
    // Not used for Saturn
    (void)game_type;
    (void)info;
    (void)num_info;
    return false;
}

RETRO_API void retro_unload_game(void) {
    brimir_log(RETRO_LOG_INFO, "Unloading game");
    
    if (g_core) {
        g_core->UnloadGame();
    }
    
}

// Disk control callbacks

static bool disk_set_eject_state(bool ejected) {
    if (!g_core) return false;
    return g_core->SetEjectState(ejected);
}

static bool disk_get_eject_state(void) {
    if (!g_core) return false;
    return g_core->GetEjectState();
}

static unsigned disk_get_image_index(void) {
    if (!g_core) return 0;
    return static_cast<unsigned>(g_core->GetCurrentDiscIndex());
}

static bool disk_set_image_index(unsigned index) {
    if (!g_core) return false;
    return g_core->SetDiscIndex(index);
}

static unsigned disk_get_num_images(void) {
    if (!g_core) return 0;
    return static_cast<unsigned>(g_core->GetNumDiscs());
}

static bool disk_replace_image_index(unsigned index, const struct retro_game_info* info) {
    if (!g_core) return false;
    if (info) {
        return g_core->ReplaceDiscIndex(index, info->path);
    } else {
        return g_core->ReplaceDiscIndex(index, nullptr);
    }
}

static bool disk_add_image_index(void) {
    if (!g_core) return false;
    return g_core->AddDiscIndex();
}

static bool disk_set_initial_image(unsigned index, const char* path) {
    if (!g_core) return false;
    return g_core->SetInitialDisc(index, path);
}

static bool disk_get_image_path(unsigned index, char* s, size_t len) {
    if (!g_core) return false;
    return g_core->GetDiscPath(index, s, len);
}

static bool disk_get_image_label(unsigned index, char* s, size_t len) {
    if (!g_core) return false;
    return g_core->GetDiscLabel(index, s, len);
}

RETRO_API unsigned retro_get_region(void) {
    if (g_core && g_core->IsGameLoaded()) {
        return g_core->GetConsoleRegion() == brimir::ConsoleRegion::PAL
               ? RETRO_REGION_PAL
               : RETRO_REGION_NTSC;
    }
    return RETRO_REGION_NTSC;
}

RETRO_API void* retro_get_memory_data(unsigned id) {
    if (!g_core) {
        return nullptr;
    }
    
    switch (id) {
        case RETRO_MEMORY_SAVE_RAM:
            return g_core->GetSRAMData();
        case RETRO_MEMORY_SYSTEM_RAM:
            return g_core->GetSystemRAMData();
        default:
            return nullptr;
    }
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
    if (!g_core) {
        return 0;
    }
    
    switch (id) {
        case RETRO_MEMORY_SAVE_RAM:
            return g_core->GetSRAMSize();
        case RETRO_MEMORY_SYSTEM_RAM:
            return g_core->GetSystemRAMSize();
        default:
            return 0;
    }
}