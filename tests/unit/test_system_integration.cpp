// Brimir System Integration Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0
//
// Tests for Saturn system configuration persistence across resets and
// save state round trips.

#include "catch_amalgamated.hpp"
#include <brimir/core_wrapper.hpp>
#include <ymir/core/configuration_defs.hpp>

using namespace brimir;
using VideoStandard = ymir::core::config::sys::VideoStandard;

TEST_CASE("System Integration - Configuration Combinations", "[system][config]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);

    std::vector<uint8_t> biosData(512 * 1024, 0xFF);
    REQUIRE(core.LoadIPL(std::span<const uint8_t>(biosData)));

    const auto standard = GENERATE(VideoStandard::NTSC, VideoStandard::PAL);
    const bool cache = GENERATE(true, false);

    saturn->SetVideoStandard(standard);
    saturn->EnableSH2CacheEmulation(cache);
    core.RunFrame();

    REQUIRE(saturn->GetVideoStandard() == standard);
    REQUIRE(saturn->IsSH2CacheEmulationEnabled() == cache);
    REQUIRE(core.GetFramebuffer() != nullptr);
    REQUIRE(core.GetFramebufferWidth() > 0);
    REQUIRE(core.GetFramebufferHeight() > 0);
}

TEST_CASE("System Integration - Configuration persists through soft reset", "[system][reset]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);

    std::vector<uint8_t> biosData(512 * 1024, 0xAA);
    REQUIRE(core.LoadIPL(std::span<const uint8_t>(biosData)));

    saturn->SetVideoStandard(VideoStandard::PAL);
    saturn->EnableSH2CacheEmulation(true);
    saturn->SMPC.SetAreaCode(0xC);  // Europe
    saturn->mem.WRAMLow[100] = 0x42;

    saturn->Reset(false);

    REQUIRE(saturn->GetVideoStandard() == VideoStandard::PAL);
    REQUIRE(saturn->IsSH2CacheEmulationEnabled());
    REQUIRE(saturn->SMPC.GetAreaCode() == 0xC);
    REQUIRE(saturn->mem.WRAMLow[100] == 0x42);
    REQUIRE(saturn->mem.IPL[0] == 0xAA);
}

TEST_CASE("System Integration - Save State Round Trip", "[system][savestate]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);

    // Uses the built-in null IPL program (no BIOS loaded). Note that user
    // configuration (video standard, SH-2 cache emulation, SMPC area code) is
    // intentionally not part of save states, so only emulated state is checked.
    for (int i = 0; i < 10; ++i) {
        core.RunFrame();
    }

    // Write memory patterns after running so the IPL program can't overwrite them
    for (size_t i = 0; i < 1000; ++i) {
        saturn->mem.WRAMLow[i] = static_cast<uint8_t>(i & 0xFF);
        saturn->mem.WRAMHigh[i] = static_cast<uint8_t>((i * 2) & 0xFF);
    }

    auto stateSize = core.GetStateSize();
    std::vector<uint8_t> state(stateSize);
    REQUIRE(core.SaveState(state.data(), stateSize));

    // Clobber state
    for (size_t i = 0; i < 1000; ++i) {
        saturn->mem.WRAMLow[i] = 0;
        saturn->mem.WRAMHigh[i] = 0;
    }

    REQUIRE(core.LoadState(state.data(), stateSize));

    REQUIRE(saturn->mem.WRAMLow[0] == 0);
    REQUIRE(saturn->mem.WRAMLow[100] == 100);
    REQUIRE(saturn->mem.WRAMLow[255] == 255);
    REQUIRE(saturn->mem.WRAMHigh[100] == 200);
}

TEST_CASE("System Integration - Multiple Save/Load Cycles", "[system][savestate]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    // Uses the built-in null IPL program (no BIOS loaded).
    auto stateSize = core.GetStateSize();
    std::vector<uint8_t> state(stateSize);

    for (int cycle = 0; cycle < 10; ++cycle) {
        CAPTURE(cycle);
        for (int i = 0; i < 5; ++i) {
            core.RunFrame();
        }
        REQUIRE(core.SaveState(state.data(), stateSize));
        REQUIRE(core.LoadState(state.data(), stateSize));
    }

    REQUIRE(core.IsInitialized());
    core.RunFrame();
    REQUIRE(core.GetFramebuffer() != nullptr);
}
