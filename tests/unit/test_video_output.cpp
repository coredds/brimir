// Brimir Video Output Unit Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>
#include <ymir/core/configuration.hpp>

using namespace brimir;

TEST_CASE("Video output framebuffer conversion", "[video][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Framebuffer dimensions are valid") {
        auto width = core.GetFramebufferWidth();
        auto height = core.GetFramebufferHeight();
        
        // Saturn can output various resolutions
        REQUIRE(width > 0);
        REQUIRE(height > 0);
        REQUIRE(width <= 704);   // Max Saturn H resolution
        REQUIRE(height <= 512);  // Max Saturn V resolution
    }
    
    SECTION("Framebuffer pitch is correct for RGB565") {
        auto width = core.GetFramebufferWidth();
        auto pitch = core.GetFramebufferPitch();
        
        // RGB565 is 2 bytes per pixel
        REQUIRE(pitch == width * 2);
    }
    
    SECTION("Pixel format is RGB565") {
        auto format = core.GetPixelFormat();
        
        // libretro RGB565 format = 2
        REQUIRE(format == 2);
    }
    
    SECTION("Framebuffer pointer is valid after init") {
        auto fb = core.GetFramebuffer();
        REQUIRE(fb != nullptr);
    }
}

TEST_CASE("Video output pixel format", "[video][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Uses RGB565 format") {
        REQUIRE(core.GetPixelFormat() == 2);
    }
}

TEST_CASE("Video output frame dimensions", "[video][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Default resolution is valid") {
        auto width = core.GetFramebufferWidth();
        auto height = core.GetFramebufferHeight();
        
        // Common Saturn resolutions:
        // 320x224 (NTSC), 320x240 (NTSC), 352x224, 352x240
        // 640x448, 640x480, 704x448, 704x480
        
        REQUIRE(width > 0);
        REQUIRE(height > 0);
    }
    
    SECTION("Pitch calculation is correct") {
        auto width = core.GetFramebufferWidth();
        auto pitch = core.GetFramebufferPitch();
        
        // For RGB565, pitch should be width * 2
        REQUIRE(pitch == width * 2);
    }
}

TEST_CASE("Video output framebuffer access", "[video][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Framebuffer is accessible") {
        const void* fb = core.GetFramebuffer();
        REQUIRE(fb != nullptr);
    }
    
    SECTION("Framebuffer persists across calls") {
        const void* fb1 = core.GetFramebuffer();
        const void* fb2 = core.GetFramebuffer();
        
        // Should return the same buffer
        REQUIRE(fb1 == fb2);
    }
    
    SECTION("Framebuffer dimensions match") {
        auto width = core.GetFramebufferWidth();
        auto height = core.GetFramebufferHeight();
        auto pitch = core.GetFramebufferPitch();
        
        // Basic sanity checks
        REQUIRE(width > 0);
        REQUIRE(height > 0);
        REQUIRE(pitch >= width * 2);  // At least width * bytes_per_pixel
    }
}

TEST_CASE("Video output without initialization", "[video][unit]") {
    CoreWrapper core;
    
    SECTION("Framebuffer access before init doesn't crash") {
        [[maybe_unused]] auto fb = core.GetFramebuffer();
        [[maybe_unused]] auto width = core.GetFramebufferWidth();
        [[maybe_unused]] auto height = core.GetFramebufferHeight();
        [[maybe_unused]] auto pitch = core.GetFramebufferPitch();
        [[maybe_unused]] auto format = core.GetPixelFormat();
        
        // Just verify no crashes
        REQUIRE(true);
    }
}

TEST_CASE("Video standard settings", "[video][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    using VideoStandard = ymir::core::config::sys::VideoStandard;
    
    SECTION("Default video standard is NTSC") {
        REQUIRE(core.GetVideoStandard() == VideoStandard::NTSC);
    }
    
    SECTION("Can switch to PAL") {
        core.SetVideoStandard(VideoStandard::PAL);
        REQUIRE(core.GetVideoStandard() == VideoStandard::PAL);
    }
    
    SECTION("Can switch back to NTSC") {
        core.SetVideoStandard(VideoStandard::PAL);
        core.SetVideoStandard(VideoStandard::NTSC);
        REQUIRE(core.GetVideoStandard() == VideoStandard::NTSC);
    }
}

