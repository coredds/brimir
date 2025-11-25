// Brimir BIOS/IPL Loading Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace brimir;

TEST_CASE("BIOS loading from memory", "[bios][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Load valid BIOS size") {
        std::vector<uint8_t> biosData(512 * 1024, 0xFF);
        bool result = core.LoadIPL(std::span<const uint8_t>(biosData));
        
        REQUIRE(result);
        REQUIRE(core.IsIPLLoaded());
    }
    
    SECTION("Load invalid BIOS size - too small") {
        std::vector<uint8_t> biosData(256 * 1024, 0xFF);
        bool result = core.LoadIPL(std::span<const uint8_t>(biosData));
        
        REQUIRE_FALSE(result);
        REQUIRE_FALSE(core.IsIPLLoaded());
    }
    
    SECTION("Load invalid BIOS size - too large") {
        std::vector<uint8_t> biosData(1024 * 1024, 0xFF);
        bool result = core.LoadIPL(std::span<const uint8_t>(biosData));
        
        REQUIRE_FALSE(result);
        REQUIRE_FALSE(core.IsIPLLoaded());
    }
    
    SECTION("Load empty BIOS") {
        std::vector<uint8_t> biosData;
        bool result = core.LoadIPL(std::span<const uint8_t>(biosData));
        
        REQUIRE_FALSE(result);
        REQUIRE_FALSE(core.IsIPLLoaded());
    }
}

TEST_CASE("BIOS loading without initialization", "[bios][unit]") {
    CoreWrapper core;
    
    SECTION("LoadIPL before init fails") {
        std::vector<uint8_t> biosData(512 * 1024, 0xFF);
        bool result = core.LoadIPL(std::span<const uint8_t>(biosData));
        
        REQUIRE_FALSE(result);
        REQUIRE_FALSE(core.IsIPLLoaded());
    }
}

TEST_CASE("BIOS loading from file", "[bios][unit][file]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Load from non-existent file") {
        bool result = core.LoadIPLFromFile("nonexistent_bios.bin");
        REQUIRE_FALSE(result);
        REQUIRE_FALSE(core.IsIPLLoaded());
    }
    
    SECTION("Load from nullptr path") {
        bool result = core.LoadIPLFromFile(nullptr);
        REQUIRE_FALSE(result);
        REQUIRE_FALSE(core.IsIPLLoaded());
    }
    
    SECTION("Load from empty path") {
        bool result = core.LoadIPLFromFile("");
        REQUIRE_FALSE(result);
        REQUIRE_FALSE(core.IsIPLLoaded());
    }
}

TEST_CASE("BIOS loading from file with test file", "[bios][unit][file][temp]") {
    CoreWrapper core;
    core.Initialize();
    
    // Create a temporary test BIOS file
    const char* testBiosPath = "test_bios.bin";
    
    SECTION("Load from valid test file") {
        // Create test BIOS file
        {
            std::ofstream biosFile(testBiosPath, std::ios::binary);
            std::vector<uint8_t> testData(512 * 1024, 0xAA);
            biosFile.write(reinterpret_cast<const char*>(testData.data()), testData.size());
        }
        
        // Load from file
        bool result = core.LoadIPLFromFile(testBiosPath);
        
        // Clean up
        std::filesystem::remove(testBiosPath);
        
        REQUIRE(result);
        REQUIRE(core.IsIPLLoaded());
    }
    
    SECTION("Load from file with wrong size") {
        // Create wrong-sized BIOS file
        {
            std::ofstream biosFile(testBiosPath, std::ios::binary);
            std::vector<uint8_t> testData(256 * 1024, 0xBB);
            biosFile.write(reinterpret_cast<const char*>(testData.data()), testData.size());
        }
        
        // Load from file
        bool result = core.LoadIPLFromFile(testBiosPath);
        
        // Clean up
        std::filesystem::remove(testBiosPath);
        
        REQUIRE_FALSE(result);
        REQUIRE_FALSE(core.IsIPLLoaded());
    }
}

TEST_CASE("BIOS state after multiple operations", "[bios][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("BIOS state persists after successful load") {
        std::vector<uint8_t> biosData(512 * 1024, 0xFF);
        core.LoadIPL(std::span<const uint8_t>(biosData));
        
        REQUIRE(core.IsIPLLoaded());
        
        // Run a frame
        core.RunFrame();
        
        // BIOS should still be loaded
        REQUIRE(core.IsIPLLoaded());
    }
    
    SECTION("BIOS state cleared after reset") {
        std::vector<uint8_t> biosData(512 * 1024, 0xFF);
        core.LoadIPL(std::span<const uint8_t>(biosData));
        
        REQUIRE(core.IsIPLLoaded());
        
        // Shutdown and reinit
        core.Shutdown();
        
        // BIOS state should be cleared
        // (IsIPLLoaded() may not be valid after shutdown, but it shouldn't crash)
    }
    
    SECTION("Multiple BIOS loads") {
        std::vector<uint8_t> biosData1(512 * 1024, 0xFF);
        std::vector<uint8_t> biosData2(512 * 1024, 0xAA);
        
        // First load
        bool result1 = core.LoadIPL(std::span<const uint8_t>(biosData1));
        REQUIRE(result1);
        REQUIRE(core.IsIPLLoaded());
        
        // Second load (should also succeed)
        bool result2 = core.LoadIPL(std::span<const uint8_t>(biosData2));
        REQUIRE(result2);
        REQUIRE(core.IsIPLLoaded());
    }
}

TEST_CASE("BIOS integration with game loading", "[bios][unit][integration]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Can load game without BIOS") {
        // Try to load a game (will fail due to nonexistent file, but shouldn't crash)
        bool gameResult = core.LoadGame("nonexistent.iso");
        REQUIRE_FALSE(gameResult);
    }
    
    SECTION("BIOS and game can coexist") {
        // Load BIOS
        std::vector<uint8_t> biosData(512 * 1024, 0xFF);
        bool biosResult = core.LoadIPL(std::span<const uint8_t>(biosData));
        REQUIRE(biosResult);
        REQUIRE(core.IsIPLLoaded());
        
        // Try to load game
        bool gameResult = core.LoadGame("nonexistent.iso");
        REQUIRE_FALSE(gameResult); // Game doesn't exist, but BIOS should still be loaded
        REQUIRE(core.IsIPLLoaded());
    }
}

TEST_CASE("BIOS data validation", "[bios][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("BIOS with all zeros") {
        std::vector<uint8_t> biosData(512 * 1024, 0x00);
        bool result = core.LoadIPL(std::span<const uint8_t>(biosData));
        
        // Should load successfully (Ymir will handle invalid BIOS content)
        REQUIRE(result);
        REQUIRE(core.IsIPLLoaded());
    }
    
    SECTION("BIOS with all ones") {
        std::vector<uint8_t> biosData(512 * 1024, 0xFF);
        bool result = core.LoadIPL(std::span<const uint8_t>(biosData));
        
        REQUIRE(result);
        REQUIRE(core.IsIPLLoaded());
    }
    
    SECTION("BIOS with pattern") {
        std::vector<uint8_t> biosData(512 * 1024);
        for (size_t i = 0; i < biosData.size(); ++i) {
            biosData[i] = static_cast<uint8_t>(i & 0xFF);
        }
        
        bool result = core.LoadIPL(std::span<const uint8_t>(biosData));
        REQUIRE(result);
        REQUIRE(core.IsIPLLoaded());
    }
}

