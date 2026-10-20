// Brimir - SH-2 JIT executor tests (cache flush semantics)
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "jit_test_backend.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/jit/backend.hpp>
#include <brimir/jit/block_cache.hpp>
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

    IrCache c{kind, pInsts + qInsts};
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

    // At the cap: the next build first releases the pool, which is enough here, so the live IR-only
    // blocks stay.
    const size_t otherInsts = cache.Get(ctx, kOther).block.code.size();
    REQUIRE(otherInsts < pInsts);
    CHECK(cache.RetiredIrReleases() == 1u);
    CHECK(cache.IrEvictions() == 0u);
    CHECK(cache.RetiredIrInsts() == 0u);
    CHECK(cache.CachedInsts() == qInsts + otherInsts);
    REQUIRE(cache.Find(kCode) != nullptr);
    CHECK(cache.Find(kCode)->block.guestOpcodes.front() == kAdd16_R3);
    CHECK(cache.Find(kOther) != nullptr);

    rig->WriteCode(kCode, kIrP);
    cache.Get(ctx, kCode); // P is gone from the pool: rebuilt
    CHECK(cache.RetiredIrReuses() == 0u);
    CHECK(cache.Compiles() == 4u);
    CHECK(cache.IrEvictions() == 0u);

    // A pool holding more than an eighth of the cap: only the oldest retired blocks (across PCs) are
    // released, until the IR is below 7/8 of the cap; the newer ones stay reusable.
    constexpr uint32_t kOther2 = kCode + 0x200;
    const auto program = [](uint32_t imm) {
        return std::vector<uint16_t>{static_cast<uint16_t>(0x7300 | imm), kAdd1_R3, kSleep};
    };
    size_t xInsts = 0, yInsts = 0, aInsts = 0, bInsts = 0;
    {
        IrCache probe{kind, brimir::jit::kMaxCachedInsts};
        rig->WriteCode(kOther2, program(10));
        xInsts = probe.cache.Get(ctx, kOther2).block.code.size();
        rig->WriteCode(kOther2, program(11));
        yInsts = probe.cache.Get(ctx, kOther2).block.code.size();
        rig->WriteCode(kCode, program(12));
        aInsts = probe.cache.Get(ctx, kCode).block.code.size();
        rig->WriteCode(kCode, program(13));
        bInsts = probe.cache.Get(ctx, kCode).block.code.size();
    }
    const size_t oldCap = xInsts + yInsts + aInsts + bInsts;
    const size_t below = oldCap - oldCap / 8;
    // Releasing X alone must be enough, and A must be needed to stay out of the cap check below.
    REQUIRE(yInsts + aInsts + bInsts < below);
    REQUIRE(otherInsts < xInsts);
    IrCache older{kind, oldCap};
    rig->WriteCode(kOther2, program(10));
    older.cache.Get(ctx, kOther2); // X
    rig->WriteCode(kOther2, program(11));
    older.cache.Get(ctx, kOther2); // X retired (the oldest), Y live
    rig->WriteCode(kCode, program(12));
    older.cache.Get(ctx, kCode); // A
    rig->WriteCode(kCode, program(13));
    older.cache.Get(ctx, kCode); // A retired, B live
    REQUIRE(older.cache.CachedInsts() == oldCap);
    REQUIRE(older.cache.RetiredIrInsts() == xInsts + aInsts);
    REQUIRE(older.cache.RetiredIrInsts() > oldCap / 8);
    older.cache.Get(ctx, kOther); // at the cap: releases X only
    CHECK(older.cache.RetiredIrReleases() == 1u);
    CHECK(older.cache.IrEvictions() == 0u);
    CHECK(older.cache.RetiredIrInsts() == aInsts);
    CHECK(older.cache.CachedInsts() - otherInsts < below);
    CHECK(older.cache.CachedInsts() == yInsts + aInsts + bInsts + otherInsts);
    REQUIRE(older.cache.Find(kOther2) != nullptr); // Y
    REQUIRE(older.cache.Find(kCode) != nullptr);   // B
    CHECK(older.cache.Compiles() == 5u);
    rig->WriteCode(kCode, program(12));
    older.cache.Get(ctx, kCode); // A comes back from the pool
    CHECK(older.cache.RetiredIrReuses() == 1u);
    CHECK(older.cache.Compiles() == 5u);
    rig->WriteCode(kOther2, program(10));
    older.cache.Get(ctx, kOther2); // X was released: rebuilt
    CHECK(older.cache.RetiredIrReuses() == 1u);
    CHECK(older.cache.Compiles() == 6u);
    CHECK(older.cache.RetiredIrReleases() == 1u); // below the cap again, no second release
    CHECK(older.cache.IrEvictions() == 0u);

    // Live IR alone at the cap (the pool is empty): the live IR-only blocks are evicted as before.
    IrCache live{kind, pInsts + qInsts};
    rig->WriteCode(kCode, kIrP);
    rig->WriteCode(kOther2, kIrQ);
    live.cache.Get(ctx, kCode);
    live.cache.Get(ctx, kOther2);
    REQUIRE(live.cache.CachedInsts() >= pInsts + qInsts);
    REQUIRE(live.cache.RetiredIrInsts() == 0u);
    CHECK(live.cache.Get(ctx, kOther).block.code.size() == otherInsts);
    CHECK(live.cache.IrEvictions() == 1u);
    CHECK(live.cache.RetiredIrReleases() == 0u);
    CHECK(live.cache.CachedInsts() == otherInsts);
    CHECK(live.cache.Find(kCode) == nullptr);
    CHECK(live.cache.Find(kOther2) == nullptr);
}

// When the whole pool cannot bring the IR below 7/8 of the cap (the live IR alone is above it), the
// live IR-only blocks are evicted too, once, so the next builds do not reach the cap at once.
TEST_CASE("Block cache: live IR above 7/8 of the IR cap is evicted after the pool", "[jit][executor][tier]") {
    const auto kind = GENERATE(from_range(sh2test::AvailableBackends()));
    INFO("backend " << brimir::jit::BackendName(kind));
    constexpr uint32_t kSmall = kCode + 0x200; // X (small, retired), then Y (live)
    constexpr uint32_t kTrigger = kCode + 0x100;
    const auto livePc = [](uint32_t i) { return kCode + 0x300 + 0x10 * i; };
    constexpr uint32_t kLive = 6;
    const std::vector<uint16_t> x = {kAdd1_R3, kSleep};
    const std::vector<uint16_t> y = {kAdd16_R3, kAdd1_R3, kSleep};
    const std::vector<uint16_t> trigger = {kAdd16_R3, kSleep};
    auto rig = std::make_unique<Rig>();
    auto &ctx = rig->sh2->GetJitContext();
    for (uint32_t i = 0; i < kLive; ++i) {
        rig->WriteCode(livePc(i), {static_cast<uint16_t>(0x7300 | (i + 1)), kAdd1_R3, kSleep});
    }
    rig->WriteCode(kTrigger, trigger);

    // The IR of the setup, from a cache without a small cap.
    size_t total = 0;
    size_t xInsts = 0;
    {
        IrCache probe{kind, brimir::jit::kMaxCachedInsts};
        rig->WriteCode(kSmall, x);
        xInsts = probe.cache.Get(ctx, kSmall).block.code.size();
        rig->WriteCode(kSmall, y);
        probe.cache.Get(ctx, kSmall);
        for (uint32_t i = 0; i < kLive; ++i) {
            probe.cache.Get(ctx, livePc(i));
        }
        total = probe.cache.CachedInsts();
    }
    const size_t cap = total;
    const size_t below = cap - cap / 8;
    REQUIRE(total - xInsts >= below); // the pool alone cannot get below 7/8

    IrCache c{kind, cap};
    auto &cache = c.cache;
    rig->WriteCode(kSmall, x);
    cache.Get(ctx, kSmall);
    rig->WriteCode(kSmall, y);
    cache.Get(ctx, kSmall); // X retired
    for (uint32_t i = 0; i < kLive; ++i) {
        cache.Get(ctx, livePc(i));
    }
    REQUIRE(cache.CachedInsts() == cap);
    REQUIRE(cache.RetiredIrInsts() == xInsts);

    const size_t triggerInsts = cache.Get(ctx, kTrigger).block.code.size();
    CHECK(cache.RetiredIrReleases() == 1u);
    CHECK(cache.IrEvictions() == 1u);
    CHECK(cache.RetiredIrInsts() == 0u);
    CHECK(cache.CachedInsts() == triggerInsts);
    CHECK(cache.Find(kSmall) == nullptr);
    CHECK(cache.Find(livePc(0)) == nullptr);
    CHECK(cache.Compiles() == 2u + kLive + 1u);

    // The next builds (the live blocks again, and a new retired block) stay far from the cap.
    for (uint32_t i = 0; i < kLive; ++i) {
        cache.Get(ctx, livePc(i));
    }
    rig->WriteCode(kTrigger, {kAdd1_R3, kSleep});
    cache.Get(ctx, kTrigger);
    CHECK(cache.RetiredIrInsts() == triggerInsts);
    CHECK(cache.RetiredIrReleases() == 1u);
    CHECK(cache.IrEvictions() == 1u);
    CHECK(cache.Compiles() == 2u + kLive + 1u + kLive + 1u);
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
