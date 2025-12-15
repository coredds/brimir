// Brimir Hardware Access Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0
//
// Tests that verify basic hardware access through the Saturn instance.
// These tests use the existing GetSaturn() API to access hardware components.

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>

using namespace brimir;

TEST_CASE("Hardware Access - SH-2 Cache Emulation", "[hardware][sh2][cache][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("SH-2 cache emulation can be enabled") {
        // Enable cache emulation
        saturn->EnableSH2CacheEmulation(true);
        
        // Verify it's enabled
        REQUIRE(saturn->IsSH2CacheEmulationEnabled());
    }
    
    SECTION("SH-2 cache emulation can be disabled") {
        // Disable cache emulation
        saturn->EnableSH2CacheEmulation(false);
        
        // Verify it's disabled
        REQUIRE_FALSE(saturn->IsSH2CacheEmulationEnabled());
    }
    
    SECTION("SH-2 cache emulation state persists") {
        // Enable
        saturn->EnableSH2CacheEmulation(true);
        REQUIRE(saturn->IsSH2CacheEmulationEnabled());
        
        // Disable
        saturn->EnableSH2CacheEmulation(false);
        REQUIRE_FALSE(saturn->IsSH2CacheEmulationEnabled());
        
        // Enable again
        saturn->EnableSH2CacheEmulation(true);
        REQUIRE(saturn->IsSH2CacheEmulationEnabled());
    }
}

TEST_CASE("Hardware Access - Video Standard", "[hardware][video][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Default video standard is NTSC") {
        auto standard = saturn->GetVideoStandard();
        REQUIRE(standard == brimir::core::config::sys::VideoStandard::NTSC);
    }
    
    SECTION("Video standard can be changed to PAL") {
        saturn->SetVideoStandard(brimir::core::config::sys::VideoStandard::PAL);
        REQUIRE(saturn->GetVideoStandard() == brimir::core::config::sys::VideoStandard::PAL);
    }
    
    SECTION("Video standard can be changed back to NTSC") {
        saturn->SetVideoStandard(brimir::core::config::sys::VideoStandard::PAL);
        saturn->SetVideoStandard(brimir::core::config::sys::VideoStandard::NTSC);
        REQUIRE(saturn->GetVideoStandard() == brimir::core::config::sys::VideoStandard::NTSC);
    }
}

TEST_CASE("Hardware Access - Slave SH-2 State", "[hardware][sh2][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Slave SH-2 is enabled by default") {
        // After initialization, slave SH-2 should be enabled
        REQUIRE(saturn->slaveSH2Enabled);
    }
}

TEST_CASE("Hardware Access - Reset Operations", "[hardware][reset][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Soft reset completes without error") {
        // Perform soft reset
        saturn->Reset(false);
        
        // System should still be valid
        REQUIRE(core.IsInitialized());
    }
    
    SECTION("Hard reset completes without error") {
        // Perform hard reset
        saturn->Reset(true);
        
        // System should still be valid
        REQUIRE(core.IsInitialized());
    }
    
    SECTION("Factory reset completes without error") {
        // Perform factory reset
        saturn->FactoryReset();
        
        // System should still be valid
        REQUIRE(core.IsInitialized());
    }
}

TEST_CASE("Hardware Access - Debug Tracing", "[hardware][debug][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Debug tracing is disabled by default") {
        REQUIRE_FALSE(saturn->IsDebugTracingEnabled());
    }
    
    SECTION("Debug tracing can be enabled") {
        saturn->EnableDebugTracing(true);
        REQUIRE(saturn->IsDebugTracingEnabled());
    }
    
    SECTION("Debug tracing can be disabled") {
        saturn->EnableDebugTracing(true);
        saturn->EnableDebugTracing(false);
        REQUIRE_FALSE(saturn->IsDebugTracingEnabled());
    }
}

TEST_CASE("Hardware Access - IPL Loading", "[hardware][bios][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("IPL loading updates IsIPLLoaded state") {
        REQUIRE_FALSE(core.IsIPLLoaded());
        
        // Create a valid BIOS (512KB)
        std::vector<uint8_t> biosData(512 * 1024, 0xFF);
        bool result = core.LoadIPL(std::span<const uint8_t>(biosData));
        
        REQUIRE(result);
        REQUIRE(core.IsIPLLoaded());
    }
    
    SECTION("IPL can be reloaded") {
        // Load first BIOS
        std::vector<uint8_t> biosData1(512 * 1024, 0xAA);
        core.LoadIPL(std::span<const uint8_t>(biosData1));
        REQUIRE(core.IsIPLLoaded());
        
        // Load second BIOS
        std::vector<uint8_t> biosData2(512 * 1024, 0x55);
        bool result = core.LoadIPL(std::span<const uint8_t>(biosData2));
        
        REQUIRE(result);
        REQUIRE(core.IsIPLLoaded());
    }
}

TEST_CASE("Hardware Access - CD Operations", "[hardware][cd][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Tray can be opened") {
        saturn->OpenTray();
        REQUIRE(saturn->IsTrayOpen());
    }
    
    SECTION("Tray can be closed") {
        saturn->OpenTray();
        saturn->CloseTray();
        REQUIRE_FALSE(saturn->IsTrayOpen());
    }
    
    SECTION("Disc can be ejected") {
        // Should not crash even if no disc is loaded
        saturn->EjectDisc();
        REQUIRE(saturn->IsTrayOpen());
    }
}

TEST_CASE("Hardware Access - Frame Execution", "[hardware][timing][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    // Load a minimal BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("Single frame runs without error") {
        // Run one frame
        core.RunFrame();
        
        // Should complete without crash
        REQUIRE(core.IsInitialized());
    }
    
    SECTION("Multiple frames run without error") {
        // Run 10 frames
        for (int i = 0; i < 10; ++i) {
            core.RunFrame();
        }
        
        // Should complete without crash
        REQUIRE(core.IsInitialized());
    }
}

TEST_CASE("Hardware Access - Framebuffer After Frame", "[hardware][video][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    // Load a minimal BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("Framebuffer is valid after running a frame") {
        core.RunFrame();
        
        auto fb = core.GetFramebuffer();
        auto width = core.GetFramebufferWidth();
        auto height = core.GetFramebufferHeight();
        
        REQUIRE(fb != nullptr);
        REQUIRE(width > 0);
        REQUIRE(height > 0);
    }
}

TEST_CASE("Hardware Access - State Size Stability", "[hardware][savestate][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("State size is consistent") {
        auto size1 = core.GetStateSize();
        REQUIRE(size1 > 0);
        
        // Enable cache emulation (should not change state size)
        saturn->EnableSH2CacheEmulation(true);
        auto size2 = core.GetStateSize();
        
        // State size should be the same
        REQUIRE(size1 == size2);
    }
    
    SECTION("State size is reasonable") {
        auto size = core.GetStateSize();
        
        // Should be > 0 but < 100MB
        REQUIRE(size > 0);
        REQUIRE(size < 100 * 1024 * 1024);
    }
}

TEST_CASE("Hardware Access - Configuration Persistence", "[hardware][config][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Cache emulation persists through frame execution") {
        saturn->EnableSH2CacheEmulation(true);
        
        // Run some frames
        std::vector<uint8_t> biosData(512 * 1024, 0xFF);
        core.LoadIPL(std::span<const uint8_t>(biosData));
        
        for (int i = 0; i < 5; ++i) {
            core.RunFrame();
        }
        
        // Setting should persist
        REQUIRE(saturn->IsSH2CacheEmulationEnabled());
    }
    
    SECTION("Video standard persists through frame execution") {
        saturn->SetVideoStandard(brimir::core::config::sys::VideoStandard::PAL);
        
        // Run some frames
        std::vector<uint8_t> biosData(512 * 1024, 0xFF);
        core.LoadIPL(std::span<const uint8_t>(biosData));
        
        for (int i = 0; i < 5; ++i) {
            core.RunFrame();
        }
        
        // Setting should persist
        REQUIRE(saturn->GetVideoStandard() == brimir::core::config::sys::VideoStandard::PAL);
    }
}

// Note: These tests use the existing API and GetSaturn() to access hardware.
// They verify basic hardware access patterns and that the emulator behaves correctly
// with different configurations. More detailed hardware accuracy tests (cache behavior,
// register access, memory addressing) require additional APIs to be implemented.

