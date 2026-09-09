#include "catch_amalgamated.hpp"

#include <ymir/hw/cdblock/cdblock.hpp>

#include <memory>

namespace {

using namespace ymir;
using namespace ymir::cdblock;

struct CDBlockFixture {
    core::Scheduler scheduler;
    media::Disc disc;
    media::fs::Filesystem fs;
    core::Configuration::CDBlock config;
    sys::SH2Bus bus;
    CDBlock block{scheduler, disc, fs, config};
    savestate::CDBlockSaveState state{};

    explicit CDBlockFixture(uint32 firstTrackIndex = 0) {
        // A preceding session ensures commands keep using the last session's track table.
        disc.sessions.resize(2);
        auto &session = disc.sessions.back();
        session.numTracks = 3;
        session.firstTrackIndex = firstTrackIndex;
        session.lastTrackIndex = firstTrackIndex + 2;
        session.startFrameAddress = 150;
        session.endFrameAddress = 9999;
        const uint32 starts[] = {150, 600, 1200};
        const uint32 index01[] = {225, 750, 1200};
        const uint32 ends[] = {599, 1199, 9999};
        for (uint32 i = 0; i < 3; ++i) {
            auto &track = session.tracks[firstTrackIndex + i];
            track.startFrameAddress = starts[i];
            track.index01FrameAddress = index01[i];
            track.endFrameAddress = ends[i];
            track.controlADR = i == 1 ? 0x01 : 0x41;
            track.SetSectorSize(2352);
            track.indices = {{starts[i], index01[i] - 1}, {index01[i], ends[i]}};
        }
        block.MapMemory(bus);
        // Only command events run: these tests do not read sectors or emulate seek delays.
        scheduler.Reset();
        block.SaveState(state);
        state.readSpeed = 2;
        block.LoadState(state);
    }

    std::array<uint16, 4> Command(uint16 cr1, uint16 cr2 = 0, uint16 cr3 = 0, uint16 cr4 = 0) {
        bus.Write<uint16>(0x5800008, 0);
        bus.Write<uint16>(0x5800018, cr1);
        bus.Write<uint16>(0x580001C, cr2);
        bus.Write<uint16>(0x5800020, cr3);
        bus.Write<uint16>(0x5800024, cr4);
        scheduler.Advance(50);
        REQUIRE((bus.Read<uint16>(0x5800008) & kHIRQ_CMOK) != 0);
        std::array<uint16, 4> response{};
        for (uint32 i = 0; i < response.size(); ++i) {
            response[i] = bus.Read<uint16>(0x5800018 + i * 4);
        }
        block.SaveState(state);
        return response;
    }

    std::array<uint16, 4> Seek(uint32 fad) {
        return Command(static_cast<uint16>(0x1180 | (fad >> 16)), static_cast<uint16>(fad));
    }

    void Position(uint32 fad) {
        const auto &session = disc.sessions.back();
        const auto *track = session.FindTrack(fad);
        REQUIRE(track != nullptr);
        state.status = {kStatusCodePause, fad, 0, 0, track->controlADR,
                        static_cast<uint8>(track->index), track->FindIndex(fad)};
        block.LoadState(state);
    }

    void Play(uint32 start, uint32 length, uint8 mode) {
        Command(static_cast<uint16>(0x1080 | (start >> 16)), static_cast<uint16>(start),
                static_cast<uint16>((mode << 8) | 0x80 | (length >> 16)), static_cast<uint16>(length));
    }
};

} // namespace

TEST_CASE("HLE FAD seek reports one-based tracks and preserves clamps", "[cdblock-hle][backport]") {
    const auto first = GENERATE(0u, 8u);
    auto cd = std::make_unique<CDBlockFixture>(first);
    for (const auto fad : {149u, 225u, 750u, 1250u, 10000u, 11000u}) {
        CAPTURE(first, fad);
        const auto response = cd->Seek(fad);
        const uint32 expectedFAD = std::clamp(fad, 150u, 10000u);
        const uint32 expectedTrack = fad >= 10000 ? 0xAA : first + (fad < 600 ? 1 : fad < 1200 ? 2 : 3);
        CHECK((response[1] & 0xFFu) == expectedTrack);
        CHECK(cd->state.status.track == expectedTrack);
        CHECK(cd->state.status.frameAddress == expectedFAD);
        CHECK(cd->state.status.statusCode == kStatusCodePause);
        CHECK(cd->state.status.index == 1);
    }
}

TEST_CASE("HLE FAD playback resets before resolving track metadata", "[cdblock-hle][backport]") {
    struct Case { uint32 current, start; uint8 track, index, controlADR; uint32 cycles; };
    const Case cases[] = {
        {800, 225, 1, 1, 0x41, kDriveCyclesPlaying1x / 2},
        {800, 600, 2, 0, 0x01, kDriveCyclesPlaying1x},
        {1250, 600, 2, 0, 0x01, kDriveCyclesPlaying1x},
        {10000, 225, 1, 1, 0x41, kDriveCyclesPlaying1x / 2},
    };
    for (const auto &c : cases) {
        CAPTURE(c.current, c.start);
        auto cd = std::make_unique<CDBlockFixture>();
        cd->Seek(c.current);
        cd->Play(c.start, 10000 - c.start, 0);
        CHECK(cd->state.status.statusCode == kStatusCodeSeek);
        CHECK(cd->state.status.frameAddress == c.start);
        CHECK(cd->state.status.track == c.track);
        CHECK(cd->state.status.index == c.index);
        CHECK(cd->state.status.controlADR == c.controlADR);
        CHECK(cd->state.targetDriveCycles == c.cycles);
        CHECK(cd->state.playEndPos == 9999);
    }
}

TEST_CASE("HLE FAD playback preserves continue and range handling", "[cdblock-hle][backport]") {
    const auto mode = GENERATE(0x80, 0xFF);
    // end + 1 remains in range, including when it is the disc leadout boundary.
    struct Case { uint32 current, length, expected; uint8 status; };
    const Case cases[] = {
        {800, 775, 800, kStatusCodeSeek},
        {150, 775, 225, kStatusCodeSeek},
        {1001, 775, 225, kStatusCodeSeek},
        {1000, 775, 1000, kStatusCodeSeek},
        {10000, 9775, 10000, kStatusCodePause},
    };
    for (const auto &c : cases) {
        CAPTURE(mode, c.current, c.length);
        auto cd = std::make_unique<CDBlockFixture>();
        cd->Seek(c.current);
        cd->Play(225, c.length, static_cast<uint8>(mode));
        CHECK(cd->state.status.frameAddress == c.expected);
        CHECK(cd->state.status.statusCode == c.status);
        CHECK(cd->state.playEndPos == 225 + c.length - 1);
        if (c.status == kStatusCodeSeek) {
            const auto *track = cd->disc.sessions.back().FindTrack(c.expected);
            REQUIRE(track != nullptr);
            CHECK(cd->state.status.track == track->index);
            CHECK(cd->state.status.index == track->FindIndex(c.expected));
            CHECK(cd->state.status.controlADR == track->controlADR);
            CHECK(cd->state.targetDriveCycles == kDriveCyclesPlaying1x / (track->controlADR == 0x41 ? 2 : 1));
        }
    }
}

TEST_CASE("HLE subcode Q counts down to INDEX 01 then counts up in BCD", "[cdblock-hle][backport]") {
    const auto first = GENERATE(0u, 8u);
    struct Case { uint32 fad; uint16 minute, second, frame; };
    const Case cases[] = {
        {150, 0, 0x01, 0}, {224, 0, 0, 0x01}, {225, 0, 0, 0}, {226, 0, 0, 0x01},
        {600, 0, 0x02, 0}, {675, 0, 0x01, 0}, {749, 0, 0, 0x01}, {750, 0, 0, 0},
        {751, 0, 0, 0x01}, {824, 0, 0, 0x74}, {825, 0, 0x01, 0},
        {1200, 0, 0, 0}, {1949, 0, 0x09, 0x74}, {1950, 0, 0x10, 0},
        {5699, 0, 0x59, 0x74}, {5700, 0x01, 0, 0},
    };
    auto cd = std::make_unique<CDBlockFixture>(first);
    for (const auto &c : cases) {
        CAPTURE(first, c.fad);
        cd->Position(c.fad);
        const auto response = cd->Command(0x2000);
        REQUIRE(response[1] == 5);
        CHECK((cd->state.HIRQ & kHIRQ_DRDY) != 0);
        // Inspect the synthesized bytes without changing the existing transfer/save layout.
        const auto &q = cd->state.xferBuffer;
        const uint16 track = first == 0 ? (c.fad < 600 ? 0x01 : c.fad < 1200 ? 0x02 : 0x03)
                                       : (c.fad < 600 ? 0x09 : c.fad < 1200 ? 0x10 : 0x11);
        CHECK(q[0] == cd->state.status.controlADR);
        CHECK(q[1] == track);
        CHECK(q[2] == cd->state.status.index);
        CHECK(q[3] == c.minute);
        CHECK(q[4] == c.second);
        CHECK(q[5] == c.frame);
        CHECK(q[6] == 0);
        const auto bcd = [](uint32 value) { return (value / 10) * 16 + value % 10; };
        CHECK(q[7] == bcd(c.fad / 4500));
        CHECK(q[8] == bcd(c.fad / 75 % 60));
        CHECK(q[9] == bcd(c.fad % 75));
    }
}
