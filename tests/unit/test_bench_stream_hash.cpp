// brimir_bench's --audio-hash and --save-state-every hashes (tools/stream_hash.hpp, header-only): stable for the same
// input, different for any changed sample, count or byte; and on a core's real output, the same per-frame audio hashes
// on every run.
#include "catch_amalgamated.hpp"

#include "../../tools/stream_hash.hpp"

#include <brimir/core_wrapper.hpp>

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

// Per-frame audio hashes of `frames` frames of a fresh core (no BIOS image: the core runs zeroed IPL).
std::vector<uint64_t> CoreAudioHashes(int frames, size_t& totalSamples) {
    brimir::CoreWrapper core;
    REQUIRE(core.Initialize());
    core.SetThreadedVDP1(false);
    core.SetThreadedVDP2(false);
    std::vector<int16_t> chunk(2048 * 2);
    std::vector<uint64_t> hashes;
    totalSamples = 0;
    for (int f = 0; f < frames; ++f) {
        core.RunFrame();
        std::vector<int16_t> pcm;
        while (const size_t n = core.GetAudioSamples(chunk.data(), 2048)) {
            pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n * 2));
        }
        totalSamples += pcm.size() / 2;
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

TEST_CASE("Audio hashes of a core's output are the same on every run", "[bench]") {
    size_t samplesA = 0;
    size_t samplesB = 0;
    const auto a = CoreAudioHashes(30, samplesA);
    const auto b = CoreAudioHashes(30, samplesB);
    CHECK(samplesA > 0); // the frames produced audio
    CHECK(samplesA == samplesB);
    CHECK(a == b);
}
