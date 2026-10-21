// Brimir - SH-2 JIT executor tests (cache flush semantics)
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "jit_test_backend.hpp"
#include "jit_worker_helpers.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/jit/backend.hpp>
#include <brimir/jit/block_cache.hpp>
#include <brimir/jit/compile_worker.hpp>
#include <brimir/jit/executor.hpp>
#include <brimir/jit/interp_backend.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <string>
#include <vector>

using sh2test::kSleep;
using sh2test::Rig;

namespace {

constexpr uint32_t kCode = 0x06001000;
// On the rig's MMIO page: every access goes through the memory callback, even once a native
// backend inlines RAM accesses, so the flushing callbacks below always run.
constexpr uint32_t kData = 0x22040000;

// The rig's MMIO page is 64 KiB, mirrored; data is big-endian.
void WriteMmio32(Rig &rig, uint32_t address, uint32_t value) {
    const uint32_t off = address & 0xFFFC;
    rig.mmio.data[off + 0] = static_cast<uint8_t>(value >> 24);
    rig.mmio.data[off + 1] = static_cast<uint8_t>(value >> 16);
    rig.mmio.data[off + 2] = static_cast<uint8_t>(value >> 8);
    rig.mmio.data[off + 3] = static_cast<uint8_t>(value);
}

uint32_t ReadMmio32(const Rig &rig, uint32_t address) {
    const uint32_t off = address & 0xFFFC;
    return (static_cast<uint32_t>(rig.mmio.data[off + 0]) << 24) | (static_cast<uint32_t>(rig.mmio.data[off + 1]) << 16) |
           (static_cast<uint32_t>(rig.mmio.data[off + 2]) << 8) | rig.mmio.data[off + 3];
}

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
    brimir::jit::Executor exec{sh2test::TestBackend(), sh2test::kNativeOnFirstRun};
    rig->WriteCode(kCode, {kMovL_R1_R2, kAdd1_R3, kAdd1_R3, kSleep});
    WriteMmio32(*rig, kData, 0x12345678);
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

namespace {

constexpr uint16_t kAdd16_R3 = 0x7310; // add #16,R3

// Counts peekInstruction callbacks (block validation falls back to it off array pages).
uint32_t g_peeks = 0;
uint16 (*g_origPeek)(void *, uint32) = nullptr;

uint16 CountingPeek(void *sh2, uint32 address) {
    ++g_peeks;
    return g_origPeek(sh2, address);
}

struct PeekCounter {
    ymir::sh2::SH2JitContext &ctx;
    explicit PeekCounter(ymir::sh2::SH2JitContext &c)
        : ctx(c) {
        g_peeks = 0;
        g_origPeek = ctx.peekInstruction;
        ctx.peekInstruction = CountingPeek;
    }
    ~PeekCounter() {
        ctx.peekInstruction = g_origPeek;
    }
};

// Code on the rig's MMIO (handler) page. The rig maps only read/write handlers there, so this adds
// side-effect-free peeks over the same bytes (like real MMIO with peek handlers).
constexpr uint32_t kMmioCode = 0x22001000;

void MapMmioPeeks(Rig &rig) {
    rig.bus.MapSideEffectFree(
        0x2000000, 0x3FFFFFF, &rig.mmio,
        [](uint32_t address, void *ctx) -> uint8_t { return static_cast<sh2test::Mmio *>(ctx)->data[address & 0xFFFF]; },
        [](uint32_t address, void *ctx) -> uint16_t {
            const auto &d = static_cast<sh2test::Mmio *>(ctx)->data;
            const uint32_t off = address & 0xFFFE;
            return static_cast<uint16_t>((d[off] << 8) | d[off + 1]);
        },
        [](uint32_t address, void *ctx) -> uint32_t {
            const auto &d = static_cast<sh2test::Mmio *>(ctx)->data;
            const uint32_t off = address & 0xFFFC;
            return (static_cast<uint32_t>(d[off]) << 24) | (static_cast<uint32_t>(d[off + 1]) << 16) |
                   (static_cast<uint32_t>(d[off + 2]) << 8) | d[off + 3];
        });
}

void WriteMmioCode(Rig &rig, uint32_t address, const std::vector<uint16_t> &words) {
    for (size_t i = 0; i < words.size(); ++i) {
        const uint32_t off = (address + static_cast<uint32_t>(i * 2)) & 0xFFFE;
        rig.mmio.data[off] = static_cast<uint8_t>(words[i] >> 8);
        rig.mmio.data[off + 1] = static_cast<uint8_t>(words[i]);
    }
}

} // namespace

TEST_CASE("Executor: modified code in RAM is recompiled", "[jit][executor]") {
    const auto kind = GENERATE(from_range(sh2test::AvailableBackends()));
    INFO("backend " << brimir::jit::BackendName(kind));
    auto rig = std::make_unique<Rig>();
    brimir::jit::Executor exec{kind, sh2test::kNativeOnFirstRun};
    auto &ctx = rig->sh2->GetJitContext();
    rig->WriteCode(kCode, {kAdd1_R3, kAdd1_R3, kSleep});
    auto state = rig->BaseState(kCode);
    state.R[3] = 0;
    rig->Load(state);

    auto info = exec.Step(ctx);
    REQUIRE(info.retired == 2);
    CHECK(rig->State().R[3] == 2u);

    // Unchanged code on a RAM page: the hit is validated without the peek callback.
    rig->Load(rig->BaseState(kCode));
    {
        const PeekCounter counter{ctx};
        info = exec.Step(ctx);
    }
    CHECK(g_peeks == 0u);
    REQUIRE(info.retired == 2);
    CHECK(rig->State().R[3] == 4u);
    CHECK(exec.Cache().Invalidations() == 0);
    CHECK(exec.Cache().Compiles() == 1);

    rig->WriteCode(kCode + 2, {kAdd16_R3});
    rig->Load(rig->BaseState(kCode));
    info = exec.Step(ctx);
    REQUIRE(info.retired == 2);
    CHECK(rig->State().R[3] == 4u + 1u + 16u);
    CHECK(rig->State().PC == kCode + 4);
    CHECK(exec.Cache().Invalidations() == 1);
    CHECK(exec.Cache().Compiles() == 2);
    CHECK(exec.Cache().Size() == 1);
    CHECK(exec.GetStats().blocksRun == 3);
    CHECK(exec.GetStats().nativeBlocksRun == (kind == brimir::jit::BackendKind::Ir ? 0u : 3u));
}

TEST_CASE("Executor: code on a handler page is checked through the callback", "[jit][executor]") {
    const auto kind = GENERATE(from_range(sh2test::AvailableBackends()));
    INFO("backend " << brimir::jit::BackendName(kind));
    auto rig = std::make_unique<Rig>();
    MapMmioPeeks(*rig);
    brimir::jit::Executor exec{kind, sh2test::kNativeOnFirstRun};
    auto &ctx = rig->sh2->GetJitContext();
    WriteMmioCode(*rig, kMmioCode, {kAdd1_R3, kAdd1_R3, kSleep});
    auto state = rig->BaseState(kMmioCode);
    state.fetchedOpcodes = (static_cast<uint32_t>(kAdd1_R3) << 16) | kAdd1_R3;
    state.R[3] = 0;
    rig->Load(state);

    auto info = exec.Step(ctx);
    REQUIRE(info.retired == 2);
    CHECK(rig->State().R[3] == 2u);
    CHECK(rig->State().PC == kMmioCode + 4);
    CHECK(exec.GetStats().blocksRun == 1);

    // A hit on a handler page validates every opcode through peekInstruction.
    state = rig->State();
    state.PC = kMmioCode;
    state.fetchedOpcodes = (static_cast<uint32_t>(kAdd1_R3) << 16) | kAdd1_R3;
    rig->Load(state);
    {
        const PeekCounter counter{ctx};
        info = exec.Step(ctx);
    }
    CHECK(g_peeks >= 2u);
    REQUIRE(info.retired == 2);
    CHECK(rig->State().R[3] == 4u);
    CHECK(exec.Cache().Invalidations() == 0);

    // Changing the MMIO bytes invalidates the block.
    WriteMmioCode(*rig, kMmioCode + 2, {kAdd16_R3});
    state = rig->State();
    state.PC = kMmioCode;
    state.fetchedOpcodes = (static_cast<uint32_t>(kAdd1_R3) << 16) | kAdd16_R3;
    rig->Load(state);
    info = exec.Step(ctx);
    REQUIRE(info.retired == 2);
    CHECK(rig->State().R[3] == 4u + 1u + 16u);
    CHECK(exec.Cache().Invalidations() == 1);
    CHECK(exec.Cache().Compiles() == 2);
    CHECK(exec.GetStats().blocksRun == 3);
}

TEST_CASE("Executor: the recent table never returns a stale block", "[jit][executor]") {
    const auto kind = GENERATE(from_range(sh2test::AvailableBackends()));
    const bool flush = GENERATE(true, false);
    INFO("backend " << brimir::jit::BackendName(kind) << (flush ? " flush" : " invalidation"));
    auto rig = std::make_unique<Rig>();
    brimir::jit::Executor exec{kind, sh2test::kNativeOnFirstRun};
    auto &ctx = rig->sh2->GetJitContext();
    rig->WriteCode(kCode, {kAdd1_R3, kAdd1_R3, kSleep});
    auto state = rig->BaseState(kCode);
    state.R[3] = 0;
    rig->Load(state);

    // Twice, so the second lookup comes from the recent table.
    for (uint32_t i = 1; i <= 2; ++i) {
        rig->Load(rig->BaseState(kCode));
        REQUIRE(exec.Step(ctx).retired == 2);
        CHECK(rig->State().R[3] == 2u * i);
    }
    REQUIRE(exec.Cache().Compiles() == 1);

    if (flush) {
        exec.Flush();
        CHECK(exec.Cache().Size() == 0);
    }
    // Same PC, different code; build a block elsewhere too so freed memory may be reused.
    rig->WriteCode(kCode, {kAdd16_R3, kAdd16_R3, kSleep});
    rig->WriteCode(kCode + 0x100, {kAdd1_R3, kSleep});
    rig->Load(rig->BaseState(kCode + 0x100));
    REQUIRE(exec.Step(ctx).retired == 1);
    CHECK(rig->State().R[3] == 5u);

    for (uint32_t i = 1; i <= 2; ++i) {
        rig->Load(rig->BaseState(kCode));
        REQUIRE(exec.Step(ctx).retired == 2);
        CHECK(rig->State().R[3] == 5u + 32u * i);
    }
    CHECK(exec.Cache().Invalidations() == (flush ? 0u : 1u));
    CHECK(exec.Cache().Compiles() == 3);
    CHECK(exec.Cache().Size() == 2);
}

// A state load no longer flushes the block cache, so code that changed under a cached block (as
// when a state saved earlier is loaded into a core whose memory has moved on) must be caught by the
// checks every block goes through: the fetch-buffer gate at PC & 2, then the block's own validation.
TEST_CASE("Executor: code changed by a state load is not run from the cache", "[jit][executor]") {
    const auto kind = GENERATE(from_range(sh2test::AvailableBackends()));
    INFO("backend " << brimir::jit::BackendName(kind));
    const bool native = kind != brimir::jit::BackendKind::Ir;
    auto ref = std::make_unique<Rig>();
    auto jit = std::make_unique<Rig>();
    brimir::jit::Executor exec{kind, sh2test::kNativeOnFirstRun};
    auto &ctx = jit->sh2->GetJitContext();

    const auto step = [&] {
        const auto info = exec.Step(ctx);
        uint64_t refCycles = 0;
        for (uint32_t i = 0; i < info.retired; ++i) {
            refCycles += ref->sh2->Step<false, false>();
        }
        CHECK(info.cycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*ref, *jit);
        INFO(diff);
        CHECK(diff.empty());
        return info;
    };
    const auto load = [&](const ymir::savestate::SH2SaveState &state) {
        ref->Load(state);
        jit->Load(state);
    };

    // P: three add #1,R3 ; Q: three add #16,R3 at the same place.
    const std::vector<uint16_t> p = {kAdd1_R3, kAdd1_R3, kAdd1_R3, kSleep};
    const std::vector<uint16_t> q = {kAdd16_R3, kAdd16_R3, kAdd16_R3, kSleep};
    ref->WriteCode(kCode, p);
    jit->WriteCode(kCode, p);
    auto state = ref->BaseState(kCode);
    state.R[3] = 0;
    load(state);
    REQUIRE(step().retired == 3);
    CHECK(jit->State().R[3] == 3u);
    CHECK(exec.Cache().Size() == 1);
    CHECK(exec.GetStats().nativeBlocksRun == (native ? 1u : 0u));

    // The code changes; the loaded state still holds P's second word in its fetch buffer and
    // resumes at PC & 2, where the interpreter runs the buffered opcode, not memory's.
    ref->WriteCode(kCode, q);
    jit->WriteCode(kCode, q);
    state = ref->BaseState(kCode + 2);
    state.fetchedOpcodes = (uint32_t{kAdd1_R3} << 16) | kAdd1_R3;
    load(state);
    REQUIRE(step().retired == 1);
    CHECK(exec.GetStats().interpreted == 1);
    CHECK(jit->State().R[3] == 4u);
    CHECK(exec.Cache().Invalidations() == 0);

    // Resuming at the block start must not run the cached P.
    load(ref->BaseState(kCode));
    REQUIRE(step().retired == 3);
    CHECK(jit->State().R[3] == 4u + 3u * 16u);
    CHECK(jit->State().PC == kCode + 6);
    CHECK(exec.Cache().Invalidations() == 1);
    CHECK(exec.Cache().Compiles() == 2);
    CHECK(exec.GetStats().staleEntries == (native ? 1u : 0u));
    CHECK(exec.Cache().Size() == 1);
}

TEST_CASE("Executor: flush outside a block clears the cache immediately", "[jit][executor]") {
    auto rig = std::make_unique<Rig>();
    brimir::jit::Executor exec{sh2test::TestBackend(), sh2test::kNativeOnFirstRun};
    rig->WriteCode(kCode, {kAdd1_R3, kAdd1_R3, kSleep});
    rig->Load(rig->BaseState(kCode));

    exec.Step(rig->sh2->GetJitContext());
    REQUIRE(exec.Cache().Size() == 1);
    exec.Flush();
    CHECK(exec.Cache().Size() == 0);
}

TEST_CASE("Executor: flush during a store aborts the block after the store", "[jit][executor]") {
    auto rig = std::make_unique<Rig>();
    brimir::jit::Executor exec{sh2test::TestBackend(), sh2test::kNativeOnFirstRun};
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
    CHECK(ReadMmio32(*rig, kData) == 0xA5A5A5A5u); // the store itself completed
    CHECK(rig->State().R[3] == 0u);           // nothing after it ran
    CHECK(rig->State().PC == kCode);
    CHECK(exec.Cache().Size() == 0);

    // Recovery: the next step recompiles and runs the block from the start (re-running the store,
    // here with the same value) through both adds.
    const auto next = exec.Step(ctx);
    CHECK_FALSE(next.aborted);
    CHECK(next.retired == 3);
    CHECK(ReadMmio32(*rig, kData) == 0xA5A5A5A5u);
    CHECK(rig->State().R[3] == 2u);
    CHECK(rig->State().PC == kCode + 6);
    CHECK(exec.Cache().Size() == 1);
}

// Tiered compilation (brimir::jit::kNativeCompileThreshold): a new block runs N-1 times with
// RunBlock, then its Nth run compiles it natively, frees its IR and runs the native code.
TEST_CASE("Executor: a block runs N-1 times on IR and then natively", "[jit][executor][tier]") {
    if (!brimir::jit::IsBackendAvailable(brimir::jit::BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    const uint32_t threshold = GENERATE(2u, 4u, brimir::jit::kNativeCompileThreshold);
    INFO("threshold " << threshold);
    auto rig = std::make_unique<Rig>();
    brimir::jit::Executor exec{brimir::jit::BackendKind::X64, threshold};
    REQUIRE(exec.Cache().NativeCompileThreshold() == threshold);
    auto &ctx = rig->sh2->GetJitContext();
    rig->WriteCode(kCode, {kAdd1_R3, kAdd1_R3, kSleep});
    auto state = rig->BaseState(kCode);
    state.R[3] = 0;
    rig->Load(state);

    uint32_t run = 1;
    for (; run < threshold; ++run) {
        INFO("IR run " << run);
        rig->Load(rig->BaseState(kCode));
        REQUIRE(exec.Step(ctx).retired == 2);
        CHECK(rig->State().R[3] == 2u * run);
        CHECK(exec.GetStats().blocksRun == run);
        CHECK(exec.GetStats().nativeBlocksRun == 0u);
        const brimir::jit::CachedBlock *entry = exec.Cache().Find(kCode);
        REQUIRE(entry != nullptr);
        CHECK(entry->code.entry == nullptr);
        CHECK_FALSE(entry->block.code.empty());
        CHECK(exec.Cache().CachedInsts() == entry->block.code.size());
        CHECK(exec.Cache().NativeCompiles() == 0u);
    }

    // The threshold run, then one more: both native, compiled once.
    for (uint32_t extra = 0; extra < 2; ++extra, ++run) {
        INFO("native run " << run);
        rig->Load(rig->BaseState(kCode));
        REQUIRE(exec.Step(ctx).retired == 2);
        CHECK(rig->State().R[3] == 2u * run);
        CHECK(exec.GetStats().blocksRun == run);
        CHECK(exec.GetStats().nativeBlocksRun == extra + 1);
        const brimir::jit::CachedBlock *entry = exec.Cache().Find(kCode);
        REQUIRE(entry != nullptr);
        CHECK(entry->code.entry != nullptr);
        // The IR is gone; what validation and the executor need is kept.
        CHECK(entry->block.code.empty());
        CHECK(entry->block.code.capacity() == 0u);
        CHECK(entry->block.startPC == kCode);
        CHECK(entry->block.guestInstrCount == 2u);
        REQUIRE(entry->block.guestOpcodes.size() >= 2u);
        CHECK(entry->block.guestOpcodes[0] == kAdd1_R3);
        CHECK(entry->block.guestOpcodes[1] == kAdd1_R3);
        CHECK(exec.Cache().CachedInsts() == 0u);
        CHECK(exec.Cache().NativeCompiles() == 1u);
        CHECK(exec.Cache().Compiles() == 1u);
    }
}

TEST_CASE("Executor: the IR backend keeps every block on IR", "[jit][executor][tier]") {
    auto rig = std::make_unique<Rig>();
    brimir::jit::Executor exec{brimir::jit::BackendKind::Ir, 2};
    auto &ctx = rig->sh2->GetJitContext();
    rig->WriteCode(kCode, {kAdd1_R3, kAdd1_R3, kSleep});
    rig->Load(rig->BaseState(kCode));
    for (uint32_t run = 1; run <= 5; ++run) {
        rig->Load(rig->BaseState(kCode));
        REQUIRE(exec.Step(ctx).retired == 2);
    }
    CHECK(exec.GetStats().blocksRun == 5u);
    CHECK(exec.GetStats().nativeBlocksRun == 0u);
    CHECK(exec.Cache().NativeCompiles() == 0u);
    const brimir::jit::CachedBlock *entry = exec.Cache().Find(kCode);
    REQUIRE(entry != nullptr);
    CHECK_FALSE(entry->block.code.empty());
    CHECK(exec.Cache().CachedInsts() == entry->block.code.size());
}

// Without a native backend nothing has native code to keep: the native pool stays empty. A block
// whose code changed goes to the IR pool instead, so code that comes back is not built again.
TEST_CASE("Executor: the IR backend has no native pool and reuses dropped IR blocks", "[jit][executor]") {
    auto rig = std::make_unique<Rig>();
    brimir::jit::Executor exec{brimir::jit::BackendKind::Ir, sh2test::kNativeOnFirstRun};
    auto &ctx = rig->sh2->GetJitContext();
    rig->WriteCode(kCode, {kAdd1_R3, kAdd1_R3, kSleep});
    auto state = rig->BaseState(kCode);
    state.R[3] = 0;
    rig->Load(state);
    REQUIRE(exec.Step(ctx).retired == 2);
    REQUIRE(exec.Cache().Compiles() == 1u);

    uint32_t r3 = 2;
    for (uint32_t i = 1; i <= 4; ++i) {
        INFO("rewrite " << i);
        const bool q = (i % 2) != 0;
        rig->WriteCode(kCode, {q ? kAdd16_R3 : kAdd1_R3, kAdd1_R3, kSleep});
        rig->Load(rig->BaseState(kCode));
        REQUIRE(exec.Step(ctx).retired == 2);
        r3 += (q ? 16u : 1u) + 1u;
        CHECK(rig->State().R[3] == r3);
        CHECK(exec.Cache().Compiles() == 2u); // the first rewrite builds Q, the others reuse
        CHECK(exec.Cache().RetiredIrReuses() == i - 1);
        CHECK(exec.Cache().Invalidations() == i);
    }
    CHECK(exec.Cache().RetiredReuses() == 0u);
    CHECK(exec.Cache().RetiredCodeBytes() == 0u);
    CHECK(exec.Cache().RetiredEvictions() == 0u);
}

// The IR-instruction budget counts IR-only blocks only. A native block whose code changed is
// dropped and rebuilt from scratch as an IR-only block (it never needs its freed IR), and a
// flush drops native blocks the same way.
TEST_CASE("Executor: kMaxCachedInsts accounting ignores native blocks", "[jit][executor][tier]") {
    if (!brimir::jit::IsBackendAvailable(brimir::jit::BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint32_t kHot = kCode;
    constexpr uint32_t kCold = kCode + 0x100;
    auto rig = std::make_unique<Rig>();
    brimir::jit::Executor exec{brimir::jit::BackendKind::X64, 2};
    auto &ctx = rig->sh2->GetJitContext();
    rig->WriteCode(kHot, {kAdd1_R3, kAdd1_R3, kSleep});
    rig->WriteCode(kCold, {kAdd16_R3, kAdd1_R3, kAdd1_R3, kSleep});
    auto state = rig->BaseState(kHot);
    state.R[3] = 0;
    rig->Load(state);
    const auto stepAt = [&](uint32_t pc) {
        rig->Load(rig->BaseState(pc));
        return exec.Step(ctx);
    };
    const auto &cache = exec.Cache();

    REQUIRE(stepAt(kHot).retired == 2);
    const size_t hotInsts = cache.CachedInsts();
    REQUIRE(hotInsts > 0u);
    REQUIRE(stepAt(kHot).retired == 2); // second run: native
    REQUIRE(cache.Find(kHot)->code.entry != nullptr);
    CHECK(cache.CachedInsts() == 0u);
    REQUIRE(stepAt(kCold).retired == 3);
    REQUIRE(cache.Find(kCold)->code.entry == nullptr);
    const size_t coldInsts = cache.Find(kCold)->block.code.size();
    CHECK(cache.CachedInsts() == coldInsts);
    CHECK(rig->State().R[3] == 2u + 2u + 16u + 2u);

    // Change the hot block: its prologue reports stale, and it is rebuilt from scratch as IR.
    rig->WriteCode(kHot + 2, {kAdd16_R3});
    REQUIRE(stepAt(kHot).retired == 2);
    CHECK(rig->State().R[3] == 22u + 1u + 16u);
    CHECK(exec.GetStats().staleEntries == 1u);
    CHECK(cache.Invalidations() == 1u);
    const brimir::jit::CachedBlock *hot = cache.Find(kHot);
    REQUIRE(hot != nullptr);
    CHECK(hot->code.entry == nullptr);
    CHECK_FALSE(hot->block.code.empty());
    CHECK(cache.CachedInsts() == coldInsts + hot->block.code.size());
    REQUIRE(stepAt(kHot).retired == 2); // native again
    CHECK(rig->State().R[3] == 39u + 17u);
    CHECK(cache.Find(kHot)->code.entry != nullptr);
    CHECK(cache.CachedInsts() == coldInsts);
    CHECK(cache.NativeCompiles() == 2u);

    // A flush drops everything; the next run builds IR again.
    exec.Flush();
    CHECK(cache.Size() == 0u);
    CHECK(cache.CachedInsts() == 0u);
    CHECK(cache.FlushesRequested() == 1u);
    REQUIRE(stepAt(kHot).retired == 2);
    CHECK(rig->State().R[3] == 56u + 17u);
    REQUIRE(cache.Find(kHot) != nullptr);
    CHECK(cache.Find(kHot)->code.entry == nullptr);
    CHECK(cache.CachedInsts() == cache.Find(kHot)->block.code.size());
    CHECK(cache.IrEvictions() == 0u);
    CHECK(cache.FlushesCodeCap() == 0u);
}

namespace {

// Runs the block at kCode on the rig as Executor::StepImpl does (a native block that reports stale
// is dropped and the PC looked up again) and returns whether the block that ran was native.
bool StepNative(brimir::jit::BlockCache &cache, brimir::jit::INativeBackend &backend, Rig &rig) {
    rig.Load(rig.BaseState(kCode));
    auto &ctx = rig.sh2->GetJitContext();
    const auto run = [&](const brimir::jit::CachedBlock &entry) {
        return entry.code.entry != nullptr ? backend.Run(entry.code, ctx) : brimir::jit::RunBlock(entry.block, ctx);
    };
    const brimir::jit::CachedBlock *entry = &cache.Get(ctx, kCode);
    brimir::jit::ExitInfo info = run(*entry);
    if (info.stale) {
        cache.Invalidate(kCode);
        entry = &cache.Get(ctx, kCode);
        info = run(*entry);
        REQUIRE_FALSE(info.stale);
    }
    REQUIRE(info.retired == 2);
    return entry->code.entry != nullptr;
}

// add #imm,R3 ; add #1,R3 ; sleep: a different program for every imm (1-127).
void WriteProgram(Rig &rig, uint32_t imm) {
    rig.WriteCode(kCode, {static_cast<uint16_t>(0x7300 | (imm & 0x7F)), kAdd1_R3, kSleep});
}

// Steps until the block runs natively, from the first run after a (re)build, and returns how many
// runs were on IR. Then requires that the next run is native too.
uint32_t IrRunsUntilNative(brimir::jit::BlockCache &cache, brimir::jit::INativeBackend &backend, Rig &rig) {
    uint32_t irRuns = 0;
    while (!StepNative(cache, backend, rig)) {
        ++irRuns;
        REQUIRE(irRuns < 1000);
    }
    CHECK(StepNative(cache, backend, rig));
    return irRuns;
}

} // namespace

// A native block that reports stale is rebuilt on IR; the more often a PC has gone stale, the more
// IR runs the rebuilt block gets before its native compile (threshold << min(stale count, shift)).
TEST_CASE("Block cache: a PC that keeps going stale waits longer for native code", "[jit][executor][tier]") {
    if (!brimir::jit::IsBackendAvailable(brimir::jit::BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    const auto backend = brimir::jit::MakeNativeBackend(brimir::jit::BackendKind::X64);
    REQUIRE(backend != nullptr);
    brimir::jit::BlockCache cache{backend.get(), brimir::jit::kMaxNativeCodeBytes, 2, brimir::jit::kMaxCachedInsts, 0, 2};
    auto rig = std::make_unique<Rig>();
    for (uint32_t k = 0; k <= 5; ++k) {
        INFO("stale rewrites " << k);
        WriteProgram(*rig, 1 + k);
        CHECK(IrRunsUntilNative(cache, *backend, *rig) == (2u << std::min(k, 2u)) - 1);
        CHECK(cache.DampedBuilds() == k);
        CHECK(cache.Invalidations() == k);
    }
}

// A PC's stale count never decays by itself, but the wait it causes is bounded: unchanged code gets
// native code after at most threshold << shift runs. A flush forgets the counts.
TEST_CASE("Block cache: a damped PC still reaches native code", "[jit][executor][tier]") {
    if (!brimir::jit::IsBackendAvailable(brimir::jit::BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    const auto backend = brimir::jit::MakeNativeBackend(brimir::jit::BackendKind::X64);
    REQUIRE(backend != nullptr);
    brimir::jit::BlockCache cache{backend.get(), brimir::jit::kMaxNativeCodeBytes, 2, brimir::jit::kMaxCachedInsts, 0, 2};
    auto rig = std::make_unique<Rig>();
    for (uint32_t k = 0; k <= 10; ++k) {
        WriteProgram(*rig, 1 + k);
        REQUIRE(StepNative(cache, *backend, *rig) == false);
        uint32_t irRuns = 0;
        while (!StepNative(cache, *backend, *rig)) {
            REQUIRE(++irRuns < 1000);
        }
    }
    REQUIRE(cache.Invalidations() == 10u);

    // Unchanged code from here on: one more rewrite, then no stale report.
    WriteProgram(*rig, 100);
    CHECK(IrRunsUntilNative(cache, *backend, *rig) == (2u << 2) - 1);
    CHECK(cache.Invalidations() == 11u);

    cache.Flush();
    CHECK(IrRunsUntilNative(cache, *backend, *rig) == 1u);
    // Counting restarts after the flush.
    WriteProgram(*rig, 101);
    CHECK(IrRunsUntilNative(cache, *backend, *rig) == (2u << 1) - 1);
}

// threshold << shift must not overflow uint32 whatever the constructor arguments: with threshold 16
// a shift of 28 or more used to give 0 (runsUntilNative = UINT32_MAX, never native) and 32 or more
// is undefined. The wait only grows with the stale count and stays finite and non-zero.
TEST_CASE("Block cache: an oversized damping shift is clamped", "[jit][executor][tier]") {
    if (!brimir::jit::IsBackendAvailable(brimir::jit::BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    const auto backend = brimir::jit::MakeNativeBackend(brimir::jit::BackendKind::X64);
    REQUIRE(backend != nullptr);
    constexpr uint32_t kThreshold = 16;
    constexpr uint32_t kMaxShift = 32 - std::bit_width(kThreshold); // largest shift that fits uint32
    brimir::jit::BlockCache cache{backend.get(), brimir::jit::kMaxNativeCodeBytes, kThreshold,
                                  brimir::jit::kMaxCachedInsts, 0, 40};
    auto rig = std::make_unique<Rig>();
    rig->Load(rig->BaseState(kCode));
    WriteProgram(*rig, 1);
    auto &ctx = rig->sh2->GetJitContext();
    uint32_t previous = 0;
    for (uint32_t k = 0; k < 40; ++k) {
        INFO("stale drops " << k);
        const brimir::jit::CachedBlock &entry = cache.Get(ctx, kCode);
        const uint32_t expected = (kThreshold << std::min(k, kMaxShift)) - 1;
        REQUIRE(entry.runsUntilNative == expected);
        REQUIRE(entry.runsUntilNative >= previous);
        previous = entry.runsUntilNative;
        cache.Invalidate(kCode);
    }
    CHECK(cache.Invalidations() == 40u);
}

TEST_CASE("Block cache: damping off keeps the global threshold", "[jit][executor][tier]") {
    if (!brimir::jit::IsBackendAvailable(brimir::jit::BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    const auto backend = brimir::jit::MakeNativeBackend(brimir::jit::BackendKind::X64);
    REQUIRE(backend != nullptr);
    brimir::jit::BlockCache cache{backend.get(), brimir::jit::kMaxNativeCodeBytes, 2, brimir::jit::kMaxCachedInsts, 0, 0};
    auto rig = std::make_unique<Rig>();
    for (uint32_t k = 0; k <= 5; ++k) {
        INFO("stale rewrites " << k);
        WriteProgram(*rig, 1 + k);
        CHECK(IrRunsUntilNative(cache, *backend, *rig) == 1u);
    }
    CHECK(cache.Invalidations() == 5u);
    CHECK(cache.DampedBuilds() == 0u);
}

// Threshold 1 means native on the first run, always (tests and diagnostics): never damped.
TEST_CASE("Block cache: threshold 1 is never damped", "[jit][executor][tier]") {
    if (!brimir::jit::IsBackendAvailable(brimir::jit::BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    const auto backend = brimir::jit::MakeNativeBackend(brimir::jit::BackendKind::X64);
    REQUIRE(backend != nullptr);
    brimir::jit::BlockCache cache{backend.get(), brimir::jit::kMaxNativeCodeBytes, sh2test::kNativeOnFirstRun,
                                  brimir::jit::kMaxCachedInsts, 0, 4};
    auto rig = std::make_unique<Rig>();
    for (uint32_t k = 0; k <= 6; ++k) {
        INFO("stale rewrites " << k);
        WriteProgram(*rig, 1 + k);
        CHECK(IrRunsUntilNative(cache, *backend, *rig) == 0u);
    }
    CHECK(cache.Invalidations() == 6u);
    CHECK(cache.DampedBuilds() == 0u);
}

// --- The retired IR pool (design/sh2-jit-m2e.md, track E) ----------------------------------------

namespace {

constexpr uint32_t kIrThreshold = 16; // blocks stay IR-only for these tests

// An executor (threshold 16) next to a reference interpreter rig.
struct IrPair {
    std::unique_ptr<Rig> ref = std::make_unique<Rig>();
    std::unique_ptr<Rig> jit = std::make_unique<Rig>();
    brimir::jit::Executor exec;

    explicit IrPair(brimir::jit::BackendKind kind)
        : exec{kind, kIrThreshold} {
        auto state = ref->BaseState(kCode);
        state.R[3] = 0;
        Load(state);
    }
    const brimir::jit::BlockCache &Cache() const {
        return exec.Cache();
    }
    void WriteCode(uint32_t address, const std::vector<uint16_t> &words) {
        ref->WriteCode(address, words);
        jit->WriteCode(address, words);
    }
    void Load(const ymir::savestate::SH2SaveState &state) {
        ref->Load(state);
        jit->Load(state);
    }
    // One executor step from pc (registers kept) and the same instructions on the reference.
    brimir::jit::ExitInfo StepAt(uint32_t pc) {
        return StepFrom(ref->BaseState(pc));
    }
    brimir::jit::ExitInfo StepFrom(const ymir::savestate::SH2SaveState &state) {
        Load(state);
        const auto info = exec.Step(jit->sh2->GetJitContext());
        uint64_t refCycles = 0;
        for (uint32_t i = 0; i < info.retired; ++i) {
            refCycles += ref->sh2->Step<false, false>();
        }
        CHECK(info.cycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*ref, *jit);
        INFO(diff);
        CHECK(diff.empty());
        return info;
    }
};

const std::vector<uint16_t> kIrP = {kAdd1_R3, kAdd1_R3, kAdd1_R3, kSleep};
const std::vector<uint16_t> kIrQ = {kAdd16_R3, kAdd16_R3, kAdd16_R3, kSleep};

// A RAM page turned into a handler page over a copy of its bytes (no side effects, peeks included).
struct HandlerPage {
    std::array<uint8_t, 0x10000> data{};

    // Maps bus 0x6000000-0x600FFFF (SH-2 0x06000000, kCode's page) of rig to handlers over data.
    void Map(Rig &rig) {
        rig.bus.MapBoth(
            0x6000000, 0x600FFFF, this,
            [](uint32_t address, void *ctx) -> uint8_t { return static_cast<HandlerPage *>(ctx)->data[address & 0xFFFF]; },
            [](uint32_t address, void *ctx) -> uint16_t {
                const auto &d = static_cast<HandlerPage *>(ctx)->data;
                const uint32_t off = address & 0xFFFE;
                return static_cast<uint16_t>((d[off] << 8) | d[off + 1]);
            },
            [](uint32_t address, void *ctx) -> uint32_t {
                const auto &d = static_cast<HandlerPage *>(ctx)->data;
                const uint32_t off = address & 0xFFFC;
                return (static_cast<uint32_t>(d[off]) << 24) | (static_cast<uint32_t>(d[off + 1]) << 16) |
                       (static_cast<uint32_t>(d[off + 2]) << 8) | d[off + 3];
            },
            [](uint32_t address, uint8_t value, void *ctx) { static_cast<HandlerPage *>(ctx)->data[address & 0xFFFF] = value; },
            [](uint32_t address, uint16_t value, void *ctx) {
                auto &d = static_cast<HandlerPage *>(ctx)->data;
                const uint32_t off = address & 0xFFFE;
                d[off] = static_cast<uint8_t>(value >> 8);
                d[off + 1] = static_cast<uint8_t>(value);
            },
            [](uint32_t address, uint32_t value, void *ctx) {
                auto &d = static_cast<HandlerPage *>(ctx)->data;
                const uint32_t off = address & 0xFFFC;
                d[off] = static_cast<uint8_t>(value >> 24);
                d[off + 1] = static_cast<uint8_t>(value >> 16);
                d[off + 2] = static_cast<uint8_t>(value >> 8);
                d[off + 3] = static_cast<uint8_t>(value);
            });
    }
    void WriteCode(uint32_t address, const std::vector<uint16_t> &words) {
        for (size_t i = 0; i < words.size(); ++i) {
            const uint32_t off = (address + static_cast<uint32_t>(i * 2)) & 0xFFFE;
            data[off] = static_cast<uint8_t>(words[i] >> 8);
            data[off + 1] = static_cast<uint8_t>(words[i]);
        }
    }
};

// A cache for `kind` (nullptr backend for Ir) with threshold 16 and the given IR cap.
struct IrCache {
    std::unique_ptr<brimir::jit::INativeBackend> backend;
    brimir::jit::BlockCache cache;

    IrCache(brimir::jit::BackendKind kind, size_t maxCachedInsts)
        : backend(kind == brimir::jit::BackendKind::Ir ? nullptr : brimir::jit::MakeNativeBackend(kind))
        , cache{backend.get(), brimir::jit::kMaxNativeCodeBytes, kIrThreshold, maxCachedInsts} {}
};

} // namespace

TEST_CASE("Block cache: a dropped IR block that comes back is reused", "[jit][executor][tier]") {
    const auto kind = GENERATE(from_range(sh2test::AvailableBackends()));
    INFO("backend " << brimir::jit::BackendName(kind));
    IrPair p{kind};
    p.WriteCode(kCode, kIrP);
    REQUIRE(p.StepAt(kCode).retired == 3);
    const size_t pInsts = p.Cache().Find(kCode)->block.code.size();

    p.WriteCode(kCode, kIrQ);
    REQUIRE(p.StepAt(kCode).retired == 3); // P dropped and retired, Q built
    const size_t qInsts = p.Cache().Find(kCode)->block.code.size();
    CHECK(p.Cache().Compiles() == 2u);
    CHECK(p.Cache().RetiredIrInsts() == pInsts);
    CHECK(p.Cache().CachedInsts() == pInsts + qInsts);

    p.WriteCode(kCode, kIrP);
    REQUIRE(p.StepAt(kCode).retired == 3); // Q retired, P taken back without the front end
    CHECK(p.Cache().Compiles() == 2u);
    CHECK(p.Cache().RetiredIrReuses() == 1u);
    CHECK(p.Cache().Invalidations() == 2u);
    CHECK(p.Cache().RetiredIrInsts() == qInsts);
    CHECK(p.Cache().CachedInsts() == pInsts + qInsts);
    const brimir::jit::CachedBlock *entry = p.Cache().Find(kCode);
    REQUIRE(entry != nullptr);
    CHECK(entry->block.guestOpcodes.front() == kAdd1_R3);
    CHECK(entry->code.entry == nullptr);
    // Its runs restart as for a fresh build (this run was the first).
    CHECK(entry->runsUntilNative == (kind == brimir::jit::BackendKind::Ir ? 0u : kIrThreshold - 1));
    CHECK(p.jit->State().R[3] == 3u + 48u + 3u);

    REQUIRE(p.StepAt(kCode).retired == 3); // a recent-slot hit on the reused block
    CHECK(p.Cache().Compiles() == 2u);
    CHECK(p.jit->State().R[3] == 3u + 48u + 6u);
    CHECK(p.Cache().RetiredReuses() == 0u);
}

// Review Focus 5: the same bytes at the same address, but no longer on an array page. A fresh build
// would not fetch from arrays (nor know its refills), so the retired block must not be taken back.
TEST_CASE("Block cache: an IR block is not reused after its page stops being an array page", "[jit][executor][tier]") {
    const auto kind = GENERATE(from_range(sh2test::AvailableBackends()));
    INFO("backend " << brimir::jit::BackendName(kind));
    // Declared before the pair: the rigs' buses point at them until the rigs are gone.
    const auto pages = std::make_unique<std::array<HandlerPage, 2>>();
    IrPair p{kind};
    p.WriteCode(kCode, kIrP);
    REQUIRE(p.StepAt(kCode).retired == 3);
    REQUIRE(p.Cache().Find(kCode)->block.fetchFromArrays);
    p.WriteCode(kCode, kIrQ);
    REQUIRE(p.StepAt(kCode).retired == 3); // P retired
    REQUIRE(p.Cache().RetiredIrInsts() > 0u);

    Rig *rigs[2] = {p.ref.get(), p.jit.get()};
    for (int i = 0; i < 2; ++i) {
        HandlerPage &page = (*pages)[i];
        std::copy_n(rigs[i]->ram->begin(), 0x10000, page.data.begin());
        page.WriteCode(kCode, kIrP);
        page.Map(*rigs[i]);
    }
    auto state = p.ref->BaseState(kCode);
    state.fetchedOpcodes = (uint32_t{kAdd1_R3} << 16) | kAdd1_R3; // what the handler page holds
    REQUIRE(p.StepFrom(state).retired == 3);
    CHECK(p.Cache().RetiredIrReuses() == 0u);
    CHECK(p.Cache().Compiles() == 3u);
    const brimir::jit::CachedBlock *entry = p.Cache().Find(kCode);
    REQUIRE(entry != nullptr);
    CHECK(entry->block.guestOpcodes.front() == kAdd1_R3);
    CHECK_FALSE(entry->block.fetchFromArrays);
    CHECK(p.jit->State().R[3] == 3u + 48u + 3u);
}

// The front end also reads the word that stops a block. Here it was SLEEP and becomes a supported
// instruction, so the block's own words are unchanged but a fresh build is longer.
TEST_CASE("Block cache: an IR block is not reused after the word that ended it changed", "[jit][executor][tier]") {
    const auto kind = GENERATE(from_range(sh2test::AvailableBackends()));
    INFO("backend " << brimir::jit::BackendName(kind));
    IrPair p{kind};
    p.WriteCode(kCode, {kAdd1_R3, kAdd1_R3, kSleep}); // last instruction at kCode + 2: no tail word
    REQUIRE(p.StepAt(kCode).retired == 2);
    REQUIRE(p.Cache().Find(kCode)->block.guestOpcodes.size() == 2u);
    p.WriteCode(kCode, kIrQ);
    REQUIRE(p.StepAt(kCode).retired == 3);

    p.WriteCode(kCode, {kAdd1_R3, kAdd1_R3, kAdd16_R3, kSleep});
    REQUIRE(p.StepAt(kCode).retired == 3);
    CHECK(p.Cache().RetiredIrReuses() == 0u);
    CHECK(p.Cache().Compiles() == 3u);
    CHECK(p.jit->State().R[3] == 2u + 48u + 18u);
}

TEST_CASE("Block cache: retired IR blocks count toward the IR cap", "[jit][executor][tier]") {
    const auto kind = GENERATE(from_range(sh2test::AvailableBackends()));
    INFO("backend " << brimir::jit::BackendName(kind));
    constexpr uint32_t kOther = kCode + 0x100;
    constexpr uint32_t kOther2 = kCode + 0x200;
    auto rig = std::make_unique<Rig>();
    auto &ctx = rig->sh2->GetJitContext();

    // The IR sizes of P and Q, from a cache without a small cap.
    size_t pInsts = 0;
    size_t qInsts = 0;
    {
        IrCache probe{kind, brimir::jit::kMaxCachedInsts};
        rig->WriteCode(kCode, kIrP);
        pInsts = probe.cache.Get(ctx, kCode).block.code.size();
        rig->WriteCode(kCode, kIrQ);
        qInsts = probe.cache.Get(ctx, kCode).block.code.size();
    }
    REQUIRE(pInsts > 0u);
    REQUIRE(qInsts > 0u);
    const size_t cap = pInsts + qInsts;
    REQUIRE(qInsts < cap - cap / 8); // freeing one of them is enough
    REQUIRE(pInsts < cap - cap / 8);

    IrCache c{kind, cap};
    auto &cache = c.cache;
    rig->WriteCode(kCode, kIrP);
    rig->WriteCode(kOther, {kAdd1_R3, kSleep});
    cache.Get(ctx, kCode);
    CHECK(cache.CachedInsts() == pInsts);
    rig->WriteCode(kCode, kIrQ);
    cache.Get(ctx, kCode); // P retired: still counted
    CHECK(cache.RetiredIrInsts() == pInsts);
    CHECK(cache.CachedInsts() == pInsts + qInsts);
    CHECK(cache.IrEvictions() == 0u);

    // At the cap: P, retired and older than Q, is freed; that is enough, so Q stays.
    const size_t otherInsts = cache.Get(ctx, kOther).block.code.size();
    REQUIRE(otherInsts < pInsts);
    CHECK(cache.RetiredIrReleases() == 1u);
    CHECK(cache.IrEvictions() == 1u); // one cap event
    CHECK(cache.IrEvictedBlocks() == 0u);
    CHECK(cache.RetiredIrInsts() == 0u);
    CHECK(cache.CachedInsts() == qInsts + otherInsts);
    REQUIRE(cache.Find(kCode) != nullptr);
    CHECK(cache.Find(kCode)->block.guestOpcodes.front() == kAdd16_R3);
    CHECK(cache.Find(kOther) != nullptr);

    rig->WriteCode(kCode, kIrP);
    cache.Get(ctx, kCode); // P is gone from the pool: rebuilt
    CHECK(cache.RetiredIrReuses() == 0u);
    CHECK(cache.Compiles() == 4u);
    CHECK(cache.IrEvictions() == 1u);

    // Live IR alone at the cap (the pool is empty): the least recently used live block is evicted.
    IrCache live{kind, cap};
    rig->WriteCode(kCode, kIrP);
    rig->WriteCode(kOther2, kIrQ);
    live.cache.Get(ctx, kCode);
    live.cache.Get(ctx, kOther2);
    REQUIRE(live.cache.CachedInsts() >= cap);
    REQUIRE(live.cache.RetiredIrInsts() == 0u);
    CHECK(live.cache.Get(ctx, kOther).block.code.size() == otherInsts);
    CHECK(live.cache.IrEvictions() == 1u);
    CHECK(live.cache.IrEvictedBlocks() == 1u);
    CHECK(live.cache.RetiredIrReleases() == 0u);
    CHECK(live.cache.CachedInsts() == qInsts + otherInsts);
    CHECK(live.cache.Find(kCode) == nullptr);
    CHECK(live.cache.Find(kOther2) != nullptr);
}

namespace {

// add #imm,R3 ; add #1,R3 ; sleep at kCode + 0x400 + 0x20 * i: blocks of one IR size, at as many PCs.
uint32_t LruPc(uint32_t i) {
    return kCode + 0x400 + 0x20 * i;
}
std::vector<uint16_t> LruProgram(uint32_t imm) {
    return {static_cast<uint16_t>(0x7300 | (imm & 0x7F)), kAdd1_R3, kSleep};
}

// The IR size of LruProgram (the same for every imm and PC, which it REQUIREs for imm 1-40).
size_t LruInsts(brimir::jit::BackendKind kind, Rig &rig) {
    auto &ctx = rig.sh2->GetJitContext();
    IrCache probe{kind, brimir::jit::kMaxCachedInsts};
    size_t insts = 0;
    for (uint32_t imm = 1; imm <= 40; ++imm) {
        rig.WriteCode(LruPc(imm), LruProgram(imm));
        const size_t n = probe.cache.Get(ctx, LruPc(imm)).block.code.size();
        REQUIRE((insts == 0 || n == insts));
        insts = n;
    }
    REQUIRE(insts > 0u);
    return insts;
}

} // namespace

// Ruling R8: at the IR cap, IR blocks are freed by last use, live and retired together, until the
// IR is below 7/8 of the cap.
TEST_CASE("Block cache: the IR cap frees the least recently used IR blocks first", "[jit][executor][tier]") {
    const auto kind = GENERATE(from_range(sh2test::AvailableBackends()));
    INFO("backend " << brimir::jit::BackendName(kind));
    auto rig = std::make_unique<Rig>();
    auto &ctx = rig->sh2->GetJitContext();
    const size_t s = LruInsts(kind, *rig);
    // Eight blocks of s instructions fill the cap; freeing two gets below 7/8 of it, one does not.
    const size_t cap = 8 * s;
    const size_t below = cap - cap / 8;
    REQUIRE(7 * s >= below);
    REQUIRE(6 * s < below);
    IrCache c{kind, cap};
    auto &cache = c.cache;
    const auto get = [&](uint32_t i, uint32_t imm) {
        rig->WriteCode(LruPc(i), LruProgram(imm));
        return &cache.Get(ctx, LruPc(i));
    };

    // Use order (oldest first): A, B, D, E (retired), E2, F (retired), F2, C (used again last).
    get(0, 1);  // A
    get(1, 2);  // B
    get(2, 3);  // C
    get(3, 4);  // D
    get(4, 5);  // E
    get(4, 6);  // E retired, E2
    get(5, 7);  // F
    get(5, 8);  // F retired (the most recently retired), F2
    const brimir::jit::CachedBlock *blockC = get(2, 3); // a hit: C is now the most recently used
    REQUIRE(cache.Compiles() == 8u);
    REQUIRE(cache.CachedInsts() == cap);
    REQUIRE(cache.RetiredIrInsts() == 2 * s);

    get(6, 9); // at the cap: frees A and B, the two least recently used
    CHECK(cache.IrEvictions() == 1u);
    CHECK(cache.IrEvictedBlocks() == 2u);
    CHECK(cache.RetiredIrReleases() == 0u);
    CHECK(cache.CachedInsts() - s < below);
    CHECK(cache.CachedInsts() == 7 * s);
    CHECK(cache.Find(LruPc(0)) == nullptr);
    CHECK(cache.Find(LruPc(1)) == nullptr);
    CHECK(cache.Find(LruPc(3)) != nullptr);
    CHECK(cache.RetiredIrInsts() == 2 * s);

    // The most recently used live block is a hit, without a rebuild.
    CHECK(get(2, 3) == blockC);
    CHECK(cache.Compiles() == 9u);
    // The most recently retired block comes back from the pool.
    get(5, 7);
    CHECK(cache.RetiredIrReuses() == 1u);
    CHECK(cache.Compiles() == 9u);
    // The oldest are gone: rebuilt.
    get(0, 1);
    CHECK(cache.Compiles() == 10u);
}

TEST_CASE("Block cache: the IR cap frees a never-used pool before any live block", "[jit][executor][tier]") {
    const auto kind = GENERATE(from_range(sh2test::AvailableBackends()));
    INFO("backend " << brimir::jit::BackendName(kind));
    auto rig = std::make_unique<Rig>();
    auto &ctx = rig->sh2->GetJitContext();
    const size_t s = LruInsts(kind, *rig);
    const size_t cap = 8 * s;
    const size_t below = cap - cap / 8;
    IrCache c{kind, cap};
    auto &cache = c.cache;
    const auto get = [&](uint32_t i, uint32_t imm) {
        rig->WriteCode(LruPc(i), LruProgram(imm));
        cache.Get(ctx, LruPc(i));
    };

    // X and Y are retired before any live block is used; they never come back.
    get(0, 1);  // X
    get(1, 2);  // Y
    get(0, 11); // X retired, X2
    get(1, 12); // Y retired, Y2
    for (uint32_t i = 2; i < 6; ++i) {
        get(i, i + 1); // four more live blocks
    }
    REQUIRE(cache.CachedInsts() == cap);
    REQUIRE(cache.RetiredIrInsts() == 2 * s);

    get(6, 9); // at the cap: the pool (X, Y) goes, every live block stays
    CHECK(cache.IrEvictions() == 1u);
    CHECK(cache.RetiredIrReleases() == 2u);
    CHECK(cache.IrEvictedBlocks() == 0u);
    CHECK(cache.RetiredIrInsts() == 0u);
    CHECK(cache.CachedInsts() - s < below);
    for (uint32_t i = 0; i < 7; ++i) {
        CHECK(cache.Find(LruPc(i)) != nullptr);
    }
    get(0, 1); // X was freed: rebuilt
    CHECK(cache.RetiredIrReuses() == 0u);
    CHECK(cache.Compiles() == 10u);
}

TEST_CASE("Block cache: the IR cap frees an old live block before a recently retired one", "[jit][executor][tier]") {
    const auto kind = GENERATE(from_range(sh2test::AvailableBackends()));
    INFO("backend " << brimir::jit::BackendName(kind));
    auto rig = std::make_unique<Rig>();
    auto &ctx = rig->sh2->GetJitContext();
    const size_t s = LruInsts(kind, *rig);
    const size_t cap = 8 * s;
    IrCache c{kind, cap};
    auto &cache = c.cache;
    const auto get = [&](uint32_t i, uint32_t imm) {
        rig->WriteCode(LruPc(i), LruProgram(imm));
        cache.Get(ctx, LruPc(i));
    };

    // L and M are live but unused since; R is used last and then retired.
    get(0, 1); // L
    get(1, 2); // M
    for (uint32_t i = 2; i < 6; ++i) {
        get(i, i + 1);
    }
    get(6, 7);  // R
    get(6, 17); // R retired, R2
    REQUIRE(cache.CachedInsts() == cap);
    REQUIRE(cache.RetiredIrInsts() == s);

    get(7, 9); // at the cap: L and M go, the retired R stays
    CHECK(cache.IrEvictions() == 1u);
    CHECK(cache.IrEvictedBlocks() == 2u);
    CHECK(cache.RetiredIrReleases() == 0u);
    CHECK(cache.RetiredIrInsts() == s);
    CHECK(cache.Find(LruPc(0)) == nullptr);
    CHECK(cache.Find(LruPc(1)) == nullptr);
    get(6, 7); // R comes back from the pool
    CHECK(cache.RetiredIrReuses() == 1u);
    CHECK(cache.Compiles() == 9u);
}

TEST_CASE("Block cache: the IR pool is bounded per PC", "[jit][executor][tier]") {
    const auto kind = GENERATE(from_range(sh2test::AvailableBackends()));
    INFO("backend " << brimir::jit::BackendName(kind));
    auto rig = std::make_unique<Rig>();
    auto &ctx = rig->sh2->GetJitContext();
    IrCache c{kind, brimir::jit::kMaxCachedInsts};
    auto &cache = c.cache;
    std::vector<size_t> insts;
    for (uint32_t k = 1; k <= 6; ++k) {
        WriteProgram(*rig, k);
        insts.push_back(cache.Get(ctx, kCode).block.code.size());
    }
    CHECK(cache.Compiles() == 6u);
    CHECK(cache.Invalidations() == 5u);
    // Programs 2-5 are retired (1, the oldest, was released), 6 is live.
    size_t retired = 0;
    for (size_t i = 5 - brimir::jit::kMaxRetiredPerPc; i < 5; ++i) {
        retired += insts[i];
    }
    CHECK(cache.RetiredIrInsts() == retired);
    CHECK(cache.CachedInsts() == retired + insts[5]);

    // Retiring 6 releases 2: the pool holds 3-6, oldest first.
    WriteProgram(*rig, 3);
    cache.Get(ctx, kCode);
    CHECK(cache.RetiredIrReuses() == 1u);
    CHECK(cache.Compiles() == 6u);
    WriteProgram(*rig, 2);
    cache.Get(ctx, kCode);
    CHECK(cache.RetiredIrReuses() == 1u);
    CHECK(cache.Compiles() == 7u);
}

// --- Resuming a slice without a cache lookup (2E track F, Task 1) ------------------------------

namespace {

constexpr uint16_t kAdd1_R4 = 0x7401; // add #1,R4
constexpr uint16_t kAdd1_R5 = 0x7501; // add #1,R5
constexpr uint16_t kAdd2_R3 = 0x7302; // add #2,R3

// bf `to`, placed at `from` (taken while T = 0).
uint16_t Bf(uint32_t from, uint32_t to) {
    const int32_t disp = (static_cast<int32_t>(to) - static_cast<int32_t>(from) - 4) / 2;
    return static_cast<uint16_t>(0x8B00 | (static_cast<uint32_t>(disp) & 0xFFu));
}

// Three chained blocks on odd halves (PC & 2), each "add #1,Rn ; bf next" with T = 0 throughout:
// A -> B -> C -> A. Every block takes the same cycles, so the 40-cycle slices end at block exits.
constexpr uint32_t kResumeA = kCode + 0x02;
constexpr uint32_t kResumeB = kCode + 0x22;
constexpr uint32_t kResumeC = kCode + 0x42;
constexpr uint64_t kSlice = 40;

// An interpreter rig and an x64 rig driven through SH2::Advance (so each Advance is one
// Executor::Run slice); after every Advance the two must match.
struct ResumePair {
    std::unique_ptr<Rig> ref = std::make_unique<Rig>();
    std::unique_ptr<Rig> jit = std::make_unique<Rig>();
    brimir::jit::Executor exec;

    explicit ResumePair(uint32_t threshold = sh2test::kNativeOnFirstRun, brimir::jit::CompileWorker *worker = nullptr)
        : exec{brimir::jit::BackendKind::X64, threshold, worker} {
        WriteCode(kResumeA, {kAdd1_R3, Bf(kResumeA + 2, kResumeB)});
        WriteCode(kResumeB, {kAdd1_R4, Bf(kResumeB + 2, kResumeC)});
        WriteCode(kResumeC, {kAdd1_R5, Bf(kResumeC + 2, kResumeA)});
        auto state = ref->BaseState(kResumeA);
        state.R[3] = 0;
        state.R[4] = 0;
        state.R[5] = 0;
        Load(state);
        jit->sh2->SetJitExecutor(&exec);
    }
    void WriteCode(uint32_t address, const std::vector<uint16_t> &words) {
        ref->WriteCode(address, words);
        jit->WriteCode(address, words);
    }
    void Load(const ymir::savestate::SH2SaveState &state) {
        ref->Load(state);
        jit->Load(state);
    }
    void Advance(uint64_t cycles = kSlice) {
        const uint64 refCycles = ref->sh2->Advance<false, false>(cycles);
        const uint64 jitCycles = jit->sh2->Advance<false, false>(cycles);
        REQUIRE(jitCycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*ref, *jit, true);
        INFO(diff);
        REQUIRE(diff.empty());
    }
    uint64_t Resumes() const {
        return exec.GetStats().resumes;
    }
    uint64_t Lookups() const {
        return exec.Cache().Lookups();
    }
};

} // namespace

TEST_CASE("Resume: consecutive slices skip Get", "[jit][executor][resume]") {
    if (!brimir::jit::IsBackendAvailable(brimir::jit::BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    ResumePair p;
    for (int i = 0; i < 4; ++i) {
        p.Advance(); // builds and links the three blocks
    }
    const uint64_t resumes = p.Resumes();
    const uint64_t lookups = p.Lookups();
    for (int i = 0; i < 200; ++i) {
        INFO("slice " << i);
        p.Advance();
    }
    WARN("resumes " << p.Resumes() - resumes << " lookups " << p.Lookups() - lookups << " of 200 slices");
    CHECK(p.Resumes() - resumes > 150u);
    CHECK(p.Lookups() - lookups < 50u);
    CHECK(p.exec.GetStats().staleEntries == 0u);
    CHECK(p.exec.GetStats().chainedBlocks > 0u);
}

TEST_CASE("Resume: dropped after a cache change", "[jit][executor][resume]") {
    if (!brimir::jit::IsBackendAvailable(brimir::jit::BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    enum class Change { None, Invalidate, Flush, Rewrite, Remap };
    const Change change = GENERATE(Change::None, Change::Invalidate, Change::Flush, Change::Rewrite, Change::Remap);
    INFO("change " << static_cast<int>(change));
    ResumePair p;
    using Page = std::array<uint8_t, 0x10000>;
    auto pagesStorage = std::make_unique<std::array<std::array<Page, 2>, 2>>(); // [rig][copy]
    auto &pages = *pagesStorage;
    for (int round = 0; round < 3; ++round) {
        INFO("round " << round);
        for (int i = 0; i < 8; ++i) {
            p.Advance();
        }
        // The last slice resumed and ended at a block exit: the next one would resume.
        const uint64_t resumes = p.Resumes();
        const uint64_t lookups = p.Lookups();
        const uint64_t stale = p.exec.GetStats().staleEntries;
        p.Advance();
        REQUIRE(p.Resumes() == resumes + 1);
        REQUIRE(p.Lookups() == lookups);

        const uint32_t pc = p.jit->State().PC;
        REQUIRE((pc == kResumeA || pc == kResumeB || pc == kResumeC));
        switch (change) {
        case Change::None: break;
        case Change::Invalidate: p.exec.MutableCache().Invalidate(pc); break;
        case Change::Flush: p.exec.Flush(); break;
        case Change::Rewrite:
            // The recorded block's first instruction, on both rigs (another CPU writing the code).
            p.WriteCode(pc, {p.ref->Read16(pc) == kAdd2_R3 ? kAdd1_R3 : kAdd2_R3});
            break;
        case Change::Remap: {
            // Same bytes on another array: each rig's code page moves to a fresh copy of itself.
            const uint32_t base = pc & 0x7FF0000u;
            for (size_t r = 0; r < 2; ++r) {
                Rig &rig = r == 0 ? *p.ref : *p.jit;
                Page &next = pages[r][round % 2];
                if (round == 0) {
                    std::copy_n(rig.ram->begin() + (base & 0xF0000u), next.size(), next.begin());
                } else {
                    next = pages[r][(round + 1) % 2];
                }
                rig.bus.MapArray(base, base + 0xFFFF, next, true);
            }
            break;
        }
        }
        const uint64_t resumesBefore = p.Resumes();
        const uint64_t lookupsBefore = p.Lookups();
        const uint64_t staleBefore = p.exec.GetStats().staleEntries;
        p.Advance(); // matches the interpreter in every case
        const bool resumed = p.Resumes() > resumesBefore;
        switch (change) {
        case Change::None:
            CHECK(resumed);
            CHECK(p.Lookups() == lookupsBefore);
            CHECK(p.exec.GetStats().staleEntries == stale);
            break;
        case Change::Invalidate:
        case Change::Flush:
            CHECK_FALSE(resumed);
            CHECK(p.Lookups() >= lookupsBefore + 1);
            break;
        case Change::Rewrite:
        case Change::Remap:
            // Either not resumed, or the resumed block's prologue reported stale.
            CHECK((!resumed || p.exec.GetStats().staleEntries >= staleBefore + 1));
            break;
        }
    }
}

TEST_CASE("Resume: Step gates still apply", "[jit][executor][resume]") {
    if (!brimir::jit::IsBackendAvailable(brimir::jit::BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    enum class Gate { Interrupt, DelaySlot, FetchBuffer };
    const Gate gate = GENERATE(Gate::Interrupt, Gate::DelaySlot, Gate::FetchBuffer);
    INFO("gate " << static_cast<int>(gate));
    constexpr uint32_t kVbr = 0x06008000;
    constexpr uint32_t kVector = 0x50;
    constexpr uint32_t kHandler = 0x06009000;
    constexpr uint32_t kStack = 0x0600F000;
    constexpr uint32_t kSlotTarget = 0x06002000;
    ResumePair p;
    p.WriteCode(kHandler, {kSleep});
    p.WriteCode(kSlotTarget, {kAdd1_R5, kSleep});
    for (Rig *rig : {p.ref.get(), p.jit.get()}) {
        rig->Write32(kVbr + kVector * 4, kHandler);
    }
    auto state = p.ref->State();
    state.SR = 0xE0; // T = 0; mask 14 lets the level-15 DIVU interrupt through
    state.VBR = kVbr;
    state.R[15] = kStack;
    p.Load(state);
    for (int i = 0; i < 8; ++i) {
        p.Advance();
    }
    const uint64_t resumes = p.Resumes();
    p.Advance();
    REQUIRE(p.Resumes() == resumes + 1); // the next slice would resume

    const uint64_t interpreted = p.exec.GetStats().interpreted;
    switch (gate) {
    case Gate::Interrupt:
        // DIVU overflow (DVSR = 0), level 15: pending and allowed before the next slice.
        for (Rig *rig : {p.ref.get(), p.jit.get()}) {
            auto &ctx = rig->sh2->GetJitContext();
            ctx.write(ctx.sh2, 0xFFFFFF08, 4, 0x2);     // DVCR.OVFIE = 1
            ctx.write(ctx.sh2, 0xFFFFFF0C, 4, kVector); // VCRDIV
            ctx.write(ctx.sh2, 0xFFFFFEE2, 1, 0xF0);    // IPRA: DIVU level 15
            ctx.write(ctx.sh2, 0xFFFFFF00, 4, 0);       // DVSR = 0
            ctx.write(ctx.sh2, 0xFFFFFF04, 4, 1234);    // DVDNT: divides by 0
        }
        REQUIRE(*p.jit->sh2->GetJitContext().intrPending);
        break;
    case Gate::DelaySlot: {
        // The instruction at PC runs as a delay slot of a branch to kSlotTarget.
        auto s = p.ref->State();
        s.delaySlot = true;
        s.delaySlotTarget = kSlotTarget;
        p.Load(s);
        break;
    }
    case Gate::FetchBuffer: {
        // The fetch buffer's low half (the opcode run at PC & 2) differs from memory.
        auto s = p.ref->State();
        REQUIRE((s.PC & 2u) != 0);
        s.fetchedOpcodes = (s.fetchedOpcodes & 0xFFFF0000u) | kAdd2_R3;
        p.Load(s);
        break;
    }
    }
    const uint64_t resumesBefore = p.Resumes();
    p.Advance();
    CHECK(p.Resumes() == resumesBefore);
    CHECK(p.exec.GetStats().interpreted > interpreted);
    for (int i = 0; i < 4; ++i) {
        p.Advance();
    }
}

TEST_CASE("Resume: waiting worker results force a Get", "[jit][executor][resume][worker]") {
    if (!brimir::jit::IsBackendAvailable(brimir::jit::BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint32_t kOther = 0x06003000;
    brimir::jit::CompileWorker worker;
    ResumePair p{2, &worker};
    p.WriteCode(kOther, {kAdd1_R5, kAdd1_R5, kSleep});
    const void *owner = &p.exec.Cache();
    const auto native = [&](uint32_t pc) {
        const auto *entry = p.exec.Cache().Find(pc);
        return entry != nullptr && entry->code.entry != nullptr;
    };
    // Compile the loop's three blocks in the background.
    for (int i = 0; i < 40 && !(native(kResumeA) && native(kResumeB) && native(kResumeC)); ++i) {
        p.Advance();
        REQUIRE(sh2test::WaitUntil([&] { return worker.Pending(owner) == 0; }));
    }
    REQUIRE((native(kResumeA) && native(kResumeB) && native(kResumeC)));
    const auto loop = p.ref->State();

    // Park a result for another block: two runs of it submit its compile, queued behind the blocker.
    sh2test::WorkerBlocker blocker{worker};
    REQUIRE(blocker.Entered());
    for (int i = 0; i < 2; ++i) {
        auto s = p.ref->BaseState(kOther);
        s.SR = loop.SR;
        p.Load(s);
        p.Advance();
    }
    REQUIRE(p.exec.Cache().PendingCompiles() == 1);
    p.Load(loop);
    for (int i = 0; i < 4; ++i) {
        p.Advance();
    }
    const uint64_t resumes = p.Resumes();
    p.Advance();
    REQUIRE(p.Resumes() == resumes + 1); // the next slice would resume

    blocker.Open();
    REQUIRE(sh2test::WaitForResults(worker, owner));
    const uint64_t async = p.exec.Cache().AsyncCompiles();
    const uint64_t lookups = p.Lookups();
    const uint64_t resumesBefore = p.Resumes();
    p.Advance();
    CHECK(p.Resumes() == resumesBefore);
    CHECK(p.Lookups() >= lookups + 1);
    CHECK(p.exec.Cache().AsyncCompiles() == async + 1);
    CHECK(native(kOther));
    p.Advance();
    CHECK(p.Resumes() == resumesBefore + 1);
}
