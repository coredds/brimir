// brimir_bench's --audio-hash and --save-state-every hashes (tools/stream_hash.hpp, header-only): stable for the same
// input, different for any changed sample, count or byte; and on a core's real output, the same per-frame audio hashes
// on every run.
#include "catch_amalgamated.hpp"

#include "../../tools/stream_hash.hpp"

#include <brimir/core_wrapper.hpp>
#include <ymir/sys/saturn.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

using brimir_tools::HashAudio;
using brimir_tools::HashBytes;

namespace {

std::vector<int16_t> Tone(size_t stereoFrames) {
    std::vector<int16_t> pcm(stereoFrames * 2);
    for (size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<int16_t>((i * 7919u + 13u) & 0xFFFFu);
    }
    return pcm;
}

// Keys on SCSP slot 0 playing a looped 16-bit square wave of `amplitude` from sound RAM at full level, direct out, so a
// core without a BIOS image (which is silent) produces audio.
void PlayTone(brimir::CoreWrapper& core, int16_t amplitude) {
    auto& bus = core.GetSaturn()->mainBus; // SCSP sound RAM at 0x5A00000, registers at 0x5B00000
    constexpr uint32_t kWave = 0x1000;
    constexpr uint16_t kLength = 0x40; // samples
    for (uint16_t i = 0; i < kLength; ++i) {
        bus.Write<uint16_t>(0x5A00000 + kWave + i * 2u, static_cast<uint16_t>(i < kLength / 2 ? amplitude : -amplitude));
    }
    bus.Write<uint16_t>(0x5B00000 + 0x400, 0x000F);           // MVOL 15
    bus.Write<uint16_t>(0x5B00000 + 0x02, kWave & 0xFFFF);     // SA
    bus.Write<uint16_t>(0x5B00000 + 0x04, 0);                  // LSA
    bus.Write<uint16_t>(0x5B00000 + 0x06, kLength);            // LEA
    bus.Write<uint16_t>(0x5B00000 + 0x08, 0x001F);             // AR 31, D1R/D2R 0
    bus.Write<uint16_t>(0x5B00000 + 0x0A, 0x001F);             // DL 0, RR 31
    bus.Write<uint16_t>(0x5B00000 + 0x0C, 0x0000);             // TL 0 (full level)
    bus.Write<uint16_t>(0x5B00000 + 0x10, 0x0000);             // OCT 0, FNS 0
    bus.Write<uint16_t>(0x5B00000 + 0x16, 0xE000);             // DISDL 7, DIPAN 0 (centre)
    bus.Write<uint16_t>(0x5B00000 + 0x00, 0x1820 | (kWave >> 16)); // KYONEX, KYONB, normal loop, 16-bit PCM
}

// Per-frame audio hashes of `frames` frames of a fresh core playing PlayTone(amplitude).
std::vector<uint64_t> CoreAudioHashes(int frames, int16_t amplitude, size_t& totalSamples, size_t& nonZero) {
    brimir::CoreWrapper core;
    REQUIRE(core.Initialize());
    core.SetThreadedVDP1(false);
    core.SetThreadedVDP2(false);
    PlayTone(core, amplitude);
    std::vector<int16_t> chunk(2048 * 2);
    std::vector<uint64_t> hashes;
    totalSamples = 0;
    nonZero = 0;
    for (int f = 0; f < frames; ++f) {
        core.RunFrame();
        std::vector<int16_t> pcm;
        while (const size_t n = core.GetAudioSamples(chunk.data(), 2048)) {
            pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n * 2));
        }
        totalSamples += pcm.size() / 2;
        nonZero += static_cast<size_t>(std::count_if(pcm.begin(), pcm.end(), [](int16_t s) { return s != 0; }));
        hashes.push_back(HashAudio(pcm.data(), pcm.size() / 2));
    }
    return hashes;
}

} // namespace

TEST_CASE("Audio hash is stable and covers every sample and the count", "[bench]") {
    const auto pcm = Tone(735);
    const uint64_t h = HashAudio(pcm.data(), 735);
    CHECK(HashAudio(pcm.data(), 735) == h);

    // FNV-1a of the 4 count bytes (0) and nothing else
    CHECK(HashAudio(nullptr, 0) == HashBytes("\0\0\0\0", 4));

    for (size_t i : {size_t{0}, size_t{1}, size_t{733}, size_t{1469}}) {
        auto changed = pcm;
        changed[i] = static_cast<int16_t>(changed[i] ^ 1); // one bit of one sample
        CHECK(HashAudio(changed.data(), 735) != h);
        changed = pcm;
        changed[i] = static_cast<int16_t>(changed[i] ^ 0x100); // the high byte
        CHECK(HashAudio(changed.data(), 735) != h);
    }

    // One sample fewer, or one silent sample more
    CHECK(HashAudio(pcm.data(), 734) != h);
    auto longer = pcm;
    longer.push_back(0);
    longer.push_back(0);
    CHECK(HashAudio(longer.data(), 736) != h);

    // Left and right swapped
    auto swapped = pcm;
    std::swap(swapped[0], swapped[1]);
    CHECK(HashAudio(swapped.data(), 735) != h);
}

TEST_CASE("State hash is stable and covers every byte", "[bench]") {
    std::vector<uint8_t> data(4096);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i * 31u);
    }
    const uint64_t h = HashBytes(data.data(), data.size());
    CHECK(HashBytes(data.data(), data.size()) == h);
    CHECK(HashBytes(data.data(), 0) == brimir_tools::kFnvOffset);
    for (size_t i : {size_t{0}, size_t{2047}, size_t{4095}}) {
        auto changed = data;
        changed[i] ^= 0x80;
        CHECK(HashBytes(changed.data(), changed.size()) != h);
    }
    CHECK(HashBytes(data.data(), data.size() - 1) != h);
}

TEST_CASE("Audio hashes of a core's output are the same on every run and follow the audio", "[bench]") {
    size_t samplesA = 0;
    size_t samplesB = 0;
    size_t samplesC = 0;
    size_t nonZeroA = 0;
    size_t nonZeroB = 0;
    size_t nonZeroC = 0;
    const auto a = CoreAudioHashes(30, 0x2000, samplesA, nonZeroA);
    const auto b = CoreAudioHashes(30, 0x2000, samplesB, nonZeroB);
    CHECK(samplesA > 0); // the frames produced audio
    INFO("non-zero samples: " << nonZeroA);
    REQUIRE(nonZeroA > 0); // and it is not silence
    CHECK(nonZeroA == nonZeroB);
    CHECK(samplesA == samplesB);
    CHECK(a == b);

    // The same tone one step louder: as many samples, other hashes on the frames that carry the tone
    const auto c = CoreAudioHashes(30, 0x2001, samplesC, nonZeroC);
    CHECK(samplesC == samplesA);
    size_t differing = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        differing += a[i] != c[i] ? 1 : 0;
    }
    CHECK(differing > 0);
}
