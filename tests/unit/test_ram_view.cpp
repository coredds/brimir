#include "catch_amalgamated.hpp"

#include <brimir/ram_view.hpp>

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

using brimir::RamView;

namespace {

struct WorkRam {
    std::vector<uint8_t> low = std::vector<uint8_t>(RamView::kHalfSize);
    std::vector<uint8_t> high = std::vector<uint8_t>(RamView::kHalfSize);

    std::span<uint8_t, RamView::kHalfSize> Low() { return std::span<uint8_t, RamView::kHalfSize>(low.data(), low.size()); }
    std::span<uint8_t, RamView::kHalfSize> High() {
        return std::span<uint8_t, RamView::kHalfSize>(high.data(), high.size());
    }

    void Fill(uint8_t seed) {
        for (size_t i = 0; i < RamView::kHalfSize; ++i) {
            low[i] = static_cast<uint8_t>(seed + i * 7);
            high[i] = static_cast<uint8_t>(seed + i * 13 + 1);
        }
    }
};

void Publish(RamView &view, WorkRam &ram) {
    view.Publish(ram.Low(), ram.High());
}

void Apply(RamView &view, WorkRam &ram) {
    view.ApplyFrontendWrites(ram.Low(), ram.High());
}

// Byte i of a Saturn-order half in the view, as Beetle Saturn stores it (host-endian 16-bit words).
size_t BeetleIndex(size_t i) {
    return std::endian::native == std::endian::little ? (i ^ 1) : i;
}

} // namespace

TEST_CASE("ram view: layout matches Beetle", "[ram_view]") {
    REQUIRE(RamView::kSize == 2 * 1024 * 1024);
    RamView view(GENERATE(RamView::Tracking::Auto, RamView::Tracking::FullScan));
    WorkRam ram;
    ram.low[0] = 0x12;
    ram.low[1] = 0x34;
    ram.low[2] = 0x56;
    ram.low[3] = 0x78;
    ram.high[0] = 0x9A;
    ram.high[1] = 0xBC;
    ram.high[RamView::kHalfSize - 1] = 0xEF;
    Publish(view, ram);

    const uint8_t *data = view.Data();
    if constexpr (std::endian::native == std::endian::little) {
        CHECK(data[0] == 0x34);
        CHECK(data[1] == 0x12);
        CHECK(data[2] == 0x78);
        CHECK(data[3] == 0x56);
        CHECK(data[0x100000 + 1] == 0x9A);
        CHECK(data[0x100000 + 0] == 0xBC);
        CHECK(data[RamView::kSize - 2] == 0xEF);
    }
    // A 16-bit read of the view in host order is the Saturn word (Beetle's uint16_t WorkRAM access).
    uint16_t word = 0;
    std::memcpy(&word, data + 0x100000, sizeof(word));
    CHECK(word == 0x9ABC);

    ram.Fill(5);
    Publish(view, ram);
    for (size_t i = 0; i < RamView::kHalfSize; i += 4097) {
        CHECK(data[BeetleIndex(i)] == ram.low[i]);
        CHECK(data[0x100000 + BeetleIndex(i)] == ram.high[i]);
    }
}

TEST_CASE("ram view: frontend write reaches RAM", "[ram_view]") {
    RamView view(GENERATE(RamView::Tracking::Auto, RamView::Tracking::FullScan));
    WorkRam ram;
    ram.Fill(3);
    Publish(view, ram);
    const WorkRam before = ram;

    view.Data()[0x100001] = 0xAB;
    Apply(view, ram);

    CHECK(ram.high[BeetleIndex(1)] == 0xAB);
    if constexpr (std::endian::native == std::endian::little) {
        CHECK(ram.high[0] == 0xAB);
    }
    ram.high[BeetleIndex(1)] = before.high[BeetleIndex(1)];
    CHECK(ram.low == before.low);
    CHECK(ram.high == before.high);

    // A write is applied once: the emulator may change the byte afterwards without the view overwriting it.
    ram.high[BeetleIndex(1)] = 0x11;
    Apply(view, ram);
    CHECK(ram.high[BeetleIndex(1)] == 0x11);

    // Writes at both ends of both halves.
    Publish(view, ram);
    view.Data()[0] = 0x01;
    view.Data()[RamView::kHalfSize - 1] = 0x02;
    view.Data()[RamView::kHalfSize] = 0x03;
    view.Data()[RamView::kSize - 1] = 0x04;
    Apply(view, ram);
    CHECK(ram.low[BeetleIndex(0)] == 0x01);
    CHECK(ram.low[BeetleIndex(RamView::kHalfSize - 1)] == 0x02);
    CHECK(ram.high[BeetleIndex(0)] == 0x03);
    CHECK(ram.high[BeetleIndex(RamView::kHalfSize - 1)] == 0x04);
}

TEST_CASE("ram view: state load publishes and does not write back", "[ram_view]") {
    RamView view(GENERATE(RamView::Tracking::Auto, RamView::Tracking::FullScan));
    WorkRam ram;
    ram.Fill(1); // A
    Publish(view, ram);

    view.Data()[0x10] = 0xEE; // frontend write X against A
    view.Data()[0x100020] = 0xDD;

    ram.Fill(9); // a state load replaces RAM with B, then publishes it
    const WorkRam b = ram;
    Publish(view, ram);
    Apply(view, ram);

    CHECK(ram.low == b.low);
    CHECK(ram.high == b.high);
    CHECK(view.Data()[0x10] == b.low[BeetleIndex(0x10)]);

    // A state whose bytes around the frontend write equal the RAM before (only far-away bytes differ): the write is
    // still dropped.
    Publish(view, ram);
    const WorkRam before = ram;
    view.Data()[0x40] ^= 0x5A;
    view.Data()[0x100080] ^= 0xA5;
    ram.low[0x80000] ^= 1;
    ram.high[0x90000] ^= 1;
    const WorkRam loaded = ram;
    Publish(view, ram);
    Apply(view, ram);
    CHECK(ram.low == loaded.low);
    CHECK(ram.high == loaded.high);
    CHECK(view.Data()[0x40] == before.low[BeetleIndex(0x40)]);
    CHECK(view.Data()[0x100080] == before.high[BeetleIndex(0x80)]);

    // The same state again (nothing differs at all): still dropped.
    view.Data()[0x41] ^= 0x33;
    Publish(view, ram);
    Apply(view, ram);
    CHECK(ram.low == loaded.low);
    CHECK(view.Data()[0x41] == loaded.low[BeetleIndex(0x41)]);
}

TEST_CASE("ram view: no frontend write, no change", "[ram_view]") {
    RamView view(GENERATE(RamView::Tracking::Auto, RamView::Tracking::FullScan));
    WorkRam ram;
    ram.Fill(7);
    Publish(view, ram);

    // The emulator changes RAM during the frame; without a frontend write the view writes nothing back.
    ram.low[100] ^= 0xFF;
    ram.high[200] ^= 0xFF;
    const WorkRam expected = ram;
    Apply(view, ram);
    CHECK(ram.low == expected.low);
    CHECK(ram.high == expected.high);
}

TEST_CASE("ram view: frontend writes across pages and repeated frames", "[ram_view]") {
    RamView view(GENERATE(RamView::Tracking::Auto, RamView::Tracking::FullScan));
#ifdef _WIN32
    CHECK(RamView().WriteWatched()); // the libretro core's view on Windows
#endif
    WorkRam ram;
    ram.Fill(6);
    Publish(view, ram);

    // Writes in many pages of both halves, as a frontend's cheats do every frame.
    for (int frame = 0; frame < 3; ++frame) {
        for (size_t offset = 0x0FFF; offset < RamView::kSize; offset += 0x11000) {
            view.Data()[offset] = static_cast<uint8_t>(frame * 31 + offset);
        }
        Apply(view, ram);
        for (size_t offset = 0x0FFF; offset < RamView::kSize; offset += 0x11000) {
            const uint8_t expected = static_cast<uint8_t>(frame * 31 + offset);
            if (offset < RamView::kHalfSize) {
                CHECK(ram.low[BeetleIndex(offset)] == expected);
            } else {
                CHECK(ram.high[BeetleIndex(offset - RamView::kHalfSize)] == expected);
            }
        }
        ram.high[0x1234] ^= 0xFF; // the frame
        Publish(view, ram);
        CHECK(view.Data()[RamView::kHighOffset + BeetleIndex(0x1234)] == ram.high[0x1234]);
    }

    // A frontend write between Apply and Publish (none should happen; RetroArch writes between retro_run calls) is
    // dropped by the Publish, never applied later.
    view.Data()[0x2000] ^= 0xFF;
    Publish(view, ram);
    const WorkRam before = ram;
    Apply(view, ram);
    CHECK(ram.low == before.low);
    CHECK(ram.high == before.high);
}

TEST_CASE("ram view: clear zeroes the view", "[ram_view]") {
    RamView view(GENERATE(RamView::Tracking::Auto, RamView::Tracking::FullScan));
    WorkRam ram;
    ram.Fill(2);
    Publish(view, ram);
    uint8_t *const data = view.Data();
    view.Clear();
    CHECK(view.Data() == data);
    for (size_t i = 0; i < RamView::kSize; i += 999) {
        CHECK(data[i] == 0);
    }
}

TEST_CASE("ram view: per-frame cost", "[ram_view][.bench]") {
    RamView view(GENERATE(RamView::Tracking::Auto, RamView::Tracking::FullScan));
    WorkRam ram;
    ram.Fill(4);
    constexpr int kFrames = 2000;
    std::chrono::nanoseconds apply{0};
    std::chrono::nanoseconds publish{0};
    for (int f = 0; f < kFrames; ++f) {
        const auto t0 = std::chrono::steady_clock::now();
        Apply(view, ram);
        const auto t1 = std::chrono::steady_clock::now();
        ram.low[static_cast<size_t>(f) * 17 % RamView::kHalfSize] ^= 1; // the frame changes RAM
        const auto t2 = std::chrono::steady_clock::now();
        Publish(view, ram);
        const auto t3 = std::chrono::steady_clock::now();
        apply += t1 - t0;
        publish += t3 - t2;
    }
    std::printf("ram view (hot cache, %s): ApplyFrontendWrites %.4f ms, Publish %.4f ms per frame (%d frames)\n",
                view.WriteWatched() ? "write watch" : "full scan", static_cast<double>(apply.count()) / kFrames / 1e6,
                static_cast<double>(publish.count()) / kFrames / 1e6, kFrames);
    SUCCEED();
}
