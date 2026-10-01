#include <brimir/jit/sh2_helpers.hpp>

namespace brimir::jit {

// sh2.cpp SH2::DIV1. All compares are unsigned.
uint32_t Div1Step(uint32_t rn, uint32_t rm, bool rmIsRn, uint32_t &sr) {
    const bool oldQ = ((sr >> 8) & 1u) != 0;
    const bool M = ((sr >> 9) & 1u) != 0;
    const uint32_t T = sr & 1u;
    bool Q = static_cast<int32_t>(rn) < 0;
    rn = (rn << 1) | T;
    const uint32_t prevVal = rn;
    const uint32_t src = rmIsRn ? rn : rm; // the handler reads R[rm] after shifting R[rn]
    rn = (oldQ == M) ? rn - src : rn + src;
    if (oldQ) {
        Q ^= M ? (rn <= prevVal) : (rn < prevVal);
    } else {
        Q ^= M ? (rn >= prevVal) : (rn > prevVal);
    }
    sr = (sr & ~0x101u) | (static_cast<uint32_t>(Q) << 8) | static_cast<uint32_t>(Q == M);
    return rn;
}

// sh2.cpp SH2::MACW. With S set only MACL accumulates (signed 32-bit, saturating); on saturation
// MACH bit 0 is set and MACH is otherwise left as is.
uint64_t MacWStep(uint64_t mac, bool s, int32_t op1, int32_t op2) {
    // The product of two sign-extended 16-bit values always fits; multiply as unsigned to keep
    // the wrap well defined for any input.
    const int32_t mul = static_cast<int32_t>(static_cast<uint32_t>(op1) * static_cast<uint32_t>(op2));
    if (!s) {
        return mac + static_cast<uint64_t>(static_cast<int64_t>(mul));
    }
    const int64_t result = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(mac))) + mul;
    constexpr int64_t kMin = -0x80000000LL;
    constexpr int64_t kMax = 0x7FFFFFFFLL;
    if (result >= kMin && result <= kMax) {
        return (mac & ~0xFFFFFFFFull) | static_cast<uint32_t>(result);
    }
    const int64_t saturated = result < kMin ? kMin : kMax;
    return ((mac | (1ull << 32)) & ~0xFFFFFFFFull) | static_cast<uint32_t>(saturated);
}

// sh2.cpp SH2::MACL. The saturation test is done on the 64-bit sum as unsigned; the direction comes
// from the sign of op1 ^ op2, not from the sum.
uint64_t MacLStep(uint64_t mac, bool s, int32_t op1, int32_t op2) {
    const int64_t mul = static_cast<int64_t>(op1) * static_cast<int64_t>(op2);
    uint64_t result = static_cast<uint64_t>(mul) + mac;
    if (s && result > 0x00007FFFFFFFFFFFull && result < 0xFFFF800000000000ull) {
        result = (op1 ^ op2) < 0 ? 0xFFFF800000000000ull : 0x00007FFFFFFFFFFFull;
    }
    return result;
}

} // namespace brimir::jit
