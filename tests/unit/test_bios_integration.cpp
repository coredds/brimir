// Brimir BIOS Integration Tests (with Real BIOS Files)
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0
//
// These tests run ONLY if real BIOS images are present in tests/fixtures/.
// They SKIP cleanly when no BIOS is available (e.g. on CI).

#include "catch_amalgamated.hpp"
#include <brimir/core_wrapper.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

using namespace brimir;

namespace {

constexpr size_t kIPLSize = 512 * 1024;

// Candidate BIOS file names, checked in order.
const char* const kBIOSCandidates[] = {
    "sega_101.bin",
    "sega_100.bin",
    "Sega Saturn BIOS (EUR).bin",
    "Sega Saturn BIOS v1.01 (JAP).bin",
    "Sega Saturn BIOS v1.00 (JAP).bin",
};

std::filesystem::path FixturesDir() {
    // tests/unit/<this file> -> tests/fixtures
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures";
}

std::vector<std::filesystem::path> AvailableBIOS() {
    std::vector<std::filesystem::path> found;
    for (const char* name : kBIOSCandidates) {
        auto path = FixturesDir() / name;
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec) && std::filesystem::file_size(path, ec) == kIPLSize) {
            found.push_back(path);
        }
    }
    return found;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("BIOS integration - real BIOS images load into IPL ROM", "[bios][integration]") {
    const auto biosFiles = AvailableBIOS();
    if (biosFiles.empty()) {
        SKIP("No BIOS files found in " << FixturesDir().string());
    }

    for (const auto& biosPath : biosFiles) {
        INFO("BIOS: " << biosPath.filename().string());

        CoreWrapper core;
        REQUIRE(core.Initialize());
        REQUIRE(core.LoadIPLFromFile(biosPath.string().c_str()));
        REQUIRE(core.IsIPLLoaded());

        const auto expected = ReadFile(biosPath);
        REQUIRE(expected.size() == kIPLSize);

        auto* saturn = core.GetSaturn();
        REQUIRE(saturn != nullptr);
        REQUIRE(std::equal(expected.begin(), expected.end(), saturn->mem.IPL.begin()));
    }
}

TEST_CASE("BIOS integration - real BIOS boots and renders", "[bios][integration]") {
    const auto biosFiles = AvailableBIOS();
    if (biosFiles.empty()) {
        SKIP("No BIOS files found in " << FixturesDir().string());
    }

    const auto& biosPath = biosFiles.front();
    INFO("BIOS: " << biosPath.filename().string());

    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(core.LoadIPLFromFile(biosPath.string().c_str()));

    // With no disc inserted the BIOS boots to its system menu / CD player.
    bool sawNonBlack = false;
    for (int i = 0; i < 600 && !sawNonBlack; ++i) {
        REQUIRE_NOTHROW(core.RunFrame());
        if (i < 60 || i % 30 != 0) {
            continue;
        }

        const auto* fb = static_cast<const uint32_t*>(core.GetFramebuffer());
        const uint32_t w = core.GetFramebufferWidth();
        const uint32_t h = core.GetFramebufferHeight();
        const uint32_t pitch = core.GetFramebufferPitch();
        REQUIRE(fb != nullptr);
        REQUIRE(w > 0);
        REQUIRE(h > 0);

        const uint32_t stride = pitch / sizeof(uint32_t);  // pitch is in bytes (XRGB8888)
        REQUIRE(stride >= w);
        for (uint32_t y = 0; y < h && !sawNonBlack; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                if ((fb[y * stride + x] & 0x00FFFFFF) != 0) {
                    sawNonBlack = true;
                    break;
                }
            }
        }
    }

    REQUIRE(sawNonBlack);
    REQUIRE(core.IsIPLLoaded());
}

TEST_CASE("BIOS integration - save/load cycles while BIOS is running", "[bios][integration][savestate]") {
    const auto biosFiles = AvailableBIOS();
    if (biosFiles.empty()) {
        SKIP("No BIOS files found in " << FixturesDir().string());
    }

    const auto& biosPath = biosFiles.front();
    INFO("BIOS: " << biosPath.filename().string());

    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(core.LoadIPLFromFile(biosPath.string().c_str()));

    const auto stateSize = core.GetStateSize();
    std::vector<uint8_t> state(stateSize);

    for (int cycle = 0; cycle < 10; ++cycle) {
        CAPTURE(cycle);
        for (int i = 0; i < 5; ++i) {
            core.RunFrame();
        }
        REQUIRE(core.SaveState(state.data(), stateSize));
        REQUIRE(core.LoadState(state.data(), stateSize));
    }

    // A state saved at frame N and restored after running further must rewind WRAM.
    REQUIRE(core.SaveState(state.data(), stateSize));
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    // Copy to the heap: WRAMHigh is 1 MiB, too large for the stack.
    const std::vector<uint8_t> wramBefore(saturn->mem.WRAMHigh.begin(), saturn->mem.WRAMHigh.end());
    for (int i = 0; i < 30; ++i) {
        core.RunFrame();
    }
    REQUIRE(core.LoadState(state.data(), stateSize));
    REQUIRE(std::equal(wramBefore.begin(), wramBefore.end(), saturn->mem.WRAMHigh.begin()));
}
