// Brimir SRAM Persistence Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include <brimir/core_wrapper.hpp>
#include <cstdint>
#include <cstring>
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
