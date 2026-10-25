// 64-bit rotation arithmetic of Ymir's cs_vdp2_render_bgs.hlsl (8cdf0a53, lines 444-445 and 502-503) in 32-bit
// integers: the Vulkan baseline has no Int64 capability. Same algorithm as src/gpu/vdp2/int64_emu.hpp (unit-tested
// against int64_t on the CPU and through test_int64_emu.hlsl on the GPU); derivation in
// docs/ymir-hw-renderer-notes/stage1b-port-inventory.md section 5.3. All int arithmetic wraps; >> on int is arithmetic.
#ifndef BRIMIR_INT64_EMU_HLSLI
#define BRIMIR_INT64_EMU_HLSLI

// int((int64_t(a0) * int64_t(b0) + int64_t(a1) * int64_t(b1) + int64_t(a2) * int64_t(b2)) >> 10), exact for a0..a2 in
// [-2^13, 2^13 - 1] (14-bit signed rotation matrix elements) and any b0..b2.
// b = bh * 2^16 + bl with bh = b >> 16 and bl = b & 0xFFFF; the sum is H * 2^16 + L with |H| < 3 * 2^28 and
// |L| < 3 * 2^29, so neither partial sum overflows, and (H * 2^16 + L) >> 10 = H * 2^6 + (L >> 10).
int RotDotShr10(int a0, int b0, int a1, int b1, int a2, int b2) {
    const int h = a0 * (b0 >> 16) + a1 * (b1 >> 16) + a2 * (b2 >> 16);
    const int l = a0 * int(uint(b0) & 0xFFFFu) + a1 * int(uint(b1) & 0xFFFFu) + a2 * int(uint(b2) & 0xFFFFu);
    return int((uint(h) << 6) + uint(l >> 10));
}

// Signed 32 x 32 -> 64-bit product as uint2(low word, high word): the unsigned product from 16-bit limbs, then the
// high word corrected for negative operands.
uint2 MulS32x32To64(int a, int b) {
    const uint ua = uint(a);
    const uint ub = uint(b);
    const uint al = ua & 0xFFFFu;
    const uint ah = ua >> 16;
    const uint bl = ub & 0xFFFFu;
    const uint bh = ub >> 16;
    const uint ll = al * bl;
    const uint lh = al * bh;
    const uint hl = ah * bl;
    const uint mid = (ll >> 16) + (lh & 0xFFFFu) + (hl & 0xFFFFu);
    const uint lo = (mid << 16) | (ll & 0xFFFFu);
    uint hi = ah * bh + (lh >> 16) + (hl >> 16) + (mid >> 16);
    if (a < 0) {
        hi -= ub;
    }
    if (b < 0) {
        hi -= ua;
    }
    return uint2(lo, hi);
}

// uint((((int64_t(k) * scr) >> 16) + p) >> 10) for any k, scr, p.
// ((P >> 16) + p) >> 10 = (P + p * 2^16) >> 26 (nested floor division); the 64-bit sum cannot overflow, and the
// result is its bits 26-57.
uint RotCoordShr(int k, int scr, int p) {
    const uint2 product = MulS32x32To64(k, scr);
    const uint lo = product.x + (uint(p) << 16);
    const uint hi = product.y + uint(p >> 16) + (lo < product.x ? 1u : 0u);
    return (hi << 6) | (lo >> 26);
}

#endif
