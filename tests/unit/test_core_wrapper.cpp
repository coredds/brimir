// Brimir CoreWrapper Unit Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_section_info.hpp>

#include <brimir/core_wrapper.hpp>
#include <ymir/core/configuration.hpp>  // For VideoStandard enum

using namespace brimir;

TEST_CASE("CoreWrapper construction", "[core][unit]") {
    CoreWrapper core;
    
    SECTION("Starts in uninitialized state") {
        REQUIRE_FALSE(core.IsInitialized());
        REQUIRE_FALSE(core.IsGameLoaded());
    }
}

TEST_CASE("CoreWrapper initialization", "[core][unit]") {
    CoreWrapper core;
    
    SECTION("Initialize succeeds") {
        REQUIRE(core.Initialize());
        REQUIRE(core.IsInitialized());
    }
    
    SECTION("Multiple Initialize calls are safe") {
        REQUIRE(core.Initialize());
        REQUIRE(core.Initialize());  // Should not crash
        REQUIRE(core.IsInitialized());
    }
    
    SECTION("Shutdown after Initialize") {
        REQUIRE(core.Initialize());
        core.Shutdown();
        REQUIRE_FALSE(core.IsInitialized());
    }
    
    SECTION("Shutdown without Initialize is safe") {
        core.Shutdown();  // Should not crash
        REQUIRE_FALSE(core.IsInitialized());
    }
}

TEST_CASE("CoreWrapper game loading", "[core][unit]") {
    CoreWrapper core;
    
    SECTION("LoadGame fails when not initialized") {
        REQUIRE_FALSE(core.LoadGame("test.iso"));
        REQUIRE_FALSE(core.IsGameLoaded());
    }
    
    SECTION("LoadGame with nullptr fails safely") {
        REQUIRE(core.Initialize());
        REQUIRE_FALSE(core.LoadGame(nullptr));
        REQUIRE_FALSE(core.IsGameLoaded());
    }
    
    SECTION("LoadGame with empty path fails safely") {
        REQUIRE(core.Initialize());
        REQUIRE_FALSE(core.LoadGame(""));
        REQUIRE_FALSE(core.IsGameLoaded());
    }
    
    SECTION("LoadGame with invalid path fails gracefully") {
        REQUIRE(core.Initialize());
        REQUIRE_FALSE(core.LoadGame("nonexistent_file.iso"));
        REQUIRE_FALSE(core.IsGameLoaded());
        // Core should still be initialized after failed load
        REQUIRE(core.IsInitialized());
    }
}

TEST_CASE("CoreWrapper reset functionality", "[core][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Reset without game loaded") {
        core.Reset();
        REQUIRE(core.IsInitialized());
    }
}

TEST_CASE("CoreWrapper framebuffer access", "[core][unit]") {
    CoreWrapper core;
    
    SECTION("Framebuffer before init") {
        // Should not crash
        [[maybe_unused]] auto fb = core.GetFramebuffer();
        auto width = core.GetFramebufferWidth();
        auto height = core.GetFramebufferHeight();
        
        // Values should be sensible even if uninitialized
        REQUIRE(width >= 0);
        REQUIRE(height >= 0);
    }
    
    SECTION("Framebuffer after init") {
        REQUIRE(core.Initialize());
        
        auto width = core.GetFramebufferWidth();
        auto height = core.GetFramebufferHeight();
        
        // Saturn video output is typically 320x224 or 640x448
        REQUIRE(width > 0);
        REQUIRE(height > 0);
        REQUIRE(width <= 704);   // Max Saturn H resolution
        REQUIRE(height <= 512);  // Max Saturn V resolution
    }
}

TEST_CASE("CoreWrapper audio access", "[core][unit]") {
    CoreWrapper core;
    
    SECTION("Audio samples before init") {
        std::vector<int16_t> buffer(2048 * 2);  // stereo
        auto count = core.GetAudioSamples(buffer.data(), 2048);
        
        // Should be 0 when not initialized
        REQUIRE(count == 0);
    }
    
    SECTION("Audio samples after init") {
        REQUIRE(core.Initialize());
        
        std::vector<int16_t> buffer(2048 * 2);  // stereo
        // Just verify no crashes
        [[maybe_unused]] auto count = core.GetAudioSamples(buffer.data(), 2048);
    }
}

TEST_CASE("CoreWrapper save state", "[core][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("State size is reasonable") {
        auto size = core.GetStateSize();
        
        // Saturn state should be > 0 but < 100MB
        REQUIRE(size > 0);
        REQUIRE(size < 100 * 1024 * 1024);
    }
    
    SECTION("SaveState with null pointer fails safely") {
        REQUIRE_FALSE(core.SaveState(nullptr, 0));
    }
    
    SECTION("LoadState with null pointer fails safely") {
        REQUIRE_FALSE(core.LoadState(nullptr, 0));
    }
    
    SECTION("SaveState with valid buffer") {
        auto stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer(stateSize);
        // Should succeed now that it's implemented
        bool result = core.SaveState(buffer.data(), stateSize);
        REQUIRE(result);
    }
    
    SECTION("SaveState with too small buffer fails") {
        auto stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer(stateSize / 2);
        bool result = core.SaveState(buffer.data(), buffer.size());
        REQUIRE_FALSE(result);
    }
    
    SECTION("Save and load state cycle") {
        auto stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer(stateSize);
        
        // Save current state
        bool saveResult = core.SaveState(buffer.data(), stateSize);
        REQUIRE(saveResult);
        
        // Load the saved state
        bool loadResult = core.LoadState(buffer.data(), stateSize);
        REQUIRE(loadResult);
    }
}

TEST_CASE("CoreWrapper IPL/BIOS loading", "[core][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("LoadIPL with empty span") {
        std::vector<uint8_t> empty;
        // Should fail with empty buffer
        bool result = core.LoadIPL(std::span<const uint8_t>(empty));
        REQUIRE_FALSE(result);
        REQUIRE_FALSE(core.IsIPLLoaded());
    }
    
    SECTION("LoadIPL with wrong size") {
        std::vector<uint8_t> small_buffer(1024, 0xFF);
        // Should fail with wrong size
        bool result = core.LoadIPL(std::span<const uint8_t>(small_buffer));
        REQUIRE_FALSE(result);
        REQUIRE_FALSE(core.IsIPLLoaded());
    }
    
    SECTION("LoadIPL with correct size") {
        std::vector<uint8_t> correct_buffer(512 * 1024, 0xFF);
        // Should succeed with correct size
        bool result = core.LoadIPL(std::span<const uint8_t>(correct_buffer));
        REQUIRE(result);
        REQUIRE(core.IsIPLLoaded());
    }
}

TEST_CASE("CoreWrapper video standard", "[core][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Get default video standard") {
        auto standard = core.GetVideoStandard();
        // Should be NTSC by default
        REQUIRE(standard == ymir::core::config::sys::VideoStandard::NTSC);
    }
    
    SECTION("Set video standard") {
        using ymir::core::config::sys::VideoStandard;
        
        // Try setting different standards
        core.SetVideoStandard(VideoStandard::NTSC);
        REQUIRE(core.GetVideoStandard() == VideoStandard::NTSC);
        
        core.SetVideoStandard(VideoStandard::PAL);
        REQUIRE(core.GetVideoStandard() == VideoStandard::PAL);
    }
}

TEST_CASE("CoreWrapper pixel format", "[core][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Pixel format is valid") {
        auto format = core.GetPixelFormat();
        
        // libretro pixel formats:
        // RETRO_PIXEL_FORMAT_0RGB1555 = 0
        // RETRO_PIXEL_FORMAT_XRGB8888 = 1
        // RETRO_PIXEL_FORMAT_RGB565   = 2
        
        REQUIRE(format == 2);  // RGB565
    }
}
