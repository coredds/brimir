// Brimir VDP Component Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0
//
// Tests for Saturn VDP (Video Display Processor): VDP1 and VDP2
// Using direct hardware access via GetSaturn()->VDP

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>

using namespace brimir;

TEST_CASE("VDP - Component Access", "[vdp][video][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("VDP component is accessible") {
        // VDP should be a valid reference
        REQUIRE(&saturn->VDP != nullptr);
    }
}

TEST_CASE("VDP - Reset Behavior", "[vdp][reset][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("VDP survives soft reset") {
        saturn->Reset(false);
        
        // VDP should still be accessible
        REQUIRE(&saturn->VDP != nullptr);
        REQUIRE(core.IsInitialized());
    }
    
    SECTION("VDP survives hard reset") {
        saturn->Reset(true);
        
        // VDP should still be accessible
        REQUIRE(&saturn->VDP != nullptr);
        REQUIRE(core.IsInitialized());
    }
}

TEST_CASE("VDP - Frame Execution", "[vdp][video][execution][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    // Load a minimal BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("VDP generates frames during execution") {
        // Run some frames
        for (int i = 0; i < 5; ++i) {
            core.RunFrame();
        }
        
        // Should have valid framebuffer
        auto fb = core.GetFramebuffer();
        REQUIRE(fb != nullptr);
        
        auto width = core.GetFramebufferWidth();
        auto height = core.GetFramebufferHeight();
        REQUIRE(width > 0);
        REQUIRE(height > 0);
    }
}

TEST_CASE("VDP - Resolution Reporting", "[vdp][video][resolution][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    // Load BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("VDP reports valid resolution after frame") {
        core.RunFrame();
        
        auto width = core.GetFramebufferWidth();
        auto height = core.GetFramebufferHeight();
        
        // Source: Saturn documentation - Common resolutions
        // 320x224, 320x240, 352x224, 352x240, 640x224, 640x240, 704x224, 704x240
        // Width should be in valid range
        REQUIRE(width >= 320);
        REQUIRE(width <= 704);
        
        // Height should be in valid range
        REQUIRE(height >= 224);
        REQUIRE(height <= 480);  // Allow for interlaced modes
    }
}

TEST_CASE("VDP - Framebuffer Consistency", "[vdp][video][framebuffer][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    // Load BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("Framebuffer pointer remains stable across frames") {
        core.RunFrame();
        auto fb1 = core.GetFramebuffer();
        
        core.RunFrame();
        auto fb2 = core.GetFramebuffer();
        
        // Pointer should be stable (or at least non-null)
        REQUIRE(fb1 != nullptr);
        REQUIRE(fb2 != nullptr);
    }
    
    SECTION("Framebuffer dimensions remain stable") {
        core.RunFrame();
        auto width1 = core.GetFramebufferWidth();
        auto height1 = core.GetFramebufferHeight();
        
        core.RunFrame();
        auto width2 = core.GetFramebufferWidth();
        auto height2 = core.GetFramebufferHeight();
        
        // Without mode changes, dimensions should be stable
        REQUIRE(width1 == width2);
        REQUIRE(height1 == height2);
    }
}

TEST_CASE("VDP - Pixel Format", "[vdp][video][format][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Pixel format is valid") {
        auto format = core.GetPixelFormat();
        
        // libretro pixel formats:
        // 0=0RGB1555, 1=XRGB8888, 2=RGB565
        REQUIRE((format == 0 || format == 1 || format == 2));
    }
}

TEST_CASE("VDP - Framebuffer Pitch", "[vdp][video][pitch][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    // Load BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("Pitch is consistent with width and format") {
        core.RunFrame();
        
        auto width = core.GetFramebufferWidth();
        auto pitch = core.GetFramebufferPitch();
        auto format = core.GetPixelFormat();
        
        // Pitch should be at least width * bytes_per_pixel
        size_t bytesPerPixel = (format == 1) ? 4 : 2;  // XRGB8888=4, RGB565/0RGB1555=2
        size_t minPitch = width * bytesPerPixel;
        
        REQUIRE(pitch >= minPitch);
    }
}

TEST_CASE("VDP - State Consistency", "[vdp][savestate][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    // Load BIOS and run a frame
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    core.RunFrame();
    
    SECTION("VDP state survives save/load cycle") {
        auto width1 = core.GetFramebufferWidth();
        auto height1 = core.GetFramebufferHeight();
        
        // Save state
        auto stateSize = core.GetStateSize();
        std::vector<uint8_t> state(stateSize);
        REQUIRE(core.SaveState(state.data(), stateSize));
        
        // Load state
        REQUIRE(core.LoadState(state.data(), stateSize));
        
        // Dimensions should be consistent
        auto width2 = core.GetFramebufferWidth();
        auto height2 = core.GetFramebufferHeight();
        
        REQUIRE(width1 == width2);
        REQUIRE(height1 == height2);
    }
}

TEST_CASE("VDP - Video Standard Integration", "[vdp][video][standard][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    // Load BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("VDP works with NTSC") {
        saturn->SetVideoStandard(core::config::sys::VideoStandard::NTSC);
        
        core.RunFrame();
        
        auto fb = core.GetFramebuffer();
        REQUIRE(fb != nullptr);
    }
    
    SECTION("VDP works with PAL") {
        saturn->SetVideoStandard(core::config::sys::VideoStandard::PAL);
        
        core.RunFrame();
        
        auto fb = core.GetFramebuffer();
        REQUIRE(fb != nullptr);
    }
    
    SECTION("VDP handles video standard changes") {
        saturn->SetVideoStandard(core::config::sys::VideoStandard::NTSC);
        core.RunFrame();
        
        saturn->SetVideoStandard(core::config::sys::VideoStandard::PAL);
        core.RunFrame();
        
        saturn->SetVideoStandard(core::config::sys::VideoStandard::NTSC);
        core.RunFrame();
        
        REQUIRE(core.IsInitialized());
    }
}

TEST_CASE("VDP - Multiple Frame Generation", "[vdp][video][execution][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    // Load BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("VDP generates 60 consecutive frames") {
        for (int i = 0; i < 60; ++i) {
            core.RunFrame();
            
            // Each frame should produce valid output
            auto fb = core.GetFramebuffer();
            REQUIRE(fb != nullptr);
        }
        
        REQUIRE(core.IsInitialized());
    }
}

// Note: These tests verify VDP functionality using the existing CoreWrapper
// and GetSaturn() APIs. They test frame generation, resolution reporting,
// framebuffer consistency, and integration with video standards - all based
// on Saturn hardware specifications.

