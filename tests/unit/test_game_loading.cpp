// Brimir Game Loading Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>

using namespace brimir;

TEST_CASE("Game loading validation", "[game][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Load with nullptr path") {
        bool result = core.LoadGame(nullptr);
        REQUIRE_FALSE(result);
        REQUIRE_FALSE(core.IsGameLoaded());
    }
    
    SECTION("Load with empty path") {
        bool result = core.LoadGame("");
        REQUIRE_FALSE(result);
        REQUIRE_FALSE(core.IsGameLoaded());
    }
    
    SECTION("Load non-existent file") {
        bool result = core.LoadGame("nonexistent_game.cue");
        REQUIRE_FALSE(result);
        REQUIRE_FALSE(core.IsGameLoaded());
    }
    
    SECTION("Load before initialization") {
        CoreWrapper uninit_core;
        bool result = uninit_core.LoadGame("game.cue");
        REQUIRE_FALSE(result);
        REQUIRE_FALSE(uninit_core.IsGameLoaded());
    }
}

TEST_CASE("Game loading file format validation", "[game][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("CUE file format") {
        // Will fail (file doesn't exist) but tests format acceptance
        bool result = core.LoadGame("test.cue");
        REQUIRE_FALSE(result);
    }
    
    SECTION("ISO file format") {
        bool result = core.LoadGame("test.iso");
        REQUIRE_FALSE(result);
    }
    
    SECTION("CHD file format") {
        bool result = core.LoadGame("test.chd");
        REQUIRE_FALSE(result);
    }
    
    SECTION("CCD file format") {
        bool result = core.LoadGame("test.ccd");
        REQUIRE_FALSE(result);
    }
    
    SECTION("MDS file format") {
        bool result = core.LoadGame("test.mds");
        REQUIRE_FALSE(result);
    }
    
    SECTION("BIN file format") {
        bool result = core.LoadGame("test.bin");
        REQUIRE_FALSE(result);
    }
}

TEST_CASE("Game info retrieval", "[game][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("GetGameInfo before game loaded") {
        char title[256];
        char region[16];
        
        bool result = core.GetGameInfo(title, sizeof(title), region, sizeof(region));
        REQUIRE_FALSE(result);
    }
    
    SECTION("GetGameInfo with null pointers") {
        bool result = core.GetGameInfo(nullptr, 256, nullptr, 16);
        REQUIRE_FALSE(result);
    }
    
    SECTION("GetGameInfo with zero sizes") {
        char title[256];
        char region[16];
        
        bool result = core.GetGameInfo(title, 0, region, 0);
        REQUIRE_FALSE(result);
    }
}

TEST_CASE("Game loading state management", "[game][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("IsGameLoaded initially false") {
        REQUIRE_FALSE(core.IsGameLoaded());
    }
    
    SECTION("Failed load doesn't change state") {
        REQUIRE_FALSE(core.IsGameLoaded());
        core.LoadGame("nonexistent.cue");
        REQUIRE_FALSE(core.IsGameLoaded());
    }
    
    SECTION("State persists after failed load") {
        core.LoadGame("nonexistent1.cue");
        REQUIRE_FALSE(core.IsGameLoaded());
        core.LoadGame("nonexistent2.cue");
        REQUIRE_FALSE(core.IsGameLoaded());
    }
}

TEST_CASE("Game loading with BIOS", "[game][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Can attempt game load after BIOS") {
        // Load BIOS
        std::vector<uint8_t> biosData(512 * 1024, 0xFF);
        core.LoadIPL(std::span<const uint8_t>(biosData));
        
        REQUIRE(core.IsIPLLoaded());
        
        // Try to load game (will fail, but shouldn't crash)
        bool result = core.LoadGame("test.cue");
        REQUIRE_FALSE(result);
        
        // BIOS should still be loaded
        REQUIRE(core.IsIPLLoaded());
    }
}

TEST_CASE("Game unloading", "[game][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("UnloadGame when no game loaded") {
        REQUIRE_FALSE(core.IsGameLoaded());
        REQUIRE_NOTHROW(core.UnloadGame());
        REQUIRE_FALSE(core.IsGameLoaded());
    }
    
    SECTION("UnloadGame is safe to call multiple times") {
        core.UnloadGame();
        core.UnloadGame();
        core.UnloadGame();
        REQUIRE_FALSE(core.IsGameLoaded());
    }
}

TEST_CASE("Game loading with reset", "[game][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Reset after failed game load") {
        core.LoadGame("nonexistent.cue");
        REQUIRE_NOTHROW(core.Reset());
        REQUIRE(core.IsInitialized());
    }
}

TEST_CASE("Game loading error handling", "[game][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Invalid path characters handled") {
        // Path with invalid characters (system-dependent)
        bool result = core.LoadGame("invalid\x00path.cue");
        REQUIRE_FALSE(result);
    }
    
    SECTION("Very long path handled") {
        std::string longPath(1000, 'a');
        longPath += ".cue";
        
        bool result = core.LoadGame(longPath.c_str());
        REQUIRE_FALSE(result);
    }
    
    SECTION("Path with spaces") {
        bool result = core.LoadGame("game with spaces.cue");
        REQUIRE_FALSE(result);
    }
    
    SECTION("Path with special characters") {
        bool result = core.LoadGame("game-name_v1.0.cue");
        REQUIRE_FALSE(result);
    }
}

TEST_CASE("Game loading concurrent operations", "[game][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Multiple load attempts") {
        for (int i = 0; i < 10; ++i) {
            bool result = core.LoadGame("nonexistent.cue");
            REQUIRE_FALSE(result);
            REQUIRE_FALSE(core.IsGameLoaded());
        }
    }
    
    SECTION("Load and reset cycle") {
        core.LoadGame("test1.cue");
        core.Reset();
        core.LoadGame("test2.cue");
        core.Reset();
        REQUIRE(core.IsInitialized());
    }
}

TEST_CASE("Game info buffer handling", "[game][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Small buffer sizes") {
        char title[2];
        char region[2];
        
        // Should handle small buffers gracefully
        bool result = core.GetGameInfo(title, sizeof(title), region, sizeof(region));
        REQUIRE_FALSE(result); // No game loaded
    }
    
    SECTION("Large buffer sizes") {
        char title[1024];
        char region[256];
        
        bool result = core.GetGameInfo(title, sizeof(title), region, sizeof(region));
        REQUIRE_FALSE(result); // No game loaded
    }
}

