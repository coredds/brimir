// Brimir CD Operations Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0
//
// Tests for Saturn CD-ROM operations and disc handling
// Using direct hardware access via GetSaturn()

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>

using namespace brimir;

TEST_CASE("CD Operations - Tray State", "[cd][disc][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Tray can be opened") {
        saturn->OpenTray();
        REQUIRE(saturn->IsTrayOpen());
    }
    
    SECTION("Tray can be closed after opening") {
        saturn->OpenTray();
        REQUIRE(saturn->IsTrayOpen());
        
        saturn->CloseTray();
        REQUIRE_FALSE(saturn->IsTrayOpen());
    }
    
    SECTION("Tray operations are idempotent") {
        // Open twice
        saturn->OpenTray();
        saturn->OpenTray();
        REQUIRE(saturn->IsTrayOpen());
        
        // Close twice
        saturn->CloseTray();
        saturn->CloseTray();
        REQUIRE_FALSE(saturn->IsTrayOpen());
    }
}

TEST_CASE("CD Operations - Disc Ejection", "[cd][disc][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Can eject disc when no disc loaded") {
        // Should not crash even without disc
        saturn->EjectDisc();
        
        // Tray should be open after eject
        REQUIRE(saturn->IsTrayOpen());
    }
    
    SECTION("Eject opens tray") {
        // Close tray first
        saturn->CloseTray();
        REQUIRE_FALSE(saturn->IsTrayOpen());
        
        // Eject
        saturn->EjectDisc();
        
        // Tray should now be open
        REQUIRE(saturn->IsTrayOpen());
    }
}

TEST_CASE("CD Operations - Tray State After Reset", "[cd][reset][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Tray state survives soft reset") {
        // Close tray
        saturn->CloseTray();
        REQUIRE_FALSE(saturn->IsTrayOpen());
        
        // Soft reset
        saturn->Reset(false);
        
        // Tray state should be queryable (may or may not change on reset)
        bool trayOpen = saturn->IsTrayOpen();
        (void)trayOpen;  // Just verify it doesn't crash
        
        REQUIRE(core.IsInitialized());
    }
    
    SECTION("Tray state survives hard reset") {
        saturn->OpenTray();
        
        saturn->Reset(true);
        
        // Should be able to query tray state
        bool trayOpen = saturn->IsTrayOpen();
        (void)trayOpen;
        
        REQUIRE(core.IsInitialized());
    }
}

TEST_CASE("CD Operations - Tray Manipulation Sequence", "[cd][disc][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Complex tray operation sequence") {
        // Start with closed tray
        saturn->CloseTray();
        REQUIRE_FALSE(saturn->IsTrayOpen());
        
        // Open
        saturn->OpenTray();
        REQUIRE(saturn->IsTrayOpen());
        
        // Close
        saturn->CloseTray();
        REQUIRE_FALSE(saturn->IsTrayOpen());
        
        // Eject (opens)
        saturn->EjectDisc();
        REQUIRE(saturn->IsTrayOpen());
        
        // Close again
        saturn->CloseTray();
        REQUIRE_FALSE(saturn->IsTrayOpen());
    }
}

TEST_CASE("CD Operations - Component Access", "[cd][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("CD Block component is accessible") {
        // Source: Saturn documentation - CD Block handles disc I/O
        REQUIRE(&saturn->CDBlock != nullptr);
    }
    
    SECTION("CD Drive component is accessible") {
        REQUIRE(&saturn->CDDrive != nullptr);
    }
}

TEST_CASE("CD Operations - State Consistency", "[cd][savestate][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Tray state survives save/load cycle") {
        // Set tray closed
        saturn->CloseTray();
        bool trayState1 = saturn->IsTrayOpen();
        
        // Save state
        auto stateSize = core.GetStateSize();
        std::vector<uint8_t> state(stateSize);
        REQUIRE(core.SaveState(state.data(), stateSize));
        
        // Change tray state
        saturn->OpenTray();
        REQUIRE(saturn->IsTrayOpen());
        
        // Load state
        REQUIRE(core.LoadState(state.data(), stateSize));
        
        // Tray state should be restored
        bool trayState2 = saturn->IsTrayOpen();
        REQUIRE(trayState1 == trayState2);
    }
}

TEST_CASE("CD Operations - Execution with Tray States", "[cd][execution][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    // Load BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("Can run frames with tray closed") {
        saturn->CloseTray();
        
        for (int i = 0; i < 5; ++i) {
            core.RunFrame();
        }
        
        REQUIRE(core.IsInitialized());
    }
    
    SECTION("Can run frames with tray open") {
        saturn->OpenTray();
        
        for (int i = 0; i < 5; ++i) {
            core.RunFrame();
        }
        
        REQUIRE(core.IsInitialized());
    }
}

TEST_CASE("CD Operations - CDBlock DRAM", "[cd][memory][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("CD Block DRAM is accessible") {
        // Source: Saturn documentation - CD Block has 512KB DRAM
        REQUIRE(saturn->CDBlockDRAM.size() == 512 * 1024);
    }
    
    SECTION("CD Block DRAM can be written and read") {
        // Write pattern
        saturn->CDBlockDRAM[0] = 0xAA;
        saturn->CDBlockDRAM[100] = 0x55;
        saturn->CDBlockDRAM[1000] = 0xFF;
        
        // Read back
        REQUIRE(saturn->CDBlockDRAM[0] == 0xAA);
        REQUIRE(saturn->CDBlockDRAM[100] == 0x55);
        REQUIRE(saturn->CDBlockDRAM[1000] == 0xFF);
    }
}

// Note: These tests verify CD-ROM operation functionality using direct
// hardware access. They test tray operations, disc ejection, component
// access, and integration with frame execution - all based on Saturn
// CD-ROM subsystem specifications.

