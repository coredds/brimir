// Brimir - Sega Saturn libretro core based on Ymir
// Copyright (C) 2025 Brimir Team
// Licensed under GPL-3.0

#include "libretro.h"
#include "brimir/core_wrapper.hpp"

#include <cstdio>
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

// Libretro API implementation

RETRO_API void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    
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
    info->library_version = "0.1.0";
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
    
    // Poll input
    if (input_poll_cb) {
        input_poll_cb();
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
    
    brimir_log(RETRO_LOG_INFO, "Loading game: %s", game->path ? game->path : "unknown");
    
    // Set pixel format
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        brimir_log(RETRO_LOG_ERROR, "RGB565 is not supported");
        return false;
    }
    
    // Load the game
    if (!g_core->LoadGame(game->path)) {
        brimir_log(RETRO_LOG_ERROR, "Failed to load game");
        return false;
    }
    
    brimir_log(RETRO_LOG_INFO, "Game loaded successfully");
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

RETRO_API unsigned retro_get_region(void) {
    // TODO: Return actual region from loaded game
    return RETRO_REGION_NTSC;
}

RETRO_API void* retro_get_memory_data(unsigned id) {
    // TODO: Implement memory access for cheats
    (void)id;
    return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
    // TODO: Implement memory size reporting
    (void)id;
    return 0;
}

