#include "catch_amalgamated.hpp"

#include <ymir/db/game_db.hpp>

TEST_CASE("Dragon Force II requires SH-2 cache emulation", "[game-db][backport]") {
    const auto *info = ymir::db::GetGameInfo("GS-9184", {});
    REQUIRE(info != nullptr);
    CHECK(info->flags == ymir::db::GameInfo::Flags::ForceSH2Cache);
    CHECK(info->GetCartridge() == ymir::db::Cartridge::None);
}

TEST_CASE("Game database preserves existing lookups", "[game-db][backport]") {
    using enum ymir::db::GameInfo::Flags;
    const auto *info = ymir::db::GetGameInfo("T-1515G", {});
    REQUIRE(info != nullptr);
    CHECK(info->flags == (Cart_DRAM8Mbit | ForceSH2Cache));

    info = ymir::db::GetGameInfo("MK-81304", {});
    REQUIRE(info != nullptr);
    CHECK(info->flags == (ForceSH2Cache | RelaxedVDP2BitmapCPAccessChecks));

    const auto hash = ymir::MakeXXH128Hash(0xCFA7E24F43C986F7, 0x051DAF831876C5FD);
    info = ymir::db::GetGameInfo("unknown", hash);
    REQUIRE(info != nullptr);
    CHECK(info->flags == Cart_DRAM48Mbit);
    info = ymir::db::GetGameInfo("T-4503G", hash);
    REQUIRE(info != nullptr);
    CHECK(info->flags == ForceSH2Cache);
    CHECK(ymir::db::GetGameInfo("unknown", {}) == nullptr);
}
