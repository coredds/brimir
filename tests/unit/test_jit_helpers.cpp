// Brimir - SH-2 JIT shared helper tests: each helper against the real interpreter instruction
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/jit/sh2_helpers.hpp>

#include <cstdint>
#include <memory>

using namespace brimir::jit;
using sh2test::Rig;

namespace {

constexpr uint32_t kCode = 0x06001000;
constexpr uint32_t kOp1Addr = 0x06040000; // @Rm (op1)
constexpr uint32_t kOp2Addr = 0x06040100; // @Rn (op2)

constexpr uint32_t kSrT = 1u << 0;
constexpr uint32_t kSrS = 1u << 1;
constexpr uint32_t kSrQ = 1u << 8;
constexpr uint32_t kSrM = 1u << 9;

constexpr uint32_t kValues[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x12345678u};

// Runs one instruction at kCode on the interpreter.
void RunOne(Rig &rig, uint16_t opcode, const ymir::savestate::SH2SaveState &state) {
    rig.WriteCode(kCode, {opcode, sh2test::kSleep});
    auto s = state;
    s.PC = kCode;
    const auto base = rig.BaseState(kCode);
    s.fetchedOpcodes = base.fetchedOpcodes;
    rig.Load(s);
    rig.sh2->Step<false, false>();
}

} // namespace

TEST_CASE("Div1Step matches the interpreter's DIV1", "[jit][helpers]") {
    auto rig = std::make_unique<Rig>();
    for (uint32_t bits = 0; bits < 8; ++bits) {
        const uint32_t srFlags = ((bits & 1) ? kSrQ : 0) | ((bits & 2) ? kSrM : 0) | ((bits & 4) ? kSrT : 0);
        // n != m
        for (const uint32_t rn : kValues) {
            for (const uint32_t rm : kValues) {
                auto s = rig->BaseState(kCode);
                s.SR = 0xF0 | srFlags;
                s.R[1] = rm;
                s.R[2] = rn;
                RunOne(*rig, 0x3214, s); // div1 R1,R2
                const auto st = rig->State();

                uint32_t sr = s.SR;
                const uint32_t result = Div1Step(rn, rm, false, sr);
                INFO("Q/M/T bits " << bits << " rn " << rn << " rm " << rm);
                CHECK(result == st.R[2]);
                CHECK(sr == st.SR);
                CHECK(st.R[1] == rm);
            }
        }
        // n == m: div1 R5,R5 (Rm is read after Rn was shifted)
        for (const uint32_t rn : kValues) {
            auto s = rig->BaseState(kCode);
            s.SR = 0xF0 | srFlags;
            s.R[5] = rn;
            RunOne(*rig, 0x3554, s);
            const auto st = rig->State();

            uint32_t sr = s.SR;
            // The rm argument must be ignored when rmIsRn is set.
            const uint32_t result = Div1Step(rn, 0xDEADBEEF, true, sr);
            INFO("n == m, Q/M/T bits " << bits << " rn " << rn);
            CHECK(result == st.R[5]);
            CHECK(sr == st.SR);
        }
    }
}

TEST_CASE("MacWStep matches the interpreter's MAC.W", "[jit][helpers]") {
    auto rig = std::make_unique<Rig>();
    const uint64_t macs[] = {
        0x0000000000000000ull, 0x000000007FFFFFFFull, 0x0000000080000000ull,
        0x000000017FFFFFFFull, 0xFFFFFFFF80000000ull, 0x0000000100000005ull, // MACH bit 0 set
    };
    const uint16_t operands[] = {0x8000, 0x7FFF, 0xFFFF, 0x0001};
    for (const bool sBit : {false, true}) {
        for (const uint64_t mac : macs) {
            for (const uint16_t a : operands) {
                for (const uint16_t b : operands) {
                    rig->Write32(kOp1Addr, static_cast<uint32_t>(a) << 16);
                    rig->Write32(kOp2Addr, static_cast<uint32_t>(b) << 16);
                    auto s = rig->BaseState(kCode);
                    s.SR = 0xF0 | (sBit ? kSrS : 0);
                    s.MACH = static_cast<uint32_t>(mac >> 32);
                    s.MACL = static_cast<uint32_t>(mac);
                    s.R[1] = kOp1Addr; // Rm
                    s.R[2] = kOp2Addr; // Rn
                    RunOne(*rig, 0x421F, s); // mac.w @R1+,@R2+
                    const auto st = rig->State();
                    const uint64_t expected = (static_cast<uint64_t>(st.MACH) << 32) | st.MACL;

                    const int32_t op1 = static_cast<int16_t>(a);
                    const int32_t op2 = static_cast<int16_t>(b);
                    INFO("S " << sBit << " mac " << mac << " op1 " << op1 << " op2 " << op2);
                    CHECK(MacWStep(mac, sBit, op1, op2) == expected);
                    CHECK(st.R[1] == kOp1Addr + 2);
                    CHECK(st.R[2] == kOp2Addr + 2);
                }
            }
        }
    }
}

TEST_CASE("MacLStep matches the interpreter's MAC.L", "[jit][helpers]") {
    auto rig = std::make_unique<Rig>();
    const uint64_t macs[] = {
        0x00007FFFFFFFFFFFull, 0x0000800000000000ull, 0xFFFF800000000000ull, 0xFFFF7FFFFFFFFFFFull,
        0x0000000000000000ull,
    };
    const uint32_t operands[] = {0x80000000u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0u, 1u};

    const auto check = [&](bool sBit, uint64_t mac, uint32_t a, uint32_t b) {
        rig->Write32(kOp1Addr, a);
        rig->Write32(kOp2Addr, b);
        auto s = rig->BaseState(kCode);
        s.SR = 0xF0 | (sBit ? kSrS : 0);
        s.MACH = static_cast<uint32_t>(mac >> 32);
        s.MACL = static_cast<uint32_t>(mac);
        s.R[1] = kOp1Addr; // Rm
        s.R[2] = kOp2Addr; // Rn
        RunOne(*rig, 0x021F, s); // mac.l @R1+,@R2+
        const auto st = rig->State();
        const uint64_t expected = (static_cast<uint64_t>(st.MACH) << 32) | st.MACL;

        INFO("S " << sBit << " mac " << mac << " op1 " << a << " op2 " << b);
        CHECK(MacLStep(mac, sBit, static_cast<int32_t>(a), static_cast<int32_t>(b)) == expected);
        CHECK(st.R[1] == kOp1Addr + 4);
        CHECK(st.R[2] == kOp2Addr + 4);
        return expected;
    };

    for (const bool sBit : {false, true}) {
        for (const uint64_t mac : macs) {
            for (const uint32_t a : operands) {
                for (const uint32_t b : operands) {
                    check(sBit, mac, a, b);
                }
            }
        }
    }

    // Zero product with one negative operand while MAC is already out of the 48-bit range:
    // the saturation direction comes from the operand signs, not from the sum.
    CHECK(check(true, 0x0000900000000000ull, 0xFFFFFFFFu, 0u) == 0xFFFF800000000000ull);
    CHECK(check(true, 0xFFFF000000000000ull, 0u, 0u) == 0x00007FFFFFFFFFFFull);
}
