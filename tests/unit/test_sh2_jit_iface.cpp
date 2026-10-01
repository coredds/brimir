// Brimir - SH-2 fork JIT hook tests
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "sh2_test_rig.hpp"

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <memory>

using sh2test::Rig;

namespace {

constexpr uint32_t kCode = 0x06001000;

// Minimal executor: runs every instruction through the interpreter callback.
class DelegatingExecutor final : public ymir::sh2::ISH2Executor {
public:
    uint64 Run(ymir::sh2::SH2JitContext &ctx, uint64 executed, uint64 target) override {
        ++runCalls;
        while (executed < target) {
            *ctx.cyclesExecuted = executed;
            executed += ctx.interpretOne(ctx.sh2);
        }
        return executed;
    }
    void Flush() override {
        ++flushes;
    }

    int runCalls = 0;
    int flushes = 0;
};

// add #1,R0 ; bra kCode ; nop  (infinite loop)
const std::vector<uint16_t> kLoop = {0x7001, 0xAFFD, 0x0009};

} // namespace

TEST_CASE("Advance routes through an attached executor with identical results", "[jit][sh2]") {
    auto ref = std::make_unique<Rig>();
    auto jit = std::make_unique<Rig>();
    ref->WriteCode(kCode, kLoop);
    jit->WriteCode(kCode, kLoop);
    const auto state = ref->BaseState(kCode);
    ref->Load(state);
    jit->Load(state);

    DelegatingExecutor exec;
    jit->sh2->SetJitExecutor(&exec);
    REQUIRE(jit->sh2->GetJitExecutor() == &exec);

    const uint64 refCycles = ref->sh2->Advance<false, false>(1000);
    const uint64 jitCycles = jit->sh2->Advance<false, false>(1000);

    REQUIRE(exec.runCalls == 1);
    REQUIRE(refCycles == jitCycles);
    const std::string diff = sh2test::DiffRigs(*ref, *jit, true);
    INFO(diff);
    REQUIRE(diff.empty());
}

TEST_CASE("Advance with cache emulation ignores the executor", "[jit][sh2]") {
    auto rig = std::make_unique<Rig>();
    rig->WriteCode(kCode, kLoop);
    rig->Load(rig->BaseState(kCode));

    DelegatingExecutor exec;
    rig->sh2->SetJitExecutor(&exec);
    rig->sh2->Advance<false, true>(200);
    rig->sh2->Advance<true, false>(200);
    REQUIRE(exec.runCalls == 0);
}

TEST_CASE("Attaching, Reset and LoadState flush the executor", "[jit][sh2]") {
    auto rig = std::make_unique<Rig>();
    DelegatingExecutor exec;
    rig->sh2->SetJitExecutor(&exec);
    REQUIRE(exec.flushes == 1);
    rig->sh2->Reset(true);
    REQUIRE(exec.flushes == 2);
    rig->Load(rig->BaseState(kCode));
    REQUIRE(exec.flushes == 3);
    rig->sh2->SetJitExecutor(nullptr);
    rig->sh2->Reset(true);
    REQUIRE(exec.flushes == 3);
}

TEST_CASE("JIT context callbacks mirror interpreter memory semantics", "[jit][sh2]") {
    auto rig = std::make_unique<Rig>();
    rig->Load(rig->BaseState(kCode));
    auto &ctx = rig->sh2->GetJitContext();
    REQUIRE(ctx.sh2 == rig->sh2.get());
    REQUIRE(*ctx.PC == kCode);
    REQUIRE(*ctx.fetchedOpcodes == rig->State().fetchedOpcodes);
    REQUIRE(ctx.cyclesExecuted != nullptr);

    rig->Write32(0x06040000, 0x11223344);
    REQUIRE(ctx.read(ctx.sh2, 0x26040000, 4, false) == 0x11223344u);
    REQUIRE(ctx.read(ctx.sh2, 0x26040001, 1, false) == 0x22u);
    REQUIRE(ctx.read(ctx.sh2, 0x26040002, 2, false) == 0x3344u);

    ctx.write(ctx.sh2, 0x26040004, 2, 0xBEEF);
    REQUIRE(rig->Read16(0x06040004) == 0xBEEF);
    ctx.write(ctx.sh2, 0x22000010, 4, 0xCAFEF00D);
    REQUIRE(ctx.read(ctx.sh2, 0x22000010, 4, false) == 0xCAFEF00Du);

    // Wait states: cache-through uses the bus table, cached area costs 1, I/O area costs 4.
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x26040000, 1, false) == 2);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x26040000, 1, true) == 3);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x26040000, 2, false) == 4);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x26040000, 2, true) == 5);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x26040000, 4, false) == 6);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x26040000, 4, true) == 7);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x22000000, 4, true) == 13);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x06040000, 4, false) == 1);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0xFFFFFE10, 4, false) == 4);

    REQUIRE_FALSE(ctx.busWait(ctx.sh2, 0x26040000, 4, false));
    rig->mmio.busWaitEvery = 1;
    REQUIRE(ctx.busWait(ctx.sh2, 0x22000000, 4, false));
    REQUIRE(rig->mmio.busWaitQueries == 1);

    rig->WriteCode(kCode, {0x1234, 0x5678});
    REQUIRE(ctx.peekInstruction(ctx.sh2, kCode + 2) == 0x5678);
    ctx.refillPipeline(ctx.sh2, kCode);
    REQUIRE(rig->State().fetchedOpcodes == 0x12345678u);

    ctx.setupDelaySlot(ctx.sh2, kCode + 0x102);
    REQUIRE(*ctx.delaySlot);
    REQUIRE(*ctx.delaySlotTarget == kCode + 0x102);
    REQUIRE_FALSE(*ctx.intrPending);
    rig->WriteCode(kCode + 0x100, {0xAAAA, 0xBBBB});
    ctx.endDelaySlot(ctx.sh2);
    REQUIRE_FALSE(*ctx.delaySlot);
    REQUIRE(*ctx.PC == kCode + 0x102);
    REQUIRE(rig->State().fetchedOpcodes == 0xAAAABBBBu); // target & 2 -> refill

    // MACH/MACL alias the MAC register halves.
    *ctx.MACH = 0x01234567;
    *ctx.MACL = 0x89ABCDEF;
    REQUIRE(rig->State().MACH == 0x01234567u);
    REQUIRE(rig->State().MACL == 0x89ABCDEFu);

    // setSR performs LDC Rm,SR: reserved bits masked, interrupt-allow cleared.
    ctx.setSR(ctx.sh2, 0xFFFFFFFF, false);
    REQUIRE(rig->State().SR == 0x3F3u);
    REQUIRE_FALSE(rig->State().intrAllow);

    // interpretOne executes exactly one instruction with the interpreter.
    rig->WriteCode(kCode + 0x200, {0x7005}); // add #5,R0
    auto s = rig->BaseState(kCode + 0x200);
    s.R[0] = 1;
    rig->Load(s);
    REQUIRE(ctx.interpretOne(ctx.sh2) == 1);
    REQUIRE(rig->State().R[0] == 6u);
    REQUIRE(*ctx.PC == kCode + 0x202);
}
