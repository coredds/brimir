// Brimir - frame hash shared by brimir_bench and brimir_vkhost
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// FNV-1a (64 bit) over the width, the height (each as 4 little-endian bytes) and the XRGB8888 pixels
// (each as 4 little-endian bytes, row by row, `pitch` bytes apart in the source). `mask` is ANDed into every
// pixel first: use 0x00FFFFFF to ignore alpha. Both tools must hash the same way for their output to compare.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace brimir_tools {

constexpr uint32_t kHashAllBits = 0xFFFFFFFFu;
constexpr uint32_t kHashIgnoreAlpha = 0x00FFFFFFu;

inline uint64_t HashFrame(uint32_t width, uint32_t height, const void* pixels, size_t pitchBytes,
                          uint32_t mask = kHashAllBits) {
    uint64_t hash = 0xcbf29ce484222325ull;
    const auto word = [&hash](uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= (value >> shift) & 0xFFu;
            hash *= 0x100000001b3ull;
        }
    };
    word(width);
    word(height);
    const auto* rows = static_cast<const uint8_t*>(pixels);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = rows + static_cast<size_t>(y) * pitchBytes;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t px;
            std::memcpy(&px, row + static_cast<size_t>(x) * 4, sizeof(px));
            word(px & mask);
        }
    }
    return hash;
}

} // namespace brimir_tools
