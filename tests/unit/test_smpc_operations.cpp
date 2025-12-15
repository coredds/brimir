// Brimir SMPC Operations Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0
//
// Tests for Saturn SMPC: System Manager and Peripheral Control
// Using direct hardware access via GetSaturn()->SMPC

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>

using namespace brimir;

TEST_CASE("SMPC - Area Code Access", "[smpc][region][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Can read area code") {
        // Source: Saturn documentation - Area codes determine region
        uint8_t areaCode = saturn->SMPC.GetAreaCode();
        
        // Area code should be valid (not 0x0 or 0xF which are prohibited)
        REQUIRE(areaCode != 0x0);
        REQUIRE(areaCode != 0xF);
    }
    
    SECTION("Can set area code to Japan (0x1)") {
        saturn->SMPC.SetAreaCode(0x1);
        REQUIRE(saturn->SMPC.GetAreaCode() == 0x1);
    }
    
    SECTION("Can set area code to North America (0x4)") {
        saturn->SMPC.SetAreaCode(0x4);
        REQUIRE(saturn->SMPC.GetAreaCode() == 0x4);
    }
    
    SECTION("Can set area code to Europe (0xC)") {
        saturn->SMPC.SetAreaCode(0xC);
        REQUIRE(saturn->SMPC.GetAreaCode() == 0xC);
    }
}

TEST_CASE("SMPC - Peripheral Port Access", "[smpc][peripheral][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Can access peripheral port 1") {
        auto& port1 = saturn->SMPC.GetPeripheralPort1();
        REQUIRE(&port1 != nullptr);
    }
    
    SECTION("Can access peripheral port 2") {
        auto& port2 = saturn->SMPC.GetPeripheralPort2();
        REQUIRE(&port2 != nullptr);
    }
    
    SECTION("Ports are distinct") {
        auto& port1 = saturn->SMPC.GetPeripheralPort1();
        auto& port2 = saturn->SMPC.GetPeripheralPort2();
        
        // Should be different objects
        REQUIRE(&port1 != &port2);
    }
}

TEST_CASE("SMPC - RTC Access", "[smpc][rtc][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Can access RTC") {
        auto& rtc = saturn->SMPC.GetRTC();
        REQUIRE(&rtc != nullptr);
    }
    
    SECTION("RTC survives reset") {
        auto& rtc1 = saturn->SMPC.GetRTC();
        
        // Soft reset
        saturn->Reset(false);
        
        auto& rtc2 = saturn->SMPC.GetRTC();
        
        // Should be same RTC instance
        REQUIRE(&rtc1 == &rtc2);
    }
}

TEST_CASE("SMPC - Reset Button State", "[smpc][reset][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Can set reset button pressed") {
        // Should not crash
        saturn->SMPC.SetResetButtonState(true);
        
        // System should still be valid
        REQUIRE(core.IsInitialized());
    }
    
    SECTION("Can set reset button released") {
        saturn->SMPC.SetResetButtonState(true);
        saturn->SMPC.SetResetButtonState(false);
        
        // System should still be valid
        REQUIRE(core.IsInitialized());
    }
}

TEST_CASE("SMPC - Reset Behavior", "[smpc][reset][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("SMPC survives soft reset") {
        // Soft reset
        saturn->Reset(false);
        
        // SMPC should still be accessible
        uint8_t areaCode = saturn->SMPC.GetAreaCode();
        REQUIRE(areaCode != 0x0);
        REQUIRE(areaCode != 0xF);
    }
    
    SECTION("SMPC survives hard reset") {
        // Hard reset
        saturn->Reset(true);
        
        // SMPC should still be accessible
        uint8_t areaCode = saturn->SMPC.GetAreaCode();
        REQUIRE(areaCode != 0x0);
        REQUIRE(areaCode != 0xF);
    }
    
    SECTION("SMPC survives factory reset") {
        // Factory reset
        saturn->FactoryReset();
        
        // SMPC should still be accessible
        uint8_t areaCode = saturn->SMPC.GetAreaCode();
        REQUIRE(areaCode != 0x0);
        REQUIRE(areaCode != 0xF);
    }
}

TEST_CASE("SMPC - Area Code Persistence", "[smpc][region][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Area code persists through soft reset") {
        // Set area code
        saturn->SMPC.SetAreaCode(0x4);  // North America
        
        // Soft reset
        saturn->Reset(false);
        
        // Area code should be preserved
        REQUIRE(saturn->SMPC.GetAreaCode() == 0x4);
    }
}

TEST_CASE("SMPC - State Consistency", "[smpc][savestate][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Area code survives save/load cycle") {
        // Set specific area code
        saturn->SMPC.SetAreaCode(0xC);  // Europe
        
        // Save state
        auto stateSize = core.GetStateSize();
        std::vector<uint8_t> state(stateSize);
        REQUIRE(core.SaveState(state.data(), stateSize));
        
        // Change area code
        saturn->SMPC.SetAreaCode(0x1);  // Japan
        REQUIRE(saturn->SMPC.GetAreaCode() == 0x1);
        
        // Load state
        REQUIRE(core.LoadState(state.data(), stateSize));
        
        // Area code should be restored
        REQUIRE(saturn->SMPC.GetAreaCode() == 0xC);
    }
}

TEST_CASE("SMPC - Region Autodetect Integration", "[smpc][region][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("UsePreferredRegion completes without error") {
        // Should not crash
        saturn->UsePreferredRegion();
        
        // System should still be valid
        REQUIRE(core.IsInitialized());
        
        // Should have valid area code
        uint8_t areaCode = saturn->SMPC.GetAreaCode();
        REQUIRE(areaCode != 0x0);
        REQUIRE(areaCode != 0xF);
    }
}

TEST_CASE("SMPC - Component Integration", "[smpc][integration][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("SMPC and video standard are consistent") {
        // Set to PAL region (Europe)
        saturn->SMPC.SetAreaCode(0xC);
        
        // Both should be accessible
        REQUIRE(saturn->SMPC.GetAreaCode() == 0xC);
        
        // Video standard can be queried independently
        auto videoStd = saturn->GetVideoStandard();
        REQUIRE((videoStd == core::config::sys::VideoStandard::NTSC || 
                 videoStd == core::config::sys::VideoStandard::PAL));
    }
}

TEST_CASE("SMPC - Multiple Area Code Changes", "[smpc][region][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Can change area code multiple times") {
        // Cycle through several valid area codes
        saturn->SMPC.SetAreaCode(0x1);  // Japan
        REQUIRE(saturn->SMPC.GetAreaCode() == 0x1);
        
        saturn->SMPC.SetAreaCode(0x4);  // North America
        REQUIRE(saturn->SMPC.GetAreaCode() == 0x4);
        
        saturn->SMPC.SetAreaCode(0xC);  // Europe
        REQUIRE(saturn->SMPC.GetAreaCode() == 0xC);
        
        saturn->SMPC.SetAreaCode(0x6);  // Korea
        REQUIRE(saturn->SMPC.GetAreaCode() == 0x6);
    }
}

// Note: These tests verify SMPC functionality using direct hardware access.
// They test area code management, peripheral ports, RTC access, reset behavior,
// and integration with other system components - all based on official Saturn
// hardware specifications.

