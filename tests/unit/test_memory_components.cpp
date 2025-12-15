// Brimir Memory Components Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0
//
// Tests for Saturn memory components: IPL ROM, Work RAM, Backup RAM
// Using direct hardware access via GetSaturn()->mem

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>
#include <brimir/sys/memory_defs.hpp>

using namespace brimir;

TEST_CASE("Memory Components - IPL ROM Size", "[memory][ipl][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("IPL ROM has correct size") {
        // Source: Saturn documentation - IPL is 512KB
        constexpr size_t expectedSize = 512 * 1024;
        REQUIRE(saturn->mem.IPL.size() == expectedSize);
        REQUIRE(saturn->mem.IPL.size() == sys::kIPLSize);
    }
}

TEST_CASE("Memory Components - Work RAM Low Size", "[memory][wram][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Work RAM Low has correct size") {
        // Source: Saturn documentation - Low WRAM is 1MB
        constexpr size_t expectedSize = 1 * 1024 * 1024;
        REQUIRE(saturn->mem.WRAMLow.size() == expectedSize);
        REQUIRE(saturn->mem.WRAMLow.size() == sys::kWRAMLowSize);
    }
}

TEST_CASE("Memory Components - Work RAM High Size", "[memory][wram][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Work RAM High has correct size") {
        // Source: Saturn documentation - High WRAM is 1MB  
        constexpr size_t expectedSize = 1 * 1024 * 1024;
        REQUIRE(saturn->mem.WRAMHigh.size() == expectedSize);
        REQUIRE(saturn->mem.WRAMHigh.size() == sys::kWRAMHighSize);
    }
}

TEST_CASE("Memory Components - IPL Loading", "[memory][ipl][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("IPL data is written correctly") {
        // Create a test BIOS with pattern
        std::vector<uint8_t> biosData(512 * 1024);
        for (size_t i = 0; i < biosData.size(); ++i) {
            biosData[i] = static_cast<uint8_t>(i & 0xFF);
        }
        
        // Load it
        core.LoadIPL(std::span<const uint8_t>(biosData));
        
        // Verify first few bytes match
        REQUIRE(saturn->mem.IPL[0] == 0x00);
        REQUIRE(saturn->mem.IPL[1] == 0x01);
        REQUIRE(saturn->mem.IPL[2] == 0x02);
        REQUIRE(saturn->mem.IPL[255] == 0xFF);
        REQUIRE(saturn->mem.IPL[256] == 0x00);  // Wraps due to & 0xFF
    }
    
    SECTION("IPL can be overwritten") {
        // Load first BIOS
        std::vector<uint8_t> biosData1(512 * 1024, 0xAA);
        core.LoadIPL(std::span<const uint8_t>(biosData1));
        REQUIRE(saturn->mem.IPL[0] == 0xAA);
        
        // Load second BIOS
        std::vector<uint8_t> biosData2(512 * 1024, 0x55);
        core.LoadIPL(std::span<const uint8_t>(biosData2));
        REQUIRE(saturn->mem.IPL[0] == 0x55);
    }
}

TEST_CASE("Memory Components - Work RAM Hard Reset", "[memory][wram][reset][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Work RAM Low is cleared on hard reset") {
        // Write some data to Work RAM Low
        saturn->mem.WRAMLow[0] = 0xAA;
        saturn->mem.WRAMLow[1000] = 0x55;
        
        // Hard reset
        saturn->Reset(true);
        
        // Work RAM should be cleared
        REQUIRE(saturn->mem.WRAMLow[0] == 0x00);
        REQUIRE(saturn->mem.WRAMLow[1000] == 0x00);
    }
    
    SECTION("Work RAM High is cleared on hard reset") {
        // Write some data to Work RAM High
        saturn->mem.WRAMHigh[0] = 0xBB;
        saturn->mem.WRAMHigh[2000] = 0x77;
        
        // Hard reset
        saturn->Reset(true);
        
        // Work RAM should be cleared
        REQUIRE(saturn->mem.WRAMHigh[0] == 0x00);
        REQUIRE(saturn->mem.WRAMHigh[2000] == 0x00);
    }
}

TEST_CASE("Memory Components - Work RAM Soft Reset", "[memory][wram][reset][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Work RAM Low is NOT cleared on soft reset") {
        // Write some data
        saturn->mem.WRAMLow[0] = 0xCC;
        saturn->mem.WRAMLow[500] = 0x33;
        
        // Soft reset
        saturn->Reset(false);
        
        // Work RAM should be preserved
        REQUIRE(saturn->mem.WRAMLow[0] == 0xCC);
        REQUIRE(saturn->mem.WRAMLow[500] == 0x33);
    }
    
    SECTION("Work RAM High is NOT cleared on soft reset") {
        // Write some data
        saturn->mem.WRAMHigh[0] = 0xDD;
        saturn->mem.WRAMHigh[1500] = 0x22;
        
        // Soft reset
        saturn->Reset(false);
        
        // Work RAM should be preserved
        REQUIRE(saturn->mem.WRAMHigh[0] == 0xDD);
        REQUIRE(saturn->mem.WRAMHigh[1500] == 0x22);
    }
}

TEST_CASE("Memory Components - IPL Persists Through Reset", "[memory][ipl][reset][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("IPL persists through soft reset") {
        // Load BIOS
        std::vector<uint8_t> biosData(512 * 1024, 0xEE);
        core.LoadIPL(std::span<const uint8_t>(biosData));
        
        // Soft reset
        saturn->Reset(false);
        
        // IPL should be preserved
        REQUIRE(saturn->mem.IPL[0] == 0xEE);
        REQUIRE(saturn->mem.IPL[1000] == 0xEE);
    }
    
    SECTION("IPL persists through hard reset") {
        // Load BIOS
        std::vector<uint8_t> biosData(512 * 1024, 0xFF);
        core.LoadIPL(std::span<const uint8_t>(biosData));
        
        // Hard reset
        saturn->Reset(true);
        
        // IPL should be preserved (ROM doesn't get cleared)
        REQUIRE(saturn->mem.IPL[0] == 0xFF);
        REQUIRE(saturn->mem.IPL[2000] == 0xFF);
    }
}

TEST_CASE("Memory Components - Internal Backup RAM", "[memory][backup][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Internal backup RAM is accessible") {
        auto& backupRAM = saturn->mem.GetInternalBackupRAM();
        
        // Should be 32KB
        // Source: Saturn documentation - internal backup RAM is 32KB
        constexpr size_t expectedSize = 32 * 1024;
        REQUIRE(backupRAM.Size() == expectedSize);
        REQUIRE(backupRAM.Size() == sys::kInternalBackupRAMSizeAmount);
    }
}

TEST_CASE("Memory Components - Memory Alignment", "[memory][alignment][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("IPL is 16-byte aligned") {
        // Check alignment (should be aligned for SIMD operations)
        auto addr = reinterpret_cast<uintptr_t>(saturn->mem.IPL.data());
        REQUIRE((addr % 16) == 0);
    }
    
    SECTION("Work RAM Low is 16-byte aligned") {
        auto addr = reinterpret_cast<uintptr_t>(saturn->mem.WRAMLow.data());
        REQUIRE((addr % 16) == 0);
    }
    
    SECTION("Work RAM High is 16-byte aligned") {
        auto addr = reinterpret_cast<uintptr_t>(saturn->mem.WRAMHigh.data());
        REQUIRE((addr % 16) == 0);
    }
}

TEST_CASE("Memory Components - Write and Read Patterns", "[memory][wram][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Can write and read back from Work RAM Low") {
        // Write a pattern
        for (size_t i = 0; i < 1000; ++i) {
            saturn->mem.WRAMLow[i] = static_cast<uint8_t>(i % 256);
        }
        
        // Read it back
        for (size_t i = 0; i < 1000; ++i) {
            REQUIRE(saturn->mem.WRAMLow[i] == static_cast<uint8_t>(i % 256));
        }
    }
    
    SECTION("Can write and read back from Work RAM High") {
        // Write a pattern
        for (size_t i = 0; i < 1000; ++i) {
            saturn->mem.WRAMHigh[i] = static_cast<uint8_t>((i * 2) % 256);
        }
        
        // Read it back
        for (size_t i = 0; i < 1000; ++i) {
            REQUIRE(saturn->mem.WRAMHigh[i] == static_cast<uint8_t>((i * 2) % 256));
        }
    }
}

TEST_CASE("Memory Components - Boundary Access", "[memory][wram][boundary][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Can access first byte of Work RAM Low") {
        saturn->mem.WRAMLow[0] = 0x42;
        REQUIRE(saturn->mem.WRAMLow[0] == 0x42);
    }
    
    SECTION("Can access last byte of Work RAM Low") {
        size_t lastIdx = sys::kWRAMLowSize - 1;
        saturn->mem.WRAMLow[lastIdx] = 0x24;
        REQUIRE(saturn->mem.WRAMLow[lastIdx] == 0x24);
    }
    
    SECTION("Can access first byte of Work RAM High") {
        saturn->mem.WRAMHigh[0] = 0x84;
        REQUIRE(saturn->mem.WRAMHigh[0] == 0x84);
    }
    
    SECTION("Can access last byte of Work RAM High") {
        size_t lastIdx = sys::kWRAMHighSize - 1;
        saturn->mem.WRAMHigh[lastIdx] = 0x48;
        REQUIRE(saturn->mem.WRAMHigh[lastIdx] == 0x48);
    }
}

TEST_CASE("Memory Components - IPL Hash", "[memory][ipl][hash][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("IPL hash changes when BIOS is loaded") {
        auto hash1 = saturn->mem.GetIPLHash();
        
        // Load a BIOS
        std::vector<uint8_t> biosData(512 * 1024, 0xAB);
        core.LoadIPL(std::span<const uint8_t>(biosData));
        
        auto hash2 = saturn->mem.GetIPLHash();
        
        // Hashes should be different
        REQUIRE(hash1 != hash2);
    }
    
    SECTION("Same BIOS produces same hash") {
        // Load BIOS
        std::vector<uint8_t> biosData(512 * 1024, 0xCD);
        core.LoadIPL(std::span<const uint8_t>(biosData));
        auto hash1 = saturn->mem.GetIPLHash();
        
        // Load same BIOS again
        core.LoadIPL(std::span<const uint8_t>(biosData));
        auto hash2 = saturn->mem.GetIPLHash();
        
        // Hashes should be identical
        REQUIRE(hash1 == hash2);
    }
    
    SECTION("Different BIOS produces different hash") {
        // Load first BIOS
        std::vector<uint8_t> biosData1(512 * 1024, 0x11);
        core.LoadIPL(std::span<const uint8_t>(biosData1));
        auto hash1 = saturn->mem.GetIPLHash();
        
        // Load different BIOS
        std::vector<uint8_t> biosData2(512 * 1024, 0x22);
        core.LoadIPL(std::span<const uint8_t>(biosData2));
        auto hash2 = saturn->mem.GetIPLHash();
        
        // Hashes should be different
        REQUIRE(hash1 != hash2);
    }
}

TEST_CASE("Memory Components - State Consistency", "[memory][savestate][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Memory content survives save/load state cycle") {
        // Write patterns to memory
        saturn->mem.WRAMLow[100] = 0x12;
        saturn->mem.WRAMHigh[200] = 0x34;
        
        // Save state
        auto stateSize = core.GetStateSize();
        std::vector<uint8_t> state(stateSize);
        REQUIRE(core.SaveState(state.data(), stateSize));
        
        // Modify memory
        saturn->mem.WRAMLow[100] = 0x00;
        saturn->mem.WRAMHigh[200] = 0x00;
        
        // Load state
        REQUIRE(core.LoadState(state.data(), stateSize));
        
        // Memory should be restored
        REQUIRE(saturn->mem.WRAMLow[100] == 0x12);
        REQUIRE(saturn->mem.WRAMHigh[200] == 0x34);
    }
}

// Note: These tests verify Saturn memory components using direct access
// via GetSaturn()->mem. They test memory sizes, reset behavior, alignment,
// read/write operations, and state persistence - all based on official
// Saturn hardware specifications.

