#pragma once

// Per-CPU cache of compiled blocks keyed by the full guest PC (see design/sh2-jit.md section 6).

#include <brimir/jit/backend.hpp>
#include <brimir/jit/ir.hpp>

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace brimir::jit {

// IR instructions of IR-only blocks kept (~20 MB of IR). Natively compiled blocks drop their IR and
// do not count. Reaching it evicts every IR-only block; native blocks stay.
constexpr size_t kMaxCachedInsts = size_t{1} << 20;

// Native code held before the cache is flushed (checked before each native compile). Sized so that
// Street Fighter Zero 3's master working set fits: compiling every block natively without a flush,
// its master made 31,476 native compiles (about 30,600 PCs plus recompiles of stale blocks, whose old
// code stays allocated until a flush) at about 2,750 bytes each, 86.6 MB held
// (design/sh2-x64-performance.md, 2C progress, Task 5).
constexpr size_t kMaxNativeCodeBytes = size_t{128} << 20;

// A block is built as IR and run with RunBlock first; its Nth run (counting the one right after it
// was built) compiles it natively and runs the native code. 1: compile on the first run. Chosen
// from 1, 2, 4 and 8 on Street Fighter Zero 3 and Panzer Dragoon II Zwei (best max frame within 2%
// of the best average SH-2 time; design/sh2-x64-performance.md, 2C progress, Task 5).
constexpr uint32_t kNativeCompileThreshold = 8;

// Entries in the recent-lookup table, indexed by (pc >> 1) & (kRecentSlots - 1).
constexpr size_t kRecentSlots = 4096;

// A cached block. IR-only (code.entry == nullptr): block holds its IR and runs with RunBlock. Once
// natively compiled, block keeps only startPC, guestOpcodes, guestInstrCount (and the flags);
// block.code is freed and never needed again: a block whose native code is lost (Flush, Reset) is
// dropped with it and rebuilt from scratch.
struct CachedBlock {
    Block block;
    NativeCode code;
    // Runs left until the native compile; 0: none pending (native, compile failed, no backend).
    uint32_t runsUntilNative = 0;
};

class BlockCache {
public:
    // native: the backend that compiles hot blocks, or nullptr to run every block with RunBlock.
    // Not owned; it must outlive the cache. maxNativeCodeBytes: flush limit for the backend's code
    // (tests pass a small value). nativeCompileThreshold: see kNativeCompileThreshold (tests pass
    // 1 to get native code on the first run; 0 is treated as 1). maxCachedInsts: the IR cap (tests
    // pass a small value).
    explicit BlockCache(INativeBackend *native = nullptr, size_t maxNativeCodeBytes = kMaxNativeCodeBytes,
                        uint32_t nativeCompileThreshold = kNativeCompileThreshold,
                        size_t maxCachedInsts = kMaxCachedInsts)
        : m_native(native)
        , m_maxNativeCodeBytes(maxNativeCodeBytes)
        , m_maxCachedInsts(maxCachedInsts)
        , m_nativeCompileThreshold(nativeCompileThreshold == 0 ? 1 : nativeCompileThreshold) {}

    // Returns the block for pc, building it on a miss or when its guest code changed, and compiling
    // it natively on its kNativeCompileThreshold-th run. A native block with code.selfValidating is
    // returned without checking its guest code (its prologue does, see ExitInfo::stale) and is
    // published to the backend's link table; IR-only blocks are never published. Every call is
    // counted as a run of the returned block.
    const CachedBlock &Get(ymir::sh2::SH2JitContext &ctx, uint32_t pc);

    // Drops every block and, with a native backend, all of its generated code.
    void Flush();

    // Drops the block for pc, if cached (a stale native block), and unpublishes it. Never called
    // while generated code runs; its native code stays allocated until the next Flush.
    void Invalidate(uint32_t pc);

    // The cached block for pc, or nullptr; no validation, no compile (tests and diagnostics).
    const CachedBlock *Find(uint32_t pc) const {
        const auto it = m_blocks.find(pc);
        return it != m_blocks.end() ? it->second.get() : nullptr;
    }

    size_t Size() const {
        return m_blocks.size();
    }
    // IR instructions counted against kMaxCachedInsts (IR-only blocks; tests and diagnostics).
    size_t CachedInsts() const {
        return m_totalInsts;
    }
    uint32_t NativeCompileThreshold() const {
        return m_nativeCompileThreshold;
    }
    uint64_t Compiles() const {
        return m_compiles;
    }
    uint64_t Invalidations() const {
        return m_invalidations;
    }
    // Blocks the native backend could not compile (they run with RunBlock).
    uint64_t CompileFallbacks() const {
        return m_compileFallbacks;
    }
    // Successful native compiles, and the generated code they produced (bytes, since construction).
    uint64_t NativeCompiles() const {
        return m_nativeCompiles;
    }
    uint64_t NativeBytesCompiled() const {
        return m_nativeBytes;
    }
    // Host time spent building blocks (front end, OptimizeBlock, VerifyBlock) and compiling them
    // natively (INativeBackend::Compile, failed attempts included), in nanoseconds.
    uint64_t BuildNs() const {
        return m_buildNs;
    }
    uint64_t NativeCompileNs() const {
        return m_nativeNs;
    }
    // Times the IR cap was reached, and the IR-only blocks evicted then (native blocks are kept).
    uint64_t IrEvictions() const {
        return m_irEvictions;
    }
    uint64_t IrEvictedBlocks() const {
        return m_irEvictedBlocks;
    }
    // Flushes by trigger: the native code cap, and Flush() calls (Executor::Flush: CPU reset, state
    // load, ...).
    uint64_t FlushesCodeCap() const {
        return m_flushesCodeCap;
    }
    uint64_t FlushesRequested() const {
        return m_flushesRequested;
    }

private:
    using Clock = std::chrono::steady_clock;
    using BlockMap = std::unordered_map<uint32_t, std::unique_ptr<CachedBlock>>;

    // A recently used block. entry == nullptr: empty. Never outlives its map entry: Invalidate and
    // Flush clear it.
    struct RecentSlot {
        uint32_t pc = 0;
        CachedBlock *entry = nullptr;
    };

    static bool IsCurrent(const Block &block, ymir::sh2::SH2JitContext &ctx);

    // IsCurrent, except for self-validating native blocks; publishes those.
    bool Validate(const CachedBlock &entry, uint32_t pc, ymir::sh2::SH2JitContext &ctx);

    RecentSlot &SlotFor(uint32_t pc) {
        return m_recent[(pc >> 1) & (kRecentSlots - 1)];
    }

    // Drops a block whose guest code changed (and its recent slot).
    void Invalidate(BlockMap::iterator it);

    // Drops every block (Flush without counting it as requested).
    void FlushAll();

    // Drops every IR-only block (and its recent slot) at the IR cap; native blocks, their code and
    // link slots stay. Only called from Get, never while a block runs.
    void EvictIrOnly();

    // A validated hit: counts the run and compiles the block natively on its threshold run.
    const CachedBlock &Hit(CachedBlock &entry, ymir::sh2::SH2JitContext &ctx, uint32_t pc) {
        if (entry.runsUntilNative != 0 && --entry.runsUntilNative == 0) {
            return CompileNative(entry, ctx, pc);
        }
        return entry;
    }

    // Builds the IR block for pc and caches it (flushing first at the IR cap); compiles it natively
    // now if compileNow or the threshold is 1.
    const CachedBlock &Build(ymir::sh2::SH2JitContext &ctx, uint32_t pc, bool compileNow);

    // Compiles entry natively and frees its IR. At the native code cap it flushes the cache
    // (entry included) and rebuilds and compiles pc from scratch. On failure entry stays IR-only.
    const CachedBlock &CompileNative(CachedBlock &entry, ymir::sh2::SH2JitContext &ctx, uint32_t pc);

    INativeBackend *m_native;
    size_t m_maxNativeCodeBytes;
    size_t m_maxCachedInsts;
    uint32_t m_nativeCompileThreshold;
    BlockMap m_blocks;
    std::array<RecentSlot, kRecentSlots> m_recent{};
    size_t m_totalInsts = 0;
    uint64_t m_compiles = 0;
    uint64_t m_invalidations = 0;
    uint64_t m_compileFallbacks = 0;
    uint64_t m_nativeCompiles = 0;
    uint64_t m_nativeBytes = 0;
    uint64_t m_buildNs = 0;
    uint64_t m_nativeNs = 0;
    uint64_t m_irEvictions = 0;
    uint64_t m_irEvictedBlocks = 0;
    uint64_t m_flushesCodeCap = 0;
    uint64_t m_flushesRequested = 0;
};

} // namespace brimir::jit
