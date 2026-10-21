#pragma once

// Per-CPU cache of compiled blocks keyed by the full guest PC (see design/sh2-jit.md section 6).

#include <brimir/jit/backend.hpp>
#include <brimir/jit/compile_worker.hpp>
#include <brimir/jit/ir.hpp>

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

namespace brimir::jit {

// IR instructions of IR-only blocks kept (~20 MB of IR). Natively compiled blocks drop their IR and
// do not count. Reaching it frees IR blocks, live IR-only ones and those of the IR pool (below)
// together, least recently used first, until the IR is below kIrCapTargetEighths / 8 of the cap;
// native blocks stay.
constexpr size_t kMaxCachedInsts = size_t{1} << 20;

// Native code held before the cache is flushed (checked before each native compile). Sized so that
// Street Fighter Zero 3's master working set fits: compiling every block natively without a flush,
// its master made 31,476 native compiles (about 30,600 PCs plus recompiles of stale blocks, whose old
// code stays allocated until a flush) at about 2,750 bytes each, 86.6 MB held
// (design/sh2-x64-performance.md, 2C progress, Task 5).
constexpr size_t kMaxNativeCodeBytes = size_t{128} << 20;

// A block is built as IR and run with RunBlock first; its Nth run (counting the one right after it
// was built) compiles it natively and runs the native code. 1: compile on the first run. Chosen
// from 1, 2, 4 and 8 (2C) and again from 8, 16 and 32 (2D) on Street Fighter Zero 3 and Panzer
// Dragoon II Zwei (best SFZ3 max frame within 2% of the best average SH-2 time;
// design/sh2-x64-performance.md, 2C progress Task 5 and 2D B5).
constexpr uint32_t kNativeCompileThreshold = 16;

// Entries in the recent-lookup table, indexed by (pc >> 1) & (kRecentSlots - 1).
constexpr size_t kRecentSlots = 4096;

// Retired blocks (design/sh2-jit-m2d.md, A1): a self-validating native block dropped because its
// guest code changed keeps its native code, unpublished, in a pool, and is taken back when the same
// opcodes return at its PC on the same pages. Native code held by retired blocks per CPU (it counts
// toward kMaxNativeCodeBytes), and retired blocks kept per PC; past either, the least recently
// retired blocks are released.
constexpr size_t kMaxRetiredCodeBytes = size_t{32} << 20;
constexpr uint32_t kMaxRetiredPerPc = 4;

// The IR pool (design/sh2-jit-m2e.md, track E): an IR-only block dropped because its guest code
// changed is kept, at most kMaxRetiredPerPc per PC (the oldest goes first), and taken back instead
// of running the front end again when BuildBlock would give the same block. Its IR keeps counting
// toward the IR cap, with the last use it had when it was dropped.
//
// After a cap event the IR is below this many eighths of the cap, so the next builds do not reach
// it again at once.
constexpr size_t kIrCapTargetEighths = 7;

// Damping (design/sh2-jit-m2d.md, A3): every time a block at a PC is dropped because its guest code
// changed, the native compile threshold for the next build at that PC doubles, up to
// kNativeCompileThreshold << kMaxStaleDampShift (counts are per PC and cleared by a flush, so the
// wait is bounded and never permanent). 0 turns damping off. Only applies to thresholds above 1:
// threshold 1 (tests and diagnostics) means native on the first run, always. Chosen from 0, 2 and 4
// on Street Fighter Zero 3, Panzer Dragoon II Zwei and Burning Rangers: only 0 (off) stayed within 2%
// of the best SH-2 total on both short titles (design/sh2-x64-performance.md, 2D A3).
constexpr uint32_t kMaxStaleDampShift = 0;

// Background compiles (design/sh2-jit-m2e.md): native compiles submitted to the CompileWorker and
// not yet taken back, per cache. A threshold run at the bound does not submit; it retries a
// threshold's runs later.
constexpr size_t kMaxPendingCompiles = 512;

// A cached block. IR-only (code.entry == nullptr): block holds its IR and runs with RunBlock. Once
// natively compiled, block keeps only startPC, guestOpcodes, guestInstrCount (and the flags);
// block.code is freed and never needed again: a block whose native code is lost (Flush, Reset) is
// dropped with it and rebuilt from scratch.
struct CachedBlock {
    Block block;
    NativeCode code;
    // Runs left until the native compile; 0: none pending (native, compile failed, no backend).
    uint32_t runsUntilNative = 0;
    // Self-validating native blocks: the page-table arrays of the pages holding startPC and the last
    // word of guestOpcodes at compile time (the pages the prologue checks). A retired block is
    // reused only while both are unchanged. With a worker, recorded at Prepare (what the emitter
    // saw), never at the commit.
    std::array<const uint8_t *, 2> codePages{};
    // IR-only entries: the cache's use clock when Get last returned the entry (a retired IR block
    // keeps the value it had when it was dropped); the IR cap frees the smallest first.
    uint64_t lastUse = 0;
    // The ticket of the background compile submitted for this IR-only entry; 0: none pending.
    // Its result is installed only into the entry still holding its ticket, so clearing it is
    // enough to have the result dropped.
    uint64_t pendingTicket = 0;
};

class BlockCache {
public:
    // native: the backend that compiles hot blocks, or nullptr to run every block with RunBlock.
    // Not owned; it must outlive the cache. maxNativeCodeBytes: flush limit for the backend's code
    // (tests pass a small value). nativeCompileThreshold: see kNativeCompileThreshold (tests pass
    // 1 to get native code on the first run; 0 is treated as 1). maxCachedInsts: the IR cap (tests
    // pass a small value). maxRetiredCodeBytes: see kMaxRetiredCodeBytes (tests pass small values;
    // 0 disables the pool, so every dropped block's code is released at once). maxStaleDampShift: see
    // kMaxStaleDampShift (0: no damping; clamped so that threshold << shift fits uint32, i.e. to
    // 32 - bit_width(threshold)). worker: finalizes native compiles in the background, or nullptr
    // to compile synchronously; not owned, it must outlive the cache. With a worker, a threshold run
    // above 1 runs Prepare and submits the compile (the block keeps running on IR), and the result
    // is committed at the start of a later Get; threshold 1 and compiles at once stay synchronous.
    explicit BlockCache(INativeBackend *native = nullptr, size_t maxNativeCodeBytes = kMaxNativeCodeBytes,
                        uint32_t nativeCompileThreshold = kNativeCompileThreshold,
                        size_t maxCachedInsts = kMaxCachedInsts, size_t maxRetiredCodeBytes = kMaxRetiredCodeBytes,
                        uint32_t maxStaleDampShift = kMaxStaleDampShift, CompileWorker *worker = nullptr)
        : m_native(native)
        , m_worker(native != nullptr ? worker : nullptr)
        , m_maxNativeCodeBytes(maxNativeCodeBytes)
        , m_maxCachedInsts(maxCachedInsts)
        , m_maxRetiredCodeBytes(maxRetiredCodeBytes)
        , m_maxStaleDampShift(std::min<uint32_t>(
              maxStaleDampShift, 32u - static_cast<uint32_t>(std::bit_width(nativeCompileThreshold == 0 ? 1u : nativeCompileThreshold))))
        , m_nativeCompileThreshold(nativeCompileThreshold == 0 ? 1 : nativeCompileThreshold) {}

    // Cancels the cache's background compiles (waiting for the one in flight).
    ~BlockCache();

    BlockCache(const BlockCache &) = delete;
    BlockCache &operator=(const BlockCache &) = delete;

    // Returns the block for pc, building it on a miss or when its guest code changed (or taking a
    // retired block back, see kMaxRetiredCodeBytes), and compiling it natively on its
    // kNativeCompileThreshold-th run. A native block with code.selfValidating is returned without
    // checking its guest code (its prologue does, see ExitInfo::stale) and is published to the
    // backend's link table; IR-only blocks are never published. Every call is counted as a run of
    // the returned block. With a worker it first installs the finished background compiles.
    const CachedBlock &Get(ymir::sh2::SH2JitContext &ctx, uint32_t pc);

    // Drops every block (retired ones included) and, with a native backend, all of its generated
    // code; cancels the background compiles.
    void Flush();

    // Drops the block for pc, if cached (a stale native block), and unpublishes it. A self-validating
    // native block is retired; any other native block's code is freed; an IR-only block goes to the
    // IR pool. Never called while generated code runs.
    void Invalidate(uint32_t pc);

    // The cached block for pc, or nullptr; no validation, no compile (tests and diagnostics).
    const CachedBlock *Find(uint32_t pc) const {
        const auto it = m_blocks.find(pc);
        return it != m_blocks.end() ? it->second.get() : nullptr;
    }

    size_t Size() const {
        return m_blocks.size();
    }
    // IR instructions counted against kMaxCachedInsts (IR-only blocks, the IR pool's included; tests
    // and diagnostics).
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
    // The most generated code the backend held after any native compile since construction (flushes
    // do not lower it; 0 without a native backend), and the generated code of native blocks dropped
    // by Invalidate, retired or freed (bytes since construction, counted when dropped).
    size_t PeakNativeCodeBytes() const {
        return m_peakNativeBytes;
    }
    uint64_t StaleNativeBytes() const {
        return m_staleNativeBytes;
    }
    // Generated code freed one block at a time (bytes since construction, counted when freed): by
    // Invalidate, and for retired blocks when the pool evicts them or the code cap empties it. Code
    // freed by a flush is not counted.
    uint64_t NativeBytesReleased() const {
        return m_releasedNativeBytes;
    }
    // Retired blocks taken back instead of rebuilt, retired blocks released by the pool's bounds
    // (kMaxRetiredPerPc, maxRetiredCodeBytes), and the native code the pool holds now.
    uint64_t RetiredReuses() const {
        return m_retiredReuses;
    }
    uint64_t RetiredEvictions() const {
        return m_retiredEvictions;
    }
    size_t RetiredCodeBytes() const {
        return m_retiredBytes;
    }
    // Retired IR blocks taken back instead of rebuilt, and the IR instructions the IR pool holds now
    // (included in CachedInsts).
    uint64_t RetiredIrReuses() const {
        return m_retiredIrReuses;
    }
    size_t RetiredIrInsts() const {
        return m_retiredIrInsts;
    }
    // Retired IR blocks freed by the IR cap (live ones are IrEvictedBlocks), and the host time the cap
    // events took on the calling thread (nanoseconds; not part of BuildNs).
    uint64_t RetiredIrReleases() const {
        return m_retiredIrReleases;
    }
    uint64_t IrCapNs() const {
        return m_irCapNs;
    }
    // Builds that waited for more than the global threshold because their PC had gone stale before.
    uint64_t DampedBuilds() const {
        return m_dampedBuilds;
    }
    // Host time spent building blocks (front end, OptimizeBlock, VerifyBlock) and compiling them
    // natively on the calling (emulation) thread, failed attempts included, in nanoseconds: the
    // synchronous INativeBackend::Compile, or Prepare and Commit of background compiles (the
    // worker's Finalize is CompileWorker::BusyNs).
    uint64_t BuildNs() const {
        return m_buildNs;
    }
    uint64_t NativeCompileNs() const {
        return m_nativeNs;
    }
    // Background compiles committed (also counted in NativeCompiles), and those discarded without a
    // commit: their result arrived for an entry that no longer waits for it (dropped, rebuilt), or
    // a flush cancelled them. Finalize and Commit failures count as CompileFallbacks instead.
    // PendingCompiles: submitted and not yet taken back.
    uint64_t AsyncCompiles() const {
        return m_asyncCompiles;
    }
    uint64_t AsyncDropped() const {
        return m_asyncDropped;
    }
    size_t PendingCompiles() const {
        return m_pendingPcs.size();
    }
    // Times the IR cap was reached, and the live IR-only blocks evicted then (native blocks are kept;
    // retired IR blocks freed are RetiredIrReleases).
    uint64_t IrEvictions() const {
        return m_irEvictions;
    }
    uint64_t IrEvictedBlocks() const {
        return m_irEvictedBlocks;
    }
    // Flushes by trigger: the native code cap, and Flush() calls (Executor::Flush: attaching the
    // executor and CPU reset; a state load does not flush, see SH2::LoadState).
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

    // A retired block: native, self-validating, unpublished, in no recent slot; or, in the IR pool,
    // IR-only, in no recent slot.
    struct RetiredBlock {
        uint32_t pc = 0;
        std::unique_ptr<CachedBlock> entry;
    };
    using RetiredList = std::list<RetiredBlock>;

    // An IR block the IR cap may free: live (live != nullptr, in the map at pc) or in the IR pool.
    struct IrVictim {
        uint64_t lastUse = 0;
        size_t insts = 0;
        uint32_t pc = 0;
        CachedBlock *live = nullptr;
        RetiredList::iterator retired;
    };

    static bool IsCurrent(const Block &block, ymir::sh2::SH2JitContext &ctx);

    // The page arrays a self-validating block's prologue checks (see CachedBlock::codePages).
    static std::array<const uint8_t *, 2> CodePages(const Block &block, const ymir::sh2::SH2JitContext &ctx);

    // IsCurrent, except for self-validating native blocks; publishes those.
    bool Validate(const CachedBlock &entry, uint32_t pc, ymir::sh2::SH2JitContext &ctx);

    RecentSlot &SlotFor(uint32_t pc) {
        return m_recent[(pc >> 1) & (kRecentSlots - 1)];
    }

    // Drops a block whose guest code changed (and its recent slot), retiring it if it is
    // self-validating and the pool is enabled, or to the IR pool if it is IR-only with IR. A pending
    // IR-only block gives up its ticket, so its result is dropped even if the block is taken back.
    void Invalidate(BlockMap::iterator it);

    // Moves a dropped block into the pool, then releases the least recently retired blocks past
    // kMaxRetiredPerPc at pc and past m_maxRetiredCodeBytes in total.
    void Retire(uint32_t pc, std::unique_ptr<CachedBlock> entry);

    // Takes a retired block for pc whose guest code and code pages are current back into the map and
    // the recent slot and publishes it; nullptr if there is none.
    CachedBlock *Reuse(ymir::sh2::SH2JitContext &ctx, uint32_t pc);

    // Whether BuildBlock(ctx, block.startPC) would give this block again: its guest code is current
    // (IsCurrent), fetchFromArrays is what FinishBlock would compute now, and the words that stopped
    // the block still stop it (BlockEndIsCurrent). The front end reads nothing else: the IR, the
    // known refills and the tail word are a function of the PC, those words and fetchFromArrays,
    // and OptimizeBlock only reads the block.
    static bool SameBuild(const Block &block, ymir::sh2::SH2JitContext &ctx);

    // Moves an IR-only block dropped by Invalidate into the IR pool (its IR stays counted), then
    // drops the oldest IR block at pc past kMaxRetiredPerPc.
    void RetireIr(uint32_t pc, std::unique_ptr<CachedBlock> entry);

    // Takes a retired IR block for pc that SameBuild accepts back into the map and the recent slot;
    // nullptr if there is none.
    CachedBlock *ReuseIr(ymir::sh2::SH2JitContext &ctx, uint32_t pc);

    // Removes an IR pool block from the pool (its entry taken or dropped); the caller adjusts
    // m_retiredIrInsts and m_totalInsts.
    void RemoveRetiredIr(RetiredList::iterator it);

    // Removes a retired block from the pool (its entry must have been taken or released).
    void RemoveRetired(RetiredList::iterator it);

    // Frees a retired block's code and removes it from the pool.
    void ReleaseRetired(RetiredList::iterator it);

    // Frees every retired block's code (at the code cap, before flushing).
    void ReleaseAllRetired();

    // Whether the backend's code is at the native code cap.
    bool AtCodeCap() const {
        return m_native->CodeBytes() >= m_maxNativeCodeBytes && m_native->CodeBytes() > 0;
    }

    // Drops every block (Flush without counting it as requested).
    void FlushAll();

    // At the IR cap: frees IR blocks, live IR-only ones (and their recent slots; pending ones
    // included, their results are dropped when they arrive) and IR pool ones together, smallest
    // lastUse first, until m_totalInsts is below kIrCapTargetEighths / 8 of the cap. Native blocks,
    // their code and link slots stay. Only called from Build, never while a block runs.
    void EvictLeastRecentlyUsedIr();

    // A validated hit: stamps the use, counts the run and compiles the block natively on its
    // threshold run (in the background with a worker).
    const CachedBlock &Hit(CachedBlock &entry, ymir::sh2::SH2JitContext &ctx, uint32_t pc) {
        entry.lastUse = ++m_useClock;
        if (entry.runsUntilNative != 0 && --entry.runsUntilNative == 0) {
            return m_worker != nullptr ? SubmitNative(entry, ctx, pc) : CompileNative(entry, ctx, pc);
        }
        return entry;
    }

    // Takes a retired block for pc back if one is current (Reuse), else a retired IR block that
    // SameBuild accepts (ReuseIr). Otherwise builds the IR block for pc and caches it (first, at the
    // IR cap, freeing the least recently used IR blocks; and at the native code cap, if it compiles
    // now, releasing the pool and then, if still at the cap, flushing everything). Then, as
    // Started: compiles it natively now if compileNow or the threshold is 1.
    const CachedBlock &Build(ymir::sh2::SH2JitContext &ctx, uint32_t pc, bool compileNow);

    // A built or reused IR block just cached at pc: stamps the use, then compiles it now (native) or
    // sets its runs until the native compile (damping included).
    const CachedBlock &Started(CachedBlock &entry, ymir::sh2::SH2JitContext &ctx, uint32_t pc, bool native);

    // Compiles entry natively and frees its IR. At the native code cap it releases the pool (entry
    // is in the map, never in the pool); if still at the cap it flushes the cache (entry included)
    // and rebuilds and compiles pc from scratch. On failure entry stays IR-only.
    const CachedBlock &CompileNative(CachedBlock &entry, ymir::sh2::SH2JitContext &ctx, uint32_t pc);

    // CompileNative's code cap handling, then (below kMaxPendingCompiles) Prepare, recording
    // codePages, and Submit; entry stays IR-only. At the bound, entry waits another threshold.
    const CachedBlock &SubmitNative(CachedBlock &entry, ymir::sh2::SH2JitContext &ctx, uint32_t pc);

    // Takes the worker's results and commits each one whose entry still waits for it.
    void InstallResults();

    // After a successful compile: counts it, frees entry's IR and publishes self-validating code.
    void Installed(CachedBlock &entry, uint32_t pc);

    INativeBackend *m_native;
    CompileWorker *m_worker; // nullptr: synchronous compiles (always without a native backend)
    size_t m_maxNativeCodeBytes;
    size_t m_maxCachedInsts;
    size_t m_maxRetiredCodeBytes;
    uint32_t m_maxStaleDampShift;
    uint32_t m_nativeCompileThreshold;
    BlockMap m_blocks;
    // Times a block at each PC was dropped as stale (saturating at 255); cleared by a flush.
    std::unordered_map<uint32_t, uint8_t> m_staleCounts;
    std::array<RecentSlot, kRecentSlots> m_recent{};
    // The pool, least recently retired first, and its blocks by PC (also oldest first).
    RetiredList m_retired;
    std::unordered_map<uint32_t, std::vector<RetiredList::iterator>> m_retiredByPc;
    // The IR pool, least recently retired first, and its blocks by PC (also oldest first).
    RetiredList m_retiredIr;
    std::unordered_map<uint32_t, std::vector<RetiredList::iterator>> m_retiredIrByPc;
    // IR instructions of IR-only blocks, in the map and in the IR pool.
    size_t m_totalInsts = 0;
    // Background compiles not yet taken back: ticket -> PC. Tickets increase and are never reused;
    // a flush (which cancels every pending one) starts a generation at m_nextTicket, so a ticket
    // below m_generationStart belongs to an older generation.
    std::unordered_map<uint64_t, uint32_t> m_pendingPcs;
    uint64_t m_nextTicket = 1;
    uint64_t m_generationStart = 1;
    std::vector<CompileResult> m_results; // InstallResults' buffer
    uint64_t m_asyncCompiles = 0;
    uint64_t m_asyncDropped = 0;
    uint64_t m_compiles = 0;
    uint64_t m_invalidations = 0;
    uint64_t m_compileFallbacks = 0;
    uint64_t m_nativeCompiles = 0;
    uint64_t m_nativeBytes = 0;
    size_t m_peakNativeBytes = 0;
    uint64_t m_staleNativeBytes = 0;
    uint64_t m_releasedNativeBytes = 0;
    uint64_t m_retiredReuses = 0;
    uint64_t m_retiredEvictions = 0;
    size_t m_retiredBytes = 0;
    uint64_t m_retiredIrReuses = 0;
    size_t m_retiredIrInsts = 0;
    uint64_t m_retiredIrReleases = 0;
    uint64_t m_irCapNs = 0;
    uint64_t m_useClock = 0;           // CachedBlock::lastUse
    std::vector<IrVictim> m_irVictims; // EvictLeastRecentlyUsedIr's buffer
    uint64_t m_dampedBuilds = 0;
    uint64_t m_buildNs = 0;
    uint64_t m_nativeNs = 0;
    uint64_t m_irEvictions = 0;
    uint64_t m_irEvictedBlocks = 0;
    uint64_t m_flushesCodeCap = 0;
    uint64_t m_flushesRequested = 0;
};

} // namespace brimir::jit
