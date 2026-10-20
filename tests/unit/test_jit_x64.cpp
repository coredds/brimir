// Brimir - SH-2 JIT x64 native backend tests
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "jit_fuzz_programs.hpp"
#include "jit_random_ir.hpp"
#include "jit_test_backend.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/core_wrapper.hpp>
#include <brimir/jit/backend.hpp>
#include <brimir/jit/executor.hpp>
#include <brimir/jit/interp_backend.hpp>
#include <brimir/jit/ir.hpp>
#include <brimir/jit/ir_opt.hpp>
#include <brimir/jit/sh2_helpers.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <utility>
#include <vector>

using brimir::jit::BackendKind;
using brimir::jit::Block;
using brimir::jit::Builder;
using brimir::jit::ExitInfo;
using brimir::jit::kNoCycleTarget;
using brimir::jit::NativeCode;
using brimir::jit::ValueId;
using sh2test::kSleep;
using sh2test::Rig;

namespace {

constexpr uint32_t kCode = 0x06001000;
constexpr uint16_t kAdd1_R3 = 0x7301;   // add #1,R3
constexpr uint16_t kMov_R3_R4 = 0x6433; // mov R3,R4
constexpr uint16_t kShll_R4 = 0x4400;   // shll R4

// CPU state the native code can see, applied identically to several rigs.
struct CpuSetup {
    ymir::savestate::SH2SaveState state;
    bool intrPending = false;
    uint64_t cycles = 0;

    void Apply(Rig &rig) const {
        rig.Load(state);
        auto &ctx = rig.sh2->GetJitContext();
        *ctx.intrPending = intrPending;
        *ctx.cyclesExecuted = cycles;
    }
};

CpuSetup RandomCpu(const Rig &rig, std::mt19937 &rng) {
    const auto word = [&] { return static_cast<uint32_t>(rng()); };
    const auto below = [&](uint32_t n) { return std::uniform_int_distribution<uint32_t>(0, n - 1)(rng); };
    CpuSetup s;
    s.state = rig.BaseState(kCode);
    for (auto &r : s.state.R) {
        r = word();
    }
    s.state.SR = word() & 0x3F3u;
    s.state.GBR = word();
    s.state.VBR = word();
    s.state.PR = word();
    s.state.MACH = word();
    s.state.MACL = word();
    s.state.delaySlotTarget = word();
    s.state.wbReg = static_cast<uint8_t>(below(5) == 0 ? 0xFF : below(17));
    s.state.intrAllow = below(2) != 0;
    s.intrPending = below(4) == 0;
    s.cycles = below(3) == 0 ? 0 : (static_cast<uint64_t>(word()) << below(24));
    // The timers only count forward from the CPU's cycle count (asserted in AdvanceTo); the rig's
    // timers carry the previous run's count, which may be ahead of the random one.
    s.state.frt.cycleCount = s.cycles;
    s.state.wdt.cycleCount = s.cycles;
    return s;
}

// Compares everything a block can change: ExitInfo, CPU state (DiffRigs) and the flags DiffRigs
// does not cover. Stops the test case at the first difference. comparePeripherals also compares
// timers, DMAC and interrupts (only meaningful when both rigs went through identical steps).
void RequireSameOutcome(const ExitInfo &ir, const ExitInfo &x64, const Rig &irRig, const Rig &x64Rig,
                        bool comparePeripherals = false) {
    REQUIRE(x64.cycles == ir.cycles);
    REQUIRE(x64.retired == ir.retired);
    REQUIRE(x64.busWait == ir.busWait);
    REQUIRE(x64.aborted == ir.aborted);
    REQUIRE(x64.boundary == ir.boundary);
    const std::string diff = sh2test::DiffRigs(irRig, x64Rig, comparePeripherals);
    INFO(diff);
    REQUIRE(diff.empty());
    auto &a = irRig.sh2->GetJitContext();
    auto &b = x64Rig.sh2->GetJitContext();
    REQUIRE(*b.cyclesExecuted == *a.cyclesExecuted);
    REQUIRE(*b.intrPending == *a.intrPending);
    REQUIRE(*b.intrAllow == *a.intrAllow);
    REQUIRE(*b.PC == *a.PC);
    REQUIRE(*b.wbReg == *a.wbReg);
}

} // namespace

TEST_CASE("x64 backend: names, parsing and availability", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    CHECK(std::string(brimir::jit::BackendName(BackendKind::Ir)) == "ir");
    CHECK(std::string(brimir::jit::BackendName(BackendKind::X64)) == "x64");

    BackendKind kind = BackendKind::Ir;
    CHECK(brimir::jit::ParseBackend("x64", kind));
    CHECK(kind == BackendKind::X64);
    CHECK(brimir::jit::ParseBackend("ir", kind));
    CHECK(kind == BackendKind::Ir);
    kind = BackendKind::X64;
    CHECK_FALSE(brimir::jit::ParseBackend("arm64", kind));
    CHECK_FALSE(brimir::jit::ParseBackend("", kind));
    CHECK(kind == BackendKind::X64); // untouched on failure

    CHECK(brimir::jit::IsBackendAvailable(BackendKind::Ir));
    CHECK(brimir::jit::DefaultBackend() == BackendKind::X64);

    const auto available = sh2test::AvailableBackends();
    REQUIRE(available.size() == 2);
    CHECK(available[0] == BackendKind::Ir);
    CHECK(available[1] == BackendKind::X64);
}

TEST_CASE("x64 backend: factory", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    CHECK(brimir::jit::MakeNativeBackend(BackendKind::Ir) == nullptr);
    const auto x64 = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(x64 != nullptr);
    CHECK(x64->Kind() == BackendKind::X64);
    CHECK(x64->CodeBytes() == 0);
    x64->Reset();
    CHECK(x64->CodeBytes() == 0);
}

// The backend compiles every block the front end produces (no fallbacks), so the executor runs this
// one natively.
TEST_CASE("x64 backend: an executor runs native blocks like the interpreter", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    auto ref = std::make_unique<Rig>();
    auto jit = std::make_unique<Rig>();
    brimir::jit::Executor exec{BackendKind::X64, sh2test::kNativeOnFirstRun};
    CHECK(exec.Backend() == BackendKind::X64);

    const std::vector<uint16_t> program{kAdd1_R3, kMov_R3_R4, kShll_R4, static_cast<uint16_t>(kSleep)};
    ref->WriteCode(kCode, program);
    jit->WriteCode(kCode, program);
    auto state = ref->BaseState(kCode);
    state.R[3] = 0x40000000;
    state.R[4] = 0;
    ref->Load(state);
    jit->Load(state);

    const auto info = exec.Step(jit->sh2->GetJitContext());
    REQUIRE(info.retired == 3);
    uint64_t refCycles = 0;
    for (uint32_t i = 0; i < info.retired; ++i) {
        refCycles += ref->sh2->Step<false, false>();
    }
    CHECK(info.cycles == refCycles);
    const std::string diff = sh2test::DiffRigs(*ref, *jit);
    INFO(diff);
    CHECK(diff.empty());
    CHECK(jit->State().R[4] == 0x80000002u);

    const auto &stats = exec.GetStats();
    CHECK(stats.blocksRun == 1);
    CHECK(stats.compileFallbacks == 0);
    CHECK(stats.nativeBlocksRun == 1);
}

TEST_CASE("x64 backend: the native code cap flushes the cache", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr size_t kLimit = 4096;
    constexpr uint32_t kBlocks = 50;
    constexpr uint32_t kStride = 0x40;
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    brimir::jit::BlockCache cache{backend.get(), kLimit, sh2test::kNativeOnFirstRun};

    auto ref = std::make_unique<Rig>();
    auto jit = std::make_unique<Rig>();
    for (uint32_t i = 0; i < kBlocks; ++i) {
        const auto imm = static_cast<uint16_t>(0x7300 | (i + 1)); // add #(i+1),R3
        const std::vector<uint16_t> program{imm,       kMov_R3_R4, kShll_R4, kAdd1_R3, kMov_R3_R4,
                                            kShll_R4,  kAdd1_R3,   kShll_R4, static_cast<uint16_t>(kSleep)};
        ref->WriteCode(kCode + i * kStride, program);
        jit->WriteCode(kCode + i * kStride, program);
    }
    auto state = ref->BaseState(kCode);
    state.R[3] = 0x1234;
    ref->Load(state);
    jit->Load(state);

    auto &ctx = jit->sh2->GetJitContext();
    size_t largest = 0;
    uint32_t flushes = 0;
    // Two passes: the second pass hits blocks still cached and recompiles flushed ones.
    for (uint32_t pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < kBlocks; ++i) {
            const uint32_t pc = kCode + i * kStride;
            INFO("pass " << pass << " block " << i);
            ref->Load(ref->BaseState(pc));
            jit->Load(jit->BaseState(pc));

            const size_t bytesBefore = backend->CodeBytes();
            const uint64_t compilesBefore = cache.Compiles();
            const brimir::jit::CachedBlock &entry = cache.Get(ctx, pc);
            const size_t bytesAfter = backend->CodeBytes();
            if (cache.Compiles() != compilesBefore) {
                const bool flushed = bytesAfter < bytesBefore;
                flushes += flushed ? 1 : 0;
                largest = std::max(largest, flushed ? bytesAfter : bytesAfter - bytesBefore);
            } else {
                CHECK(bytesAfter == bytesBefore);
            }
            CHECK(bytesAfter <= kLimit + largest);
            REQUIRE(entry.code.entry != nullptr);

            const ExitInfo info = backend->Run(entry.code, ctx);
            REQUIRE(info.retired == 8);
            uint64_t refCycles = 0;
            for (uint32_t n = 0; n < info.retired; ++n) {
                refCycles += ref->sh2->Step<false, false>();
            }
            CHECK(info.cycles == refCycles);
            const std::string diff = sh2test::DiffRigs(*ref, *jit);
            INFO(diff);
            REQUIRE(diff.empty());
        }
    }
    CHECK(flushes >= 1u);
    CHECK(cache.Size() < kBlocks);
    CHECK(cache.Invalidations() == 0);
    CHECK(cache.CompileFallbacks() == 0);
    CHECK(cache.FlushesCodeCap() == flushes);
    CHECK(cache.IrEvictions() == 0u);
    CHECK(cache.CachedInsts() == 0u); // every cached block is native
}

TEST_CASE("x64 backend: peak and stale native bytes are counted", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    // No retired pool: the dropped block's code is freed at once (that is what this test pins).
    brimir::jit::BlockCache cache{backend.get(), brimir::jit::kMaxNativeCodeBytes, sh2test::kNativeOnFirstRun,
                                  brimir::jit::kMaxCachedInsts, 0};
    CHECK(cache.PeakNativeCodeBytes() == 0u);
    CHECK(cache.StaleNativeBytes() == 0u);

    auto jit = std::make_unique<Rig>();
    auto &ctx = jit->sh2->GetJitContext();
    jit->WriteCode(kCode, {kAdd1_R3, kMov_R3_R4, kShll_R4, static_cast<uint16_t>(kSleep)});
    jit->Load(jit->BaseState(kCode));

    REQUIRE(cache.Get(ctx, kCode).code.entry != nullptr);
    const size_t firstBytes = backend->CodeBytes();
    REQUIRE(firstBytes > 0u);
    CHECK(cache.PeakNativeCodeBytes() == firstBytes);
    CHECK(cache.StaleNativeBytes() == 0u);

    // One opcode changes. This block validates itself in its prologue, so Get does not look at its
    // guest code; the executor drops it when the prologue reports stale (done here by hand). It is
    // compiled again, and the old code is freed at once, so the backend holds only the new block.
    jit->WriteCode(kCode, {kAdd1_R3, kMov_R3_R4, 0x4401 /* shlr R4 */, static_cast<uint16_t>(kSleep)});
    const brimir::jit::CachedBlock *old = cache.Find(kCode);
    REQUIRE(old != nullptr);
    REQUIRE(old->code.selfValidating);
    CHECK(old->code.bytes == firstBytes);
    cache.Invalidate(kCode);
    CHECK(backend->CodeBytes() == 0u);
    const brimir::jit::CachedBlock &rebuilt = cache.Get(ctx, kCode);
    REQUIRE(rebuilt.code.entry != nullptr);
    CHECK(cache.Invalidations() == 1u);
    CHECK(cache.StaleNativeBytes() == firstBytes);
    CHECK(cache.NativeBytesReleased() == firstBytes);
    CHECK(backend->CodeBytes() == rebuilt.code.bytes);
    CHECK(cache.PeakNativeCodeBytes() == std::max<size_t>(firstBytes, rebuilt.code.bytes));

    const size_t peak = cache.PeakNativeCodeBytes();
    cache.Flush();
    CHECK(backend->CodeBytes() == 0u);
    CHECK(cache.PeakNativeCodeBytes() == peak);
    CHECK(cache.StaleNativeBytes() == firstBytes);
}

// Dropped blocks give their code back at once, so rewriting one block over and over never fills the
// code cap (without it, every rewrite adds a block's code until the next flush).
TEST_CASE("x64 backend: invalidated blocks free their code", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint32_t kRewrites = 500;
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    // No retired pool (it would keep and reuse the two programs' code instead of freeing it).
    brimir::jit::BlockCache cache{backend.get(), 4096, sh2test::kNativeOnFirstRun, brimir::jit::kMaxCachedInsts, 0};

    auto ref = std::make_unique<Rig>();
    auto jit = std::make_unique<Rig>();
    auto &ctx = jit->sh2->GetJitContext();
    for (uint32_t i = 0; i < kRewrites; ++i) {
        INFO("rewrite " << i);
        const auto imm = static_cast<uint16_t>(0x7300 | (1 + i % 2)); // add #1,R3 / add #2,R3
        const std::vector<uint16_t> program{imm, kMov_R3_R4, kShll_R4, static_cast<uint16_t>(kSleep)};
        ref->WriteCode(kCode, program);
        jit->WriteCode(kCode, program);
        auto state = ref->BaseState(kCode);
        state.R[3] = 0x1234 + i;
        ref->Load(state);
        jit->Load(state);

        const brimir::jit::CachedBlock *entry = &cache.Get(ctx, kCode);
        REQUIRE(entry->code.entry != nullptr);
        CHECK(backend->CodeBytes() == entry->code.bytes);
        ExitInfo info = backend->Run(entry->code, ctx);
        if (i > 0) {
            // The old block validates itself in its prologue, so Get returned it: it reports stale
            // and is dropped, as Executor::StepImpl does.
            REQUIRE(info.stale);
            cache.Invalidate(kCode);
            entry = &cache.Get(ctx, kCode);
            REQUIRE(entry->code.entry != nullptr);
            CHECK(backend->CodeBytes() == entry->code.bytes);
            info = backend->Run(entry->code, ctx);
        }
        REQUIRE_FALSE(info.stale);
        REQUIRE(info.retired == 3); // SLEEP ends the block without retiring
        uint64_t refCycles = 0;
        for (uint32_t n = 0; n < info.retired; ++n) {
            refCycles += ref->sh2->Step<false, false>();
        }
        CHECK(info.cycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*ref, *jit);
        INFO(diff);
        REQUIRE(diff.empty());
    }
    CHECK(cache.FlushesCodeCap() == 0u);
    CHECK(cache.Invalidations() == kRewrites - 1);
    CHECK(cache.StaleNativeBytes() > 0u);
    CHECK(cache.NativeBytesReleased() == cache.StaleNativeBytes());
}

namespace {

// add #imm,R3 ; mov R3,R4 ; shll R4 ; sleep (imm 1-127: distinct programs of the same shape).
std::vector<uint16_t> AddProgram(uint32_t imm) {
    return {static_cast<uint16_t>(0x7300 | (imm & 0x7F)), kMov_R3_R4, kShll_R4, static_cast<uint16_t>(kSleep)};
}

// An interpreter rig and an x64 rig with the same code. Step runs the block at pc on the x64 rig as
// Executor::StepImpl does (a native block that reports stale is dropped and pc is looked up again,
// which must not be stale) and the same instructions on the interpreter rig, and requires the same
// cycles and state.
struct ReusePair {
    std::unique_ptr<Rig> ref = std::make_unique<Rig>();
    std::unique_ptr<Rig> jit = std::make_unique<Rig>();

    void WriteCode(uint32_t address, const std::vector<uint16_t> &words) {
        ref->WriteCode(address, words);
        jit->WriteCode(address, words);
    }

    const brimir::jit::CachedBlock &Step(brimir::jit::BlockCache &cache, brimir::jit::INativeBackend &backend,
                                         uint32_t pc) {
        auto refState = ref->BaseState(pc);
        auto jitState = jit->BaseState(pc);
        refState.R[3] = jitState.R[3] = 0x1234;
        ref->Load(refState);
        jit->Load(jitState);
        auto &ctx = jit->sh2->GetJitContext();
        const auto run = [&](const brimir::jit::CachedBlock &entry) {
            return entry.code.entry != nullptr ? backend.Run(entry.code, ctx) : brimir::jit::RunBlock(entry.block, ctx);
        };
        const brimir::jit::CachedBlock *entry = &cache.Get(ctx, pc);
        ExitInfo info = run(*entry);
        if (info.stale) {
            cache.Invalidate(pc);
            entry = &cache.Get(ctx, pc);
            info = run(*entry);
            REQUIRE_FALSE(info.stale);
        }
        REQUIRE(info.retired == 3);
        uint64_t refCycles = 0;
        for (uint32_t n = 0; n < info.retired; ++n) {
            refCycles += ref->sh2->Step<false, false>();
        }
        REQUIRE(info.cycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*ref, *jit);
        INFO(diff);
        REQUIRE(diff.empty());
        return *entry;
    }
};

} // namespace

// A self-validating block dropped because its code changed is retired with its native code. When
// the same opcodes come back at its PC (on the same pages), that code is taken back without a
// front-end build or a native compile.
TEST_CASE("Block cache: code that comes back reuses its native code", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    brimir::jit::BlockCache cache{backend.get(), brimir::jit::kMaxNativeCodeBytes, sh2test::kNativeOnFirstRun};
    ReusePair p;

    p.WriteCode(kCode, AddProgram(1)); // P
    const brimir::jit::CachedBlock &first = p.Step(cache, *backend, kCode);
    REQUIRE(first.code.entry != nullptr);
    REQUIRE(first.code.selfValidating);
    const void *const firstEntry = first.code.entry;
    const uint32_t firstBytes = first.code.bytes;

    p.WriteCode(kCode, AddProgram(2)); // Q: P reports stale and is retired
    const brimir::jit::CachedBlock &q = p.Step(cache, *backend, kCode);
    REQUIRE(q.code.entry != nullptr);
    REQUIRE(q.code.entry != firstEntry);
    const uint32_t qBytes = q.code.bytes;
    CHECK(cache.RetiredReuses() == 0u);
    CHECK(cache.RetiredCodeBytes() == firstBytes);
    CHECK(backend->CodeBytes() == firstBytes + qBytes);

    p.WriteCode(kCode, AddProgram(1)); // P again: Q is retired, P comes back
    const brimir::jit::CachedBlock &back = p.Step(cache, *backend, kCode);
    CHECK(back.code.entry == firstEntry);
    CHECK(cache.Find(kCode) == &back);
    CHECK(cache.Compiles() == 2u);
    CHECK(cache.NativeCompiles() == 2u);
    CHECK(cache.RetiredReuses() == 1u);
    CHECK(cache.RetiredCodeBytes() == qBytes);
    CHECK(backend->CodeBytes() == firstBytes + qBytes);
    CHECK(cache.Invalidations() == 2u);
    CHECK(cache.StaleNativeBytes() == firstBytes + qBytes);
    CHECK(cache.NativeBytesReleased() == 0u);

    // The reused block is cached again: the next run is a hit.
    const brimir::jit::CachedBlock &hit = p.Step(cache, *backend, kCode);
    CHECK(&hit == &back);
    CHECK(cache.Compiles() == 2u);
    CHECK(cache.RetiredReuses() == 1u);
}

// The prologue checks the code pages' arrays as well as the bytes, so a retired block is only
// reused on the arrays it was compiled for: otherwise it would report stale at once, right after
// being returned for a rebuild (the executor asserts that never happens).
TEST_CASE("Block cache: retired code is not reused on a remapped page", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    auto ref = std::make_unique<Rig>();
    auto jit = std::make_unique<Rig>();
    brimir::jit::Executor exec{BackendKind::X64, sh2test::kNativeOnFirstRun};
    const auto step = [&] {
        auto refState = ref->BaseState(kCode);
        auto jitState = jit->BaseState(kCode);
        refState.R[3] = jitState.R[3] = 0x1234;
        ref->Load(refState);
        jit->Load(jitState);
        const ExitInfo info = exec.Step(jit->sh2->GetJitContext());
        REQUIRE(info.retired == 3);
        uint64_t refCycles = 0;
        for (uint32_t n = 0; n < info.retired; ++n) {
            refCycles += ref->sh2->Step<false, false>();
        }
        REQUIRE(info.cycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*ref, *jit);
        INFO(diff);
        REQUIRE(diff.empty());
    };
    const auto program = AddProgram(1);
    for (Rig *rig : {ref.get(), jit.get()}) {
        rig->WriteCode(kCode, program);
    }
    step(); // P on the RAM array
    for (Rig *rig : {ref.get(), jit.get()}) {
        rig->WriteCode(kCode, AddProgram(2));
    }
    step(); // Q: P reports stale and is retired
    REQUIRE(exec.GetStats().staleEntries == 1u);
    REQUIRE(exec.Cache().RetiredCodeBytes() > 0u);

    // The code's page now maps a second array holding P: same bytes, different array.
    constexpr uint32_t kPage = kCode & 0x7FF0000u;
    std::vector<std::unique_ptr<std::array<uint8_t, 0x10000>>> pages;
    for (Rig *rig : {ref.get(), jit.get()}) {
        auto &page = pages.emplace_back(std::make_unique<std::array<uint8_t, 0x10000>>());
        std::copy_n(rig->ram->begin() + (kPage & 0xF0000u), 0x10000, page->begin());
        for (size_t i = 0; i < program.size(); ++i) {
            const uint32_t off = (kCode & 0xFFFFu) + static_cast<uint32_t>(i * 2);
            (*page)[off] = static_cast<uint8_t>(program[i] >> 8);
            (*page)[off + 1] = static_cast<uint8_t>(program[i]);
        }
        rig->bus.MapArray(kPage, kPage + 0xFFFF, *page, true);
    }
    step(); // Q reports stale (its page changed); P is built again, not taken from the pool
    const auto &stats = exec.GetStats();
    CHECK(stats.staleEntries == 2u);
    CHECK(stats.interpreted == 0u);
    CHECK(exec.Cache().RetiredReuses() == 0u);
    CHECK(exec.Cache().Compiles() == 3u);
    CHECK(exec.Cache().NativeCompiles() == 3u);
    const brimir::jit::CachedBlock *rebuilt = exec.Cache().Find(kCode);
    REQUIRE(rebuilt != nullptr);
    CHECK(rebuilt->code.entry != nullptr);

    step(); // the rebuilt P is a hit and not stale
    CHECK(stats.staleEntries == 2u);
    CHECK(exec.Cache().Compiles() == 3u);
}

TEST_CASE("Block cache: the retired pool is bounded", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    using brimir::jit::kMaxRetiredPerPc;
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    const auto liveBytes = [](const brimir::jit::BlockCache &cache, std::initializer_list<uint32_t> pcs) {
        size_t bytes = 0;
        for (const uint32_t pc : pcs) {
            if (const brimir::jit::CachedBlock *entry = cache.Find(pc); entry != nullptr) {
                bytes += entry->code.bytes;
            }
        }
        return bytes;
    };

    SECTION("per PC") {
        brimir::jit::BlockCache cache{backend.get(), brimir::jit::kMaxNativeCodeBytes, sh2test::kNativeOnFirstRun};
        ReusePair p;
        std::vector<uint32_t> bytes;
        // kMaxRetiredPerPc + 2 programs: the last one retires kMaxRetiredPerPc + 1 blocks.
        for (uint32_t i = 0; i < kMaxRetiredPerPc + 2; ++i) {
            INFO("program " << i);
            p.WriteCode(kCode, AddProgram(i + 1));
            bytes.push_back(p.Step(cache, *backend, kCode).code.bytes);
            const size_t retired = std::min<size_t>(i, kMaxRetiredPerPc);
            size_t expected = 0;
            for (size_t k = i - retired; k < i; ++k) {
                expected += bytes[k];
            }
            CHECK(cache.RetiredCodeBytes() == expected);
            CHECK(cache.RetiredEvictions() == (i > kMaxRetiredPerPc ? 1u : 0u));
            CHECK(backend->CodeBytes() == liveBytes(cache, {kCode}) + cache.RetiredCodeBytes());
        }
        CHECK(cache.NativeBytesReleased() == bytes[0]); // the oldest was released
        CHECK(cache.RetiredReuses() == 0u);
        CHECK(cache.Compiles() == kMaxRetiredPerPc + 2);
    }

    SECTION("in total") {
        constexpr uint32_t kPcs[] = {kCode, kCode + 0x100, kCode + 0x200};
        size_t blockBytes = 0;
        {
            brimir::jit::BlockCache probe{backend.get(), brimir::jit::kMaxNativeCodeBytes, sh2test::kNativeOnFirstRun};
            ReusePair p;
            p.WriteCode(kCode, AddProgram(1));
            blockBytes = p.Step(probe, *backend, kCode).code.bytes;
            probe.Flush();
        }
        REQUIRE(blockBytes > 0u);
        const size_t maxRetired = 2 * blockBytes;
        brimir::jit::BlockCache cache{backend.get(), brimir::jit::kMaxNativeCodeBytes, sh2test::kNativeOnFirstRun,
                                      brimir::jit::kMaxCachedInsts, maxRetired};
        ReusePair p;
        for (uint32_t round = 0; round < 3; ++round) {
            for (const uint32_t pc : kPcs) {
                INFO("round " << round << " pc 0x" << std::hex << pc);
                p.WriteCode(pc, AddProgram(round + 1));
                p.Step(cache, *backend, pc);
                CHECK(cache.RetiredCodeBytes() <= maxRetired);
                CHECK(backend->CodeBytes() == liveBytes(cache, {kPcs[0], kPcs[1], kPcs[2]}) + cache.RetiredCodeBytes());
            }
        }
        // Six blocks retired; at most two fit.
        CHECK(cache.Invalidations() == 6u);
        CHECK(cache.RetiredEvictions() >= 4u);
        CHECK(cache.RetiredCodeBytes() > 0u);
        CHECK(cache.NativeBytesReleased() + cache.RetiredCodeBytes() == cache.StaleNativeBytes());
    }
}

// The pool counts toward the code cap. Reaching the cap at a tiered compile releases the retired
// code first and flushes only if the live blocks alone are still at the cap; the block being
// compiled is never in the pool, so it is compiled in place.
TEST_CASE("Block cache: the code cap empties the pool before flushing", "[jit][x64][tier]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr size_t kLimit = 4096;
    constexpr uint32_t kStride = 0x40;
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    brimir::jit::BlockCache cache{backend.get(), kLimit, 2};
    ReusePair p;

    // Retired blocks: P runs twice (native on its second run), then Q replaces it (IR-only).
    uint32_t r = 0;
    for (; cache.RetiredCodeBytes() < kLimit / 2; ++r) {
        INFO("retire " << r);
        REQUIRE(r < 64);
        const uint32_t pc = kCode + r * kStride;
        p.WriteCode(pc, AddProgram(r + 1));
        p.Step(cache, *backend, pc);
        REQUIRE(p.Step(cache, *backend, pc).code.entry != nullptr);
        p.WriteCode(pc, AddProgram(r + 2));
        REQUIRE(p.Step(cache, *backend, pc).code.entry == nullptr);
    }
    REQUIRE(cache.FlushesCodeCap() == 0u);
    REQUIRE(cache.RetiredEvictions() == 0u);
    REQUIRE(backend->CodeBytes() == cache.RetiredCodeBytes());

    // New blocks, each native on its second run, until the cap is reached twice.
    bool emptied = false;
    bool flushed = false;
    for (uint32_t n = 0; !flushed; ++n) {
        INFO("new block " << n);
        REQUIRE(n < 100);
        const uint32_t pc = kCode + 0x4000 + n * kStride;
        p.WriteCode(pc, AddProgram(n + 1));
        REQUIRE(p.Step(cache, *backend, pc).code.entry == nullptr);
        const size_t bytesBefore = backend->CodeBytes();
        const size_t retiredBefore = cache.RetiredCodeBytes();
        const size_t sizeBefore = cache.Size();
        const brimir::jit::CachedBlock &entry = p.Step(cache, *backend, pc); // the tiered compile
        REQUIRE(entry.code.entry != nullptr);
        if (bytesBefore < kLimit) {
            CHECK(cache.FlushesCodeCap() == 0u);
            CHECK(cache.RetiredCodeBytes() == retiredBefore);
            CHECK(backend->CodeBytes() == bytesBefore + entry.code.bytes);
        } else if (!emptied) {
            // Releasing the pool makes room: nothing is flushed.
            emptied = true;
            REQUIRE(retiredBefore > 0u);
            REQUIRE(bytesBefore - retiredBefore < kLimit);
            CHECK(cache.FlushesCodeCap() == 0u);
            CHECK(cache.RetiredCodeBytes() == 0u);
            CHECK(cache.NativeBytesReleased() == retiredBefore);
            CHECK(cache.Size() == sizeBefore);
            CHECK(cache.Find(pc) == &entry);
            CHECK(backend->CodeBytes() == bytesBefore - retiredBefore + entry.code.bytes);
        } else {
            // Live code alone is at the cap: flushed as before, and the block rebuilt and compiled.
            flushed = true;
            CHECK(retiredBefore == 0u);
            CHECK(cache.FlushesCodeCap() == 1u);
            CHECK(cache.Size() == 1u);
            CHECK(cache.Find(pc) == &entry);
            CHECK(backend->CodeBytes() == entry.code.bytes);
        }
    }
    CHECK(emptied);
    CHECK(cache.RetiredEvictions() == 0u);
    CHECK(cache.CompileFallbacks() == 0u);
}

// With tiered compilation the cap is checked before each native compile, which happens on a
// block's second run here. Reaching it there flushes the cache, including the block being
// compiled, which is rebuilt from scratch and compiled.
TEST_CASE("x64 backend: the native code cap flushes at a tiered compile", "[jit][x64][tier]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr size_t kLimit = 4096;
    constexpr uint32_t kBlocks = 50;
    constexpr uint32_t kStride = 0x40;
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    brimir::jit::BlockCache cache{backend.get(), kLimit, 2};

    auto ref = std::make_unique<Rig>();
    auto jit = std::make_unique<Rig>();
    for (uint32_t i = 0; i < kBlocks; ++i) {
        const auto imm = static_cast<uint16_t>(0x7300 | (i + 1)); // add #(i+1),R3
        const std::vector<uint16_t> program{imm,       kMov_R3_R4, kShll_R4, kAdd1_R3, kMov_R3_R4,
                                            kShll_R4,  kAdd1_R3,   kShll_R4, static_cast<uint16_t>(kSleep)};
        ref->WriteCode(kCode + i * kStride, program);
        jit->WriteCode(kCode + i * kStride, program);
    }
    auto state = ref->BaseState(kCode);
    state.R[3] = 0x1234;
    ref->Load(state);
    jit->Load(state);

    auto &ctx = jit->sh2->GetJitContext();
    uint32_t irRuns = 0;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < kBlocks; ++i) {
            const uint32_t pc = kCode + i * kStride;
            for (uint32_t run = 0; run < 2; ++run) {
                INFO("pass " << pass << " block " << i << " run " << run);
                ref->Load(ref->BaseState(pc));
                jit->Load(jit->BaseState(pc));
                const uint64_t compilesBefore = cache.Compiles();
                const brimir::jit::CachedBlock &entry = cache.Get(ctx, pc);
                ExitInfo info;
                if (run == 0 && cache.Compiles() != compilesBefore) {
                    // Built now (first pass, or flushed since): IR-only, not counted as native.
                    REQUIRE(entry.code.entry == nullptr);
                    REQUIRE_FALSE(entry.block.code.empty());
                    CHECK(cache.CachedInsts() == entry.block.code.size());
                    info = brimir::jit::RunBlock(entry.block, ctx);
                    ++irRuns;
                } else {
                    REQUIRE(entry.code.entry != nullptr);
                    CHECK(entry.block.code.empty());
                    CHECK(cache.CachedInsts() == 0u);
                    info = backend->Run(entry.code, ctx);
                }
                REQUIRE(info.retired == 8);
                uint64_t refCycles = 0;
                for (uint32_t n = 0; n < info.retired; ++n) {
                    refCycles += ref->sh2->Step<false, false>();
                }
                CHECK(info.cycles == refCycles);
                const std::string diff = sh2test::DiffRigs(*ref, *jit);
                INFO(diff);
                REQUIRE(diff.empty());
            }
        }
    }
    CHECK(irRuns >= kBlocks + 1); // pass 1 rebuilt at least one flushed block
    CHECK(cache.FlushesCodeCap() >= 1u);
    CHECK(cache.IrEvictions() == 0u);
    CHECK(cache.Size() < kBlocks);
    CHECK(cache.NativeCompiles() >= kBlocks + 1);
    CHECK(cache.CompileFallbacks() == 0);
}

// Reaching the IR cap evicts only IR-only blocks: native blocks keep their code and keep running
// natively without a recompile, evicted blocks are rebuilt from scratch on their next run, and
// the IR accounting covers exactly the IR-only blocks still cached.
TEST_CASE("x64 backend: the IR cap evicts only IR-only blocks", "[jit][x64][tier]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint32_t kBlocks = 30;
    constexpr uint32_t kHot = 2; // blocks 0 and 1 become native
    constexpr uint32_t kStride = 0x40;
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);

    auto ref = std::make_unique<Rig>();
    auto jit = std::make_unique<Rig>();
    for (uint32_t i = 0; i < kBlocks; ++i) {
        const auto imm = static_cast<uint16_t>(0x7300 | (i + 1)); // add #(i+1),R3
        const std::vector<uint16_t> program{imm,       kMov_R3_R4, kShll_R4, kAdd1_R3, kMov_R3_R4,
                                            kShll_R4,  kAdd1_R3,   kShll_R4, static_cast<uint16_t>(kSleep)};
        ref->WriteCode(kCode + i * kStride, program);
        jit->WriteCode(kCode + i * kStride, program);
    }
    auto state = ref->BaseState(kCode);
    state.R[3] = 0x1234;
    ref->Load(state);
    jit->Load(state);
    auto &ctx = jit->sh2->GetJitContext();

    // A cap of about three blocks' IR.
    const size_t blockInsts = brimir::jit::BuildBlock(ctx, kCode).code.size();
    REQUIRE(blockInsts > 0u);
    const size_t cap = 3 * blockInsts;
    brimir::jit::BlockCache cache{backend.get(), brimir::jit::kMaxNativeCodeBytes, 2, cap};

    // Runs pc through the cache (native or RunBlock) and compares it with the interpreter.
    const auto runAt = [&](uint32_t pc) -> const brimir::jit::CachedBlock & {
        ref->Load(ref->BaseState(pc));
        jit->Load(jit->BaseState(pc));
        const brimir::jit::CachedBlock &entry = cache.Get(ctx, pc);
        const ExitInfo info =
            entry.code.entry != nullptr ? backend->Run(entry.code, ctx) : brimir::jit::RunBlock(entry.block, ctx);
        REQUIRE(info.retired == 8);
        uint64_t refCycles = 0;
        for (uint32_t n = 0; n < info.retired; ++n) {
            refCycles += ref->sh2->Step<false, false>();
        }
        CHECK(info.cycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*ref, *jit);
        INFO(diff);
        REQUIRE(diff.empty());
        return entry;
    };
    // The IR accounting is the IR of the IR-only blocks still cached.
    const auto checkAccounting = [&] {
        size_t irInsts = 0;
        for (uint32_t i = 0; i < kBlocks; ++i) {
            if (const brimir::jit::CachedBlock *e = cache.Find(kCode + i * kStride); e != nullptr) {
                irInsts += e->code.entry == nullptr ? e->block.code.size() : 0;
                CHECK((e->code.entry == nullptr) != e->block.code.empty());
            }
        }
        CHECK(cache.CachedInsts() == irInsts);
        CHECK(cache.CachedInsts() <= cap + blockInsts);
    };

    const void *hotCode[kHot] = {};
    for (uint32_t i = 0; i < kHot; ++i) {
        runAt(kCode + i * kStride);
        hotCode[i] = runAt(kCode + i * kStride).code.entry; // second run: native
        REQUIRE(hotCode[i] != nullptr);
    }
    REQUIRE(cache.NativeCompiles() == kHot);
    CHECK(cache.CachedInsts() == 0u);

    // Cold blocks, one run each: IR-only, so the cap is reached and evicts them repeatedly.
    for (uint32_t i = kHot; i < kBlocks; ++i) {
        INFO("cold block " << i);
        const uint64_t evictionsBefore = cache.IrEvictions();
        const brimir::jit::CachedBlock &entry = runAt(kCode + i * kStride);
        CHECK(entry.code.entry == nullptr);
        if (cache.IrEvictions() != evictionsBefore) {
            // Every earlier cold block is gone; only this one counts.
            for (uint32_t k = kHot; k < i; ++k) {
                CHECK(cache.Find(kCode + k * kStride) == nullptr);
            }
            CHECK(cache.CachedInsts() == entry.block.code.size());
        }
        for (uint32_t h = 0; h < kHot; ++h) {
            const brimir::jit::CachedBlock *hot = cache.Find(kCode + h * kStride);
            REQUIRE(hot != nullptr);
            CHECK(hot->code.entry == hotCode[h]);
        }
        checkAccounting();
    }
    CHECK(cache.IrEvictions() >= 5u);
    CHECK(cache.IrEvictedBlocks() >= 3 * cache.IrEvictions());
    CHECK(cache.Size() < kBlocks);

    // The native blocks still run natively, with the same code, without a rebuild.
    const uint64_t compilesBefore = cache.Compiles();
    for (uint32_t h = 0; h < kHot; ++h) {
        CHECK(runAt(kCode + h * kStride).code.entry == hotCode[h]);
    }
    CHECK(cache.Compiles() == compilesBefore);
    CHECK(cache.NativeCompiles() == kHot);

    // An evicted block (its recent slot cleared too) is rebuilt from scratch as IR.
    REQUIRE(cache.Find(kCode + kHot * kStride) == nullptr);
    const brimir::jit::CachedBlock &rebuilt = runAt(kCode + kHot * kStride);
    CHECK(rebuilt.code.entry == nullptr);
    CHECK(cache.Compiles() == compilesBefore + 1);
    checkAccounting();
    CHECK(cache.FlushesCodeCap() == 0u);
    CHECK(cache.FlushesRequested() == 0u);
    CHECK(cache.CompileFallbacks() == 0u);
}

// With tiered compilation, a native block whose successor is still IR-only returns to the
// executor instead of chaining (only native blocks are in the link table); once both are native
// they chain.
TEST_CASE("x64 does not chain to an IR-only block", "[jit][x64][tier]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint16_t kAdd1_R4 = 0x7401;
    constexpr uint16_t kNopOp = 0x0009;
    const auto bra = [](uint32_t at, uint32_t target) {
        const int32_t disp = (static_cast<int32_t>(target) - static_cast<int32_t>(at) - 4) / 2;
        return static_cast<uint16_t>(0xA000 | (static_cast<uint32_t>(disp) & 0xFFF));
    };
    const uint32_t b = kCode + 0x20;
    auto rig = std::make_unique<Rig>();
    brimir::jit::Executor exec{BackendKind::X64, 3};
    // A: add #1,R3 ; bra B ; nop      B: add #1,R4 ; bra A ; nop
    rig->WriteCode(kCode, {kAdd1_R3, bra(kCode + 2, b), kNopOp});
    rig->WriteCode(b, {kAdd1_R4, bra(b + 2, kCode), kNopOp});
    auto state = rig->BaseState(kCode);
    state.R[3] = 0;
    state.R[4] = 0;
    rig->Load(state);
    auto &ctx = rig->sh2->GetJitContext();

    // Two IR runs of each (Step never chains).
    uint64_t cyclesA = 0;
    uint64_t cyclesB = 0;
    for (int i = 0; i < 2; ++i) {
        const ExitInfo a = exec.Step(ctx);
        REQUIRE(a.retired == 3);
        REQUIRE(*ctx.PC == b);
        const ExitInfo bb = exec.Step(ctx);
        REQUIRE(bb.retired == 3);
        REQUIRE(*ctx.PC == kCode);
        cyclesA = a.cycles;
        cyclesB = bb.cycles;
    }
    REQUIRE(exec.GetStats().nativeBlocksRun == 0u);

    // A's third run is native; B is still IR-only, so A returns to the executor, whose next step
    // compiles B (its third run). B then stops at the budget instead of chaining back to A.
    *ctx.cyclesExecuted = 0;
    CHECK(exec.Run(ctx, 0, cyclesA + cyclesB) == cyclesA + cyclesB);
    CHECK(exec.GetStats().nativeBlocksRun == 2u);
    CHECK(exec.GetStats().chainedBlocks == 0u);
    CHECK(exec.GetStats().blocksRun == 6u);
    CHECK(*ctx.PC == kCode);
    CHECK(rig->State().R[3] == 3u);
    CHECK(rig->State().R[4] == 3u);
    REQUIRE(exec.Cache().Find(kCode)->code.entry != nullptr);
    REQUIRE(exec.Cache().Find(b)->code.entry != nullptr);

    // Both native: A chains to B, and B to A.
    exec.Run(ctx, 0, 2 * (cyclesA + cyclesB));
    CHECK(exec.GetStats().chainedBlocks == 3u);
    CHECK(exec.GetStats().nativeBlocksRun == 6u);
    CHECK(rig->State().R[3] == 5u);
    CHECK(rig->State().R[4] == 5u);
    CHECK(exec.Cache().NativeCompiles() == 2u);
}

TEST_CASE("x64 backend: CoreWrapper backend selection recreates the executors", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    brimir::CoreWrapper core;
    CHECK(core.GetSH2JitBackend() == brimir::jit::DefaultBackend());
    core.SetSH2JitBackend(BackendKind::Ir);
    core.SetSH2JitEnabled(true);
    REQUIRE(core.Initialize());
    auto *saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    REQUIRE(core.GetSH2JitExecutor(true) != nullptr);
    CHECK(core.GetSH2JitExecutor(true)->Backend() == BackendKind::Ir);
    CHECK(core.GetSH2JitExecutor(false)->Backend() == BackendKind::Ir);

    core.SetSH2JitBackend(BackendKind::X64);
    CHECK(core.GetSH2JitBackend() == BackendKind::X64);
    REQUIRE(core.GetSH2JitExecutor(true) != nullptr);
    REQUIRE(core.GetSH2JitExecutor(false) != nullptr);
    CHECK(core.GetSH2JitExecutor(true)->Backend() == BackendKind::X64);
    CHECK(core.GetSH2JitExecutor(false)->Backend() == BackendKind::X64);
    CHECK(saturn->masterSH2.GetJitExecutor() == core.GetSH2JitExecutor(true));
    CHECK(saturn->slaveSH2.GetJitExecutor() == core.GetSH2JitExecutor(false));

    // With the JIT off, changing the backend drops the executors and leaves the CPUs detached.
    core.SetSH2JitEnabled(false);
    core.SetSH2JitBackend(BackendKind::Ir);
    CHECK(core.GetSH2JitExecutor(true) == nullptr);
    CHECK(saturn->masterSH2.GetJitExecutor() == nullptr);
    core.SetSH2JitEnabled(true);
    REQUIRE(core.GetSH2JitExecutor(true) != nullptr);
    CHECK(core.GetSH2JitExecutor(true)->Backend() == BackendKind::Ir);
}

TEST_CASE("x64 backend: an IR executor runs without a native backend", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    brimir::jit::Executor exec{BackendKind::Ir};
    CHECK(exec.Backend() == BackendKind::Ir);
    CHECK(exec.GetStats().compileFallbacks == 0);
}

namespace {

// Guest registers as callbacks see them. With the register cache (x64_emitter.cpp) the generated
// code keeps R0-R15 and SR in host registers and must store them before every call out; these
// hooks record R0-R15 and SR from memory when a callback is entered. Only calls that both backends
// make are recorded: RunBlock calls back for every access, x64 only off array pages, so reads,
// writes and refills are recorded for the MMIO page and the I/O area, bus-wait queries for the
// MMIO page (x64 answers array pages, and any page whose entry has an array, inline), and every
// setSR and RMW-cycle lookup.
struct CallView {
    char kind;
    uint32_t address;
    std::array<uint32_t, 17> regs; // R0-R15, SR
    bool operator==(const CallView &) const = default;
};

struct CallRecorder {
    void *sh2 = nullptr;
    const ymir::sh2::SH2JitContext *ctx = nullptr;
    std::vector<CallView> log;
};

std::vector<CallRecorder *> g_recorders;
ymir::sh2::SH2JitContext g_recOrig; // the original callbacks (the same for every rig)

void RecordCall(void *sh2, char kind, uint32_t address) {
    for (CallRecorder *r : g_recorders) {
        if (r->sh2 == sh2) {
            CallView view{kind, address, {}};
            for (int i = 0; i < 16; ++i) {
                view.regs[i] = r->ctx->R[i];
            }
            view.regs[16] = *r->ctx->SR;
            r->log.push_back(view);
        }
    }
}

bool CalledByBoth(uint32 address) {
    return (address >> 24) == 0x22 || (address >> 29) == 0b111;
}

uint32 RecRead(void *sh2, uint32 address, uint32 size, bool instrFetch) {
    if (CalledByBoth(address)) {
        RecordCall(sh2, 'R', address);
    }
    return g_recOrig.read(sh2, address, size, instrFetch);
}
// x64 reads the on-chip registers through onChipRead where RunBlock uses read.
uint32 RecOnChipRead(void *sh2, uint32 address, uint32 size) {
    RecordCall(sh2, 'R', address);
    return g_recOrig.onChipRead(sh2, address, size);
}
void RecWrite(void *sh2, uint32 address, uint32 size, uint32 value) {
    if (CalledByBoth(address)) {
        RecordCall(sh2, 'W', address);
    }
    g_recOrig.write(sh2, address, size, value);
}
void RecRefill(void *sh2, uint32 address) {
    if (CalledByBoth(address)) {
        RecordCall(sh2, 'F', address);
    }
    g_recOrig.refillPipeline(sh2, address);
}
bool RecBusWait(void *sh2, uint32 address, uint32 size, bool write) {
    if ((address >> 24) == 0x22) {
        RecordCall(sh2, 'B', address);
    }
    return g_recOrig.busWait(sh2, address, size, write);
}
void RecSetSR(void *sh2, uint32 value, bool delaySlot) {
    RecordCall(sh2, 'S', value);
    g_recOrig.setSR(sh2, value, delaySlot);
}
uint64 RecRMW(void *sh2, uint32 address) {
    RecordCall(sh2, 'M', address);
    return g_recOrig.accessCyclesRMWByte(sh2, address);
}

void HookCalls(ymir::sh2::SH2JitContext &ctx, CallRecorder &recorder) {
    g_recOrig = ctx;
    recorder.sh2 = ctx.sh2;
    recorder.ctx = &ctx;
    recorder.log.clear();
    g_recorders.push_back(&recorder);
    ctx.read = RecRead;
    ctx.onChipRead = RecOnChipRead;
    ctx.write = RecWrite;
    ctx.refillPipeline = RecRefill;
    ctx.busWait = RecBusWait;
    ctx.setSR = RecSetSR;
    ctx.accessCyclesRMWByte = RecRMW;
}

void UnhookCalls(ymir::sh2::SH2JitContext &ctx) {
    ctx.read = g_recOrig.read;
    ctx.onChipRead = g_recOrig.onChipRead;
    ctx.write = g_recOrig.write;
    ctx.refillPipeline = g_recOrig.refillPipeline;
    ctx.busWait = g_recOrig.busWait;
    ctx.setSR = g_recOrig.setSR;
    ctx.accessCyclesRMWByte = g_recOrig.accessCyclesRMWByte;
}

std::string DescribeCalls(const std::vector<CallView> &log) {
    std::string s;
    for (const CallView &v : log) {
        s += v.kind;
        s += " " + std::to_string(v.address) + ":";
        for (const uint32_t r : v.regs) {
            s += " " + std::to_string(r);
        }
        s += "\n";
    }
    return s;
}

// Runs `seeds` random blocks on RunBlock and on the x64 backend, from identical random CPU states,
// and requires identical outcomes. A third rig runs each block on RunBlock with every refill
// turned back into a refillPipeline call (the interpreter's behavior), so known refill values and
// their codeDirty fallback must reproduce exactly what the callback reads. A fourth runs the block
// after OptimizeBlock on x64 (cycles-only checks, folded stalls), against RunBlock's original.
// With opt.registers the calls both backends make are also recorded (HookCalls) on RunBlock and
// both x64 runs, and must have seen the same R0-R15 and SR.
void CompareRandomBlocks(const sh2test::RandomIrOptions &opt, uint32_t seeds = 2000) {
    auto irRig = std::make_unique<Rig>();
    auto x64Rig = std::make_unique<Rig>();
    auto callbackRig = std::make_unique<Rig>();
    auto optRig = std::make_unique<Rig>();
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    uint64_t knownRefills = 0;
    uint64_t recordedCalls = 0;
    CallRecorder irCalls;
    CallRecorder x64Calls;
    CallRecorder optCalls;
    g_recorders.clear(); // a failed earlier run may have left entries behind

    for (uint32_t seed = 0; seed < seeds; ++seed) {
        std::mt19937 rng(seed);
        const Block block = sh2test::RandomBlock(rng, kCode, opt);
        INFO("seed " << seed << "\n" << brimir::jit::PrintBlock(block));
        REQUIRE(brimir::jit::VerifyBlock(block).empty());
        Block callbackBlock = block;
        callbackBlock.fetchFromArrays = false;
        for (brimir::jit::Inst &inst : callbackBlock.code) {
            if (inst.op == brimir::jit::Op::Refill && inst.flag) {
                inst.flag = false;
                inst.imm2 = 0;
                ++knownRefills;
            }
        }

        Block optimized = block;
        brimir::jit::OptimizeBlock(optimized);
        REQUIRE(brimir::jit::VerifyBlock(optimized).empty());

        const CpuSetup cpu = RandomCpu(*irRig, rng);
        for (Rig *rig : {irRig.get(), x64Rig.get(), callbackRig.get(), optRig.get()}) {
            rig->WriteCode(kCode, block.guestOpcodes); // the entry check's guarantee
            cpu.Apply(*rig);
        }
        uint64_t target = kNoCycleTarget;
        switch (rng() % 4) {
        case 0: break;
        case 1: target = cpu.cycles - std::min<uint64_t>(cpu.cycles, 1 + rng() % 4); break; // below entry
        default: target = cpu.cycles + rng() % 41; break;
        }
        // Bus-wait answers depend on the query count, so both rigs restart it identically.
        const uint32_t busWaitEvery = opt.memory ? static_cast<uint32_t>(rng() % 3 == 0 ? 0 : 2 + rng() % 2) : 0;
        for (Rig *rig : {irRig.get(), x64Rig.get(), callbackRig.get(), optRig.get()}) {
            rig->mmio.busWaitEvery = busWaitEvery;
            rig->mmio.busWaitQueries = 0;
            rig->mmio.log.clear();
        }
        INFO("entry cycles " << cpu.cycles << " target " << target << " intrPending " << cpu.intrPending
                             << " intrAllow " << cpu.state.intrAllow << " busWaitEvery " << busWaitEvery);

        auto &x64Ctx = x64Rig->sh2->GetJitContext();
        auto &irCtx = irRig->sh2->GetJitContext();
        auto &optCtx = optRig->sh2->GetJitContext();
        if (opt.registers) {
            HookCalls(irCtx, irCalls);
            HookCalls(x64Ctx, x64Calls);
            HookCalls(optCtx, optCalls);
        }
        NativeCode code;
        REQUIRE(backend->Compile(block, x64Ctx, code));
        REQUIRE(code.entry != nullptr);
        const ExitInfo ir = brimir::jit::RunBlock(block, irCtx, target);
        const ExitInfo x64 = backend->Run(code, x64Ctx, target);
        if (opt.registers) {
            INFO("RunBlock calls\n" << DescribeCalls(irCalls.log) << "x64 calls\n" << DescribeCalls(x64Calls.log));
            REQUIRE(x64Calls.log == irCalls.log);
            recordedCalls += irCalls.log.size();
        }
        // Both rigs ran exactly the same steps, so the peripherals (the FRT the generator touches)
        // must match too.
        RequireSameOutcome(ir, x64, *irRig, *x64Rig, true);
        REQUIRE(x64Rig->State().fetchedOpcodes == irRig->State().fetchedOpcodes);
        REQUIRE(*x64Ctx.delaySlot == *irRig->sh2->GetJitContext().delaySlot);
        const ExitInfo callback = brimir::jit::RunBlock(callbackBlock, callbackRig->sh2->GetJitContext(), target);
        INFO("RunBlock with refill callbacks vs RunBlock with known refills");
        RequireSameOutcome(callback, ir, *callbackRig, *irRig, true);
        REQUIRE(irRig->State().fetchedOpcodes == callbackRig->State().fetchedOpcodes);
        {
            INFO("x64 on the optimized block vs RunBlock on the original\n" << brimir::jit::PrintBlock(optimized));
            NativeCode optCode;
            REQUIRE(backend->Compile(optimized, optCtx, optCode));
            const ExitInfo optInfo = backend->Run(optCode, optCtx, target);
            if (opt.registers) {
                INFO("RunBlock calls\n" << DescribeCalls(irCalls.log) << "x64 calls\n" << DescribeCalls(optCalls.log));
                REQUIRE(optCalls.log == irCalls.log);
            }
            RequireSameOutcome(ir, optInfo, *irRig, *optRig, true);
            REQUIRE(optRig->State().fetchedOpcodes == irRig->State().fetchedOpcodes);
            REQUIRE(*optCtx.delaySlot == *irRig->sh2->GetJitContext().delaySlot);
        }
        if (opt.registers) {
            UnhookCalls(irCtx);
            UnhookCalls(x64Ctx);
            UnhookCalls(optCtx);
            g_recorders.clear();
        }

        if (seed % 256 == 255) {
            backend->Reset(); // keeps memory bounded and exercises compiling after a reset
        }
    }
    if (opt.calls) {
        CHECK(knownRefills > 0);
    }
    if (opt.registers && (opt.calls || opt.memory)) {
        CHECK(recordedCalls > 0);
    }
}

} // namespace

TEST_CASE("x64 matches the IR interpreter on random blocks", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    CompareRandomBlocks({});
}

TEST_CASE("x64 matches IR on random blocks with calls and memory", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    sh2test::RandomIrOptions opt;
    opt.calls = true;
    opt.memory = true;
    CompareRandomBlocks(opt);
}

TEST_CASE("x64 matches IR on random blocks with calls only", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    sh2test::RandomIrOptions opt;
    opt.calls = true;
    CompareRandomBlocks(opt);
}

TEST_CASE("x64 matches IR on random blocks with memory only", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    sh2test::RandomIrOptions opt;
    opt.memory = true;
    CompareRandomBlocks(opt);
}

// Register caching: many guest registers set and read across callback loads and stores, SetSR,
// Div1/MAC, ExitIf and boundary checks at random positions; every recorded callback must see the
// same R0-R15 and SR as on RunBlock.
TEST_CASE("x64 matches IR on register-heavy random blocks", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    sh2test::RandomIrOptions opt;
    opt.calls = true;
    opt.memory = true;
    opt.registers = true;
    CompareRandomBlocks(opt, 4000);
}

TEST_CASE("x64 matches IR on register-only random blocks", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    sh2test::RandomIrOptions opt;
    opt.registers = true;
    CompareRandomBlocks(opt);
}

TEST_CASE("x64 spills", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    // 300 values, all live until the final reduction into R0-R15: far more than the host registers.
    Block block;
    block.startPC = kCode;
    block.guestInstrCount = 1;
    Builder b(block);
    std::vector<ValueId> values;
    std::mt19937 rng(1234);
    for (uint32_t i = 0; i < 300; ++i) {
        if (i < 2 || i % 3 == 0) {
            values.push_back(b.Const(static_cast<uint32_t>(rng())));
        } else {
            values.push_back(b.Add(values[i - 1], values[i - 2]));
        }
    }
    for (uint32_t r = 0; r < 16; ++r) {
        ValueId acc = values[r];
        for (uint32_t i = r + 16; i < values.size(); i += 16) {
            acc = (i / 16) % 2 != 0 ? b.Add(acc, values[i]) : b.Xor(acc, values[i]);
        }
        b.SetReg(r, acc);
    }
    b.Exit(kCode + 2, 1);
    INFO(brimir::jit::PrintBlock(block));
    REQUIRE(brimir::jit::VerifyBlock(block).empty());

    auto irRig = std::make_unique<Rig>();
    auto x64Rig = std::make_unique<Rig>();
    irRig->Load(irRig->BaseState(kCode));
    x64Rig->Load(x64Rig->BaseState(kCode));
    const ExitInfo ir = brimir::jit::RunBlock(block, irRig->sh2->GetJitContext());
    const ExitInfo x64 = sh2test::RunOnBackend(BackendKind::X64, block, x64Rig->sh2->GetJitContext());
    RequireSameOutcome(ir, x64, *irRig, *x64Rig);
}

TEST_CASE("x64 boundary at every check", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    // A straight 32-instruction block: one check before every instruction after the first. It
    // starts by clearing interrupt-allow; with allowAt = k > 0 it allows interrupts again right
    // before check k, so a pending interrupt can only stop the block there.
    const auto makeBlock = [](uint32_t allowAt) {
        Block block;
        block.startPC = kCode;
        block.guestInstrCount = 32;
        Builder b(block);
        b.ClearIntrAllow();
        for (uint32_t i = 0; i < 32; ++i) {
            if (i > 0) {
                if (i == allowAt) {
                    b.SetIntrAllow();
                }
                b.CheckBoundary(kCode + 2 * i, static_cast<uint8_t>(i));
            }
            const uint32_t reg = i % 16;
            b.SetReg(reg, b.Add(b.GetReg(reg), b.Const(i + 1)));
            b.AddCycles(1 + i % 3);
        }
        b.Exit(kCode + 64, 32);
        return block;
    };

    auto irRig = std::make_unique<Rig>();
    auto x64Rig = std::make_unique<Rig>();
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);

    // allowAt 0: interrupts stay disallowed, only the cycle target stops the block.
    // Each block also runs after OptimizeBlock, which tests only cycles at every check except
    // allowAt's (the others follow ClearIntrAllow or a passed check with no changing op between).
    for (uint32_t run = 0; run < 64; ++run) {
        const uint32_t allowAt = run / 2;
        const bool optimize = (run & 1) != 0;
        Block block = makeBlock(allowAt);
        if (optimize) {
            brimir::jit::OptimizeBlock(block);
            uint32_t full = 0;
            for (const brimir::jit::Inst &in : block.code) {
                full += in.op == brimir::jit::Op::CheckBoundary && !in.flag ? 1 : 0;
            }
            CHECK(full == (allowAt > 0 ? 1u : 0u));
        }
        INFO(brimir::jit::PrintBlock(block));
        REQUIRE(brimir::jit::VerifyBlock(block).empty());
        NativeCode code;
        REQUIRE(backend->Compile(block, x64Rig->sh2->GetJitContext(), code));

        for (const bool interrupt : {false, true}) {
            for (uint64_t extra = 0; extra <= 41; ++extra) {
                CpuSetup cpu;
                cpu.state = irRig->BaseState(kCode);
                cpu.state.intrAllow = true;
                cpu.intrPending = interrupt;
                cpu.cycles = 1000;
                cpu.Apply(*irRig);
                cpu.Apply(*x64Rig);
                const uint64_t target = extra == 41 ? kNoCycleTarget : cpu.cycles + extra;
                INFO("allowAt " << allowAt << " optimized " << optimize << " interrupt " << interrupt
                                << " target entry+" << extra);
                const ExitInfo ir = brimir::jit::RunBlock(block, irRig->sh2->GetJitContext(), target);
                const ExitInfo x64 = backend->Run(code, x64Rig->sh2->GetJitContext(), target);
                RequireSameOutcome(ir, x64, *irRig, *x64Rig);
                if (interrupt && allowAt > 0) {
                    CHECK(x64.boundary);
                    CHECK(x64.retired <= allowAt);
                    if (target == kNoCycleTarget) {
                        CHECK(x64.retired == allowAt);
                    }
                }
                if (target == kNoCycleTarget && (!interrupt || allowAt == 0)) {
                    CHECK_FALSE(x64.boundary);
                    CHECK(x64.retired == 32);
                }
            }
        }
    }
}

TEST_CASE("x64 code is freed by Reset", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    Block block;
    block.startPC = kCode;
    block.guestInstrCount = 1;
    Builder b(block);
    b.SetReg(1, b.Add(b.GetReg(1), b.Const(1)));
    b.AddCycles(1);
    b.Exit(kCode + 2, 1);

    auto rig = std::make_unique<Rig>();
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    NativeCode code;
    REQUIRE(backend->Compile(block, rig->sh2->GetJitContext(), code));
    CHECK(code.entry != nullptr);
    CHECK(backend->CodeBytes() > 0);
    backend->Reset();
    CHECK(backend->CodeBytes() == 0);
}

TEST_CASE("x64 Compile fails cleanly on a context with a null state pointer", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    Block block;
    block.startPC = kCode;
    block.guestInstrCount = 1;
    Builder b(block);
    b.SetReg(1, b.Add(b.GetReg(1), b.Const(1)));
    b.AddCycles(1);
    b.Exit(kCode + 2, 1);

    auto rig = std::make_unique<Rig>();
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    NativeCode good;
    REQUIRE(backend->Compile(block, rig->sh2->GetJitContext(), good));
    const size_t bytes = backend->CodeBytes();
    REQUIRE(bytes > 0);

    using Ctx = ymir::sh2::SH2JitContext;
    const std::vector<std::pair<const char *, void (*)(Ctx &)>> fields{
        {"R", [](Ctx &c) { c.R = nullptr; }},
        {"PC", [](Ctx &c) { c.PC = nullptr; }},
        {"PR", [](Ctx &c) { c.PR = nullptr; }},
        {"GBR", [](Ctx &c) { c.GBR = nullptr; }},
        {"VBR", [](Ctx &c) { c.VBR = nullptr; }},
        {"SR", [](Ctx &c) { c.SR = nullptr; }},
        {"MACL", [](Ctx &c) { c.MACL = nullptr; }},
        {"MACH", [](Ctx &c) { c.MACH = nullptr; }},
        {"delaySlotTarget", [](Ctx &c) { c.delaySlotTarget = nullptr; }},
        {"wbReg", [](Ctx &c) { c.wbReg = nullptr; }},
        {"intrPending", [](Ctx &c) { c.intrPending = nullptr; }},
        {"intrAllow", [](Ctx &c) { c.intrAllow = nullptr; }},
        {"cyclesExecuted", [](Ctx &c) { c.cyclesExecuted = nullptr; }},
    };
    for (const auto &[name, clear] : fields) {
        INFO("null " << name);
        Ctx ctx = rig->sh2->GetJitContext();
        clear(ctx);
        static const int sentinel = 0;
        NativeCode out;
        out.entry = &sentinel;
        CHECK_FALSE(backend->Compile(block, ctx, out));
        CHECK(out.entry == nullptr);
        CHECK(backend->CodeBytes() == bytes);
    }

    // The fetch buffer, delay-slot flag and pending level are optional: without them refills and
    // delay-slot ops call their trampolines (the block still compiles).
    const std::vector<std::pair<const char *, void (*)(Ctx &)>> optional{
        {"fetchedOpcodes", [](Ctx &c) { c.fetchedOpcodes = nullptr; }},
        {"delaySlot", [](Ctx &c) { c.delaySlot = nullptr; }},
        {"intcPendingLevel", [](Ctx &c) { c.intcPendingLevel = nullptr; }},
    };
    for (const auto &[name, clear] : optional) {
        INFO("null " << name);
        Ctx ctx = rig->sh2->GetJitContext();
        clear(ctx);
        NativeCode out;
        CHECK(backend->Compile(block, ctx, out));
        CHECK(out.entry != nullptr);
    }
}

TEST_CASE("x64 values live across calls", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint32_t kMmio = 0x22000100;
    Block block;
    block.startPC = kCode;
    block.guestInstrCount = 1;
    Builder b(block);
    std::mt19937 rng(77);
    std::vector<ValueId> values;
    for (uint32_t i = 0; i < 40; ++i) {
        const ValueId x = b.Add(b.GetReg(i % 16), b.Const(static_cast<uint32_t>(rng())));
        values.push_back(i % 3 == 0 ? b.Mul(x, b.Const(static_cast<uint32_t>(rng()) | 1u)) : b.Xor(x, b.Const(i)));
    }
    // Two calls with all 40 values live: a callback load from MMIO and SetSR.
    b.SyncCycles();
    const ValueId address = b.Const(kMmio);
    b.AddAccessCycles(address, 4, false);
    const ValueId loaded = b.Load(address, 4, false);
    b.SetSR(b.GetReg(5), false);
    for (uint32_t r = 0; r < 16; ++r) {
        ValueId acc = loaded;
        for (uint32_t i = r; i < values.size(); i += 16) {
            acc = b.Add(acc, values[i]);
        }
        b.SetReg(r, acc);
    }
    b.SetMACL(b.Xor(values[39], values[0]));
    b.AddCycles(1);
    b.Exit(kCode + 2, 1);
    INFO(brimir::jit::PrintBlock(block));
    REQUIRE(brimir::jit::VerifyBlock(block).empty());

    auto irRig = std::make_unique<Rig>();
    auto x64Rig = std::make_unique<Rig>();
    CpuSetup cpu = RandomCpu(*irRig, rng);
    cpu.intrPending = false;
    for (Rig *rig : {irRig.get(), x64Rig.get()}) {
        cpu.Apply(*rig);
        rig->mmio.data[kMmio & 0xFFFF] = 0xDE;
        rig->mmio.data[(kMmio & 0xFFFF) + 1] = 0xAD;
        rig->mmio.data[(kMmio & 0xFFFF) + 2] = 0xBE;
        rig->mmio.data[(kMmio & 0xFFFF) + 3] = 0xEF;
    }
    const ExitInfo ir = brimir::jit::RunBlock(block, irRig->sh2->GetJitContext());
    const ExitInfo x64 = sh2test::RunOnBackend(BackendKind::X64, block, x64Rig->sh2->GetJitContext());
    RequireSameOutcome(ir, x64, *irRig, *x64Rig);
    REQUIRE(x64Rig->mmio.log.size() == 1);
    CHECK(x64Rig->mmio.log[0].kind == 'R');
    CHECK(x64Rig->mmio.log[0].value == 0xDEADBEEFu);
}

namespace {

constexpr uint32_t kAbortMmio = 0x22000200;

// Hooked callbacks that request an abort when they touch the MMIO page, like a WDT access that
// resets the CPU in the middle of a block.
bool g_abort = false;
uint32 (*g_origRead)(void *, uint32, uint32, bool) = nullptr;
void (*g_origWrite)(void *, uint32, uint32, uint32) = nullptr;
void (*g_origRefill)(void *, uint32) = nullptr;

bool IsMmio(uint32 address) {
    return (address >> 24) == 0x22;
}

uint32 AbortingRead(void *sh2, uint32 address, uint32 size, bool instrFetch) {
    const uint32 value = g_origRead(sh2, address, size, instrFetch);
    g_abort = g_abort || IsMmio(address);
    return value;
}

void AbortingWrite(void *sh2, uint32 address, uint32 size, uint32 value) {
    g_origWrite(sh2, address, size, value);
    g_abort = g_abort || IsMmio(address);
}

void AbortingRefill(void *sh2, uint32 address) {
    g_origRefill(sh2, address);
    g_abort = g_abort || IsMmio(address);
}

uint32 ThrowingRead(void *sh2, uint32 address, uint32 size, bool instrFetch) {
    if (IsMmio(address)) {
        throw std::runtime_error("bus fault reading MMIO");
    }
    return g_origRead(sh2, address, size, instrFetch);
}

} // namespace

TEST_CASE("x64 aborts after each memory kind", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    enum class Kind { Load, Store, Refill, ExitIfRefill };
    for (const Kind kind : {Kind::Load, Kind::Store, Kind::Refill, Kind::ExitIfRefill}) {
        INFO("kind " << static_cast<int>(kind));
        Block block;
        block.startPC = kCode;
        block.guestInstrCount = 1;
        Builder b(block);
        b.SetReg(1, b.Const(0x1111));
        b.AddCycles(3);
        b.SyncCycles();
        const ValueId address = b.Const(kAbortMmio);
        switch (kind) {
        case Kind::Load: b.SetReg(2, b.Load(address, 4, false)); break;
        case Kind::Store: b.Store(address, 4, b.Const(0xA5A5A5A5)); break;
        case Kind::Refill: b.Refill(kAbortMmio); break;
        case Kind::ExitIfRefill: b.ExitIf(b.Const(1), kAbortMmio, 5, true, 1); break;
        }
        // Nothing from here on may run.
        b.SetReg(3, b.Const(0x3333));
        b.SetT(b.Const(1));
        b.Store(b.Const(0x06040000), 4, b.Const(0x55555555));
        b.SetSR(b.Const(0), false);
        b.AddCycles(7);
        b.Exit(kCode + 2, 1);
        INFO(brimir::jit::PrintBlock(block));
        REQUIRE(brimir::jit::VerifyBlock(block).empty());

        auto irRig = std::make_unique<Rig>();
        auto x64Rig = std::make_unique<Rig>();
        ExitInfo results[2];
        Rig *rigs[2] = {irRig.get(), x64Rig.get()};
        for (int i = 0; i < 2; ++i) {
            Rig &rig = *rigs[i];
            rig.Load(rig.BaseState(kCode));
            auto &ctx = rig.sh2->GetJitContext();
            g_origRead = ctx.read;
            g_origWrite = ctx.write;
            g_origRefill = ctx.refillPipeline;
            ctx.read = AbortingRead;
            ctx.write = AbortingWrite;
            ctx.refillPipeline = AbortingRefill;
            g_abort = false;
            results[i] = sh2test::RunOnBackend(i == 0 ? BackendKind::Ir : BackendKind::X64, block, ctx,
                                               kNoCycleTarget, &g_abort);
            ctx.read = g_origRead;
            ctx.write = g_origWrite;
            ctx.refillPipeline = g_origRefill;
            CHECK(g_abort);
        }
        const ExitInfo &x64 = results[1];
        RequireSameOutcome(results[0], x64, *irRig, *x64Rig);
        CHECK(x64.aborted);
        CHECK(x64.retired == 0);
        CHECK(x64.cycles == (kind == Kind::ExitIfRefill ? 8u : 3u));
        const auto st = x64Rig->State();
        CHECK(st.PC == kCode);
        CHECK(st.R[1] == 0x1111u);
        CHECK(st.R[3] != 0x3333u);
        CHECK(x64Rig->Read32(0x06040000) != 0x55555555u);
        CHECK(st.SR == irRig->BaseState(kCode).SR);
    }
}

TEST_CASE("x64 propagates callback exceptions", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint32_t kMmio = 0x22040000;

    SECTION("Run rethrows and leaves the same state as RunBlock") {
        Block block;
        block.startPC = kCode;
        block.guestInstrCount = 1;
        Builder b(block);
        b.SetReg(1, b.Const(0x1111));
        b.AddCycles(3);
        b.SyncCycles();
        b.SetReg(2, b.Load(b.Const(kMmio), 4, false));
        b.SetReg(3, b.Const(0x3333));
        b.AddCycles(1);
        b.Exit(kCode + 2, 1);
        REQUIRE(brimir::jit::VerifyBlock(block).empty());

        auto irRig = std::make_unique<Rig>();
        auto x64Rig = std::make_unique<Rig>();
        Rig *rigs[2] = {irRig.get(), x64Rig.get()};
        for (int i = 0; i < 2; ++i) {
            Rig &rig = *rigs[i];
            rig.Load(rig.BaseState(kCode));
            auto &ctx = rig.sh2->GetJitContext();
            g_origRead = ctx.read;
            ctx.read = ThrowingRead;
            bool caught = false;
            try {
                sh2test::RunOnBackend(i == 0 ? BackendKind::Ir : BackendKind::X64, block, ctx);
            } catch (const std::runtime_error &e) {
                caught = true;
                CHECK(typeid(e) == typeid(std::runtime_error));
                CHECK(std::string(e.what()) == "bus fault reading MMIO");
            }
            ctx.read = g_origRead;
            INFO("backend " << i);
            CHECK(caught);
        }
        const std::string diff = sh2test::DiffRigs(*irRig, *x64Rig);
        INFO(diff);
        CHECK(diff.empty());
        CHECK(x64Rig->State().R[1] == 0x1111u);
        CHECK(x64Rig->State().R[3] != 0x3333u);
        CHECK(x64Rig->State().PC == kCode);
    }

    SECTION("Executor::Step rethrows and flushes normally afterwards") {
        constexpr uint16_t kMovL_R1_R2 = 0x6212; // mov.l @R1,R2
        const std::vector<uint16_t> program{kMovL_R1_R2, kAdd1_R3, kAdd1_R3, static_cast<uint16_t>(kSleep)};
        auto irRig = std::make_unique<Rig>();
        auto x64Rig = std::make_unique<Rig>();
        brimir::jit::Executor irExec{BackendKind::Ir};
        brimir::jit::Executor x64Exec{BackendKind::X64, sh2test::kNativeOnFirstRun};
        Rig *rigs[2] = {irRig.get(), x64Rig.get()};
        brimir::jit::Executor *execs[2] = {&irExec, &x64Exec};
        for (int i = 0; i < 2; ++i) {
            Rig &rig = *rigs[i];
            rig.WriteCode(kCode, program);
            auto state = rig.BaseState(kCode);
            state.R[1] = kMmio;
            state.R[2] = 0;
            state.R[3] = 0;
            rig.Load(state);
            auto &ctx = rig.sh2->GetJitContext();
            g_origRead = ctx.read;
            ctx.read = ThrowingRead;
            bool caught = false;
            try {
                execs[i]->Step(ctx);
            } catch (const std::runtime_error &e) {
                caught = true;
                CHECK(typeid(e) == typeid(std::runtime_error));
                CHECK(std::string(e.what()) == "bus fault reading MMIO");
            }
            ctx.read = g_origRead;
            INFO("backend " << i);
            CHECK(caught);
        }
        CHECK(x64Exec.GetStats().nativeBlocksRun == 1);
        CHECK(x64Exec.GetStats().compileFallbacks == 0);
        {
            const std::string diff = sh2test::DiffRigs(*irRig, *x64Rig);
            INFO(diff);
            CHECK(diff.empty());
        }

        // The block is no longer running: a flush applies at once.
        REQUIRE(x64Exec.Cache().Size() == 1);
        x64Exec.Flush();
        CHECK(x64Exec.Cache().Size() == 0);

        // The next step recompiles and runs the block natively.
        const auto next = x64Exec.Step(x64Rig->sh2->GetJitContext());
        CHECK_FALSE(next.aborted);
        CHECK(next.retired == 3);
        CHECK(x64Rig->State().R[3] == 2u);
        CHECK(x64Rig->State().PC == kCode + 6);
        CHECK(x64Exec.GetStats().nativeBlocksRun == 2);
    }
}

TEST_CASE("x64 inline memory matches IR", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    auto irRig = std::make_unique<Rig>();
    auto x64Rig = std::make_unique<Rig>();
    auto irRom = std::make_unique<sh2test::FastRom>();
    auto x64Rom = std::make_unique<sh2test::FastRom>();
    sh2test::MapFastPathTestPages(*irRig, *irRom);
    sh2test::MapFastPathTestPages(*x64Rig, *x64Rom);
    const sh2test::FastRom romBefore = *irRom;
    REQUIRE(*x64Rom == romBefore);
    for (Rig *rig : {irRig.get(), x64Rig.get()}) {
        for (uint32_t i = 0; i < 0x40; ++i) {
            rig->Write32(sh2test::kFastRamOffset - 0x80 + i * 4, 0x01020304u * (i + 1) ^ 0xA5C3E1F7u);
        }
        for (uint32_t i = 0; i < 0x100; ++i) {
            rig->mmio.data[(sh2test::kFastMmioOffset + i - 0x80) & 0xFFFF] = static_cast<uint8_t>(i * 13 + 7);
        }
    }
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);

    enum class Kind { Load, LoadFetch, Store, CyclesRead, CyclesWrite, BusWaitRead, BusWaitWrite, Count };
    uint32_t runs = 0;
    // A context without the page table compiles every access to its callback.
    for (const bool pageTable : {true, false}) {
        for (const uint32_t address : sh2test::FastPathTestAddresses()) {
            for (const uint8_t size : {uint8_t{1}, uint8_t{2}, uint8_t{4}}) {
                for (int k = 0; k < static_cast<int>(Kind::Count); ++k) {
                    const auto kind = static_cast<Kind>(k);
                    Block block;
                    block.startPC = kCode;
                    block.guestInstrCount = 1;
                    Builder b(block);
                    b.SetReg(1, b.Const(0x1111));
                    b.AddCycles(2);
                    b.SyncCycles();
                    // The address is computed at run time half of the time.
                    const ValueId addr =
                        (address & 1) != 0 ? b.Add(b.GetReg(5), b.Const(address)) : b.Const(address);
                    switch (kind) {
                    case Kind::Load: b.SetReg(2, b.Load(addr, size, false)); break;
                    case Kind::LoadFetch: b.SetReg(2, b.Load(addr, size, true)); break;
                    case Kind::Store: b.Store(addr, size, b.Const(0xA1B2C3D4u ^ address)); break;
                    case Kind::CyclesRead: b.AddAccessCycles(addr, size, false); break;
                    case Kind::CyclesWrite: b.AddAccessCycles(addr, size, true); break;
                    case Kind::BusWaitRead: b.ExitIfBusWait(addr, size, false, kCode, 0); break;
                    case Kind::BusWaitWrite: b.ExitIfBusWait(addr, size, true, kCode, 0); break;
                    case Kind::Count: break;
                    }
                    b.SetReg(3, b.Const(0x3333));
                    b.AddCycles(1);
                    b.Exit(kCode + 2, 1);
                    REQUIRE(brimir::jit::VerifyBlock(block).empty());

                    for (const uint32_t busWaitEvery : {0u, 1u}) {
                        INFO("page table " << pageTable << " address 0x" << std::hex << address << std::dec
                                           << " size " << int{size} << " kind " << k << " busWaitEvery "
                                           << busWaitEvery);
                        auto state = irRig->BaseState(kCode);
                        state.R[2] = 0xDEADBEEF;
                        state.R[5] = 0; // address = R5 + Const(address)
                        for (Rig *rig : {irRig.get(), x64Rig.get()}) {
                            rig->Load(state);
                            rig->mmio.busWaitEvery = busWaitEvery;
                            rig->mmio.busWaitQueries = 0;
                            rig->mmio.log.clear();
                        }
                        ymir::sh2::SH2JitContext x64Ctx = x64Rig->sh2->GetJitContext();
                        if (!pageTable) {
                            x64Ctx.bus.pages = nullptr;
                        }
                        NativeCode code;
                        REQUIRE(backend->Compile(block, x64Ctx, code));
                        const ExitInfo ir = brimir::jit::RunBlock(block, irRig->sh2->GetJitContext());
                        const ExitInfo x64 = backend->Run(code, x64Ctx, kNoCycleTarget);
                        RequireSameOutcome(ir, x64, *irRig, *x64Rig, true);
                        REQUIRE(*irRom == romBefore);
                        REQUIRE(*x64Rom == romBefore);
                        if (++runs % 512 == 0) {
                            backend->Reset();
                        }
                    }
                }
            }
        }
    }
    CHECK(runs == 2u * 128u * 3u * 7u * 2u);
}

namespace {

// Each throws an exception naming the callback.
uint64 ThrowAccessCycles(void *, uint32, uint32, bool) {
    throw std::runtime_error("accessCycles");
}
uint64 ThrowAccessCyclesRMWByte(void *, uint32) {
    throw std::runtime_error("accessCyclesRMWByte");
}
bool ThrowBusWait(void *, uint32, uint32, bool) {
    throw std::runtime_error("busWait");
}
void ThrowSetSR(void *, uint32, bool) {
    throw std::runtime_error("setSR");
}
void ThrowSetupDelaySlot(void *, uint32) {
    throw std::runtime_error("setupDelaySlot");
}
void ThrowEndDelaySlot(void *) {
    throw std::runtime_error("endDelaySlot");
}
uint32 ThrowRead(void *, uint32, uint32, bool) {
    throw std::runtime_error("read");
}
void ThrowWrite(void *, uint32, uint32, uint32) {
    throw std::runtime_error("write");
}
void ThrowRefill(void *, uint32) {
    throw std::runtime_error("refillPipeline");
}

} // namespace

TEST_CASE("x64 stops after an exception from every callback", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    using Ctx = ymir::sh2::SH2JitContext;
    constexpr uint32_t kMmio = 0x22000200;
    struct Row {
        const char *name;
        void (*hook)(Ctx &);
        void (*emit)(Builder &);
    };
    // accessCycles is inline for every address while the context has the page table, so that row
    // runs on a context without it (the fallback that still calls the callback).
    const std::vector<Row> rows{
        {"accessCycles",
         [](Ctx &c) {
             c.accessCycles = ThrowAccessCycles;
             c.bus.pages = nullptr;
         },
         [](Builder &b) { b.AddAccessCycles(b.Const(kMmio), 4, false); }},
        {"accessCyclesRMWByte", [](Ctx &c) { c.accessCyclesRMWByte = ThrowAccessCyclesRMWByte; },
         [](Builder &b) { b.AddAccessCyclesRMWByte(b.Const(kMmio)); }},
        {"busWait", [](Ctx &c) { c.busWait = ThrowBusWait; },
         [](Builder &b) { b.ExitIfBusWait(b.Const(kMmio), 4, false, kCode, 0); }},
        {"setSR", [](Ctx &c) { c.setSR = ThrowSetSR; }, [](Builder &b) { b.SetSR(b.Const(0), false); }},
        // Delay-slot setup and end are inline when the context has intcPendingLevel (and the
        // delay-slot flag and fetch buffer); without it they call their trampolines.
        {"setupDelaySlot",
         [](Ctx &c) {
             c.setupDelaySlot = ThrowSetupDelaySlot;
             c.intcPendingLevel = nullptr;
         },
         [](Builder &b) { b.SetupDelaySlot(b.Const(kCode + 0x100)); }},
        {"endDelaySlot",
         [](Ctx &c) {
             c.endDelaySlot = ThrowEndDelaySlot;
             c.intcPendingLevel = nullptr;
         },
         [](Builder &b) {
             b.SetupDelaySlot(b.Const(kCode + 0x100));
             b.EndDelaySlot();
         }},
        // The inline end calls the trampoline when the target has bit 1 set and is off array pages.
        {"endDelaySlot", [](Ctx &c) { c.endDelaySlot = ThrowEndDelaySlot; },
         [](Builder &b) {
             b.SetupDelaySlot(b.Const(kMmio + 2));
             b.EndDelaySlot();
         }},
        {"read", [](Ctx &c) { c.read = ThrowRead; }, [](Builder &b) { b.SetReg(2, b.Load(b.Const(kMmio), 4, false)); }},
        {"write", [](Ctx &c) { c.write = ThrowWrite; },
         [](Builder &b) { b.Store(b.Const(kMmio), 4, b.Const(0x55)); }},
        {"refillPipeline", [](Ctx &c) { c.refillPipeline = ThrowRefill; }, [](Builder &b) { b.Refill(kCode + 4); }},
    };

    for (const Row &row : rows) {
        INFO("callback " << row.name);
        Block block;
        block.startPC = kCode;
        block.guestInstrCount = 1;
        Builder b(block);
        b.SetReg(1, b.Const(0x1111));
        b.AddCycles(3);
        b.SyncCycles();
        row.emit(b);
        // Nothing from here on may run.
        b.SetReg(3, b.Const(0x3333));
        b.SetT(b.Const(1));
        b.Store(b.Const(0x06040000), 4, b.Const(0x55555555));
        b.AddCycles(1);
        b.Exit(kCode + 2, 1);
        INFO(brimir::jit::PrintBlock(block));
        REQUIRE(brimir::jit::VerifyBlock(block).empty());

        auto irRig = std::make_unique<Rig>();
        auto x64Rig = std::make_unique<Rig>();
        Rig *rigs[2] = {irRig.get(), x64Rig.get()};
        for (int i = 0; i < 2; ++i) {
            Rig &rig = *rigs[i];
            rig.Load(rig.BaseState(kCode));
            rig.mmio.busWaitEvery = 0;
            Ctx ctx = rig.sh2->GetJitContext(); // a copy: the hooks do not outlive this run
            row.hook(ctx);
            bool caught = false;
            try {
                sh2test::RunOnBackend(i == 0 ? BackendKind::Ir : BackendKind::X64, block, ctx);
            } catch (const std::runtime_error &e) {
                caught = true;
                CHECK(typeid(e) == typeid(std::runtime_error));
                CHECK(std::string(e.what()) == row.name);
            }
            INFO("backend " << i);
            CHECK(caught);
        }
        const std::string diff = sh2test::DiffRigs(*irRig, *x64Rig, true);
        INFO(diff);
        CHECK(diff.empty());
        const auto st = x64Rig->State();
        CHECK(st.R[1] == 0x1111u);
        CHECK(st.R[3] != 0x3333u);
        CHECK((st.SR & 1u) == (irRig->BaseState(kCode).SR & 1u));
        CHECK(x64Rig->Read32(0x06040000) != 0x55555555u);
        CHECK(*x64Rig->sh2->GetJitContext().cyclesExecuted == *irRig->sh2->GetJitContext().cyclesExecuted);
    }
}

// Register caching: guest registers and SR are dirty (in host registers only) when a callback
// throws or requests an abort. R0-R7 are written before a passed boundary check (which stores
// them: the write-back at every CheckBoundary) and a not-taken ExitIf; R8-R15, R5 again, T and
// M/Q/S are written after them, so they are still dirty at the call. The state left in memory must
// be RunBlock's: all writes before the call, none after it.
TEST_CASE("x64 leaves dirty guest registers in memory when a callback throws or aborts", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    using Ctx = ymir::sh2::SH2JitContext;
    constexpr uint32_t kMmio = 0x22000200;
    struct Row {
        const char *name; // the exception text, or nullptr for an abort
        void (*hook)(Ctx &);
        void (*emit)(Builder &);
    };
    const std::vector<Row> rows{
        {"write", [](Ctx &c) { c.write = ThrowWrite; }, [](Builder &b) { b.Store(b.Const(kMmio), 4, b.GetReg(9)); }},
        {"read", [](Ctx &c) { c.read = ThrowRead; }, [](Builder &b) { b.SetReg(2, b.Load(b.Const(kMmio), 2, false)); }},
        {"refillPipeline", [](Ctx &c) { c.refillPipeline = ThrowRefill; }, [](Builder &b) { b.Refill(kMmio); }},
        {"busWait", [](Ctx &c) { c.busWait = ThrowBusWait; },
         [](Builder &b) { b.ExitIfBusWait(b.Const(kMmio), 4, true, kCode + 2, 1); }},
        {"setSR", [](Ctx &c) { c.setSR = ThrowSetSR; }, [](Builder &b) { b.SetSR(b.GetReg(4), false); }},
        {"accessCyclesRMWByte", [](Ctx &c) { c.accessCyclesRMWByte = ThrowAccessCyclesRMWByte; },
         [](Builder &b) { b.AddAccessCyclesRMWByte(b.Const(kMmio)); }},
        // Inline setup (no call); the inline end calls the trampoline: the target has bit 1 set and
        // is off array pages. SR (and R8-R15) are dirty at that call.
        {"endDelaySlot", [](Ctx &c) { c.endDelaySlot = ThrowEndDelaySlot; },
         [](Builder &b) {
             b.SetupDelaySlot(b.Const(kMmio + 2));
             b.EndDelaySlot();
         }},
        {nullptr,
         [](Ctx &c) {
             g_origWrite = c.write;
             c.write = AbortingWrite;
         },
         [](Builder &b) { b.Store(b.Const(kMmio), 1, b.GetReg(9)); }},
        {nullptr,
         [](Ctx &c) {
             g_origRead = c.read;
             c.read = AbortingRead;
         },
         [](Builder &b) { b.SetReg(2, b.Load(b.Const(kMmio), 4, false)); }},
    };

    for (const Row &row : rows) {
        INFO("callback " << (row.name != nullptr ? row.name : "abort"));
        Block block;
        block.startPC = kCode;
        block.guestInstrCount = 2;
        Builder b(block);
        for (uint32_t r = 0; r < 8; ++r) {
            b.SetReg(r, b.Add(b.GetReg(r), b.Const(0x100 * r + 1)));
        }
        b.AddCycles(1);
        b.CheckBoundary(kCode + 2, 1); // stores R0-R7
        b.ExitIf(b.CmpGtU(b.GetReg(0), b.GetReg(0)), kCode + 0x40, 2, false, 1); // never taken
        // Dirty at the call below:
        for (uint32_t r = 8; r < 16; ++r) {
            b.SetReg(r, b.Add(b.GetReg(r), b.Const(0x100 * r + 1)));
        }
        b.SetT(b.Const(1));
        b.SetSRBits(b.Const(0x302), 0x302); // M, Q, S
        b.SetReg(5, b.Xor(b.GetReg(5), b.GetReg(6)));
        b.AddCycles(2);
        b.SyncCycles();
        row.emit(b);
        // Nothing from here on may run.
        b.SetReg(3, b.Const(0x3333));
        b.SetReg(7, b.Const(0x7777));
        b.SetT(b.Const(0));
        b.AddCycles(1);
        b.Exit(kCode + 4, 2);
        INFO(brimir::jit::PrintBlock(block));
        REQUIRE(brimir::jit::VerifyBlock(block).empty());

        auto irRig = std::make_unique<Rig>();
        auto x64Rig = std::make_unique<Rig>();
        Rig *rigs[2] = {irRig.get(), x64Rig.get()};
        ExitInfo results[2];
        auto base = irRig->BaseState(kCode);
        for (uint32_t r = 0; r < 16; ++r) {
            base.R[r] = 0x01010101u * r;
        }
        base.SR = 0x0F0;
        for (int i = 0; i < 2; ++i) {
            Rig &rig = *rigs[i];
            rig.Load(base);
            rig.mmio.busWaitEvery = 0;
            Ctx ctx = rig.sh2->GetJitContext(); // a copy: the hooks do not outlive this run
            row.hook(ctx);
            g_abort = false;
            bool caught = false;
            try {
                results[i] = sh2test::RunOnBackend(i == 0 ? BackendKind::Ir : BackendKind::X64, block, ctx,
                                                   kNoCycleTarget, &g_abort);
            } catch (const std::runtime_error &e) {
                caught = true;
                CHECK(row.name != nullptr);
                if (row.name != nullptr) {
                    CHECK(std::string(e.what()) == row.name);
                }
            }
            INFO("backend " << i);
            CHECK(caught == (row.name != nullptr));
            if (row.name == nullptr) {
                CHECK(g_abort);
                CHECK(results[i].aborted);
            }
        }
        if (row.name == nullptr) {
            RequireSameOutcome(results[0], results[1], *irRig, *x64Rig, true);
        }
        const std::string diff = sh2test::DiffRigs(*irRig, *x64Rig, true);
        INFO(diff);
        CHECK(diff.empty());
        const auto st = x64Rig->State();
        for (uint32_t r = 0; r < 16; ++r) {
            INFO("R" << r);
            if (r == 2 || r == 3 || r == 5 || r == 7) {
                continue;
            }
            CHECK(st.R[r] == 0x01010101u * r + 0x100 * r + 1);
        }
        CHECK(st.R[5] == ((0x01010101u * 5 + 0x501) ^ (0x01010101u * 6 + 0x601)));
        CHECK(st.R[3] != 0x3333u);
        CHECK(st.R[7] == 0x01010101u * 7 + 0x701);
        CHECK(st.R[2] == 0x01010101u * 2 + 0x201);
        CHECK(st.SR == (0x0F0u | 0x302u | 1u));
        CHECK(*x64Rig->sh2->GetJitContext().cyclesExecuted == *irRig->sh2->GetJitContext().cyclesExecuted);
    }
}

// A block with known refills checks at entry that its code pages still have the arrays they had
// at compile time. After a remap it returns out.stale and changes nothing.
TEST_CASE("x64 reports a remapped code page as stale", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    for (const uint32_t start : {kCode, 0x0600FFF8u}) { // inside one page; across two pages
        INFO("start 0x" << std::hex << start);
        auto page = std::make_unique<std::array<uint8_t, 0x10000>>();
        auto rig = std::make_unique<Rig>();
        rig->WriteCode(start, {kAdd1_R3, kAdd1_R3, kAdd1_R3, kAdd1_R3, kAdd1_R3});
        Block block;
        block.startPC = start;
        block.guestInstrCount = 4;
        block.hasTailWord = true;
        block.fetchFromArrays = true;
        block.guestOpcodes = {kAdd1_R3, kAdd1_R3, kAdd1_R3, kAdd1_R3, kAdd1_R3};
        Builder b(block);
        b.KnownRefill(start, (uint32_t{kAdd1_R3} << 16) | kAdd1_R3);
        b.SetReg(3, b.Const(0x77));
        b.AddCycles(4);
        b.KnownRefill(start + 4, (uint32_t{kAdd1_R3} << 16) | kAdd1_R3);
        b.Exit(start + 8, 4);
        REQUIRE(brimir::jit::VerifyBlock(block).empty());

        auto state = rig->BaseState(start);
        state.R[3] = 5;
        state.fetchedOpcodes = 0xCAFEF00D;
        rig->Load(state);
        auto &ctx = rig->sh2->GetJitContext();
        const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
        REQUIRE(backend != nullptr);
        NativeCode code;
        REQUIRE(backend->Compile(block, ctx, code));

        // Same bytes, different array: remap the page holding the last code word.
        const uint32_t lastPage = (start + 8) & 0x7FF0000u;
        std::copy_n(rig->ram->begin() + (lastPage & 0xF0000u), 0x10000, page->begin());
        rig->bus.MapArray(lastPage, lastPage + 0xFFFF, *page, true);

        const auto before = rig->State();
        const ExitInfo info = backend->Run(code, ctx);
        CHECK(info.stale);
        CHECK(info.cycles == 0);
        CHECK(info.retired == 0);
        CHECK_FALSE(info.aborted);
        CHECK_FALSE(info.boundary);
        const auto after = rig->State();
        CHECK(after.R[3] == before.R[3]);
        CHECK(after.PC == before.PC);
        CHECK(after.fetchedOpcodes == 0xCAFEF00Du);

        // Compiled against the new mapping, the block runs.
        NativeCode fresh;
        REQUIRE(backend->Compile(block, ctx, fresh));
        const ExitInfo ran = backend->Run(fresh, ctx);
        CHECK_FALSE(ran.stale);
        CHECK(ran.retired == 4);
        CHECK(rig->State().R[3] == 0x77u);
        CHECK(rig->State().fetchedOpcodes == ((uint32_t{kAdd1_R3} << 16) | kAdd1_R3));
    }
}

// Prepare runs the emitter on the calling thread, Finalize only touches the prepared object (so it can
// run on a worker), and Commit adds the code to the backend.
TEST_CASE("x64 backend: a prepared compile finalized on another thread equals Compile", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    auto irRig = std::make_unique<Rig>();
    auto x64Rig = std::make_unique<Rig>();
    const auto direct = brimir::jit::MakeNativeBackend(BackendKind::X64);
    const auto split = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(direct != nullptr);
    REQUIRE(split != nullptr);
    auto &irCtx = irRig->sh2->GetJitContext();
    auto &x64Ctx = x64Rig->sh2->GetJitContext();

    sh2test::RandomIrOptions memory;
    memory.calls = true;
    memory.memory = true;
    uint32_t selfValidating = 0;
    for (const sh2test::RandomIrOptions &opt : {sh2test::RandomIrOptions{}, memory}) {
        for (uint32_t seed = 0; seed < 300; ++seed) {
            std::mt19937 rng(seed);
            const Block block = sh2test::RandomBlock(rng, kCode, opt);
            INFO("seed " << seed << " memory " << opt.memory << "\n" << brimir::jit::PrintBlock(block));
            REQUIRE(brimir::jit::VerifyBlock(block).empty());
            const CpuSetup cpu = RandomCpu(*irRig, rng);
            const uint64_t target = rng() % 2 == 0 ? kNoCycleTarget : cpu.cycles + rng() % 41;
            const uint32_t busWaitEvery = opt.memory ? static_cast<uint32_t>(rng() % 3 == 0 ? 0 : 2 + rng() % 2) : 0;

            NativeCode whole;
            REQUIRE(direct->Compile(block, x64Ctx, whole));
            auto prepared = split->Prepare(block, x64Ctx);
            REQUIRE(prepared != nullptr);
            CHECK(split->CodeBytes() == direct->CodeBytes() - whole.bytes); // nothing is added before Commit
            bool finalized = false;
            std::thread worker([&] { finalized = prepared->Finalize(); });
            worker.join();
            REQUIRE(finalized);
            NativeCode parts;
            REQUIRE(split->Commit(*prepared, parts));
            REQUIRE(parts.entry != nullptr);
            CHECK(parts.bytes == whole.bytes);
            CHECK(parts.selfValidating == whole.selfValidating);
            CHECK(split->CodeBytes() == direct->CodeBytes());
            selfValidating += parts.selfValidating ? 1 : 0;

            const std::array<std::pair<brimir::jit::INativeBackend *, const NativeCode *>, 2> runs{
                {{direct.get(), &whole}, {split.get(), &parts}}};
            for (const auto &[backend, code] : runs) {
                for (Rig *rig : {irRig.get(), x64Rig.get()}) {
                    rig->WriteCode(kCode, block.guestOpcodes);
                    cpu.Apply(*rig);
                    rig->mmio.busWaitEvery = busWaitEvery;
                    rig->mmio.busWaitQueries = 0;
                    rig->mmio.log.clear();
                }
                const ExitInfo ir = brimir::jit::RunBlock(block, irCtx, target);
                const ExitInfo x64 = backend->Run(*code, x64Ctx, target);
                RequireSameOutcome(ir, x64, *irRig, *x64Rig, true);
            }
        }
        direct->Reset();
        split->Reset();
    }
    CHECK(selfValidating > 0);
}

TEST_CASE("x64 backend: Finalize does not read the page table", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint32_t start = kCode;
    auto page = std::make_unique<std::array<uint8_t, 0x10000>>();
    auto rig = std::make_unique<Rig>();
    auto ref = std::make_unique<Rig>();
    const std::vector<uint16_t> program{kAdd1_R3, kAdd1_R3, kAdd1_R3, kAdd1_R3, kAdd1_R3};
    rig->WriteCode(start, program);
    ref->WriteCode(start, program);
    Block block;
    block.startPC = start;
    block.guestInstrCount = 4;
    block.hasTailWord = true;
    block.fetchFromArrays = true;
    block.guestOpcodes = program;
    Builder b(block);
    b.KnownRefill(start, (uint32_t{kAdd1_R3} << 16) | kAdd1_R3);
    b.SetReg(3, b.Const(0x77));
    b.AddCycles(4);
    b.KnownRefill(start + 4, (uint32_t{kAdd1_R3} << 16) | kAdd1_R3);
    b.Exit(start + 8, 4);
    REQUIRE(brimir::jit::VerifyBlock(block).empty());

    auto state = rig->BaseState(start);
    state.R[3] = 5;
    state.fetchedOpcodes = 0xCAFEF00D;
    rig->Load(state);
    ref->Load(state);
    auto &ctx = rig->sh2->GetJitContext();
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);

    auto prepared = backend->Prepare(block, ctx);
    REQUIRE(prepared != nullptr);
    // Same bytes, different array, after Prepare and before Finalize.
    std::copy_n(rig->ram->begin() + (start & 0xF0000u), 0x10000, page->begin());
    rig->bus.MapArray(start & 0x7FF0000u, (start & 0x7FF0000u) + 0xFFFF, *page, true);
    bool finalized = false;
    std::thread worker([&] { finalized = prepared->Finalize(); });
    worker.join();
    REQUIRE(finalized);
    NativeCode code;
    REQUIRE(backend->Commit(*prepared, code));
    REQUIRE(code.entry != nullptr);
    CHECK(code.selfValidating);

    // The block carries the arrays of Prepare time, so it reports stale on its first run.
    const auto before = rig->State();
    const ExitInfo info = backend->Run(code, ctx);
    CHECK(info.stale);
    CHECK(info.cycles == 0);
    CHECK(info.retired == 0);
    CHECK(rig->State().R[3] == before.R[3]);
    CHECK(rig->State().PC == before.PC);
    CHECK(rig->State().fetchedOpcodes == 0xCAFEF00Du);

    // Rebuilt against the new mapping, the block runs and matches the interpreter.
    NativeCode fresh;
    REQUIRE(backend->Compile(block, ctx, fresh));
    const ExitInfo ran = backend->Run(fresh, ctx);
    const ExitInfo expected = brimir::jit::RunBlock(block, ref->sh2->GetJitContext());
    CHECK_FALSE(ran.stale);
    CHECK(ran.retired == expected.retired);
    CHECK(ran.cycles == expected.cycles);
    const std::string diff = sh2test::DiffRigs(*ref, *rig);
    INFO(diff);
    CHECK(diff.empty());
    CHECK(rig->State().R[3] == 0x77u);
}

TEST_CASE("x64 backend: Prepare rejects what Compile rejects", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    Block block;
    block.startPC = kCode;
    block.guestInstrCount = 1;
    Builder b(block);
    b.SetReg(1, b.Add(b.GetReg(1), b.Const(1)));
    b.AddCycles(1);
    b.Exit(kCode + 2, 1);
    const brimir::jit::Op original = block.code.front().op;
    block.code.front().op = static_cast<brimir::jit::Op>(0xFF); // not an op

    auto rig = std::make_unique<Rig>();
    auto &ctx = rig->sh2->GetJitContext();
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    CHECK(backend->Prepare(block, ctx) == nullptr);
    static const int sentinel = 0;
    NativeCode out;
    out.entry = &sentinel;
    out.bytes = 7;
    CHECK_FALSE(backend->Compile(block, ctx, out));
    CHECK(out.entry == nullptr);
    CHECK(out.bytes == 0);
    CHECK(backend->CodeBytes() == 0);

    // A state field that does not fit rejects it in Prepare too.
    block.code.front().op = original;
    auto badCtx = ctx;
    badCtx.R = nullptr;
    CHECK(backend->Prepare(block, badCtx) == nullptr);
    CHECK(backend->CodeBytes() == 0);
}

// ---- Block chaining (milestone 2C item 2, design/sh2-x64-performance.md) ----

namespace {

constexpr uint16_t kNop = 0x0009;
constexpr uint16_t kAdd1_R4 = 0x7401; // add #1,R4

// bra `to`, placed at `from`.
uint16_t Bra(uint32_t from, uint32_t to) {
    const int32_t disp = (static_cast<int32_t>(to) - static_cast<int32_t>(from) - 4) / 2;
    return static_cast<uint16_t>(0xA000 | (static_cast<uint32_t>(disp) & 0xFFFu));
}

// An interpreter rig and an x64 rig, both run through SH2::Advance (so peripherals compare too).
// The x64 executor runs with chaining, as Advance always does.
struct ChainPair {
    std::unique_ptr<Rig> ref = std::make_unique<Rig>();
    std::unique_ptr<Rig> jit = std::make_unique<Rig>();
    brimir::jit::Executor exec{BackendKind::X64, sh2test::kNativeOnFirstRun};

    void WriteCode(uint32_t address, const std::vector<uint16_t> &words) {
        ref->WriteCode(address, words);
        jit->WriteCode(address, words);
    }
    // Loads `state` on both rigs and attaches the executor (which flushes it).
    void Start(const ymir::savestate::SH2SaveState &state) {
        ref->Load(state);
        jit->Load(state);
        jit->sh2->SetJitExecutor(&exec);
    }
    // Advance(cycles) on both rigs: same cycles, state, memory, bus accesses and peripherals.
    void Advance(uint64_t cycles) {
        const uint64 refCycles = ref->sh2->Advance<false, false>(cycles);
        const uint64 jitCycles = jit->sh2->Advance<false, false>(cycles);
        REQUIRE(jitCycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*ref, *jit, true);
        INFO(diff);
        REQUIRE(diff.empty());
    }
};

constexpr uint32_t kIntrVbr = 0x06008000;
constexpr uint32_t kIntrVector = 0x50;
constexpr uint32_t kIntrHandler = 0x06009000;
constexpr uint32_t kIntrStack = 0x0600F000;

// The DIVU overflow interrupt (level 15, vector kIntrVector, handler: SLEEP), raised by a write to
// DVDNT while DVSR is 0 (as in test_jit_diff.cpp's interrupt tests).
void SetUpDivuInterrupt(Rig &rig) {
    rig.Write32(kIntrVbr + kIntrVector * 4, kIntrHandler);
    rig.WriteCode(kIntrHandler, {static_cast<uint16_t>(kSleep)});
    auto &ctx = rig.sh2->GetJitContext();
    ctx.write(ctx.sh2, 0xFFFFFF08, 4, 0x2);         // DVCR.OVFIE = 1
    ctx.write(ctx.sh2, 0xFFFFFF0C, 4, kIntrVector); // VCRDIV
    ctx.write(ctx.sh2, 0xFFFFFEE2, 1, 0xF0);        // IPRA: DIVU level 15
}

} // namespace

// The random programs of the fuzz test (jit_fuzz_programs.hpp), each on three rigs through
// SH2::Advance in random chunks: the interpreter, an x64 executor that chains blocks, and an x64
// executor with chaining off (Executor::Run then runs one block per Step). Both must match the
// interpreter at every Advance return, and chaining must run exactly the stepped executor's blocks.
TEST_CASE("Chained run equals stepped run", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr int kPrograms = 300;
    constexpr int kAdvances = 40;
    // 1: every block native from its first run. 3: blocks switch from RunBlock to native code in
    // the middle of the run (tiered compilation), on both executors at the same block run.
    const uint32_t threshold = GENERATE(sh2test::kNativeOnFirstRun, 3u);
    INFO("native compile threshold " << threshold);
    uint64_t totalChained = 0;
    uint64_t totalNative = 0;
    for (int prog = 0; prog < kPrograms; ++prog) {
        const uint32_t seed = 0xF0220000u + static_cast<uint32_t>(prog);
        const sh2test::FuzzProgram fuzz = sh2test::MakeFuzzProgram(seed);
        INFO("seed 0x" << std::hex << seed);
        auto ref = std::make_unique<Rig>();
        auto chainRig = std::make_unique<Rig>();
        auto stepRig = std::make_unique<Rig>();
        brimir::jit::Executor chainExec{BackendKind::X64, threshold};
        brimir::jit::Executor stepExec{BackendKind::X64, threshold};
        stepExec.SetChaining(false);
        for (Rig *rig : {ref.get(), chainRig.get(), stepRig.get()}) {
            rig->mmio.busWaitEvery = fuzz.busWaitEvery;
            rig->WriteCode(sh2test::kFuzzCode, fuzz.words);
            rig->Load(fuzz.state);
        }
        chainRig->sh2->SetJitExecutor(&chainExec);
        stepRig->sh2->SetJitExecutor(&stepExec);

        std::mt19937 rng(seed ^ 0xC4A1u);
        for (int i = 0; i < kAdvances; ++i) {
            const uint64_t cycles = 1 + rng() % 64;
            INFO("advance " << std::dec << i << " cycles " << cycles);
            const uint64 refCycles = ref->sh2->Advance<false, false>(cycles);
            const uint64 chainCycles = chainRig->sh2->Advance<false, false>(cycles);
            const uint64 stepCycles = stepRig->sh2->Advance<false, false>(cycles);
            REQUIRE(chainCycles == refCycles);
            REQUIRE(stepCycles == refCycles);
            {
                const std::string diff = sh2test::DiffRigs(*ref, *chainRig, true);
                INFO("chained: " << diff);
                REQUIRE(diff.empty());
            }
            {
                const std::string diff = sh2test::DiffRigs(*ref, *stepRig, true);
                INFO("stepped: " << diff);
                REQUIRE(diff.empty());
            }
            if (ref->State().sleep) {
                break;
            }
        }
        // A chain runs the blocks the stepped executor runs, one per Step.
        const auto &chained = chainExec.GetStats();
        const auto &stepped = stepExec.GetStats();
        REQUIRE(chained.blocksRun == stepped.blocksRun);
        REQUIRE(chained.nativeBlocksRun == stepped.nativeBlocksRun);
        REQUIRE(chained.interpreted == stepped.interpreted);
        REQUIRE(chained.compileFallbacks == stepped.compileFallbacks);
        // Stale entries may legitimately differ between chained and stepped runs (see
        // Executor::Stats::staleEntries), and with them the counts above. The fuzz programs never
        // write their own code (stores only reach the data areas), so there are none here.
        REQUIRE(chained.staleEntries == 0u);
        REQUIRE(stepped.staleEntries == 0u);
        REQUIRE(chainExec.Cache().Compiles() == stepExec.Cache().Compiles());
        REQUIRE(chainExec.Cache().NativeCompiles() == stepExec.Cache().NativeCompiles());
        REQUIRE(stepped.chainedBlocks == 0u);
        totalChained += chained.chainedBlocks;
        totalNative += chained.nativeBlocksRun;
    }
    // Measured (milestone 2C task 2): 34533 of 42789 native blocks chained (81%). The bound leaves
    // room for generator changes but fails if chaining mostly stops happening.
    WARN("threshold " << threshold << ": chained blocks " << totalChained << " of " << totalNative
                      << " native blocks");
    if (threshold == 1) {
        CHECK(totalChained * 100 >= totalNative * 60);
    } else {
        CHECK(totalChained > 0u); // fewer: blocks that never reach the threshold stay IR-only
    }
}

// A store raises the DIVU overflow interrupt at the end of a block (in a delay slot, or as the last
// instruction of a maximum-length block), so no boundary check inside the block sees it. The block
// chains to B, whose prologue must return to the executor: the interrupt is taken before B runs,
// exactly where the interpreter takes it.
TEST_CASE("Chain stops at a pending interrupt", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint16_t kStoreDvsr = 0x2762;  // mov.l R6,@R7 (R7 = DVSR)
    constexpr uint16_t kClearR6 = 0xE600;    // mov #0,R6
    constexpr uint16_t kStoreDvdnt = 0x2212; // mov.l R1,@R2 (R2 = DVDNT): divides by DVSR
    for (const bool inDelaySlot : {true, false}) {
        INFO((inDelaySlot ? "store in a delay slot" : "store ends a maximum-length block"));
        ChainPair p;
        // A sets DVSR = R6 (1 on the first pass, 0 afterwards) and then writes DVDNT; B is
        // add #1,R4 ; bra A ; nop. The first pass compiles A and B; the second raises the interrupt.
        std::vector<uint16_t> a;
        uint32_t b = 0;
        if (inDelaySlot) {
            b = kCode + 0x20;
            a = {kStoreDvsr, kClearR6, kAdd1_R3, Bra(kCode + 6, b), kStoreDvdnt};
        } else {
            a = {kStoreDvsr, kClearR6};
            while (a.size() < brimir::jit::kMaxBlockInstructions - 1) {
                a.push_back(kAdd1_R3);
            }
            a.push_back(kStoreDvdnt);
            b = kCode + 2 * static_cast<uint32_t>(a.size());
        }
        p.WriteCode(kCode, a);
        p.WriteCode(b, {kAdd1_R4, Bra(b + 2, kCode), kNop});
        for (Rig *rig : {p.ref.get(), p.jit.get()}) {
            SetUpDivuInterrupt(*rig);
        }
        auto state = p.ref->BaseState(kCode);
        // Interrupt mask 14: blocks IRL (raised at its reset level 1), lets the level-15 DIVU through.
        state.SR = 0xE0;
        state.VBR = kIntrVbr;
        state.R[1] = 1234;
        state.R[2] = 0xFFFFFF04;
        state.R[3] = 0;
        state.R[4] = 0;
        state.R[6] = 1;
        state.R[7] = 0xFFFFFF00;
        state.R[15] = kIntrStack;
        p.Start(state);
        REQUIRE(brimir::jit::BuildBlock(p.jit->sh2->GetJitContext(), kCode).guestInstrCount == a.size());

        p.Advance(400);
        const auto end = p.ref->State();
        REQUIRE(end.sleep);                            // the handler ran
        CHECK(end.R[4] == 1u);                         // B ran on the first pass only
        CHECK(p.ref->Read32(kIntrStack - 8) == b);     // stacked PC: B, before its first instruction
        CHECK(p.exec.GetStats().chainedBlocks >= 1u);  // B chained into A for the second pass
    }
}

// A ends at the maximum block length with a store over the next word (B's first instruction), after
// the refill that loaded it into the fetch buffer. The interpreter then runs the old opcode from the
// buffer. B, compiled earlier with the new opcode and linked, matches memory, so only the chained
// PC & 2 gate (buffer vs the compiled opcode) keeps it from running.
TEST_CASE("Chain respects the PC & 2 fetch-buffer gate", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint16_t kOld = 0x7401;      // add #1,R4
    constexpr uint16_t kNew = 0x7410;      // add #16,R4
    constexpr uint16_t kStoreR2 = 0x2121;  // mov.w R2,@R1 (R1 = B): B = kNew
    constexpr uint16_t kStoreR5 = 0x2151;  // mov.w R5,@R1: B = kOld
    const uint32_t a = kCode + 2;          // odd half
    const uint32_t b = kCode + 0x42;       // odd half, right after A's last instruction
    // kCode: nop (not run) ; A: 31 x add #1,R3 ; mov.w R2,@R1
    // B: kNew ; mov.w R5,@R1 ; bra A ; nop
    // Pass 1: B = kNew already, so the buffer matches and B is compiled with kNew. B then restores
    // kOld; every later pass refills kOld into the buffer before A's store writes kNew.
    std::vector<uint16_t> program{kNop};
    for (uint32_t i = 0; i < brimir::jit::kMaxBlockInstructions - 1; ++i) {
        program.push_back(kAdd1_R3);
    }
    program.push_back(kStoreR2);
    REQUIRE(kCode + 2 * program.size() == b);
    program.push_back(kNew);
    program.push_back(kStoreR5);
    program.push_back(Bra(b + 4, a));
    program.push_back(kNop);

    ChainPair p;
    p.WriteCode(kCode, program);
    auto state = p.ref->BaseState(a);
    state.R[1] = b;
    state.R[2] = kNew;
    state.R[3] = 0;
    state.R[4] = 0;
    state.R[5] = kOld;
    p.Start(state);
    REQUIRE(brimir::jit::BuildBlock(p.jit->sh2->GetJitContext(), a).guestInstrCount ==
            brimir::jit::kMaxBlockInstructions);

    // About 10 passes of about 37 cycles (fewer than 16, so R4 < 32 tells kOld from kNew).
    for (int i = 0; i < 4; ++i) {
        INFO("advance " << i);
        p.Advance(97);
    }
    const auto end = p.ref->State();
    CHECK(end.R[4] > 16u + 2u); // later passes ran kOld from the fetch buffer...
    CHECK(end.R[4] < 32u);      // ...never kNew
    CHECK(p.exec.GetStats().chainedBlocks >= 1u);
}

// A and B branch to each other. Once both are linked, B's first instruction is changed between
// Advance calls while the CPU is at A, so the next Advance reaches B by chaining. B's prologue
// reports stale; the executor recompiles it, and the results match the interpreter.
TEST_CASE("Stale block inside a chain", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint16_t kAdd2_R4 = 0x7402; // add #2,R4
    const uint32_t b = kCode + 0x20;
    ChainPair p;
    // A: add #1,R3 ; bra B ; nop      B: add #1,R4 ; bra A ; nop
    p.WriteCode(kCode, {kAdd1_R3, Bra(kCode + 2, b), kNop});
    p.WriteCode(b, {kAdd1_R4, Bra(b + 2, kCode), kNop});
    auto state = p.ref->BaseState(kCode);
    state.R[3] = 0;
    state.R[4] = 0;
    p.Start(state);

    bool modified = false;
    uint32_t r4AtChange = 0;
    for (int i = 0; i < 60; ++i) {
        INFO("advance " << i << (modified ? " (B changed)" : ""));
        const bool firstAfterChange = modified && p.exec.GetStats().staleEntries == 0;
        const uint64_t chainedBefore = p.exec.GetStats().chainedBlocks;
        p.Advance(modified ? 61 : 13);
        if (firstAfterChange) {
            // This Advance starts at A and reaches B by chaining: B is found stale there.
            CHECK(p.exec.GetStats().staleEntries == 1u);
            CHECK(p.exec.GetStats().chainedBlocks > chainedBefore);
        }
        const auto st = p.ref->State();
        if (!modified && i >= 8 && st.PC == kCode && !st.delaySlot) {
            REQUIRE(p.exec.GetStats().chainedBlocks >= 1u); // A and B are linked
            p.WriteCode(b, {kAdd2_R4});
            modified = true;
            r4AtChange = st.R[4];
        }
    }
    REQUIRE(modified);
    const auto &stats = p.exec.GetStats();
    CHECK(stats.staleEntries == 1u);
    CHECK(p.exec.Cache().Invalidations() == 1u);
    CHECK(p.ref->State().R[4] > r4AtChange + 2u); // B now adds 2 per pass
}

// A and B branch to each other and are linked. B's second opcode changes while the CPU is at A: the
// chain from A reaches B, B reports stale there and is dropped, and its code is retired (freed later,
// when the pool evicts it). Its link slot is cleared first (Release asserts that), so the next chain
// from A reaches the rebuilt B and never the old code.
TEST_CASE("x64 backend: a block dropped inside a chain is not chained to again", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint16_t kAdd2_R4 = 0x7402; // add #2,R4
    const uint32_t b = kCode + 0x20;
    ChainPair p;
    // A: add #1,R3 ; bra B ; nop      B: add #1,R4 ; add #1,R4 ; bra A ; nop
    p.WriteCode(kCode, {kAdd1_R3, Bra(kCode + 2, b), kNop});
    p.WriteCode(b, {kAdd1_R4, kAdd1_R4, Bra(b + 4, kCode), kNop});
    auto state = p.ref->BaseState(kCode);
    state.R[3] = 0;
    state.R[4] = 0;
    p.Start(state);

    // Both native and published once the chain has run from A to B.
    for (int i = 0; i < 20; ++i) {
        p.Advance(13);
    }
    const auto at = [&](uint32_t pc) {
        const auto *block = p.exec.Cache().Find(pc);
        REQUIRE(block != nullptr);
        CHECK(block->code.entry != nullptr);
        CHECK(block->code.selfValidating);
    };
    at(kCode);
    at(b);
    REQUIRE(p.exec.GetStats().chainedBlocks >= 1u);
    REQUIRE(p.exec.Cache().Invalidations() == 0u);
    REQUIRE(p.exec.GetStats().staleEntries == 0u);

    // Advance until the CPU is at A (not in a delay slot).
    const auto advanceToA = [&] {
        for (int i = 0; i < 64; ++i) {
            const auto st = p.ref->State();
            if (st.PC == kCode && !st.delaySlot) {
                return;
            }
            p.Advance(1);
        }
        FAIL("the CPU never got back to A");
    };

    // Change B's second opcode while the CPU is at A.
    advanceToA();
    p.WriteCode(b + 2, {kAdd2_R4});

    const uint64_t chainedBefore = p.exec.GetStats().chainedBlocks;
    p.Advance(61); // from A: B is stale in the chain, dropped and rebuilt
    CHECK(p.exec.GetStats().staleEntries == 1u);
    CHECK(p.exec.Cache().Invalidations() == 1u);
    CHECK(p.exec.GetStats().chainedBlocks > chainedBefore);
    // B's old code is retired (unpublished and kept for reuse), not freed.
    CHECK(p.exec.Cache().StaleNativeBytes() > 0u);
    CHECK(p.exec.Cache().RetiredCodeBytes() == p.exec.Cache().StaleNativeBytes());
    CHECK(p.exec.Cache().NativeBytesReleased() == 0u);
    at(b);

    // Chains from A now reach the rebuilt B.
    advanceToA();
    const uint64_t chainedAfterRebuild = p.exec.GetStats().chainedBlocks;
    const uint32_t r4 = p.ref->State().R[4];
    p.Advance(61);
    p.Advance(61);
    CHECK(p.exec.GetStats().staleEntries == 1u); // the rebuilt B is not stale
    CHECK(p.exec.Cache().Invalidations() == 1u);
    CHECK(p.exec.GetStats().chainedBlocks > chainedAfterRebuild);
    CHECK(p.ref->State().R[4] > r4 + 2u);
}

namespace {

brimir::jit::Executor *g_chainExec = nullptr;
uint32 (*g_chainOrigRead)(void *, uint32, uint32, bool) = nullptr;
size_t g_cacheSizeAtFlush = 0;
uint32_t g_chainFlushes = 0;

// Requests a flush on every MMIO read, like a watchdog reset during a memory access.
uint32 FlushingMmioRead(void *sh2, uint32 address, uint32 size, bool instrFetch) {
    const uint32 value = g_chainOrigRead(sh2, address, size, instrFetch);
    if (IsMmio(address)) {
        g_cacheSizeAtFlush = g_chainExec->Cache().Size();
        ++g_chainFlushes;
        g_chainExec->Flush();
    }
    return value;
}

} // namespace

// A flush requested by a block reached through chaining is deferred (the cache stays intact while
// the chain runs), aborts that block and so the chain, and is applied once Run's block returns.
TEST_CASE("Flush during a chain", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint16_t kLoadR2 = 0x6212; // mov.l @R1,R2 (R1: MMIO)
    constexpr uint16_t kAdd1_R5 = 0x7501;
    const uint32_t b = kCode + 0x20;
    auto rig = std::make_unique<Rig>();
    brimir::jit::Executor exec{BackendKind::X64, sh2test::kNativeOnFirstRun};
    // A: add #1,R3 ; bra B ; nop      B: mov.l @R1,R2 ; add #1,R5 ; sleep
    rig->WriteCode(kCode, {kAdd1_R3, Bra(kCode + 2, b), kNop});
    rig->WriteCode(b, {kLoadR2, kAdd1_R5, static_cast<uint16_t>(kSleep)});
    auto state = rig->BaseState(kCode);
    state.R[1] = 0x22040000;
    state.R[2] = 0;
    state.R[3] = 0;
    state.R[5] = 0;
    rig->Load(state);
    auto &ctx = rig->sh2->GetJitContext();

    // Compile A and B (Step never chains), and measure A's cycles.
    const ExitInfo first = exec.Step(ctx);
    REQUIRE(first.retired == 3);
    REQUIRE(*ctx.PC == b);
    REQUIRE(exec.Step(ctx).retired == 2);
    REQUIRE(exec.Cache().Size() == 2);
    REQUIRE(exec.GetStats().chainedBlocks == 0u);

    // The executor is not attached to the CPU, so loading the state does not flush it.
    rig->Load(state);
    g_chainExec = &exec;
    g_chainOrigRead = ctx.read;
    g_chainFlushes = 0;
    ctx.read = FlushingMmioRead;
    // Budget for A plus one cycle: A chains to B, and Run stops after B aborts.
    const uint64 executed = exec.Run(ctx, 0, first.cycles + 1);
    ctx.read = g_chainOrigRead;
    g_chainExec = nullptr;

    CHECK(g_chainFlushes == 1u);
    CHECK(g_cacheSizeAtFlush == 2u);  // deferred while the chain ran
    CHECK(exec.Cache().Size() == 0u); // applied after it returned
    CHECK(executed > first.cycles);
    const auto st = rig->State();
    CHECK(st.PC == b);       // B aborted: PC is the one A wrote
    CHECK(st.R[3] == 1u);    // A ran
    CHECK(st.R[5] == 0u);    // B stopped at its load
    CHECK(exec.GetStats().chainedBlocks == 1u);
    CHECK(exec.GetStats().nativeBlocksRun == 4u);

    // The next step recompiles B and runs it.
    const ExitInfo next = exec.Step(ctx);
    CHECK_FALSE(next.aborted);
    CHECK(next.retired == 2);
    CHECK(rig->State().R[5] == 1u);
}
namespace {

// Records the refillPipeline and endDelaySlot callbacks with the running cycle count each sees.
struct CallbackRecord {
    char kind; // 'R' refillPipeline, 'E' endDelaySlot
    uint32_t address;
    uint64_t cyclesExecuted;
};
std::vector<CallbackRecord> *g_records = nullptr;
const ymir::sh2::SH2JitContext *g_recordCtx = nullptr;
void (*g_recordOrigRefill)(void *, uint32) = nullptr;
void (*g_recordOrigEndDelaySlot)(void *) = nullptr;

void RecordingRefill(void *sh2, uint32 address) {
    g_records->push_back({'R', address, *g_recordCtx->cyclesExecuted});
    g_recordOrigRefill(sh2, address);
}

void RecordingEndDelaySlot(void *sh2) {
    g_records->push_back({'E', 0, *g_recordCtx->cyclesExecuted});
    g_recordOrigEndDelaySlot(sh2);
}

} // namespace

// A chained block must see what the executor's own step would give it: Executor::Run's
// *cyclesExecuted update (seen by C's delay-slot end, a callback before any SyncCycles in C), and
// a codeDirty flag of its own (A's handler write must not turn B's known refill into a callback).
TEST_CASE("Chained blocks see the state a step would give them", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint32_t kMmioTarget = 0x22001002; // handler page, bit 1 set: TrEndDelaySlot
    constexpr uint16_t kStoreMmio = 0x2122;      // mov.l R2,@R1 (R1: MMIO), before A's last refill
    constexpr uint16_t kJmpR3 = 0x432B;          // jmp @R3
    constexpr uint16_t kStoreRam = 0x2452;       // mov.l R5,@R4 (R4: RAM data, not code)
    const uint32_t b = kCode + 0x20;
    const uint32_t c = kCode + 0x40;
    std::vector<CallbackRecord> records[2];
    uint64_t chainedBlocks[2] = {};
    for (int chaining = 0; chaining < 2; ++chaining) {
        INFO("chaining " << chaining);
        auto rig = std::make_unique<Rig>();
        brimir::jit::Executor exec{BackendKind::X64, sh2test::kNativeOnFirstRun};
        exec.SetChaining(chaining != 0);
        // A: mov.l R2,@R1 ; bra B ; nop      kMmioTarget: SLEEP
        // B: mov.l R5,@R4 ; nop ; bra C ; nop (the refill before bra follows B's store, so it
        // consults codeDirty, which B's store leaves clear)
        // C: jmp @R3 ; nop (no SyncCycles before the delay-slot end)
        rig->WriteCode(kCode, {kStoreMmio, Bra(kCode + 2, b), kNop});
        rig->WriteCode(b, {kStoreRam, kNop, Bra(b + 4, c), kNop});
        rig->WriteCode(c, {kJmpR3, kNop});
        for (uint32_t i = 0; i < 4; ++i) {
            rig->mmio.data[(kMmioTarget + 2 * i) & 0xFFFF] = 0x00;
            rig->mmio.data[(kMmioTarget + 2 * i + 1) & 0xFFFF] = static_cast<uint8_t>(kSleep);
        }
        auto state = rig->BaseState(kCode);
        state.R[1] = 0x22040000;
        state.R[2] = 0x12345678;
        state.R[3] = kMmioTarget;
        state.R[4] = 0x06040000;
        state.R[5] = 0x55AA55AA;
        rig->Load(state);
        auto &ctx = rig->sh2->GetJitContext();
        exec.Run(ctx, 1000, 1200); // compiles A and B, and links them
        REQUIRE(exec.Cache().Size() >= 3);
        rig->Load(state); // the executor is not attached: no flush

        g_records = &records[chaining];
        g_recordCtx = &ctx;
        g_recordOrigRefill = ctx.refillPipeline;
        g_recordOrigEndDelaySlot = ctx.endDelaySlot;
        ctx.refillPipeline = RecordingRefill;
        ctx.endDelaySlot = RecordingEndDelaySlot;
        const uint64_t chainedBefore = exec.GetStats().chainedBlocks;
        exec.Run(ctx, 5000, 5200);
        ctx.refillPipeline = g_recordOrigRefill;
        ctx.endDelaySlot = g_recordOrigEndDelaySlot;
        g_records = nullptr;
        chainedBlocks[chaining] = exec.GetStats().chainedBlocks - chainedBefore;
        CHECK(rig->State().PC == kMmioTarget); // asleep at the target
    }
    CHECK(chainedBlocks[0] == 0u);
    CHECK(chainedBlocks[1] >= 2u); // B and C were entered by chaining
    REQUIRE(records[0].size() >= 2u);
    for (size_t i = 0; i < std::max(records[0].size(), records[1].size()); ++i) {
        INFO("callback " << i);
        REQUIRE(i < records[0].size());
        REQUIRE(i < records[1].size());
        CHECK(records[1][i].kind == records[0][i].kind);
        CHECK(records[1][i].address == records[0][i].address);
        CHECK(records[1][i].cyclesExecuted == records[0][i].cyclesExecuted);
    }
}
namespace {

brimir::jit::Executor *g_srExec = nullptr;
void (*g_srOrigSetSR)(void *, uint32, bool) = nullptr;
uint32_t g_srFlushes = 0;

// setSR that requests a flush. Unlike read/write/refill, the setSR trampoline does not stop the
// block on an abort request (nor does RunBlock): the block runs to its exit.
void FlushingSetSR(void *sh2, uint32 value, bool delaySlot) {
    g_srOrigSetSR(sh2, value, delaySlot);
    ++g_srFlushes;
    g_srExec->Flush();
}

} // namespace

// A flush requested by a callback that does not stop the block (here setSR) must end the chain at
// that block's exit, as a step would: the executor applies the flush and steps on. Chaining on
// with the request pending would abort the next block at its first memory callback, after part of
// its effects, and re-run it.
TEST_CASE("A flush from a non-stopping callback ends the chain", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint16_t kLdcR6Sr = 0x460E;   // ldc R6,SR (R6 = 0xF0: SR unchanged)
    constexpr uint16_t kAdd1_R7 = 0x7701;
    constexpr uint16_t kLoadR2 = 0x6212;    // mov.l @R1,R2 (R1: MMIO, a read callback)
    const uint32_t c = kCode + 0x20;
    const auto setUp = [&](Rig &rig) {
        // A: add #1,R3 ; ldc R6,SR ; bra C ; nop
        // C: add #1,R7 ; mov.l @R1,R2 ; add #1,R4 ; bra A ; nop
        rig.WriteCode(kCode, {kAdd1_R3, kLdcR6Sr, Bra(kCode + 4, c), kNop});
        rig.WriteCode(c, {kAdd1_R7, kLoadR2, kAdd1_R4, Bra(c + 6, kCode), kNop});
        auto state = rig.BaseState(kCode);
        state.R[1] = 0x22040000;
        state.R[3] = 0;
        state.R[4] = 0;
        state.R[6] = 0xF0;
        state.R[7] = 0;
        rig.Load(state);
        return state;
    };
    const auto hook = [](brimir::jit::Executor &exec, ymir::sh2::SH2JitContext &ctx) {
        g_srExec = &exec;
        g_srOrigSetSR = ctx.setSR;
        g_srFlushes = 0;
        ctx.setSR = FlushingSetSR;
    };

    SECTION("the flush is applied when the block returns") {
        auto rig = std::make_unique<Rig>();
        brimir::jit::Executor exec{BackendKind::X64, sh2test::kNativeOnFirstRun};
        const auto state = setUp(*rig);
        auto &ctx = rig->sh2->GetJitContext();
        const ExitInfo a = exec.Step(ctx); // compiles A
        REQUIRE(a.retired == 4);
        rig->Load(state); // not attached: no flush
        hook(exec, ctx);
        const uint64 executed = exec.Run(ctx, 0, a.cycles); // exactly A
        ctx.setSR = g_srOrigSetSR;
        CHECK(executed == a.cycles);
        CHECK(g_srFlushes == 1u);
        CHECK(exec.Cache().Size() == 0u);
        CHECK(rig->State().PC == c);
        CHECK(rig->State().R[3] == 1u);
    }

    SECTION("chained and stepped runs stay identical") {
        std::unique_ptr<Rig> rigs[2];
        std::unique_ptr<brimir::jit::Executor> execs[2];
        uint32_t flushes[2] = {};
        for (int chaining = 0; chaining < 2; ++chaining) {
            rigs[chaining] = std::make_unique<Rig>();
            execs[chaining] = std::make_unique<brimir::jit::Executor>(BackendKind::X64, sh2test::kNativeOnFirstRun);
            execs[chaining]->SetChaining(chaining != 0);
            setUp(*rigs[chaining]);
            auto &ctx = rigs[chaining]->sh2->GetJitContext();
            hook(*execs[chaining], ctx);
            execs[chaining]->Run(ctx, 0, 400);
            ctx.setSR = g_srOrigSetSR;
            flushes[chaining] = g_srFlushes;
        }
        const std::string diff = sh2test::DiffRigs(*rigs[0], *rigs[1], true);
        INFO(diff);
        CHECK(diff.empty());
        CHECK(flushes[1] == flushes[0]);
        CHECK(flushes[0] >= 3u);
        const auto end = rigs[1]->State();
        CHECK(end.R[7] - end.R[4] <= 1u); // C never re-ran after a partial run
        const auto &stepped = execs[0]->GetStats();
        const auto &chained = execs[1]->GetStats();
        CHECK(chained.blocksRun == stepped.blocksRun);
        CHECK(chained.interpreted == stepped.interpreted);
        CHECK(execs[1]->Cache().Compiles() == execs[0]->Cache().Compiles());
        CHECK(execs[1]->Cache().Size() == execs[0]->Cache().Size());
    }
}

// A chained block counts its cycles from the start of the chain, so its guarded run must compare
// against the same chain-relative limit as its checks (ir_opt.hpp, rule 5).
// A: add #1,R3 x3 ; bra B ; nop      B: add #1,R4 x10 ; bra A ; nop
// Both have guarded runs. After a first run has compiled and linked them, each run starts at A
// with the target at every instruction of B (on the first and later passes, so B runs second and
// later in a chain); the chaining x64 executor must stop exactly where the RunBlock executor does.
TEST_CASE("x64 guarded run in a chained block", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    const uint32_t b = kCode + 0x40;
    const auto setUp = [&](Rig &rig) {
        rig.WriteCode(kCode, {kAdd1_R3, kAdd1_R3, kAdd1_R3, Bra(kCode + 6, b), kNop});
        std::vector<uint16_t> bCode(10, kAdd1_R4);
        bCode.push_back(Bra(b + 20, kCode));
        bCode.push_back(kNop);
        rig.WriteCode(b, bCode);
        auto state = rig.BaseState(kCode);
        state.R[3] = 0;
        state.R[4] = 0;
        rig.Load(state);
    };
    auto refRig = std::make_unique<Rig>();
    auto jitRig = std::make_unique<Rig>();
    brimir::jit::Executor refExec{BackendKind::Ir};
    brimir::jit::Executor jitExec{BackendKind::X64, sh2test::kNativeOnFirstRun};
    setUp(*refRig);
    setUp(*jitRig);
    auto &refCtx = refRig->sh2->GetJitContext();
    auto &jitCtx = jitRig->sh2->GetJitContext();
    for (const uint32_t pc : {kCode, b}) {
        Block block = brimir::jit::BuildBlock(jitCtx, pc);
        brimir::jit::OptimizeBlock(block);
        INFO(brimir::jit::PrintBlock(block));
        CHECK(std::count_if(block.code.begin(), block.code.end(),
                            [](const brimir::jit::Inst &in) { return in.op == brimir::jit::Op::GuardRun; }) == 1);
    }
    refExec.Run(refCtx, 0, 200); // compiles A and B, and links them
    jitExec.Run(jitCtx, 0, 200);
    const uint64_t chainedBefore = jitExec.GetStats().chainedBlocks;
    constexpr uint64_t kLoop = 6 + 13; // cycles of A and of B
    for (uint64_t target = 1; target <= 3 * kLoop; ++target) {
        INFO("target " << target);
        setUp(*refRig); // the executors are not attached: no flush
        setUp(*jitRig);
        const uint64 refCycles = refExec.Run(refCtx, 0, target);
        const uint64 jitCycles = jitExec.Run(jitCtx, 0, target);
        REQUIRE(jitCycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*refRig, *jitRig, true);
        INFO(diff);
        REQUIRE(diff.empty());
    }
    CHECK(jitExec.GetStats().chainedBlocks - chainedBefore >= 3 * 2);
}

// Div1 lowers to inline code on the cached SR (x64_emitter.cpp, Op::Div1); Div1Step is the reference.
TEST_CASE("x64 inline Div1 matches Div1Step", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    constexpr uint32_t kInputs[] = {0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678};
    for (const bool rmIsRn : {false, true}) {
        Block block;
        block.startPC = kCode;
        block.guestInstrCount = 1;
        Builder b(block);
        const ValueId rn = b.GetReg(2);
        b.SetReg(2, b.Div1(rn, rmIsRn ? rn : b.GetReg(1), rmIsRn));
        b.AddCycles(1);
        b.Exit(kCode + 2, 1);
        REQUIRE(brimir::jit::VerifyBlock(block).empty());
        for (uint32_t bits = 0; bits < 8; ++bits) {
            for (const uint32_t rnValue : kInputs) {
                for (const uint32_t rmValue : kInputs) {
                    const uint32_t srIn = 0x0F0 | (bits & 1u) | ((bits & 2u) << 7) | ((bits & 4u) << 7);
                    INFO("rmIsRn " << rmIsRn << " SR 0x" << std::hex << srIn << " rn 0x" << rnValue << " rm 0x"
                                   << rmValue);
                    auto rig = std::make_unique<Rig>();
                    auto state = rig->BaseState(kCode);
                    state.SR = srIn;
                    state.R[2] = rnValue;
                    state.R[1] = rmValue;
                    rig->Load(state);
                    auto &ctx = rig->sh2->GetJitContext();
                    NativeCode code;
                    REQUIRE(backend->Compile(block, ctx, code));
                    backend->Run(code, ctx, kNoCycleTarget);
                    uint32_t sr = srIn;
                    const uint32_t expected = brimir::jit::Div1Step(rnValue, rmValue, rmIsRn, sr);
                    const auto st = rig->State();
                    CHECK(st.R[2] == expected);
                    CHECK(st.SR == sr);
                    CHECK(st.R[1] == rmValue);
                }
            }
        }
        backend->Reset();
    }
}

// SetT leaves SR dirty in the register cache and each Div1 leaves it dirty again; the Div1s and GetT
// must use the cached value, not memory (which holds the entry SR, T clear and Q/M set here).
TEST_CASE("x64 inline Div1 uses the cached SR", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    Block block;
    block.startPC = kCode;
    block.guestInstrCount = 3;
    Builder b(block);
    b.SetT(b.Const(1));
    b.SetReg(2, b.Div1(b.GetReg(2), b.GetReg(1), false));
    b.SetReg(3, b.Div1(b.GetReg(3), b.GetReg(1), false));
    b.SetReg(0, b.GetT());
    b.AddCycles(3);
    b.Exit(kCode + 6, 3);
    REQUIRE(brimir::jit::VerifyBlock(block).empty());

    for (const uint32_t srIn : {0x0F0u, 0x0F0u | 0x100, 0x0F0u | 0x200, 0x0F0u | 0x300}) {
        auto irRig = std::make_unique<Rig>();
        auto x64Rig = std::make_unique<Rig>();
        auto state = irRig->BaseState(kCode);
        state.SR = srIn;
        state.R[1] = 0x0C0FFEE5;
        state.R[2] = 0x80000001;
        state.R[3] = 0x7FFFFFF0;
        state.R[0] = 0xDEADBEEF;
        for (Rig *rig : {irRig.get(), x64Rig.get()}) {
            rig->Load(state);
        }
        INFO("SR in 0x" << std::hex << srIn);
        auto &x64Ctx = x64Rig->sh2->GetJitContext();
        NativeCode code;
        REQUIRE(backend->Compile(block, x64Ctx, code));
        const ExitInfo ir = brimir::jit::RunBlock(block, irRig->sh2->GetJitContext());
        const ExitInfo x64 = backend->Run(code, x64Ctx, kNoCycleTarget);
        RequireSameOutcome(ir, x64, *irRig, *x64Rig);
        CHECK(x64Rig->State().R[0] == (x64Rig->State().SR & 1u));
        CHECK(x64Rig->State().R[2] == irRig->State().R[2]);
        CHECK(x64Rig->State().R[3] == irRig->State().R[3]);
    }
}

// A run of DIV1s is a guarded run: the target falls at every instruction of it, so both copies
// (with and without per-check stores of the cached SR) must stop exactly where RunBlock does.
TEST_CASE("x64 inline Div1 in a guarded run", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint16_t kDiv1_R1_R2 = 0x3214; // div1 R1,R2
    constexpr uint16_t kDiv1_R2_R2 = 0x3224; // div1 R2,R2
    std::vector<uint16_t> program;
    for (int i = 0; i < 12; ++i) {
        program.push_back(i % 5 == 4 ? kDiv1_R2_R2 : kDiv1_R1_R2);
    }
    program.push_back(static_cast<uint16_t>(kSleep));
    brimir::jit::Executor refExec{BackendKind::Ir};
    brimir::jit::Executor jitExec{BackendKind::X64, sh2test::kNativeOnFirstRun};
    auto refRig = std::make_unique<Rig>();
    auto jitRig = std::make_unique<Rig>();
    for (Rig *rig : {refRig.get(), jitRig.get()}) {
        rig->WriteCode(kCode, program);
    }
    auto &refCtx = refRig->sh2->GetJitContext();
    auto &jitCtx = jitRig->sh2->GetJitContext();
    Block block = brimir::jit::BuildBlock(jitCtx, kCode);
    brimir::jit::OptimizeBlock(block);
    INFO(brimir::jit::PrintBlock(block));
    REQUIRE(std::count_if(block.code.begin(), block.code.end(),
                          [](const brimir::jit::Inst &in) { return in.op == brimir::jit::Op::GuardRun; }) >= 1);
    for (uint32_t srBits = 0; srBits < 8; ++srBits) {
        for (uint64_t target = 1; target <= 16; ++target) {
            INFO("SR bits " << srBits << " target " << target);
            auto state = refRig->BaseState(kCode);
            state.SR = 0x0F0 | (srBits & 1u) | ((srBits & 6u) << 7);
            state.R[1] = 0x0C0FFEE5;
            state.R[2] = 0x80000001u + srBits * 0x01234567u;
            for (Rig *rig : {refRig.get(), jitRig.get()}) {
                rig->Load(state);
            }
            const uint64 refCycles = refExec.Run(refCtx, 0, target);
            const uint64 jitCycles = jitExec.Run(jitCtx, 0, target);
            REQUIRE(jitCycles == refCycles);
            const std::string diff = sh2test::DiffRigs(*refRig, *jitRig, true);
            INFO(diff);
            REQUIRE(diff.empty());
        }
    }
}

// ---- Constant addresses (2D B2): the block's page is resolved at compile time ----

namespace {

enum class ConstKind { Load, LoadFetch, Store, CyclesRead, CyclesWrite, BusWaitRead, BusWaitWrite, Count };

// A block with one access at the constant `address`, between instructions that must not be disturbed.
Block MakeConstAccessBlock(ConstKind kind, uint32_t address, uint8_t size) {
    Block block;
    block.startPC = kCode;
    block.guestInstrCount = 1;
    Builder b(block);
    b.SetReg(1, b.Const(0x1111));
    b.AddCycles(2);
    b.SyncCycles();
    const ValueId addr = b.Const(address);
    switch (kind) {
    case ConstKind::Load: b.SetReg(2, b.Load(addr, size, false)); break;
    case ConstKind::LoadFetch: b.SetReg(2, b.Load(addr, size, true)); break;
    case ConstKind::Store: b.Store(addr, size, b.Const(0xA1B2C3D4u ^ address)); break;
    case ConstKind::CyclesRead: b.AddAccessCycles(addr, size, false); break;
    case ConstKind::CyclesWrite: b.AddAccessCycles(addr, size, true); break;
    case ConstKind::BusWaitRead: b.ExitIfBusWait(addr, size, false, kCode, 0); break;
    case ConstKind::BusWaitWrite: b.ExitIfBusWait(addr, size, true, kCode, 0); break;
    case ConstKind::Count: break;
    }
    b.SetReg(3, b.Const(0x3333));
    b.AddCycles(1);
    b.Exit(kCode + 2, 1);
    REQUIRE(brimir::jit::VerifyBlock(block).empty());
    return block;
}

// Maps the rig's MMIO handlers on [start, end] (a RAM page turned into a handler page).
void MapMmioHandlers(Rig &rig, uint32_t start, uint32_t end) {
    rig.bus.MapNormal(
        start, end, &rig.mmio,
        [](uint32_t address, void *ctx) -> uint8_t {
            auto &m = *static_cast<sh2test::Mmio *>(ctx);
            const uint8_t value = m.data[address & 0xFFFF];
            m.log.push_back({'R', 1, address, value});
            return value;
        },
        [](uint32_t address, void *ctx) -> uint16_t {
            auto &m = *static_cast<sh2test::Mmio *>(ctx);
            const uint16_t value = static_cast<uint16_t>((m.data[address & 0xFFFE] << 8) | m.data[(address & 0xFFFE) + 1]);
            m.log.push_back({'R', 2, address, value});
            return value;
        },
        [](uint32_t address, void *ctx) -> uint32_t {
            auto &m = *static_cast<sh2test::Mmio *>(ctx);
            const uint8_t *p = &m.data[address & 0xFFFC];
            const uint32_t value = (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
            m.log.push_back({'R', 4, address, value});
            return value;
        },
        [](uint32_t address, uint8_t value, void *ctx) {
            auto &m = *static_cast<sh2test::Mmio *>(ctx);
            m.data[address & 0xFFFF] = value;
            m.log.push_back({'W', 1, address, value});
        },
        [](uint32_t address, uint16_t value, void *ctx) {
            auto &m = *static_cast<sh2test::Mmio *>(ctx);
            m.data[address & 0xFFFE] = static_cast<uint8_t>(value >> 8);
            m.data[(address & 0xFFFE) + 1] = static_cast<uint8_t>(value);
            m.log.push_back({'W', 2, address, value});
        },
        [](uint32_t address, uint32_t value, void *ctx) {
            auto &m = *static_cast<sh2test::Mmio *>(ctx);
            uint8_t *p = &m.data[address & 0xFFFC];
            p[0] = static_cast<uint8_t>(value >> 24);
            p[1] = static_cast<uint8_t>(value >> 16);
            p[2] = static_cast<uint8_t>(value >> 8);
            p[3] = static_cast<uint8_t>(value);
            m.log.push_back({'W', 4, address, value});
        },
        [](uint32_t address, uint32_t size, bool, void *ctx) -> bool {
            auto &m = *static_cast<sh2test::Mmio *>(ctx);
            m.log.push_back({'B', static_cast<uint8_t>(size), address, 0});
            return false;
        });
}

} // namespace

TEST_CASE("x64 constant-address fast path matches the dynamic path", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    auto irRig = std::make_unique<Rig>();
    auto x64Rig = std::make_unique<Rig>();
    auto irRom = std::make_unique<sh2test::FastRom>();
    auto x64Rom = std::make_unique<sh2test::FastRom>();
    sh2test::MapFastPathTestPages(*irRig, *irRom);
    sh2test::MapFastPathTestPages(*x64Rig, *x64Rom);
    const sh2test::FastRom romBefore = *irRom;
    for (Rig *rig : {irRig.get(), x64Rig.get()}) {
        for (uint32_t i = 0; i < 0x40; ++i) {
            rig->Write32(sh2test::kFastRamOffset - 0x80 + i * 4, 0x01020304u * (i + 1) ^ 0xA5C3E1F7u);
        }
        for (uint32_t i = 0; i < 0x100; ++i) {
            rig->mmio.data[(sh2test::kFastMmioOffset + i - 0x80) & 0xFFFF] = static_cast<uint8_t>(i * 13 + 7);
        }
    }
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);

    uint32_t runs = 0;
    for (const uint32_t address : sh2test::FastPathTestAddresses()) {
        for (const uint8_t size : {uint8_t{1}, uint8_t{2}, uint8_t{4}}) {
            for (int k = 0; k < static_cast<int>(ConstKind::Count); ++k) {
                const Block block = MakeConstAccessBlock(static_cast<ConstKind>(k), address, size);
                for (const uint32_t busWaitEvery : {0u, 1u}) {
                    INFO("address 0x" << std::hex << address << std::dec << " size " << int{size} << " kind " << k
                                      << " busWaitEvery " << busWaitEvery);
                    auto state = irRig->BaseState(kCode);
                    state.R[2] = 0xDEADBEEF;
                    for (Rig *rig : {irRig.get(), x64Rig.get()}) {
                        rig->Load(state);
                        rig->mmio.busWaitEvery = busWaitEvery;
                        rig->mmio.busWaitQueries = 0;
                        rig->mmio.log.clear();
                    }
                    ymir::sh2::SH2JitContext x64Ctx = x64Rig->sh2->GetJitContext();
                    NativeCode code;
                    REQUIRE(backend->Compile(block, x64Ctx, code));
                    const ExitInfo ir = brimir::jit::RunBlock(block, irRig->sh2->GetJitContext());
                    const ExitInfo x64 = backend->Run(code, x64Ctx, kNoCycleTarget);
                    RequireSameOutcome(ir, x64, *irRig, *x64Rig, true);
                    REQUIRE(*irRom == romBefore);
                    REQUIRE(*x64Rom == romBefore);
                    if (++runs % 512 == 0) {
                        backend->Reset();
                    }
                }
            }
        }
    }
    CHECK(runs == 128u * 3u * 7u * 2u);
}

// Compiled against one mapping, run against another: the compile-time page must not be used.
TEST_CASE("x64 constant-address access follows a page remapped after compile", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    enum class Remap { None, RamToRom, RamToRam, RamToMmio, RamToUnmapped, CyclesChanged, RomToRam, RamReadOnly, RomWritable, Count };
    constexpr uint32_t kPageStart = 0x06000000; // the 64 KiB page of kFastRamOffset
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    uint32_t runs = 0;
    // Partition 0b000 (cached) and the cache-through mirrors of the page.
    for (const uint32_t partition : {0u, 1u, 5u}) {
        for (int r = 0; r < static_cast<int>(Remap::Count); ++r) {
            const auto remap = static_cast<Remap>(r);
            for (const uint8_t size : {uint8_t{1}, uint8_t{2}, uint8_t{4}}) {
                for (int k = 0; k < static_cast<int>(ConstKind::Count); ++k) {
                    const auto kind = static_cast<ConstKind>(k);
                    // RomToRam compiles against the ROM page (a read-only array) at 0x4000000.
                    const uint32_t address =
                        (partition << 29) | (remap == Remap::RomToRam || remap == Remap::RomWritable ? sh2test::kFastRomOffset : sh2test::kFastRamOffset);
                    const Block block = MakeConstAccessBlock(kind, address, size);
                    INFO("partition " << partition << " remap " << r << " size " << int{size} << " kind " << k);

                    auto irRig = std::make_unique<Rig>();
                    auto x64Rig = std::make_unique<Rig>();
                    auto irRom = std::make_unique<sh2test::FastRom>();
                    auto x64Rom = std::make_unique<sh2test::FastRom>();
                    sh2test::MapFastPathTestPages(*irRig, *irRom);
                    sh2test::MapFastPathTestPages(*x64Rig, *x64Rom);
                    auto irOther = std::make_unique<std::array<uint8_t, 0x10000>>();
                    auto x64Other = std::make_unique<std::array<uint8_t, 0x10000>>();
                    for (size_t i = 0; i < irOther->size(); ++i) {
                        (*irOther)[i] = (*x64Other)[i] = static_cast<uint8_t>(i * 91 + 3);
                    }
                    for (Rig *rig : {irRig.get(), x64Rig.get()}) {
                        for (uint32_t i = 0; i < 0x40; ++i) {
                            rig->Write32(sh2test::kFastRamOffset - 0x80 + i * 4, 0x01020304u * (i + 1) ^ 0xA5C3E1F7u);
                        }
                        for (uint32_t i = 0; i < 0x100; ++i) {
                            rig->mmio.data[(sh2test::kFastRamOffset + i - 0x80) & 0xFFFF] = static_cast<uint8_t>(i * 13 + 7);
                            rig->mmio.data[(sh2test::kFastRomOffset + i - 0x80) & 0xFFFF] = static_cast<uint8_t>(i * 13 + 7);
                        }
                    }
                    ymir::sh2::SH2JitContext x64Ctx = x64Rig->sh2->GetJitContext();
                    NativeCode code;
                    REQUIRE(backend->Compile(block, x64Ctx, code));

                    const auto apply = [&](Rig &rig, std::array<uint8_t, 0x10000> &other, sh2test::FastRom &rom) {
                        switch (remap) {
                        case Remap::None: break;
                        case Remap::RamToRom: rig.bus.MapArray(kPageStart, kPageStart + 0xFFFF, rom, false); break;
                        case Remap::RamToRam: rig.bus.MapArray(kPageStart, kPageStart + 0xFFFF, other, true); break;
                        case Remap::RamToMmio: MapMmioHandlers(rig, kPageStart, kPageStart + 0xFFFF); break;
                        case Remap::RamToUnmapped: rig.bus.Unmap(kPageStart, kPageStart + 0xFFFF); break;
                        case Remap::CyclesChanged:
                            rig.bus.SetAccessCycles(kPageStart, kPageStart + 0xFFFF, 11, 12, 13, 14, 15, 16);
                            break;
                        case Remap::RomToRam: rig.bus.MapArray(0x4000000, 0x400FFFF, other, true); break;
                        // The same array, only writable changes.
                        case Remap::RamReadOnly: rig.bus.MapArray(kPageStart, kPageStart + 0xFFFF, *rig.ram, false); break;
                        case Remap::RomWritable: rig.bus.MapArray(0x4000000, 0x400FFFF, rom, true); break;
                        case Remap::Count: break;
                        }
                    };
                    apply(*irRig, *irOther, *irRom);
                    apply(*x64Rig, *x64Other, *x64Rom);

                    auto state = irRig->BaseState(kCode);
                    state.R[2] = 0xDEADBEEF;
                    for (Rig *rig : {irRig.get(), x64Rig.get()}) {
                        rig->Load(state);
                        rig->mmio.busWaitEvery = 0;
                        rig->mmio.log.clear();
                    }
                    const ExitInfo ir = brimir::jit::RunBlock(block, irRig->sh2->GetJitContext());
                    const ExitInfo x64 = backend->Run(code, x64Ctx, kNoCycleTarget);
                    RequireSameOutcome(ir, x64, *irRig, *x64Rig, true);
                    REQUIRE(*irOther == *x64Other);
                    REQUIRE(*irRom == *x64Rom);
                    if (++runs % 256 == 0) {
                        backend->Reset();
                    }
                }
            }
        }
    }
    CHECK(runs == 3u * static_cast<uint32_t>(Remap::Count) * 3u * 7u);
}
