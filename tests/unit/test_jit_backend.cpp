// Brimir - SH-2 JIT IR interpreter backend tests
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "jit_test_backend.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/jit/interp_backend.hpp>
#include <brimir/jit/ir.hpp>
#include <brimir/jit/sh2_helpers.hpp>

#include <memory>

using namespace brimir::jit;
using sh2test::Rig;

namespace {

constexpr uint32_t kCode = 0x06001000;

struct Fixture {
    std::unique_ptr<Rig> rig = std::make_unique<Rig>();
    Block block;
    Builder b{block};

    Fixture() {
        rig->Load(rig->BaseState(kCode));
        block.startPC = kCode;
        block.guestInstrCount = 1;
    }

    // Ir runs RunBlock; a native kind compiles and runs the block (sh2test::RunOnBackend).
    ExitInfo Run(BackendKind kind = BackendKind::Ir, uint64_t target = kNoCycleTarget) {
        INFO(PrintBlock(block));
        INFO("backend " << BackendName(kind));
        REQUIRE(VerifyBlock(block).empty());
        return sh2test::RunOnBackend(kind, block, rig->sh2->GetJitContext(), target);
    }
};

} // namespace

// Tests whose blocks use only ops every native backend lowers run once per available backend.

TEST_CASE("Backend: registers, ALU and T bit", "[jit][backend]") {
    const BackendKind kind = GENERATE(from_range(sh2test::AvailableBackends()));
    Fixture f;
    auto s = f.rig->BaseState(kCode);
    s.R[1] = 40;
    s.PR = 0x06002000;
    f.rig->Load(s);

    const ValueId r1 = f.b.GetReg(1);
    const ValueId two = f.b.Const(2);
    f.b.SetReg(2, f.b.Add(r1, two));
    f.b.SetReg(3, f.b.Sub(r1, two));
    f.b.SetReg(4, f.b.SExt8(f.b.Const(0x80)));
    f.b.SetReg(5, f.b.SExt16(f.b.Const(0x8001)));
    f.b.SetReg(6, f.b.GetPR());
    f.b.SetT(f.b.CmpEq(r1, f.b.Const(40)));
    f.b.SetReg(7, f.b.GetT());
    f.b.Exit(kCode + 2, 1);

    const ExitInfo info = f.Run(kind);
    const auto st = f.rig->State();
    REQUIRE(st.R[2] == 42u);
    REQUIRE(st.R[3] == 38u);
    REQUIRE(st.R[4] == 0xFFFFFF80u);
    REQUIRE(st.R[5] == 0xFFFF8001u);
    REQUIRE(st.R[6] == 0x06002000u);
    REQUIRE((st.SR & 1u) == 1u);
    REQUIRE(st.R[7] == 1u);
    REQUIRE(st.PC == kCode + 2);
    REQUIRE(info.cycles == 0);
    REQUIRE(info.retired == 1);
    REQUIRE_FALSE(info.busWait);
}

TEST_CASE("Backend: memory, access cycles and write-back stalls", "[jit][backend]") {
    Fixture f;
    auto s = f.rig->BaseState(kCode);
    s.wbReg = 3;
    f.rig->Load(s);
    f.rig->Write32(0x06040000, 0x89ABCDEF);

    const ValueId addr = f.b.Const(0x26040000);
    f.b.AddAccessCycles(addr, 4, false); // +6
    f.b.SetReg(1, f.b.Load(addr, 4, false));
    f.b.Store(f.b.Const(0x26040010), 2, f.b.Const(0x1234));
    f.b.AddAccessCycles(addr, 1, true);  // +3
    f.b.WbStall((1u << 3) | (1u << 5)); // wbReg = 3 -> +1
    f.b.WbStall(1u << 16);              // PR not in WB -> +0
    f.b.SetWb(7);
    f.b.AddCycles(10);
    f.b.Exit(kCode + 2, 1);

    const ExitInfo info = f.Run();
    REQUIRE(f.rig->State().R[1] == 0x89ABCDEFu);
    REQUIRE(f.rig->Read16(0x06040010) == 0x1234);
    REQUIRE(f.rig->State().wbReg == 7);
    REQUIRE(info.cycles == 6 + 3 + 1 + 10);
}

TEST_CASE("Backend: SyncCycles publishes the running cycle count", "[jit][backend]") {
    const BackendKind kind = GENERATE(from_range(sh2test::AvailableBackends()));
    Fixture f;
    auto &ctx = f.rig->sh2->GetJitContext();
    *ctx.cyclesExecuted = 100;
    f.b.AddCycles(7);
    f.b.SyncCycles();
    f.b.AddCycles(5);
    f.b.Exit(kCode + 2, 1);

    const ExitInfo info = f.Run(kind);
    REQUIRE(info.cycles == 12);
    REQUIRE(*ctx.cyclesExecuted == 107); // the executor, not the backend, publishes the final count
}

TEST_CASE("Backend: WbStall ignores the 'no register' marker", "[jit][backend]") {
    const BackendKind kind = GENERATE(from_range(sh2test::AvailableBackends()));
    Fixture f; // BaseState sets wbReg = 0xFF
    f.b.WbStall(0xFFFFFFFFu);
    f.b.Exit(kCode + 2, 1);
    REQUIRE(f.Run(kind).cycles == 0);
}

TEST_CASE("Backend: bus-wait exit leaves state for a retry", "[jit][backend]") {
    Fixture f;
    f.rig->mmio.busWaitEvery = 1;
    const ValueId addr = f.b.Const(0x22000000);
    f.b.AddAccessCycles(addr, 4, false); // +12
    f.b.ExitIfBusWait(addr, 4, false, kCode, 0);
    f.b.SetReg(1, f.b.Load(addr, 4, false));
    f.b.Exit(kCode + 2, 1);

    const ExitInfo info = f.Run();
    REQUIRE(info.busWait);
    REQUIRE(info.retired == 0);
    REQUIRE(info.cycles == 12);
    REQUIRE(f.rig->State().PC == kCode);
    REQUIRE(f.rig->State().R[1] == 0u);
}

TEST_CASE("Backend: conditional exit with refill", "[jit][backend]") {
    Fixture f;
    f.rig->WriteCode(kCode + 0x40, {0x1111, 0x2222});
    const ValueId one = f.b.Const(1);
    f.b.ExitIf(one, kCode + 0x40, 3, true, 1);
    f.b.AddCycles(1);
    f.b.Exit(kCode + 2, 1);

    const ExitInfo info = f.Run();
    REQUIRE(info.cycles == 3);
    REQUIRE(info.retired == 1);
    REQUIRE(f.rig->State().PC == kCode + 0x40);
    REQUIRE(f.rig->State().fetchedOpcodes == 0x11112222u);
}

TEST_CASE("Backend: conditional exit not taken falls through", "[jit][backend]") {
    Fixture f;
    const ValueId zero = f.b.Const(0);
    f.b.ExitIf(zero, kCode + 0x40, 3, true, 1);
    f.b.AddCycles(1);
    f.b.Exit(kCode + 2, 1);

    const ExitInfo info = f.Run();
    REQUIRE(info.cycles == 1);
    REQUIRE(f.rig->State().PC == kCode + 2);
}

TEST_CASE("Backend: delay slot setup and dynamic exit", "[jit][backend]") {
    Fixture f;
    f.b.SetupDelaySlot(f.b.Const(kCode + 0x80));
    f.b.Refill(kCode);
    f.b.EndDelaySlot();
    f.b.ExitDynamic(2);

    const ExitInfo info = f.Run();
    REQUIRE(info.retired == 2);
    const auto st = f.rig->State();
    REQUIRE(st.PC == kCode + 0x80);
    REQUIRE_FALSE(st.delaySlot);
    REQUIRE(st.delaySlotTarget == kCode + 0x80);
}

TEST_CASE("Backend: CheckBoundary stops at the cycle target or a pending interrupt", "[jit][backend]") {
    const BackendKind kind = GENERATE(from_range(sh2test::AvailableBackends()));
    Fixture f;
    auto &ctx = f.rig->sh2->GetJitContext();
    f.b.AddCycles(5);
    f.b.CheckBoundary(kCode + 2, 1);
    f.b.AddCycles(7);
    f.b.Exit(kCode + 4, 2);
    REQUIRE(VerifyBlock(f.block).empty());
    *ctx.cyclesExecuted = 100;

    SECTION("budget left") {
        const ExitInfo info = f.Run(kind, 106);
        REQUIRE_FALSE(info.boundary);
        REQUIRE(info.cycles == 12);
        REQUIRE(info.retired == 2);
        REQUIRE(*ctx.PC == kCode + 4);
    }
    SECTION("budget used up") {
        const ExitInfo info = f.Run(kind, 105);
        REQUIRE(info.boundary);
        REQUIRE(info.cycles == 5);
        REQUIRE(info.retired == 1);
        REQUIRE(*ctx.PC == kCode + 2);
    }
    SECTION("interrupt pending and allowed") {
        *ctx.intrPending = true;
        *ctx.intrAllow = true;
        const ExitInfo info = f.Run(kind);
        REQUIRE(info.boundary);
        REQUIRE(info.cycles == 5);
        REQUIRE(*ctx.PC == kCode + 2);
    }
    SECTION("interrupt pending but not allowed") {
        *ctx.intrPending = true;
        *ctx.intrAllow = false;
        const ExitInfo info = f.Run(kind);
        REQUIRE_FALSE(info.boundary);
        REQUIRE(info.cycles == 12);
    }
}

TEST_CASE("Backend: logic, shifts and compares", "[jit][backend]") {
    const BackendKind kind = GENERATE(from_range(sh2test::AvailableBackends()));
    Fixture f;
    auto s = f.rig->BaseState(kCode);
    s.R[1] = 0xF0F0000F;
    s.R[2] = 0x8000FF01;
    f.rig->Load(s);
    const ValueId a = f.b.GetReg(1);
    const ValueId b = f.b.GetReg(2);
    f.b.SetReg(3, f.b.And(a, b));
    f.b.SetReg(4, f.b.Or(a, b));
    f.b.SetReg(5, f.b.Xor(a, b));
    f.b.SetReg(6, f.b.Not(a));
    f.b.SetReg(7, f.b.Shl(b, 4));
    f.b.SetReg(8, f.b.Shr(b, 4));
    f.b.SetReg(9, f.b.Sar(b, 4));
    f.b.SetReg(10, f.b.CmpGtU(b, a)); // 0x8000FF01 > 0xF0F0000F unsigned? no -> 0
    f.b.SetReg(11, f.b.CmpGtS(a, b)); // -252706801 > -2147418367 -> 1
    f.b.SetReg(12, f.b.CmpGeU(a, a)); // 1
    f.b.SetReg(13, f.b.CmpGeS(b, a)); // 0
    f.b.Exit(kCode + 2, 1);
    f.Run(kind);
    const auto st = f.rig->State();
    CHECK(st.R[3] == 0x80000001u);
    CHECK(st.R[4] == 0xF0F0FF0Fu);
    CHECK(st.R[5] == 0x70F0FF0Eu);
    CHECK(st.R[6] == 0x0F0FFFF0u);
    CHECK(st.R[7] == 0x000FF010u);
    CHECK(st.R[8] == 0x08000FF0u);
    CHECK(st.R[9] == 0xF8000FF0u);
    CHECK(st.R[10] == 0u);
    CHECK(st.R[11] == 1u);
    CHECK(st.R[12] == 1u);
    CHECK(st.R[13] == 0u);
}

TEST_CASE("Backend: system registers, MAC and interrupt-allow", "[jit][backend]") {
    const BackendKind kind = GENERATE(from_range(sh2test::AvailableBackends()));
    Fixture f;
    auto s = f.rig->BaseState(kCode);
    s.R[1] = 0x06001234;
    s.SR = 0xF1; // mask 15, T = 1
    s.delaySlotTarget = 0x06003000;
    f.rig->Load(s);
    auto &ctx = f.rig->sh2->GetJitContext();
    const ValueId v = f.b.GetReg(1);
    f.b.SetGBR(v);
    f.b.SetVBR(f.b.Add(v, f.b.Const(4)));
    f.b.SetPR(f.b.Add(v, f.b.Const(8)));
    f.b.SetMACH(f.b.Const(0x11112222));
    f.b.SetMACL(f.b.Const(0x33334444));
    f.b.SetReg(2, f.b.GetGBR());
    f.b.SetReg(3, f.b.GetVBR());
    f.b.SetReg(4, f.b.GetMACH());
    f.b.SetReg(5, f.b.GetMACL());
    f.b.SetReg(6, f.b.GetSR());
    f.b.SetReg(7, f.b.GetDelayTarget());
    f.b.ClearIntrAllow();
    f.b.Exit(kCode + 2, 1);
    f.Run(kind);
    const auto st = f.rig->State();
    CHECK(st.GBR == 0x06001234u);
    CHECK(st.VBR == 0x06001238u);
    CHECK(st.PR == 0x0600123Cu);
    CHECK(st.MACH == 0x11112222u);
    CHECK(st.MACL == 0x33334444u);
    CHECK(st.R[2] == 0x06001234u);
    CHECK(st.R[3] == 0x06001238u);
    CHECK(st.R[4] == 0x11112222u);
    CHECK(st.R[5] == 0x33334444u);
    CHECK(st.R[6] == 0xF1u);
    CHECK(st.R[7] == 0x06003000u);
    CHECK_FALSE(st.intrAllow);
    CHECK_FALSE(*ctx.intrAllow);
}

TEST_CASE("Backend: 32x32 multiplies", "[jit][backend]") {
    const BackendKind kind = GENERATE(from_range(sh2test::AvailableBackends()));
    struct Case {
        uint32_t a, b, lo, hiS, hiU;
    };
    const Case cases[] = {
        {0x80000000u, 0x80000000u, 0x00000000u, 0x40000000u, 0x40000000u},
        {0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000001u, 0x00000000u, 0xFFFFFFFEu},
        {0x12345678u, 0x9ABCDEF0u, 0x242D2080u, 0xF8CC93D6u, 0x0B00EA4Eu},
    };
    for (const Case &c : cases) {
        Fixture f;
        const ValueId a = f.b.Const(c.a);
        const ValueId b = f.b.Const(c.b);
        f.b.SetReg(1, f.b.Mul(a, b));
        f.b.SetReg(2, f.b.MulHiS(a, b));
        f.b.SetReg(3, f.b.MulHiU(a, b));
        f.b.Exit(kCode + 2, 1);
        f.Run(kind);
        const auto st = f.rig->State();
        INFO("a " << c.a << " b " << c.b);
        CHECK(st.R[1] == c.lo);
        CHECK(st.R[2] == c.hiS);
        CHECK(st.R[3] == c.hiU);
    }
}

TEST_CASE("Backend: SetSRBits changes only the masked bits", "[jit][backend]") {
    const BackendKind kind = GENERATE(from_range(sh2test::AvailableBackends()));
    Fixture f;
    auto s = f.rig->BaseState(kCode);
    s.SR = 0x0F0; // ILevel 15, S/Q/M/T clear
    f.rig->Load(s);
    auto &ctx = f.rig->sh2->GetJitContext();
    *ctx.intrAllow = true;
    f.b.SetSRBits(f.b.Const(0xFFFFFFFF), 0x301);
    f.b.Exit(kCode + 2, 1);
    f.Run(kind);
    CHECK(f.rig->State().SR == 0x3F1u); // M, Q, T set; S and ILevel untouched
    CHECK(*ctx.intrAllow);              // no LDC-style side effects

    Fixture g;
    auto s2 = g.rig->BaseState(kCode);
    s2.SR = 0x3F3;
    g.rig->Load(s2);
    g.b.SetSRBits(g.b.Const(0), 0x301);
    g.b.Exit(kCode + 2, 1);
    g.Run(kind);
    CHECK(g.rig->State().SR == 0x0F2u);
}

TEST_CASE("Backend: Div1 applies Div1Step through the context", "[jit][backend]") {
    for (const bool rmIsRn : {false, true}) {
        Fixture f;
        auto s = f.rig->BaseState(kCode);
        s.SR = 0x0F0 | 0x200 | 0x1; // M = 1, Q = 0, T = 1
        s.R[1] = 0x12345678;
        s.R[2] = 0x80000001;
        f.rig->Load(s);
        f.b.SetReg(2, f.b.Div1(f.b.GetReg(2), f.b.GetReg(1), rmIsRn));
        f.b.Exit(kCode + 2, 1);
        f.Run();

        uint32_t sr = s.SR;
        const uint32_t expected = Div1Step(0x80000001, 0x12345678, rmIsRn, sr);
        const auto st = f.rig->State();
        INFO("rmIsRn " << rmIsRn);
        CHECK(st.R[2] == expected);
        CHECK(st.SR == sr);
        CHECK(st.R[1] == 0x12345678u);
    }
}

TEST_CASE("Backend: MacW and MacL apply the helpers to MACH:MACL", "[jit][backend]") {
    for (const bool sBit : {false, true}) {
        for (const bool isLong : {false, true}) {
            Fixture f;
            auto s = f.rig->BaseState(kCode);
            s.SR = 0x0F0 | (sBit ? 0x2u : 0u);
            s.MACH = 0x00007FFF;
            s.MACL = 0x7FFFFFF0;
            f.rig->Load(s);
            const ValueId op1 = f.b.Const(isLong ? 0x7FFFFFFFu : 0x00007FFFu);
            const ValueId op2 = f.b.Const(isLong ? 0x7FFFFFFFu : 0x00007FFFu);
            if (isLong) {
                f.b.MacL(op1, op2);
            } else {
                f.b.MacW(op1, op2);
            }
            f.b.Exit(kCode + 2, 1);
            f.Run();

            const uint64_t mac = 0x00007FFF7FFFFFF0ull;
            const uint64_t expected = isLong ? MacLStep(mac, sBit, 0x7FFFFFFF, 0x7FFFFFFF)
                                             : MacWStep(mac, sBit, 0x7FFF, 0x7FFF);
            const auto st = f.rig->State();
            INFO("S " << sBit << " long " << isLong);
            CHECK(st.MACH == static_cast<uint32_t>(expected >> 32));
            CHECK(st.MACL == static_cast<uint32_t>(expected));
            CHECK(st.SR == s.SR);
        }
    }
}

TEST_CASE("Backend: AddAccessCyclesRMWByte uses the RMW-cycles callback", "[jit][backend]") {
    for (const uint32_t address : {0x26040000u, 0x22000000u}) {
        Fixture f;
        auto &ctx = f.rig->sh2->GetJitContext();
        f.b.AddAccessCyclesRMWByte(f.b.Const(address));
        f.b.AddCycles(4);
        f.b.Exit(kCode + 2, 1);
        const ExitInfo info = f.Run();
        INFO("address " << address);
        CHECK(info.cycles == ctx.accessCyclesRMWByte(ctx.sh2, address) + 4);
    }
}

TEST_CASE("Backend: SetSR masks reserved bits and recomputes interrupt flags", "[jit][backend]") {
    Fixture f;
    auto s = f.rig->BaseState(kCode);
    s.R[1] = 0xFFFFFFFF;
    f.rig->Load(s);
    f.b.SetSR(f.b.GetReg(1), false);
    f.b.SetIntrAllow();
    f.b.Exit(kCode + 2, 1);
    f.Run();
    CHECK(f.rig->State().SR == 0x3F3u);
    CHECK(f.rig->State().intrAllow);
}
