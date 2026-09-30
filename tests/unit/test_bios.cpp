// Brimir BIOS/IPL Loading Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0
//
// Basic LoadIPL size validation lives in test_core_wrapper.cpp; this file
// covers the remaining edge cases and LoadIPLFromFile.

#include "catch_amalgamated.hpp"
#include <brimir/core_wrapper.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace brimir;

namespace {

constexpr size_t kIPLSize = 512 * 1024;

// Creates a uniquely named file path under the system temp directory and
// removes it on destruction.
struct TempFile {
    std::filesystem::path path;

    TempFile() {
        auto dir = std::filesystem::temp_directory_path() /
                   ("brimir_bios_test_" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(dir);
        path = dir / "bios.bin";
    }

    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove_all(path.parent_path(), ec);
    }

    void Write(size_t size, uint8_t fill) const {
        std::ofstream file(path, std::ios::binary);
        std::vector<uint8_t> data(size, fill);
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
};

} // namespace

TEST_CASE("BIOS loading from memory rejects oversized image", "[bios][unit]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    std::vector<uint8_t> biosData(1024 * 1024, 0xFF);
    REQUIRE_FALSE(core.LoadIPL(std::span<const uint8_t>(biosData)));
    REQUIRE_FALSE(core.IsIPLLoaded());
}

TEST_CASE("BIOS loading without initialization fails", "[bios][unit]") {
    CoreWrapper core;

    std::vector<uint8_t> biosData(kIPLSize, 0xFF);
    REQUIRE_FALSE(core.LoadIPL(std::span<const uint8_t>(biosData)));
    REQUIRE_FALSE(core.IsIPLLoaded());
}

TEST_CASE("BIOS loading from file", "[bios][unit][file]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    SECTION("Non-existent file fails") {
        TempFile tmp;  // never written
        REQUIRE_FALSE(core.LoadIPLFromFile(tmp.path.string().c_str()));
        REQUIRE_FALSE(core.IsIPLLoaded());
    }

    SECTION("nullptr path fails") {
        REQUIRE_FALSE(core.LoadIPLFromFile(nullptr));
        REQUIRE_FALSE(core.IsIPLLoaded());
    }

    SECTION("Empty path fails") {
        REQUIRE_FALSE(core.LoadIPLFromFile(""));
        REQUIRE_FALSE(core.IsIPLLoaded());
    }

    SECTION("Valid 512KB file loads into IPL ROM") {
        TempFile tmp;
        tmp.Write(kIPLSize, 0xAA);

        REQUIRE(core.LoadIPLFromFile(tmp.path.string().c_str()));
        REQUIRE(core.IsIPLLoaded());

        auto* saturn = core.GetSaturn();
        REQUIRE(saturn != nullptr);
        REQUIRE(saturn->mem.IPL[0] == 0xAA);
        REQUIRE(saturn->mem.IPL[kIPLSize - 1] == 0xAA);
    }

    SECTION("Wrong-sized file fails") {
        TempFile tmp;
        tmp.Write(256 * 1024, 0xBB);

        REQUIRE_FALSE(core.LoadIPLFromFile(tmp.path.string().c_str()));
        REQUIRE_FALSE(core.IsIPLLoaded());
    }
}

TEST_CASE("BIOS loading from file without initialization fails", "[bios][unit][file]") {
    TempFile tmp;
    tmp.Write(kIPLSize, 0xAA);

    CoreWrapper core;
    REQUIRE_FALSE(core.LoadIPLFromFile(tmp.path.string().c_str()));
    REQUIRE_FALSE(core.IsIPLLoaded());
}
