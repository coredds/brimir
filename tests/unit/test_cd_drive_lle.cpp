// CD drive of the low-level CD Block: the seek target of the drive commands the SH-1 sends.

#include "catch_amalgamated.hpp"

#include <ymir/hw/cdblock/cd_drive.hpp>

namespace {

using namespace ymir;
using Operation = cdblock::CDDrive::Operation;

constexpr uint8 kCmdReadSector = 0x06;
constexpr uint8 kCmdSeekSector = 0x09;

struct CDDriveFixture {
    core::Scheduler scheduler;
    media::Disc disc;
    media::fs::Filesystem fs;
    core::Configuration::CDBlock config;
    std::unique_ptr<cdblock::CDDrive> drive;
    savestate::CDDriveSaveState state{};

    // Track 1 data (150-599), track 2 audio (pregap 600-749, INDEX 01 from 750), track 3 audio with three indices
    // (pregap 1200-1349, INDEX 01 1350-1499, INDEX 02 from 1500).
    CDDriveFixture() {
        auto &session = disc.sessions.emplace_back();
        session.numTracks = 3;
        session.firstTrackIndex = 0;
        session.lastTrackIndex = 2;
        session.startFrameAddress = 150;
        session.endFrameAddress = 9999;
        const uint32 starts[] = {150, 600, 1200};
        const uint32 ends[] = {599, 1199, 9999};
        for (uint32 i = 0; i < 3; ++i) {
            auto &track = session.tracks[i];
            track.startFrameAddress = starts[i];
            track.endFrameAddress = ends[i];
            track.controlADR = i == 0 ? 0x41 : 0x01;
            track.SetSectorSize(2352);
        }
        session.tracks[0].index01FrameAddress = 150;
        session.tracks[0].indices = {{150, 150}, {150, 599}};
        session.tracks[1].index01FrameAddress = 750;
        session.tracks[1].indices = {{600, 749}, {750, 1199}};
        session.tracks[2].index01FrameAddress = 1350;
        session.tracks[2].indices = {{1200, 1349}, {1350, 1499}, {1500, 9999}};

        config.useLLE = true; // the drive's events run only in low-level mode
        drive = std::make_unique<cdblock::CDDrive>(scheduler, disc, fs, config);
    }

    // Has the drive process the given 13-byte command (as received from the SH-1) and returns its state after it.
    const savestate::CDDriveSaveState &Command(uint8 command, uint32 fad, uint8 index) {
        drive->SaveState(state);
        state.commandData.fill(0);
        state.commandData[0] = command;
        state.commandData[1] = static_cast<uint8>(fad >> 16);
        state.commandData[2] = static_cast<uint8>(fad >> 8);
        state.commandData[3] = static_cast<uint8>(fad);
        state.commandData[4] = index;
        state.commandData[10] = 1; // 1x
        state.state = savestate::CDDriveSaveState::TxState::TxEnd;
        drive->LoadState(state);
        // Run to the drive event (the only one scheduled): TxEnd processes the command
        const sint64 remaining = scheduler.RemainingCount();
        scheduler.Advance(remaining > 0 ? remaining : 1);
        drive->SaveState(state);
        return state;
    }
};

} // namespace

// Ymir 0a9a0e2: a seek with index 0 means "seek to this FAD". The CD Block resumes a paused audio track that way, and
// the drive restarted the track from its pregap (INDEX 00).
TEST_CASE("LLE drive resumes a paused audio track at the commanded FAD", "[cdblock-lle][backport]") {
    CDDriveFixture cd;
    const auto &s = cd.Command(kCmdReadSector, 900, 0);
    CHECK(s.targetFAD == 900 - 4);
    CHECK(s.currFAD == 900 - 4);
    CHECK(static_cast<Operation>(s.status.operation) == Operation::Seek);
    CHECK(static_cast<Operation>(s.seekOp) == Operation::ReadAudioSector);

    // In the pregap too
    CHECK(cd.Command(kCmdReadSector, 650, 0).targetFAD == 650 - 4);
}

TEST_CASE("LLE drive seeks an audio track index to the start of that index", "[cdblock-lle][backport]") {
    CDDriveFixture cd;
    CHECK(cd.Command(kCmdReadSector, 900, 1).targetFAD == 750 - 4);
    CHECK(cd.Command(kCmdReadSector, 1400, 2).targetFAD == 1500 - 4);
    CHECK(static_cast<Operation>(cd.state.seekOp) == Operation::ReadAudioSector);
    // From another index of the same track
    CHECK(cd.Command(kCmdReadSector, 1600, 1).targetFAD == 1350 - 4);
}

TEST_CASE("LLE drive seeks to the commanded FAD for an index the track does not have", "[cdblock-lle][brimir]") {
    CDDriveFixture cd;
    // Track 2 has indices 00 and 01: index 2 used to read one past the end of the index list
    CHECK(cd.Command(kCmdReadSector, 900, 2).targetFAD == 900 - 4);
    CHECK(cd.Command(kCmdReadSector, 1400, 99).targetFAD == 1400 - 4);
    CHECK(static_cast<Operation>(cd.state.seekOp) == Operation::ReadAudioSector);
}

TEST_CASE("LLE drive seeks data tracks and pauses by FAD", "[cdblock-lle][backport]") {
    CDDriveFixture cd;
    const auto &data = cd.Command(kCmdReadSector, 300, 1);
    CHECK(data.targetFAD == 300 - 4);
    CHECK(static_cast<Operation>(data.seekOp) == Operation::ReadDataSector);

    // Seek Sector (seek then pause) on an audio track goes to the FAD and idles, whatever the index
    const auto &pause = cd.Command(kCmdSeekSector, 900, 1);
    CHECK(pause.targetFAD == 900 - 4);
    CHECK(static_cast<Operation>(pause.seekOp) == Operation::Idle);
}
