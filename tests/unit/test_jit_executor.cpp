// Brimir - SH-2 JIT executor tests (cache flush semantics)
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "jit_test_backend.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/jit/executor.hpp>

#include <memory>
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
    CHECK(cache.FlushesInstCap() == 0u);
    CHECK(cache.FlushesCodeCap() == 0u);
}
