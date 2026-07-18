// Brimir SRAM Persistence Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include <brimir/core_wrapper.hpp>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace brimir;

TEST_CASE("SRAM set/get round-trip", "[sram][unit]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    size_t size = core.GetSRAMSize();
    REQUIRE(size > 0);

    std::vector<uint8_t> testData(size, 0xA5);
    REQUIRE(core.SetSRAMData(testData.data(), size));

    const uint8_t* out = static_cast<const uint8_t*>(core.GetSRAMData());
    REQUIRE(out != nullptr);
    REQUIRE(std::memcmp(out, testData.data(), size) == 0);
}

TEST_CASE("SRAM SetSRAMData rejects bad inputs", "[sram][unit]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    size_t size = core.GetSRAMSize();
    REQUIRE(size > 0);

    REQUIRE_FALSE(core.SetSRAMData(nullptr, size));
    REQUIRE_FALSE(core.SetSRAMData(nullptr, 0));

    std::vector<uint8_t> smallData(size / 2, 0x42);
    REQUIRE_FALSE(core.SetSRAMData(smallData.data(), smallData.size()));

    std::vector<uint8_t> testData(size, 0x5A);
    REQUIRE(core.SetSRAMData(testData.data(), size));
}

TEST_CASE("LoadGame preserves frontend-provided SRAM buffer", "[sram][unit]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    // Seed the SRAM buffer before LoadGame, simulating a frontend that calls
    // retro_get_memory_data / retro_get_memory_size before retro_load_game.
    size_t size = core.GetSRAMSize();
    REQUIRE(size > 0);
    uint8_t* sram = static_cast<uint8_t*>(core.GetSRAMData());
    REQUIRE(sram != nullptr);
    std::vector<uint8_t> expected(size, 0xB5);
    std::memcpy(sram, expected.data(), size);

    // Set up a temporary save/system directory and a dummy disc file. The
    // game load is expected to fail because the file is not a valid disc image,
    // but the SRAM handling in LoadGame runs before disc parsing.
    std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() / "brimir_sram_preload_test";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir / "saves");
    std::filesystem::create_directories(tempDir / "system");

    std::filesystem::path gamePath = tempDir / "saves" / "dummy.iso";
    {
        std::ofstream dummy(gamePath, std::ios::binary);
        dummy.write("not a real disc image", 21);
    }

    REQUIRE_FALSE(core.LoadGame(gamePath.string().c_str(),
                                (tempDir / "saves").string().c_str(),
                                (tempDir / "system").string().c_str()));

    const uint8_t* after = static_cast<const uint8_t*>(core.GetSRAMData());
    REQUIRE(after != nullptr);
    REQUIRE(std::memcmp(after, expected.data(), size) == 0);

    // Ymir memory-maps the .bup file; shut down before removing the temp tree.
    core.Shutdown();
    std::filesystem::remove_all(tempDir);
}
