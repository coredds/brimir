// Brimir CD Operations Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0
//
// Tests for Saturn CD tray state handling via GetSaturn().
// (EjectDisc() is a no-op without a loaded disc, so it is not covered here.)

#include "catch_amalgamated.hpp"
#include <brimir/core_wrapper.hpp>

using namespace brimir;

TEST_CASE("CD Operations - Tray State", "[cd][disc]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);

    SECTION("Tray can be opened") {
        saturn->OpenTray();
        REQUIRE(saturn->IsTrayOpen());
    }

    SECTION("Tray can be closed after opening") {
        saturn->OpenTray();
        REQUIRE(saturn->IsTrayOpen());

        saturn->CloseTray();
        REQUIRE_FALSE(saturn->IsTrayOpen());
    }

    SECTION("Tray operations are idempotent") {
        saturn->OpenTray();
        saturn->OpenTray();
        REQUIRE(saturn->IsTrayOpen());

        saturn->CloseTray();
        saturn->CloseTray();
        REQUIRE_FALSE(saturn->IsTrayOpen());
    }
}

TEST_CASE("CD Operations - Tray state is restored by save states", "[cd][savestate]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);

    saturn->CloseTray();
    REQUIRE_FALSE(saturn->IsTrayOpen());

    auto stateSize = core.GetStateSize();
    std::vector<uint8_t> state(stateSize);
    REQUIRE(core.SaveState(state.data(), stateSize));

    saturn->OpenTray();
    REQUIRE(saturn->IsTrayOpen());

    REQUIRE(core.LoadState(state.data(), stateSize));
    REQUIRE_FALSE(saturn->IsTrayOpen());
}
