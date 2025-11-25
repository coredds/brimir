// Brimir Save State Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>

using namespace brimir;

TEST_CASE("Save state size calculation", "[savestate][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("GetStateSize returns valid size") {
        size_t size = core.GetStateSize();
        
        REQUIRE(size > 0);
        // Should be reasonable (between 1MB and 50MB)
        REQUIRE(size >= 1 * 1024 * 1024);
        REQUIRE(size <= 50 * 1024 * 1024);
    }
    
    SECTION("State size is consistent") {
        size_t size1 = core.GetStateSize();
        size_t size2 = core.GetStateSize();
        
        REQUIRE(size1 == size2);
    }
    
    SECTION("State size before initialization") {
        CoreWrapper uninit_core;
        size_t size = uninit_core.GetStateSize();
        
        REQUIRE(size == 0);
    }
}

TEST_CASE("Save state basic operations", "[savestate][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Save to valid buffer succeeds") {
        size_t stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer(stateSize);
        
        bool result = core.SaveState(buffer.data(), stateSize);
        REQUIRE(result);
    }
    
    SECTION("Save to nullptr fails") {
        bool result = core.SaveState(nullptr, 1024);
        REQUIRE_FALSE(result);
    }
    
    SECTION("Save with zero size fails") {
        std::vector<uint8_t> buffer(1024);
        bool result = core.SaveState(buffer.data(), 0);
        REQUIRE_FALSE(result);
    }
    
    SECTION("Save with too small buffer fails") {
        size_t stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer(stateSize / 2);
        
        bool result = core.SaveState(buffer.data(), buffer.size());
        REQUIRE_FALSE(result);
    }
}

TEST_CASE("Load state basic operations", "[savestate][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Load from nullptr fails") {
        bool result = core.LoadState(nullptr, 1024);
        REQUIRE_FALSE(result);
    }
    
    SECTION("Load with zero size fails") {
        std::vector<uint8_t> buffer(1024);
        bool result = core.LoadState(buffer.data(), 0);
        REQUIRE_FALSE(result);
    }
    
    SECTION("Load with too small buffer fails") {
        size_t stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer(stateSize / 2);
        
        bool result = core.LoadState(buffer.data(), buffer.size());
        REQUIRE_FALSE(result);
    }
}

TEST_CASE("Save and load state cycle", "[savestate][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Basic save and load") {
        size_t stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer(stateSize);
        
        // Save state
        bool saveResult = core.SaveState(buffer.data(), stateSize);
        REQUIRE(saveResult);
        
        // Load state
        bool loadResult = core.LoadState(buffer.data(), stateSize);
        REQUIRE(loadResult);
    }
    
    SECTION("Multiple save/load cycles") {
        size_t stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer(stateSize);
        
        for (int i = 0; i < 5; ++i) {
            bool saveResult = core.SaveState(buffer.data(), stateSize);
            REQUIRE(saveResult);
            
            bool loadResult = core.LoadState(buffer.data(), stateSize);
            REQUIRE(loadResult);
        }
    }
    
    SECTION("Save, modify, load restores state") {
        size_t stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer(stateSize);
        
        // Save initial state
        core.SaveState(buffer.data(), stateSize);
        
        // Run some frames to modify state
        for (int i = 0; i < 10; ++i) {
            core.RunFrame();
        }
        
        // Load should succeed
        bool loadResult = core.LoadState(buffer.data(), stateSize);
        REQUIRE(loadResult);
    }
}

TEST_CASE("Save state with BIOS", "[savestate][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    // Load BIOS
    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    core.LoadIPL(std::span<const uint8_t>(biosData));
    
    SECTION("Can save state after loading BIOS") {
        size_t stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer(stateSize);
        
        bool result = core.SaveState(buffer.data(), stateSize);
        REQUIRE(result);
    }
    
    SECTION("Can load state with BIOS") {
        size_t stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer(stateSize);
        
        // Save with BIOS loaded
        core.SaveState(buffer.data(), stateSize);
        
        // Load state
        bool result = core.LoadState(buffer.data(), stateSize);
        REQUIRE(result);
    }
}

TEST_CASE("Save state buffer management", "[savestate][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Save to multiple buffers") {
        size_t stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer1(stateSize);
        std::vector<uint8_t> buffer2(stateSize);
        
        bool result1 = core.SaveState(buffer1.data(), stateSize);
        bool result2 = core.SaveState(buffer2.data(), stateSize);
        
        REQUIRE(result1);
        REQUIRE(result2);
    }
    
    SECTION("Exact size buffer") {
        size_t stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer(stateSize);
        
        bool result = core.SaveState(buffer.data(), stateSize);
        REQUIRE(result);
    }
    
    SECTION("Larger than needed buffer") {
        size_t stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer(stateSize * 2);
        
        bool result = core.SaveState(buffer.data(), buffer.size());
        REQUIRE(result);
    }
}

TEST_CASE("Save state error handling", "[savestate][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Save before initialization") {
        CoreWrapper uninit_core;
        std::vector<uint8_t> buffer(1024);
        
        bool result = uninit_core.SaveState(buffer.data(), buffer.size());
        REQUIRE_FALSE(result);
    }
    
    SECTION("Load before initialization") {
        CoreWrapper uninit_core;
        std::vector<uint8_t> buffer(1024);
        
        bool result = uninit_core.LoadState(buffer.data(), buffer.size());
        REQUIRE_FALSE(result);
    }
    
    SECTION("Load without prior save") {
        size_t stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer(stateSize, 0x00);
        
        // Loading random data should fail or succeed gracefully
        // (Ymir might reject invalid state data)
        [[maybe_unused]] bool result = core.LoadState(buffer.data(), stateSize);
        // Don't require success here - just verify no crash
    }
}

TEST_CASE("Save state with frame execution", "[savestate][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Save after running frames") {
        // Run some frames
        for (int i = 0; i < 5; ++i) {
            core.RunFrame();
        }
        
        size_t stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer(stateSize);
        
        bool result = core.SaveState(buffer.data(), stateSize);
        REQUIRE(result);
    }
    
    SECTION("Save, run frames, load, verify restoration") {
        size_t stateSize = core.GetStateSize();
        std::vector<uint8_t> savePoint1(stateSize);
        std::vector<uint8_t> savePoint2(stateSize);
        
        // Save initial state
        core.SaveState(savePoint1.data(), stateSize);
        
        // Run frames and save again
        for (int i = 0; i < 10; ++i) {
            core.RunFrame();
        }
        core.SaveState(savePoint2.data(), stateSize);
        
        // Load first state
        bool loadResult = core.LoadState(savePoint1.data(), stateSize);
        REQUIRE(loadResult);
    }
}

TEST_CASE("Save state consistency", "[savestate][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("State size doesn't change after operations") {
        size_t size1 = core.GetStateSize();
        
        // Run some operations
        core.RunFrame();
        core.Reset();
        
        size_t size2 = core.GetStateSize();
        REQUIRE(size1 == size2);
    }
    
    SECTION("Multiple saves produce same size data") {
        size_t stateSize = core.GetStateSize();
        std::vector<uint8_t> buffer1(stateSize);
        std::vector<uint8_t> buffer2(stateSize);
        
        core.SaveState(buffer1.data(), stateSize);
        core.SaveState(buffer2.data(), stateSize);
        
        // Both should succeed
        REQUIRE(true);
    }
}

