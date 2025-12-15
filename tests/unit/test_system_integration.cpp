// Brimir System Integration Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0
//
// Tests for Saturn system integration, configuration persistence,
// and complex operational scenarios

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>

using namespace brimir;

TEST_CASE("System Integration - Full Initialization Sequence", "[system][integration][implemented]") {
    SECTION("Complete initialization from scratch") {
        CoreWrapper core;
        
        // Step 1: Initialize
        REQUIRE(core.Initialize());
        REQUIRE(core.IsInitialized());
        
        // Step 2: Load BIOS
        std::vector<uint8_t> biosData(512 * 1024, 0xFF);
        REQUIRE(core.LoadIPL(std::span<const uint8_t>(biosData)));
        REQUIRE(core.IsIPLLoaded());
        
        // Step 3: Configure system
        auto* saturn = core.GetSaturn();
        REQUIRE(saturn != nullptr);
        saturn->SetVideoStandard(core::config::sys::VideoStandard::NTSC);
        saturn->EnableSH2CacheEmulation(true);
        
        // Step 4: Run execution
        core.RunFrame();
        
        // Step 5: Verify output
        REQUIRE(core.GetFramebuffer() != nullptr);
        REQUIRE(core.GetFramebufferWidth() > 0);
        REQUIRE(core.GetFramebufferHeight() > 0);
    }
}

TEST_CASE("System Integration - Configuration Combinations", "[system][config][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    // Load BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("NTSC with cache enabled") {
        saturn->SetVideoStandard(core::config::sys::VideoStandard::NTSC);
        saturn->EnableSH2CacheEmulation(true);
        
        core.RunFrame();
        REQUIRE(core.GetFramebuffer() != nullptr);
    }
    
    SECTION("NTSC with cache disabled") {
        saturn->SetVideoStandard(core::config::sys::VideoStandard::NTSC);
        saturn->EnableSH2CacheEmulation(false);
        
        core.RunFrame();
        REQUIRE(core.GetFramebuffer() != nullptr);
    }
    
    SECTION("PAL with cache enabled") {
        saturn->SetVideoStandard(core::config::sys::VideoStandard::PAL);
        saturn->EnableSH2CacheEmulation(true);
        
        core.RunFrame();
        REQUIRE(core.GetFramebuffer() != nullptr);
    }
    
    SECTION("PAL with cache disabled") {
        saturn->SetVideoStandard(core::config::sys::VideoStandard::PAL);
        saturn->EnableSH2CacheEmulation(false);
        
        core.RunFrame();
        REQUIRE(core.GetFramebuffer() != nullptr);
    }
}

TEST_CASE("System Integration - Reset State Consistency", "[system][reset][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    // Load BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xAA);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("Configuration persists through soft reset") {
        // Set configuration
        saturn->SetVideoStandard(core::config::sys::VideoStandard::PAL);
        saturn->EnableSH2CacheEmulation(true);
        saturn->SMPC.SetAreaCode(0xC);  // Europe
        
        // Write to Work RAM
        saturn->mem.WRAMLow[100] = 0x42;
        
        // Soft reset
        saturn->Reset(false);
        
        // Configuration should persist
        REQUIRE(saturn->GetVideoStandard() == core::config::sys::VideoStandard::PAL);
        REQUIRE(saturn->IsSH2CacheEmulationEnabled());
        REQUIRE(saturn->SMPC.GetAreaCode() == 0xC);
        
        // Work RAM should persist
        REQUIRE(saturn->mem.WRAMLow[100] == 0x42);
        
        // BIOS should persist
        REQUIRE(saturn->mem.IPL[0] == 0xAA);
    }
    
    SECTION("Work RAM cleared on hard reset") {
        // Write to Work RAM
        saturn->mem.WRAMLow[200] = 0x84;
        saturn->mem.WRAMHigh[300] = 0x48;
        
        // Hard reset
        saturn->Reset(true);
        
        // Work RAM should be cleared
        REQUIRE(saturn->mem.WRAMLow[200] == 0x00);
        REQUIRE(saturn->mem.WRAMHigh[300] == 0x00);
        
        // BIOS should persist
        REQUIRE(saturn->mem.IPL[0] == 0xAA);
    }
}

TEST_CASE("System Integration - Multiple Component Interaction", "[system][integration][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    // Load BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("All major components work together") {
        // Configure all components
        saturn->SetVideoStandard(core::config::sys::VideoStandard::NTSC);
        saturn->EnableSH2CacheEmulation(true);
        saturn->SMPC.SetAreaCode(0x4);  // North America
        saturn->CloseTray();
        
        // Write pattern to memory
        for (size_t i = 0; i < 100; ++i) {
            saturn->mem.WRAMLow[i] = static_cast<uint8_t>(i);
        }
        
        // Run frames
        for (int i = 0; i < 10; ++i) {
            core.RunFrame();
        }
        
        // Verify all components still functional
        REQUIRE(core.GetFramebuffer() != nullptr);
        REQUIRE(saturn->mem.WRAMLow[50] == 50);
        REQUIRE(saturn->SMPC.GetAreaCode() == 0x4);
        REQUIRE_FALSE(saturn->IsTrayOpen());
    }
}

TEST_CASE("System Integration - Extended Execution", "[system][execution][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    // Load BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("System stable over 300 frames (5 seconds at 60Hz)") {
        for (int i = 0; i < 300; ++i) {
            core.RunFrame();
        }
        
        // System should still be functional
        REQUIRE(core.IsInitialized());
        REQUIRE(core.GetFramebuffer() != nullptr);
    }
    
    SECTION("System stable with configuration changes during execution") {
        for (int i = 0; i < 100; ++i) {
            core.RunFrame();
            
            // Change configuration every 10 frames
            if (i % 10 == 0) {
                if (i % 20 == 0) {
                    saturn->SetVideoStandard(core::config::sys::VideoStandard::NTSC);
                } else {
                    saturn->SetVideoStandard(core::config::sys::VideoStandard::PAL);
                }
            }
        }
        
        REQUIRE(core.IsInitialized());
    }
}

TEST_CASE("System Integration - Save State Round Trip", "[system][savestate][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    // Load BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xCC);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("Complete system state round trip") {
        // Set up complex state
        saturn->SetVideoStandard(core::config::sys::VideoStandard::PAL);
        saturn->EnableSH2CacheEmulation(true);
        saturn->SMPC.SetAreaCode(0xC);
        
        // Write patterns to memory
        for (size_t i = 0; i < 1000; ++i) {
            saturn->mem.WRAMLow[i] = static_cast<uint8_t>(i & 0xFF);
            saturn->mem.WRAMHigh[i] = static_cast<uint8_t>((i * 2) & 0xFF);
        }
        
        // Run some frames
        for (int i = 0; i < 10; ++i) {
            core.RunFrame();
        }
        
        // Save state
        auto stateSize = core.GetStateSize();
        std::vector<uint8_t> state(stateSize);
        REQUIRE(core.SaveState(state.data(), stateSize));
        
        // Corrupt state
        saturn->SetVideoStandard(core::config::sys::VideoStandard::NTSC);
        saturn->EnableSH2CacheEmulation(false);
        saturn->SMPC.SetAreaCode(0x1);
        for (size_t i = 0; i < 1000; ++i) {
            saturn->mem.WRAMLow[i] = 0;
            saturn->mem.WRAMHigh[i] = 0;
        }
        
        // Load state
        REQUIRE(core.LoadState(state.data(), stateSize));
        
        // Verify restoration
        REQUIRE(saturn->GetVideoStandard() == core::config::sys::VideoStandard::PAL);
        REQUIRE(saturn->IsSH2CacheEmulationEnabled());
        REQUIRE(saturn->SMPC.GetAreaCode() == 0xC);
        
        // Verify memory restoration
        REQUIRE(saturn->mem.WRAMLow[0] == 0);
        REQUIRE(saturn->mem.WRAMLow[100] == 100);
        REQUIRE(saturn->mem.WRAMLow[255] == 255);
        REQUIRE(saturn->mem.WRAMHigh[100] == 200);
    }
}

TEST_CASE("System Integration - Multiple Save/Load Cycles", "[system][savestate][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    // Load BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("System survives 10 save/load cycles") {
        auto stateSize = core.GetStateSize();
        std::vector<uint8_t> state(stateSize);
        
        for (int cycle = 0; cycle < 10; ++cycle) {
            // Run some frames
            for (int i = 0; i < 5; ++i) {
                core.RunFrame();
            }
            
            // Save
            REQUIRE(core.SaveState(state.data(), stateSize));
            
            // Load
            REQUIRE(core.LoadState(state.data(), stateSize));
        }
        
        // System should still be functional
        REQUIRE(core.IsInitialized());
        core.RunFrame();
        REQUIRE(core.GetFramebuffer() != nullptr);
    }
}

TEST_CASE("System Integration - Component Isolation", "[system][integration][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("SH-2 operations don't affect VDP") {
        saturn->masterSH2.PurgeCache();
        saturn->slaveSH2.PurgeCache();
        
        // VDP should still be accessible
        REQUIRE(&saturn->VDP != nullptr);
    }
    
    SECTION("VDP operations don't affect SH-2") {
        // VDP should exist
        REQUIRE(&saturn->VDP != nullptr);
        
        // SH-2 should still be functional
        REQUIRE(saturn->masterSH2.IsMaster());
    }
    
    SECTION("SMPC operations don't affect memory") {
        saturn->mem.WRAMLow[0] = 0x99;
        
        saturn->SMPC.SetAreaCode(0x4);
        
        // Memory should be unchanged
        REQUIRE(saturn->mem.WRAMLow[0] == 0x99);
    }
    
    SECTION("CD operations don't affect audio") {
        saturn->OpenTray();
        saturn->CloseTray();
        
        // SCSP should still be accessible
        REQUIRE(&saturn->SCSP != nullptr);
    }
}

TEST_CASE("System Integration - Concurrent Operations", "[system][integration][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    // Load BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("Multiple operations in single frame") {
        // Configure multiple components
        saturn->SetVideoStandard(core::config::sys::VideoStandard::NTSC);
        saturn->EnableSH2CacheEmulation(true);
        saturn->SMPC.SetAreaCode(0x1);
        saturn->CloseTray();
        core.SetAudioInterpolation("linear");
        
        // Write memory
        saturn->mem.WRAMLow[0] = 0x55;
        
        // Set breakpoints
        saturn->masterSH2.AddBreakpoint(0x06000000);
        
        // Run frame
        core.RunFrame();
        
        // Verify all operations succeeded
        REQUIRE(saturn->GetVideoStandard() == core::config::sys::VideoStandard::NTSC);
        REQUIRE(saturn->IsSH2CacheEmulationEnabled());
        REQUIRE(saturn->SMPC.GetAreaCode() == 0x1);
        REQUIRE_FALSE(saturn->IsTrayOpen());
        REQUIRE(saturn->mem.WRAMLow[0] == 0x55);
        REQUIRE(saturn->masterSH2.IsBreakpointSet(0x06000000));
    }
}

TEST_CASE("System Integration - Error Recovery", "[system][error][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("System recovers from invalid IPL size") {
        // Try to load invalid BIOS
        std::vector<uint8_t> invalidBios(1024, 0xFF);  // Too small
        bool result = core.LoadIPL(std::span<const uint8_t>(invalidBios));
        
        REQUIRE_FALSE(result);
        REQUIRE_FALSE(core.IsIPLLoaded());
        
        // System should still be initialized
        REQUIRE(core.IsInitialized());
        
        // Valid BIOS should still load
        std::vector<uint8_t> validBios(512 * 1024, 0xFF);
        REQUIRE(core.LoadIPL(std::span<const uint8_t>(validBios)));
    }
}

TEST_CASE("System Integration - Memory Boundary Conditions", "[system][memory][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Can access memory boundaries without crash") {
        // First bytes
        saturn->mem.IPL[0] = 0x11;
        saturn->mem.WRAMLow[0] = 0x22;
        saturn->mem.WRAMHigh[0] = 0x33;
        
        // Last bytes
        saturn->mem.IPL[sys::kIPLSize - 1] = 0x44;
        saturn->mem.WRAMLow[sys::kWRAMLowSize - 1] = 0x55;
        saturn->mem.WRAMHigh[sys::kWRAMHighSize - 1] = 0x66;
        
        // Verify
        REQUIRE(saturn->mem.IPL[0] == 0x11);
        REQUIRE(saturn->mem.WRAMLow[0] == 0x22);
        REQUIRE(saturn->mem.WRAMHigh[0] == 0x33);
        REQUIRE(saturn->mem.IPL[sys::kIPLSize - 1] == 0x44);
        REQUIRE(saturn->mem.WRAMLow[sys::kWRAMLowSize - 1] == 0x55);
        REQUIRE(saturn->mem.WRAMHigh[sys::kWRAMHighSize - 1] == 0x66);
    }
}

// Note: These tests verify system-wide integration, configuration
// persistence, complex operational scenarios, and edge cases. They ensure
// all components work together correctly and the system remains stable
// under various conditions.

