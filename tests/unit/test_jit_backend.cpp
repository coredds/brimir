// Brimir - SH-2 JIT IR interpreter backend tests
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/jit/interp_backend.hpp>
#include <brimir/jit/ir.hpp>

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

    ExitInfo Run() {
        INFO(PrintBlock(block));
        REQUIRE(VerifyBlock(block).empty());
        return RunBlock(block, rig->sh2->GetJitContext());
    }
};

} // namespace

TEST_CASE("Backend: registers, ALU and T bit", "[jit][backend]") {
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

    const ExitInfo info = f.Run();
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
    Fixture f;
    auto &ctx = f.rig->sh2->GetJitContext();
    *ctx.cyclesExecuted = 100;
    f.b.AddCycles(7);
    f.b.SyncCycles();
    f.b.AddCycles(5);
    f.b.Exit(kCode + 2, 1);

    const ExitInfo info = f.Run();
    REQUIRE(info.cycles == 12);
    REQUIRE(*ctx.cyclesExecuted == 107); // the executor, not the backend, publishes the final count
}

TEST_CASE("Backend: WbStall ignores the 'no register' marker", "[jit][backend]") {
    Fixture f; // BaseState sets wbReg = 0xFF
    f.b.WbStall(0xFFFFFFFFu);
    f.b.Exit(kCode + 2, 1);
    REQUIRE(f.Run().cycles == 0);
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
    Fixture f;
    auto &ctx = f.rig->sh2->GetJitContext();
    f.b.AddCycles(5);
    f.b.CheckBoundary(kCode + 2, 1);
    f.b.AddCycles(7);
    f.b.Exit(kCode + 4, 2);
    REQUIRE(VerifyBlock(f.block).empty());
    *ctx.cyclesExecuted = 100;

    SECTION("budget left") {
        const ExitInfo info = RunBlock(f.block, ctx, 106);
        REQUIRE_FALSE(info.boundary);
        REQUIRE(info.cycles == 12);
        REQUIRE(info.retired == 2);
        REQUIRE(*ctx.PC == kCode + 4);
    }
    SECTION("budget used up") {
        const ExitInfo info = RunBlock(f.block, ctx, 105);
        REQUIRE(info.boundary);
        REQUIRE(info.cycles == 5);
        REQUIRE(info.retired == 1);
        REQUIRE(*ctx.PC == kCode + 2);
    }
    SECTION("interrupt pending and allowed") {
        *ctx.intrPending = true;
        *ctx.intrAllow = true;
        const ExitInfo info = RunBlock(f.block, ctx);
        REQUIRE(info.boundary);
        REQUIRE(info.cycles == 5);
        REQUIRE(*ctx.PC == kCode + 2);
    }
    SECTION("interrupt pending but not allowed") {
        *ctx.intrPending = true;
        *ctx.intrAllow = false;
        const ExitInfo info = RunBlock(f.block, ctx);
        REQUIRE_FALSE(info.boundary);
        REQUIRE(info.cycles == 12);
    }
}
