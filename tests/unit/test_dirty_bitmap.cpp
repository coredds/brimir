#include "catch_amalgamated.hpp"

#include <ymir/util/dirty_bitmap.hpp>

// Ymir 7c2833fa (SetAll sets anySet for sizes that are a multiple of 64), 6a6504d2 (FindNextGroup).

TEST_CASE("DirtyBitmap SetAll marks a multiple-of-64 bitmap as set", "[util][dirty-bitmap]") {
    util::DirtyBitmap<128> bitmap{};
    CHECK_FALSE(bitmap.AnySet());
    bitmap.SetAll();
    CHECK(bitmap.AnySet());
    CHECK(bitmap.Get(0));
    CHECK(bitmap.Get(127));
}

TEST_CASE("DirtyBitmap SetAll stops at the last bit of a partial entry", "[util][dirty-bitmap]") {
    util::DirtyBitmap<100> bitmap{};
    bitmap.SetAll();
    CHECK(bitmap.Get(99));
    CHECK_FALSE(bitmap.Get(100));

    size_t count = 0;
    CHECK(bitmap.FindNext(count, 0) == 0);
    CHECK(count == 100);
}

TEST_CASE("DirtyBitmap FindNext returns runs of set bits", "[util][dirty-bitmap]") {
    util::DirtyBitmap<100> bitmap{};
    bitmap.Set(3);
    bitmap.Set(4);
    bitmap.Set(70);

    size_t count = 0;
    CHECK(bitmap.FindNext(count, 0) == 3);
    CHECK(count == 2);
    CHECK(bitmap.FindNext(count, 5) == 70);
    CHECK(count == 1);
    CHECK(bitmap.FindNext(count, 71) == 100);
}

TEST_CASE("DirtyBitmap FindNext handles a run crossing an entry boundary", "[util][dirty-bitmap]") {
    util::DirtyBitmap<100> bitmap{};
    for (size_t i = 60; i < 68; i++) {
        bitmap.Set(i);
    }

    size_t count = 0;
    CHECK(bitmap.FindNext(count, 0) == 60);
    CHECK(count == 8);
}

TEST_CASE("DirtyBitmap FindNext walks a fully set entry", "[util][dirty-bitmap]") {
    // A whole 64-bit entry of ones makes countr_one return 64; the shift by that amount must not be undefined.
    util::DirtyBitmap<64> bitmap{};
    bitmap.SetAll();

    size_t count = 0;
    CHECK(bitmap.FindNext(count, 0) == 0);
    CHECK(count == 64);
    CHECK(bitmap.FindNext(count, 1) == 1);
    CHECK(count == 63);
    CHECK(bitmap.FindNext(count, 64) == 64);

    util::DirtyBitmap<192> wide{};
    wide.SetAll();
    CHECK(wide.FindNext(count, 0) == 0);
    CHECK(count == 192);
}

TEST_CASE("DirtyBitmap FindNextGroup merges groups with any bit set", "[util][dirty-bitmap]") {
    util::DirtyBitmap<100> bitmap{};
    bitmap.Set(9);
    bitmap.Set(17);

    size_t count = 0;
    CHECK(bitmap.FindNextGroup<8>(count, 0) == 8);
    CHECK(count == 16);
}

TEST_CASE("DirtyBitmap ClearAll resets every bit", "[util][dirty-bitmap]") {
    util::DirtyBitmap<100> bitmap{};
    bitmap.SetAll();
    REQUIRE(bitmap.AnySet());
    bitmap.ClearAll();
    CHECK_FALSE(bitmap.AnySet());
    CHECK_FALSE(bitmap.Get(0));
}
