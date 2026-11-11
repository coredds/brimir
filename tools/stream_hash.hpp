// Brimir - audio and byte-stream hashes for brimir_bench (--audio-hash, --save-state-every)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// FNV-1a (64 bit), the same function as frame_hash.hpp. HashAudio covers one frame's PCM output: the stereo sample
// count (4 little-endian bytes), then every int16 sample (L, R, L, R, ...) as 2 little-endian bytes. HashBytes covers
// a raw byte buffer (a save state).

#pragma once

#include <cstddef>
#include <cstdint>

namespace brimir_tools {

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

inline uint64_t HashBytes(const void* data, size_t size, uint64_t hash = kFnvOffset) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

inline uint64_t HashAudio(const int16_t* samples, size_t stereoFrames) {
    uint64_t hash = kFnvOffset;
    const auto byte = [&hash](uint32_t value) {
        hash ^= value & 0xFFu;
        hash *= kFnvPrime;
    };
    const auto count = static_cast<uint32_t>(stereoFrames);
    for (int shift = 0; shift < 32; shift += 8) {
        byte(count >> shift);
    }
    for (size_t i = 0; i < stereoFrames * 2; ++i) {
        const auto s = static_cast<uint16_t>(samples[i]);
        byte(s);
        byte(static_cast<uint32_t>(s) >> 8);
    }
    return hash;
}

} // namespace brimir_tools
