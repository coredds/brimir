// Brimir - brimir_vkhost: headless libretro host
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// The sample points of a frame at internal resolution N (--hash-sample) and the sharpness metric (--sharpness), as
// pure functions on XRGB8888 frames (tests/unit/test_vkhost_sample_points.cpp). See tools/README.md.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vkhost {

// Native output coordinates (after crop and rotation).
struct Region {
    uint32_t x = 0, y = 0, w = UINT32_MAX, h = UINT32_MAX;
};

// The sample point of native output pixel (ox, oy) in an N x frame of the native output size w x h, through the
// output rotation: the pre-rotation native pixel (px, py) the presenter reads for it (cs_frame_output.hlsl's inverse
// rotation at 1x; the crop only offsets it by an even amount, 8 px per overscan step), then the N x output pixel
// (X, Y) that shows the N x source pixel (N px, N py).
struct SamplePoint {
    uint32_t X, Y;   // in the N x frame
    uint32_t px, py; // pre-rotation native pixel
};

inline SamplePoint MapSample(uint32_t n, uint32_t rotation, uint32_t w, uint32_t h, uint32_t ox, uint32_t oy) {
    const bool swap = rotation == 90 || rotation == 270;
    const uint32_t pw = swap ? h : w; // pre-rotation native width
    SamplePoint s{};
    switch (rotation) {
    case 90: s.px = oy, s.py = w - 1 - ox; break;
    case 180: s.px = w - 1 - ox, s.py = h - 1 - oy; break;
    case 270: s.px = pw - 1 - oy, s.py = ox; break;
    default: s.px = ox, s.py = oy; break;
    }
    const uint32_t sx = n * s.px;
    const uint32_t sy = n * s.py;
    switch (rotation) {
    case 90: s.X = n * w - 1 - sy, s.Y = sx; break;
    case 180: s.X = n * w - 1 - sx, s.Y = n * h - 1 - sy; break;
    case 270: s.X = sy, s.Y = n * pw - 1 - sx; break;
    default: s.X = sx, s.Y = sy; break;
    }
    return s;
}

// Ruling 2's host-side lattice: hi-res frames (pre-rotation native width above 400 px: 640/704 minus at most 48 px of
// overscan crop, against at most 352 for the low-res widths) skip odd native columns, interlaced frames (height above
// 300: 448/480/512 minus the crop, against at most 256) odd native rows. The crop offsets are even, so the parities are
// those of the uncropped frame.
inline bool SampleExcluded(const SamplePoint &s, uint32_t rotation, uint32_t w, uint32_t h) {
    const bool swap = rotation == 90 || rotation == 270;
    const uint32_t pw = swap ? h : w;
    const uint32_t ph = swap ? w : h;
    return (pw > 400 && (s.px & 1) != 0) || (ph > 300 && (s.py & 1) != 0);
}

// The (W / n) x (H / n) native image of the sample points of a W x H frame at scale n, the excluded lattice set to 0.
// Empty when W or H is not a multiple of n.
inline std::vector<uint32_t> SampleImage(const uint32_t *xrgb, uint32_t W, uint32_t H, uint32_t n, uint32_t rotation) {
    if (n == 0 || W % n != 0 || H % n != 0) {
        return {};
    }
    const uint32_t w = W / n;
    const uint32_t h = H / n;
    std::vector<uint32_t> native(static_cast<size_t>(w) * h);
    for (uint32_t oy = 0; oy < h; ++oy) {
        for (uint32_t ox = 0; ox < w; ++ox) {
            const SamplePoint s = MapSample(n, rotation, w, h, ox, oy);
            native[static_cast<size_t>(oy) * w + ox] =
                SampleExcluded(s, rotation, w, h) ? 0 : xrgb[static_cast<size_t>(s.Y) * W + s.X];
        }
    }
    return native;
}

struct Sharpness {
    bool valid = false; // W and H are multiples of n
    uint32_t x0 = 0, y0 = 0, w = 0, h = 0; // the region, clamped to the frame (native output pixels)
    uint64_t blocks = 0;
    uint64_t nonConstant = 0;
    double nonConstantFraction = 0; // nonConstant / blocks
    double meanAbsDiff = 0;         // over every pixel of the blocks and R, G, B (0-255)
};

// Per n x n block of native output pixel (ox, oy) in the region: the block is not constant when one of its pixels
// differs (RGB) from the block's sample point (nearest(img1) at the block, by the sample-point invariant); the mean
// absolute difference is over every pixel of the region's blocks and the R, G and B channels.
inline Sharpness ComputeSharpness(const uint32_t *xrgb, uint32_t W, uint32_t H, uint32_t n, uint32_t rotation,
                                  Region region = {}) {
    Sharpness result;
    if (n == 0 || W % n != 0 || H % n != 0) {
        return result;
    }
    result.valid = true;
    const uint32_t w = W / n;
    const uint32_t h = H / n;
    const uint32_t x0 = std::min(region.x, w);
    const uint32_t y0 = std::min(region.y, h);
    const uint32_t x1 = region.w > w - x0 ? w : x0 + region.w;
    const uint32_t y1 = region.h > h - y0 ? h : y0 + region.h;
    uint64_t diff = 0;
    for (uint32_t oy = y0; oy < y1; ++oy) {
        for (uint32_t ox = x0; ox < x1; ++ox) {
            const SamplePoint s = MapSample(n, rotation, w, h, ox, oy);
            const uint32_t ref = xrgb[static_cast<size_t>(s.Y) * W + s.X];
            bool constant = true;
            for (uint32_t Y = oy * n; Y < (oy + 1) * n; ++Y) {
                for (uint32_t X = ox * n; X < (ox + 1) * n; ++X) {
                    const uint32_t px = xrgb[static_cast<size_t>(Y) * W + X];
                    constant = constant && (px & 0xFFFFFFu) == (ref & 0xFFFFFFu);
                    for (uint32_t shift : {0u, 8u, 16u}) {
                        const int a = static_cast<int>((px >> shift) & 0xFF);
                        const int b = static_cast<int>((ref >> shift) & 0xFF);
                        diff += static_cast<uint64_t>(a > b ? a - b : b - a);
                    }
                }
            }
            ++result.blocks;
            result.nonConstant += constant ? 0 : 1;
        }
    }
    result.x0 = x0;
    result.y0 = y0;
    result.w = x1 - x0;
    result.h = y1 - y0;
    if (result.blocks != 0) {
        result.nonConstantFraction = static_cast<double>(result.nonConstant) / static_cast<double>(result.blocks);
        result.meanAbsDiff = static_cast<double>(diff) / (static_cast<double>(result.blocks) * n * n * 3.0);
    }
    return result;
}

} // namespace vkhost
