#pragma once

// 64-bit rotation arithmetic of Ymir's cs_vdp2_render_bgs.hlsl (8cdf0a53, lines 444-445 and 502-503) in 32-bit
// integers, the same algorithm as src/gpu/shaders/include/int64_emu.hlsli (the Vulkan baseline has no Int64
// capability). The C++ copy is the reference the shader copy is tested against; derivation in
// docs/ymir-hw-renderer-notes/stage1b-port-inventory.md section 5.3. Wrapping arithmetic is done in uint32_t; >> on
// negative int32_t is arithmetic (C++20). No Vulkan header may be included here.

#include <cstdint>

namespace brimir::gpu::vdp2 {

// int32_t((int64_t(a0) * b0 + int64_t(a1) * b1 + int64_t(a2) * b2) >> 10), exact for a0..a2 in [-2^13, 2^13 - 1]
// (14-bit signed rotation matrix elements) and any b0..b2.
// b = bh * 2^16 + bl with bh = b >> 16 and bl = b & 0xFFFF; the sum is H * 2^16 + L with |H| < 3 * 2^28 and
// |L| < 3 * 2^29, so neither partial sum overflows, and (H * 2^16 + L) >> 10 = H * 2^6 + (L >> 10).
constexpr int32_t RotDotShr10(int32_t a0, int32_t b0, int32_t a1, int32_t b1, int32_t a2, int32_t b2) {
    const int32_t h = a0 * (b0 >> 16) + a1 * (b1 >> 16) + a2 * (b2 >> 16);
    const int32_t l = a0 * static_cast<int32_t>(static_cast<uint32_t>(b0) & 0xFFFFu) +
                      a1 * static_cast<int32_t>(static_cast<uint32_t>(b1) & 0xFFFFu) +
                      a2 * static_cast<int32_t>(static_cast<uint32_t>(b2) & 0xFFFFu);
    return static_cast<int32_t>((static_cast<uint32_t>(h) << 6) + static_cast<uint32_t>(l >> 10));
}

struct U64Parts {
    uint32_t lo;
    uint32_t hi;
};

// Signed 32 x 32 -> 64-bit product as (low word, high word): the unsigned product from 16-bit limbs, then the high
// word corrected for negative operands.
constexpr U64Parts MulS32x32To64(int32_t a, int32_t b) {
    const auto ua = static_cast<uint32_t>(a);
    const auto ub = static_cast<uint32_t>(b);
    const uint32_t al = ua & 0xFFFFu;
    const uint32_t ah = ua >> 16;
    const uint32_t bl = ub & 0xFFFFu;
    const uint32_t bh = ub >> 16;
    const uint32_t ll = al * bl;
    const uint32_t lh = al * bh;
    const uint32_t hl = ah * bl;
    const uint32_t mid = (ll >> 16) + (lh & 0xFFFFu) + (hl & 0xFFFFu);
    const uint32_t lo = (mid << 16) | (ll & 0xFFFFu);
    uint32_t hi = ah * bh + (lh >> 16) + (hl >> 16) + (mid >> 16);
    if (a < 0) {
        hi -= ub;
    }
    if (b < 0) {
        hi -= ua;
    }
    return {lo, hi};
}

// uint32_t((((int64_t(k) * scr) >> 16) + p) >> 10) for any k, scr, p.
// ((P >> 16) + p) >> 10 = (P + p * 2^16) >> 26 (nested floor division); the 64-bit sum cannot overflow, and the
// result is its bits 26-57.
constexpr uint32_t RotCoordShr(int32_t k, int32_t scr, int32_t p) {
    const U64Parts product = MulS32x32To64(k, scr);
    const uint32_t lo = product.lo + (static_cast<uint32_t>(p) << 16);
    const uint32_t hi = product.hi + static_cast<uint32_t>(p >> 16) + (lo < product.lo ? 1u : 0u);
    return (hi << 6) | (lo >> 26);
}

} // namespace brimir::gpu::vdp2
