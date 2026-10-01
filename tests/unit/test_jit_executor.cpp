// Brimir - SH-2 JIT executor tests (cache flush semantics)
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/jit/executor.hpp>

#include <memory>

using sh2test::kSleep;
using sh2test::Rig;

namespace {

constexpr uint32_t kCode = 0x06001000;
constexpr uint32_t kData = 0x06040000;

constexpr uint16_t kMovL_R1_R2 = 0x6212; // mov.l @R1,R2
constexpr uint16_t kAdd1_R3 = 0x7301;    // add #1,R3
constexpr uint16_t kMovL_R2_atR1 = 0x2122; // mov.l R2,@R1

// A read callback that requests a flush, like a WDT register access that triggers a watchdog
// reset (SH2::Reset flushes the executor) in the middle of a compiled block.
brimir::jit::Executor *g_exec = nullptr;
uint32 (*g_origRead)(void *, uint32, uint32, bool) = nullptr;

uint32 FlushingRead(void *sh2, uint32 address, uint32 size, bool instrFetch) {
    const uint32 value = g_origRead(sh2, address, size, instrFetch);
    g_exec->Flush();
    return value;
}

void (*g_origWrite)(void *, uint32, uint32, uint32) = nullptr;

void FlushingWrite(void *sh2, uint32 address, uint32 size, uint32 value) {
    g_origWrite(sh2, address, size, value);
    g_exec->Flush();
}

} // namespace

TEST_CASE("Executor: flush during a block is deferred and aborts the block", "[jit][executor]") {
    auto rig = std::make_unique<Rig>();
    brimir::jit::Executor exec;
    rig->WriteCode(kCode, {kMovL_R1_R2, kAdd1_R3, kAdd1_R3, kSleep});
    rig->Write32(kData, 0x12345678);
    auto state = rig->BaseState(kCode);
    state.R[1] = kData;
    state.R[2] = 0;
    state.R[3] = 0;
    rig->Load(state);

    auto &ctx = rig->sh2->GetJitContext();
    g_exec = &exec;
    g_origRead = ctx.read;
    ctx.read = FlushingRead;

    const auto info = exec.Step(ctx);
    ctx.read = g_origRead;
    g_exec = nullptr;

    CHECK(info.aborted);
    CHECK(rig->State().R[3] == 0u); // the rest of the block did not run
    CHECK(rig->State().PC == kCode); // PC is left to whoever requested the flush
    CHECK(exec.Cache().Size() == 0); // the deferred flush was applied after the block

    // The next step recompiles and runs the block normally.
    const auto next = exec.Step(ctx);
    CHECK_FALSE(next.aborted);
    CHECK(next.retired == 3);
    CHECK(rig->State().R[2] == 0x12345678u);
    CHECK(rig->State().R[3] == 2u);
    CHECK(rig->State().PC == kCode + 6);
    CHECK(exec.Cache().Size() == 1);
}

TEST_CASE("Executor: flush outside a block clears the cache immediately", "[jit][executor]") {
    auto rig = std::make_unique<Rig>();
    brimir::jit::Executor exec;
    rig->WriteCode(kCode, {kAdd1_R3, kAdd1_R3, kSleep});
    rig->Load(rig->BaseState(kCode));

    exec.Step(rig->sh2->GetJitContext());
    REQUIRE(exec.Cache().Size() == 1);
    exec.Flush();
    CHECK(exec.Cache().Size() == 0);
}

TEST_CASE("Executor: flush during a store aborts the block after the store", "[jit][executor]") {
    auto rig = std::make_unique<Rig>();
    brimir::jit::Executor exec;
    rig->WriteCode(kCode, {kMovL_R2_atR1, kAdd1_R3, kAdd1_R3, kSleep});
    auto state = rig->BaseState(kCode);
    state.R[1] = kData;
    state.R[2] = 0xA5A5A5A5;
    state.R[3] = 0;
    rig->Load(state);

    auto &ctx = rig->sh2->GetJitContext();
    g_exec = &exec;
    g_origWrite = ctx.write;
    ctx.write = FlushingWrite;
    const auto info = exec.Step(ctx);
    ctx.write = g_origWrite;
    g_exec = nullptr;

    CHECK(info.aborted);
    CHECK(rig->Read32(kData) == 0xA5A5A5A5u); // the store itself completed
    CHECK(rig->State().R[3] == 0u);           // nothing after it ran
    CHECK(rig->State().PC == kCode);
    CHECK(exec.Cache().Size() == 0);

    // Recovery: the next step recompiles and runs the block from the start (re-running the store,
    // here with the same value) through both adds.
    const auto next = exec.Step(ctx);
    CHECK_FALSE(next.aborted);
    CHECK(next.retired == 3);
    CHECK(rig->Read32(kData) == 0xA5A5A5A5u);
    CHECK(rig->State().R[3] == 2u);
    CHECK(rig->State().PC == kCode + 6);
    CHECK(exec.Cache().Size() == 1);
}
