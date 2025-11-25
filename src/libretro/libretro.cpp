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

// SRAM sync state (reset on game load/unload)
static bool g_sram_synced = false;

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

// Helper function to get option value
static const char* get_option_value(const char* key, const char* default_value = nullptr) {
    if (!environ_cb) return default_value;
    
    struct retro_variable var = { key, nullptr };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        return var.value;
    }
    return default_value;
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
    bool no_content = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);
    
    // Request log interface
    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
        log_cb = logging.log;
    }
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
    info->library_version = "0.1.1";
    info->need_fullpath = true;
    info->valid_extensions = "chd|cue|bin|iso|ccd|img|mds|mdf|m3u";
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
    memset(info, 0, sizeof(*info));
    
    // Saturn resolution (can vary, this is a common default)
    info->geometry.base_width = 320;
    info->geometry.base_height = 224;
    info->geometry.max_width = 704;
    info->geometry.max_height = 512;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    
    // NTSC timing
    info->timing.fps = 59.94;
    info->timing.sample_rate = 44100.0;
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
    
    // On the very first frame: Restore .bup data to our buffer
    // Problem: RetroArch loaded .srm into our buffer, overwriting the .bup data!
    // Solution: Read from Ymir's .bup AGAIN to restore the clock settings
    if (!g_sram_synced) {
        g_core->RefreshSRAMFromEmulator();
        brimir_log(RETRO_LOG_INFO, "Restored .bup data - clock should persist now");
        g_sram_synced = true;
    }
    
    // Performance profiling: dump report every 300 frames (~5 seconds)
    static size_t frame_count = 0;
    frame_count++;
    if (frame_count == 300) {
        brimir_log(RETRO_LOG_INFO, "=== Performance Profile (300 frames) ===");
        std::string report = g_core->GetProfilingReport();
        // Split report by newlines and log each line
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
    
    // Poll input
    if (input_poll_cb) {
        input_poll_cb();
    }
    
    // Read and update controller input for both players
    if (input_state_cb) {
        // Player 1
        // Build button mask using libretro standard button IDs as bit positions
        uint16_t buttons_p1 = 0;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))      buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_B);       // bit 0
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))      buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_Y);       // bit 1
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT)) buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_SELECT);  // bit 2
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))  buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_START);   // bit 3
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))     buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_UP);      // bit 4
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))   buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_DOWN);    // bit 5
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))   buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);    // bit 6
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))  buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT);   // bit 7
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))      buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_A);       // bit 8
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X))      buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_X);       // bit 9
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L))      buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_L);       // bit 10
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R))      buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_R);       // bit 11
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2))     buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_L2);      // bit 12
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2))     buttons_p1 |= (1 << RETRO_DEVICE_ID_JOYPAD_R2);      // bit 13
        
        g_core->SetControllerState(0, buttons_p1);
        
        // Player 2
        uint16_t buttons_p2 = 0;
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
    
    // Reset SRAM sync flag for new game
    g_sram_synced = false;
    
    brimir_log(RETRO_LOG_INFO, "Loading game: %s", game->path ? game->path : "unknown");
    
    // Set pixel format
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        brimir_log(RETRO_LOG_ERROR, "RGB565 is not supported");
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
    
    // Load the game (with save directory for per-game saves and system directory for RTC)
    brimir_log(RETRO_LOG_INFO, "Calling LoadGame...");
    brimir_log(RETRO_LOG_INFO, "  Path: %s", game->path);
    brimir_log(RETRO_LOG_INFO, "  Save dir: %s", save_dir ? save_dir : "(null)");
    brimir_log(RETRO_LOG_INFO, "  System dir: %s", system_dir ? system_dir : "(null)");
    brimir_log(RETRO_LOG_INFO, "  g_core pointer: %p", (void*)g_core.get());
    brimir_log(RETRO_LOG_INFO, "About to call g_core->LoadGame()...");
    
    bool load_result = false;
    try {
        brimir_log(RETRO_LOG_INFO, "TEST: About to invoke LoadGame");
        brimir_log(RETRO_LOG_INFO, "TEST: path=%s", game->path);
        brimir_log(RETRO_LOG_INFO, "TEST: save_dir=%s", save_dir ? save_dir : "NULL");
        brimir_log(RETRO_LOG_INFO, "TEST: system_dir=%s", system_dir ? system_dir : "NULL");
        load_result = g_core->LoadGame(game->path, save_dir, system_dir);
        brimir_log(RETRO_LOG_INFO, "TEST: LoadGame call completed");
        brimir_log(RETRO_LOG_INFO, "LoadGame returned: %s", load_result ? "true" : "false");
        
        // Log last error even on success for debugging
        const std::string& debug_error = g_core->GetLastError();
        if (!debug_error.empty()) {
            brimir_log(RETRO_LOG_INFO, "DEBUG LastError: %s", debug_error.c_str());
        }
    } catch (const std::exception& e) {
        brimir_log(RETRO_LOG_ERROR, "C++ Exception in LoadGame: %s", e.what());
        const std::string& error = g_core->GetLastError();
        if (!error.empty()) {
            brimir_log(RETRO_LOG_ERROR, "LastError: %s", error.c_str());
        }
        return false;
    } catch (...) {
        brimir_log(RETRO_LOG_ERROR, "Unknown exception in LoadGame");
        const std::string& error = g_core->GetLastError();
        if (!error.empty()) {
            brimir_log(RETRO_LOG_ERROR, "LastError: %s", error.c_str());
        }
        return false;
    }
    
    if (!load_result) {
        const std::string& error = g_core->GetLastError();
        if (!error.empty()) {
            brimir_log(RETRO_LOG_ERROR, "Failed to load game: %s", error.c_str());
        } else {
            brimir_log(RETRO_LOG_ERROR, "Failed to load game (no error message)");
        }
        return false;
    }
    
    brimir_log(RETRO_LOG_INFO, "Game loaded successfully!");

    // Initialize SRAM - Ymir has loaded backup RAM from persistent file
    size_t sram_size = g_core->GetSRAMSize();
    if (sram_size > 0) {
        brimir_log(RETRO_LOG_INFO, "Backup RAM initialized: %zu bytes", sram_size);
        // Note: RetroArch will call retro_get_memory_data to get the buffer,
        // then load the .srm file into it. We'll sync it back to Ymir's file
        // on the first frame.
    }

    // Apply core options
    const char* audio_interp = get_option_value("brimir_audio_interpolation", "linear");
    g_core->SetAudioInterpolation(audio_interp);
    brimir_log(RETRO_LOG_INFO, "Audio interpolation: %s", audio_interp);

    const char* cd_speed_str = get_option_value("brimir_cd_speed", "2");
    uint8_t cd_speed = static_cast<uint8_t>(atoi(cd_speed_str));
    g_core->SetCDReadSpeed(cd_speed);
    brimir_log(RETRO_LOG_INFO, "CD read speed: %ux", cd_speed);

    const char* autodetect_region_str = get_option_value("brimir_autodetect_region", "enabled");
    bool autodetect_region = strcmp(autodetect_region_str, "enabled") == 0;
    g_core->SetAutodetectRegion(autodetect_region);
    brimir_log(RETRO_LOG_INFO, "Autodetect region: %s", autodetect_region ? "enabled" : "disabled");
    
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
    
    // Reset SRAM sync flag for next game load
    g_sram_synced = false;
}

RETRO_API unsigned retro_get_region(void) {
    // TODO: Return actual region from loaded game
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
            // Could expose main RAM here if needed
            return nullptr;
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
            return 0; // Not exposed yet
        default:
            return 0;
    }
}

