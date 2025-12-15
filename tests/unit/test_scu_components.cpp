// Brimir SCU Component Tests  
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0
//
// Tests for Saturn SCU: DMA, DSP, cartridge slot
// Using direct hardware access via GetSaturn()->SCU

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>

using namespace brimir;

TEST_CASE("SCU - DMA State", "[scu][dma][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("DMA is not active after initialization") {
        // After init, no DMA should be running
        REQUIRE_FALSE(saturn->SCU.IsDMAActive());
    }
    
    SECTION("DMA state persists through soft reset") {
        // Verify DMA inactive
        REQUIRE_FALSE(saturn->SCU.IsDMAActive());
        
        // Soft reset
        saturn->Reset(false);
        
        // Should still be inactive
        REQUIRE_FALSE(saturn->SCU.IsDMAActive());
    }
}

TEST_CASE("SCU - Cartridge Slot Operations", "[scu][cartridge][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Can access cartridge slot") {
        // Get cartridge reference (should be NoCartridge by default)
        auto& cart = saturn->SCU.GetCartridge();
        
        // Should not be null
        REQUIRE(&cart != nullptr);
    }
    
    SECTION("Cartridge can be removed") {
        // Should not crash
        saturn->SCU.RemoveCartridge();
        
        // Should still be able to get cartridge (will be NoCartridge)
        auto& cart = saturn->SCU.GetCartridge();
        REQUIRE(&cart != nullptr);
    }
}

TEST_CASE("SCU - DSP Access", "[scu][dsp][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("Can access SCU DSP") {
        auto& dsp = saturn->SCU.GetDSP();
        
        // DSP should be accessible
        REQUIRE(&dsp != nullptr);
    }
}

TEST_CASE("SCU - Reset Behavior", "[scu][reset][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("SCU survives soft reset") {
        // Soft reset
        saturn->Reset(false);
        
        // SCU should still be accessible
        REQUIRE_FALSE(saturn->SCU.IsDMAActive());
        auto& cart = saturn->SCU.GetCartridge();
        REQUIRE(&cart != nullptr);
    }
    
    SECTION("SCU survives hard reset") {
        // Hard reset
        saturn->Reset(true);
        
        // SCU should still be accessible
        REQUIRE_FALSE(saturn->SCU.IsDMAActive());
        auto& cart = saturn->SCU.GetCartridge();
        REQUIRE(&cart != nullptr);
    }
}

TEST_CASE("SCU - State Consistency", "[scu][savestate][implemented]") {
    CoreWrapper core;
    core.Initialize();
    
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    
    SECTION("SCU state survives save/load cycle") {
        // Get initial state
        bool dmaActive1 = saturn->SCU.IsDMAActive();
        
        // Save state
        auto stateSize = core.GetStateSize();
        std::vector<uint8_t> state(stateSize);
        REQUIRE(core.SaveState(state.data(), stateSize));
        
        // Load state
        REQUIRE(core.LoadState(state.data(), stateSize));
        
        // DMA state should be consistent
        bool dmaActive2 = saturn->SCU.IsDMAActive();
        REQUIRE(dmaActive1 == dmaActive2);
    }
}

// Note: These tests verify basic SCU component access and state management.
// Detailed DMA operation tests require additional APIs. These tests ensure
// the SCU component is properly initialized and accessible through the
// GetSaturn() API.

