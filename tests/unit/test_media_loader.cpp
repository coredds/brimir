#include <catch_amalgamated.hpp>

#include <ymir/media/disc.hpp>
#include <ymir/media/loader/loader.hpp>
#include <ymir/util/data_ops.hpp>
#include <ymir/util/scope_guard.hpp>

#include <dr_libs/dr_mp3.h>
#define STB_VORBIS_HEADER_ONLY
#include <stb/stb_vorbis.c>
#undef STB_VORBIS_HEADER_ONLY

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

// Returns the directory containing the test executable.
std::filesystem::path TestBinaryDir() {
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

// Returns the fixtures/audio directory.
std::filesystem::path AudioFixturesDir() {
    return TestBinaryDir() / "fixtures" / "audio";
}

// Creates a uniquely named temporary directory under the system temp path.
std::filesystem::path MakeTempDir() {
    auto dir = std::filesystem::temp_directory_path() / ("brimir_media_test_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    return dir;
}

// Writes a text file.
void WriteText(const std::filesystem::path &path, std::string_view content) {
    std::ofstream out{path, std::ios::binary};
    out << content;
}

// Writes a minimal PCM WAV file.
// sampleFrames stereo 16-bit samples at 44100 Hz.
void WriteWav(const std::filesystem::path &path, uint32_t sampleFrames) {
    const uint16_t numChannels = 2;
    const uint32_t sampleRate = 44100;
    const uint16_t bitsPerSample = 16;
    const uint32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
    const uint16_t blockAlign = numChannels * bitsPerSample / 8;
    const uint32_t dataSize = sampleFrames * numChannels * bitsPerSample / 8;
    const uint32_t riffSize = 36 + dataSize;

    std::ofstream out{path, std::ios::binary};
    auto writeU32 = [&](uint32_t v) {
        out.put(static_cast<char>(v & 0xFF));
        out.put(static_cast<char>((v >> 8) & 0xFF));
        out.put(static_cast<char>((v >> 16) & 0xFF));
        out.put(static_cast<char>((v >> 24) & 0xFF));
    };
    auto writeU16 = [&](uint16_t v) {
        out.put(static_cast<char>(v & 0xFF));
        out.put(static_cast<char>((v >> 8) & 0xFF));
    };

    out.write("RIFF", 4);
    writeU32(riffSize);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeU32(16);            // Subchunk1Size
    writeU16(1);             // AudioFormat = PCM
    writeU16(numChannels);
    writeU32(sampleRate);
    writeU32(byteRate);
    writeU16(blockAlign);
    writeU16(bitsPerSample);
    out.write("data", 4);
    writeU32(dataSize);
    for (uint32_t i = 0; i < dataSize; ++i) {
        out.put('\0');
    }
}

// A loader message sink that ignores everything.
ymir::media::CbLoaderMessage SilentCb() {
    return [](ymir::media::MessageType, std::string) {};
}

} // namespace

TEST_CASE("CUE loader handles sparse track numbers", "[media][loader][cue]") {
    auto tmp = MakeTempDir();
    const auto track1 = tmp / "track1.wav";
    const auto track3 = tmp / "track3.wav";
    WriteWav(track1, 1000);
    WriteWav(track3, 1000);

    const auto cue = tmp / "sparse.cue";
    WriteText(cue, R"(FILE "track1.wav" WAVE
  TRACK 01 AUDIO
    INDEX 01 00:00:00
FILE "track3.wav" WAVE
  TRACK 03 AUDIO
    INDEX 01 00:00:00
)");

    ymir::media::Disc disc;
    REQUIRE(ymir::media::LoadDisc(cue, disc, false, SilentCb()));
    REQUIRE(!disc.sessions.empty());

    const auto &session = disc.sessions.back();
    REQUIRE(session.firstTrackIndex == 0);
    REQUIRE(session.lastTrackIndex == 2);
    REQUIRE(session.numTracks == 3);

    // The track at slot 2 (track number 3) must be reachable.
    const auto &t3 = session.tracks[2];
    REQUIRE(t3.controlADR != 0);
    REQUIRE(session.FindTrack(t3.startFrameAddress) != nullptr);
}

TEST_CASE("CUE loader pads WAV audio tracks to a multiple of 2352 bytes", "[media][loader][cue]") {
    auto tmp = MakeTempDir();
    const auto wav = tmp / "odd.wav";
    // 589 stereo frames => 2356 bytes of payload, not a multiple of 2352.
    WriteWav(wav, 589);

    const auto cue = tmp / "odd.cue";
    WriteText(cue, R"(FILE "odd.wav" WAVE
  TRACK 01 AUDIO
    INDEX 01 00:00:00
)");

    ymir::media::Disc disc;
    REQUIRE(ymir::media::LoadDisc(cue, disc, false, SilentCb()));
    REQUIRE(!disc.sessions.empty());

    const auto &track = disc.sessions.back().tracks[0];
    REQUIRE(track.binaryReader != nullptr);
    // 589 stereo frames = 2356 bytes of payload, which must be padded up to the next 2352-byte boundary.
    REQUIRE(track.binaryReader->Size() == 4704);
}

TEST_CASE("CCD loader rejects a file starting with a null byte", "[media][loader][ccd]") {
    auto tmp = MakeTempDir();
    const auto ccd = tmp / "empty.ccd";
    {
        std::ofstream out{ccd, std::ios::binary};
        out.put('\0');
    }

    ymir::media::Disc disc;
    REQUIRE_FALSE(ymir::media::LoadDisc(ccd, disc, false, SilentCb()));
}

TEST_CASE("CUE loader can load an MP3 audio track", "[media][loader][cue]") {
    const auto mp3 = AudioFixturesDir() / "silence.mp3";
    REQUIRE(std::filesystem::exists(mp3));
    const auto mp3Size = std::filesystem::file_size(mp3);

    auto tmp = MakeTempDir();
    const auto cue = tmp / "mp3.cue";
    WriteText(cue, R"(FILE ")" + mp3.filename().string() + R"(" MP3
  TRACK 01 AUDIO
    INDEX 01 00:00:00
)");
    std::filesystem::copy(mp3, tmp / mp3.filename());

    ymir::media::Disc disc;
    REQUIRE(ymir::media::LoadDisc(cue, disc, false, SilentCb()));
    REQUIRE(!disc.sessions.empty());

    const auto &track = disc.sessions.back().tracks[0];
    REQUIRE(track.controlADR != 0);
    REQUIRE(track.unitSize == 2352);
    // The file must have been decoded and expanded, not passed through raw.
    REQUIRE(track.binaryReader->Size() > mp3Size);
    REQUIRE(track.binaryReader->Size() % 2352 == 0);
}

TEST_CASE("CUE loader can load an OGG audio track", "[media][loader][cue]") {
    const auto ogg = AudioFixturesDir() / "silence.ogg";
    REQUIRE(std::filesystem::exists(ogg));
    const auto oggSize = std::filesystem::file_size(ogg);

    auto tmp = MakeTempDir();
    const auto cue = tmp / "ogg.cue";
    WriteText(cue, R"(FILE ")" + ogg.filename().string() + R"(" OGG
  TRACK 01 AUDIO
    INDEX 01 00:00:00
)");
    std::filesystem::copy(ogg, tmp / ogg.filename());

    ymir::media::Disc disc;
    REQUIRE(ymir::media::LoadDisc(cue, disc, false, SilentCb()));
    REQUIRE(!disc.sessions.empty());

    const auto &track = disc.sessions.back().tracks[0];
    REQUIRE(track.controlADR != 0);
    REQUIRE(track.unitSize == 2352);
    REQUIRE(track.binaryReader->Size() > oggSize);
    REQUIRE(track.binaryReader->Size() % 2352 == 0);
}

TEST_CASE("CUE loader can load multiple MP3 files", "[media][loader][cue]") {
    const auto mp3 = AudioFixturesDir() / "silence.mp3";
    REQUIRE(std::filesystem::exists(mp3));

    auto tmp = MakeTempDir();
    const auto track1 = tmp / "track1.mp3";
    const auto track2 = tmp / "track2.mp3";
    std::filesystem::copy(mp3, track1);
    std::filesystem::copy(mp3, track2);

    const auto cue = tmp / "multi_mp3.cue";
    WriteText(cue, R"(FILE "track1.mp3" MP3
  TRACK 01 AUDIO
    INDEX 01 00:00:00
FILE "track2.mp3" MP3
  TRACK 02 AUDIO
    INDEX 01 00:00:00
)");

    ymir::media::Disc disc;
    REQUIRE(ymir::media::LoadDisc(cue, disc, false, SilentCb()));
    REQUIRE(!disc.sessions.empty());

    const auto &session = disc.sessions.back();
    REQUIRE(session.numTracks == 2);

    for (size_t i = 0; i < 2; ++i) {
        const auto &track = session.tracks[i];
        REQUIRE(track.controlADR != 0);
        REQUIRE(track.unitSize == 2352);
        REQUIRE(track.binaryReader->Size() > std::filesystem::file_size(mp3));
        REQUIRE(track.binaryReader->Size() % 2352 == 0);
    }
}

TEST_CASE("CUE loader can load multiple OGG files", "[media][loader][cue]") {
    const auto ogg = AudioFixturesDir() / "silence.ogg";
    REQUIRE(std::filesystem::exists(ogg));

    auto tmp = MakeTempDir();
    const auto track1 = tmp / "track1.ogg";
    const auto track2 = tmp / "track2.ogg";
    std::filesystem::copy(ogg, track1);
    std::filesystem::copy(ogg, track2);

    const auto cue = tmp / "multi_ogg.cue";
    WriteText(cue, R"(FILE "track1.ogg" OGG
  TRACK 01 AUDIO
    INDEX 01 00:00:00
FILE "track2.ogg" OGG
  TRACK 02 AUDIO
    INDEX 01 00:00:00
)");

    ymir::media::Disc disc;
    REQUIRE(ymir::media::LoadDisc(cue, disc, false, SilentCb()));
    REQUIRE(!disc.sessions.empty());

    const auto &session = disc.sessions.back();
    REQUIRE(session.numTracks == 2);

    for (size_t i = 0; i < 2; ++i) {
        const auto &track = session.tracks[i];
        REQUIRE(track.controlADR != 0);
        REQUIRE(track.unitSize == 2352);
        REQUIRE(track.binaryReader->Size() > std::filesystem::file_size(ogg));
        REQUIRE(track.binaryReader->Size() % 2352 == 0);
    }
}

TEST_CASE("CUE loader rejects an invalid WAVE file", "[media][loader][cue]") {
    auto tmp = MakeTempDir();
    const auto wav = tmp / "invalid.wav";
    WriteText(wav, "not a wave file");

    const auto cue = tmp / "invalid.cue";
    WriteText(cue, R"(FILE "invalid.wav" WAVE
  TRACK 01 AUDIO
    INDEX 01 00:00:00
)");

    ymir::media::Disc disc;
    REQUIRE_FALSE(ymir::media::LoadDisc(cue, disc, false, SilentCb()));
}

TEST_CASE("CUE loader preserves mono PCM when resampling compressed audio", "[media][loader][cue][resample]") {
    const std::string format = GENERATE("MP3", "OGG");
    const size_t fileCount = GENERATE(1u, 2u);
    const bool preload = GENERATE(false, true);
    CAPTURE(format, fileCount, preload);

    // Synthetic fixtures generated with ffmpeg (no runtime ffmpeg dependency):
    // ffmpeg -f lavfi -i "sine=frequency=997:sample_rate=22050:duration=0.037" -ac 1
    //        -c:a libmp3lame -b:a 64k tests/fixtures/audio/mono_22050.mp3
    // Same source for mono_22050.ogg, with -c:a libvorbis -q:a 3 instead.
    const std::string extension = format == "MP3" ? ".mp3" : ".ogg";
    const auto fixture = AudioFixturesDir() / ("mono_22050" + extension);
    REQUIRE(std::filesystem::exists(fixture));

    // Decode at the source rate so the reference uses actual lossy PCM, not the original sine wave.
    std::vector<int16_t> source;
    if (format == "MP3") {
        drmp3_config config{};
        drmp3_uint64 frames = 0;
        auto *pcm = drmp3_open_file_and_read_pcm_frames_s16(fixture.string().c_str(), &config, &frames, nullptr);
        util::ScopeGuard freePCM{[&] { drmp3_free(pcm, nullptr); }};
        REQUIRE(pcm != nullptr);
        REQUIRE(config.channels == 1);
        REQUIRE(config.sampleRate == 22050);
        REQUIRE(frames > 1);
        source.assign(pcm, pcm + frames);
    } else {
        int channels = 0;
        int sampleRate = 0;
        short *pcm = nullptr;
        const int frames = stb_vorbis_decode_filename(fixture.string().c_str(), &channels, &sampleRate, &pcm);
        util::ScopeGuard freePCM{[&] { std::free(pcm); }};
        REQUIRE(pcm != nullptr);
        REQUIRE(channels == 1);
        REQUIRE(sampleRate == 22050);
        REQUIRE(frames > 1);
        source.assign(pcm, pcm + frames);
    }
    REQUIRE(std::any_of(source.begin(), source.end(), [](int16_t sample) { return sample != 0; }));
    REQUIRE(source.back() != 0); // Makes a zero-filled or lost endpoint observable.

    const size_t outputFrames = source.size() * 2;
    const size_t pcmBytes = outputFrames * 4;
    const size_t paddedBytes = ((pcmBytes + 2351) / 2352) * 2352;
    REQUIRE(paddedBytes > pcmBytes);

    const auto tmp = MakeTempDir();
    util::ScopeGuard cleanup{[&] {
        std::error_code error;
        std::filesystem::remove_all(tmp, error);
    }};
    std::string cueText;
    for (size_t i = 0; i < fileCount; ++i) {
        const auto name = "track" + std::to_string(i + 1) + extension;
        std::filesystem::copy_file(fixture, tmp / name);
        cueText += "FILE \"" + name + "\" " + format + "\n  TRACK 0" + std::to_string(i + 1) +
                   " AUDIO\n    INDEX 01 00:00:00\n";
    }
    const auto cue = tmp / "mono.cue";
    WriteText(cue, cueText);

    ymir::media::Disc disc;
    REQUIRE(ymir::media::LoadDisc(cue, disc, preload, SilentCb()));
    REQUIRE(disc.sessions.size() == 1);
    const auto &session = disc.sessions.back();
    REQUIRE(session.numTracks == fileCount);
    for (size_t trackIndex = 0; trackIndex < fileCount; ++trackIndex) {
        CAPTURE(trackIndex);
        const auto &track = session.tracks[trackIndex];
        REQUIRE(track.controlADR == 0x01);
        REQUIRE(track.unitSize == 2352);
        REQUIRE(track.binaryReader != nullptr);
        REQUIRE(track.binaryReader->Size() == paddedBytes);
        std::vector<uint8_t> actual(paddedBytes, 0xCD);
        REQUIRE(track.binaryReader->Read(0, actual.size(), actual) == actual.size());
        REQUIRE(std::any_of(actual.begin(), actual.begin() + pcmBytes, [](uint8_t byte) { return byte != 0; }));

        // At exactly 2x, even frames reproduce the source and odd frames are truncated midpoints.
        // The last odd frame has no successor and must repeat the final source sample.
        for (size_t frame = 0; frame < outputFrames; ++frame) {
            CAPTURE(frame);
            int16_t expected = source[frame / 2];
            if (frame % 2 != 0 && frame + 1 < outputFrames) {
                expected = static_cast<int16_t>((static_cast<int32_t>(expected) + source[frame / 2 + 1]) / 2);
            }
            const auto left = util::ReadLE<int16_t>(&actual[frame * 4]);
            const auto right = util::ReadLE<int16_t>(&actual[frame * 4 + 2]);
            REQUIRE(left == expected);
            REQUIRE(right == left);
        }
        REQUIRE(std::all_of(actual.begin() + pcmBytes, actual.end(), [](uint8_t byte) { return byte == 0; }));
        std::array<uint8_t, 2352> lastSector{};
        REQUIRE(track.ReadSector(track.endFrameAddress, lastSector));
        REQUIRE(std::equal(lastSector.begin(), lastSector.end(), actual.end() - lastSector.size()));
    }
}
