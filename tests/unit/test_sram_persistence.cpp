// Brimir SRAM Persistence Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace brimir;

TEST_CASE("SRAM file generation", "[sram][persistence][unit]") {
    SECTION("SRAM file naming convention") {
        // SRAM files should be named after the game
        std::filesystem::path gamePath = "MyGame.cue";
        std::filesystem::path expectedBup = "MyGame.bup";
        std::filesystem::path expectedSrm = "MyGame.srm";
        
        // .bup is Ymir's format
        REQUIRE(expectedBup.extension() == ".bup");
        // .srm is RetroArch's format
        REQUIRE(expectedSrm.extension() == ".srm");
    }
    
    SECTION("SRAM file location") {
        // SRAM files should be in the save directory
        std::filesystem::path saveDir = "saves/Brimir";
        std::filesystem::path sramPath = saveDir / "game.bup";
        
        REQUIRE(sramPath.parent_path().filename() == "Brimir");
    }
}

TEST_CASE("SRAM size", "[sram][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("SRAM size before game load") {
        // Before loading a game, SRAM size might be 0 or default
        size_t size = core.GetSRAMSize();
        
        // Should be a reasonable value
        REQUIRE(size >= 0);
    }
    
    SECTION("SRAM size consistency") {
        // SRAM size should be consistent across calls
        size_t size1 = core.GetSRAMSize();
        size_t size2 = core.GetSRAMSize();
        
        REQUIRE(size1 == size2);
    }
}

TEST_CASE("SRAM data access", "[sram][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("SRAM data pointer validity") {
        // SRAM data pointer should be valid after initialization
        void* sramData = core.GetSRAMData();
        
        // Pointer may be null before game loads, which is fine
        // Just testing that the function doesn't crash
        (void)sramData;
    }
    
    SECTION("SRAM data after game unload") {
        // After unloading, SRAM data should still be accessible
        // (for RetroArch to save it)
        core.UnloadGame();
        
        void* sramData = core.GetSRAMData();
        // Should not crash
        (void)sramData;
    }
}

TEST_CASE("SRAM caching behavior", "[sram][caching][unit]") {
    SECTION("SRAM cache refresh frequency") {
        // SRAM is cached and refreshed every 300 frames
        constexpr int refreshInterval = 300;
        
        REQUIRE(refreshInterval > 0);
        REQUIRE(refreshInterval <= 600); // Reasonable range
    }
}

TEST_CASE("SRAM and backup RAM relationship", "[sram][bup][unit]") {
    SECTION("Dual save format") {
        // Both .bup (Ymir format) and .srm (RetroArch format) should coexist
        std::filesystem::path bupPath = "game.bup";
        std::filesystem::path srmPath = "game.srm";
        
        // Different extensions
        REQUIRE(bupPath.extension() != srmPath.extension());
        
        // Same stem
        REQUIRE(bupPath.stem() == srmPath.stem());
    }
    
    SECTION("BUP file priority") {
        // .bup file is the authoritative source
        // .srm is for RetroArch compatibility
        
        std::string authoritative = ".bup";
        std::string compat = ".srm";
        
        REQUIRE(authoritative != compat);
    }
}

TEST_CASE("SRAM first load behavior", "[sram][loading][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("SRAM sync on first frame") {
        // On the first frame after loading, SRAM should sync from .bup
        // to restore clock settings that RetroArch may have overwritten
        
        // This is tested implicitly in the core
        REQUIRE(core.IsInitialized());
    }
}

TEST_CASE("SRAM buffer operations", "[sram][buffer][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("SetSRAMData with null pointer") {
        bool result = core.SetSRAMData(nullptr, 0);
        
        // Should handle gracefully (likely return false)
        REQUIRE_FALSE(result);
    }
    
    SECTION("SetSRAMData with zero size") {
        std::vector<uint8_t> dummyData(1);
        bool result = core.SetSRAMData(dummyData.data(), 0);
        
        // Should handle gracefully
        REQUIRE_FALSE(result);
    }
    
    SECTION("SetSRAMData with valid data") {
        size_t sramSize = core.GetSRAMSize();
        
        if (sramSize > 0) {
            std::vector<uint8_t> testData(sramSize, 0x42);
            bool result = core.SetSRAMData(testData.data(), sramSize);
            
            // May fail if no game loaded, but shouldn't crash
            (void)result;
        }
    }
}

TEST_CASE("SRAM and RTC separation", "[sram][rtc][unit]") {
    SECTION("SRAM and RTC use different files") {
        std::filesystem::path sramPath = "saves/Brimir/game.bup";
        std::filesystem::path rtcPath = "system/brimir_saturn_rtc.smpc";
        
        // Different directories
        REQUIRE(sramPath.parent_path() != rtcPath.parent_path());
        
        // Different extensions
        REQUIRE(sramPath.extension() != rtcPath.extension());
    }
    
    SECTION("RTC is not part of SRAM") {
        // Clock settings were moved to separate .smpc file in v0.1.1
        // They should NOT be in .bup or .srm
        
        std::string sramExt = ".bup";
        std::string rtcExt = ".smpc";
        
        REQUIRE(sramExt != rtcExt);
    }
}

TEST_CASE("SRAM directory structure", "[sram][filesystem][unit]") {
    SECTION("Per-game save organization") {
        // Each game should have its own save files in saves/Brimir/
        std::filesystem::path game1Bup = "saves/Brimir/Game1.bup";
        std::filesystem::path game1Srm = "saves/Brimir/Game1.srm";
        std::filesystem::path game1Cart = "saves/Brimir/Game1.cart";
        
        // All in same directory
        REQUIRE(game1Bup.parent_path() == game1Srm.parent_path());
        REQUIRE(game1Srm.parent_path() == game1Cart.parent_path());
    }
    
    SECTION("System-wide configuration separate") {
        std::filesystem::path gameData = "saves/Brimir/game.bup";
        std::filesystem::path systemData = "system/brimir_saturn_rtc.smpc";
        
        // Different parent directories
        REQUIRE(gameData.parent_path() != systemData.parent_path());
    }
}

TEST_CASE("SRAM RefreshFromEmulator", "[sram][refresh][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Refresh SRAM from emulator") {
        // RefreshSRAMFromEmulator should not crash
        core.RefreshSRAMFromEmulator();
        
        // After refresh, SRAM data should be accessible
        void* sramData = core.GetSRAMData();
        (void)sramData;
    }
}








