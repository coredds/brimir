#pragma once

// SH-2 JIT executor: dispatches compiled blocks and falls back to the interpreter
// (see design/sh2-jit.md section 4.3).

#include <brimir/jit/backend.hpp>
#include <brimir/jit/block_cache.hpp>
#include <brimir/jit/frontend.hpp>
#include <brimir/jit/interp_backend.hpp>

#include <ymir/core/types.hpp>
#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <cstdint>
#include <memory>

namespace brimir::jit {

class Executor final : public ymir::sh2::ISH2Executor {
public:
    struct Stats {
        uint64_t blocksRun = 0;        // compiled blocks run (native or RunBlock)
        uint64_t interpreted = 0;      // single instructions run by the interpreter
        uint64_t nativeBlocksRun = 0;  // compiled blocks run as native code
        uint64_t compileFallbacks = 0; // blocks the native backend could not compile (run with RunBlock)
        // Native blocks dropped at entry: guest code or code page changed. A chained run can count
        // differently from a stepped one: a chained block checks staleness before the step gates
        // (pending interrupt, PC & 2), so it may count a stale block that a step would interpret
        // around first, and count later or never (code changed back, cache flushed).
        uint64_t staleEntries = 0;
        uint64_t chainedBlocks = 0;    // native blocks entered from another block (Run with chaining)
        // Steps that entered native code through a resume (Run), without a BlockCache::Get; one
        // whose block then reports stale still counts (its retry calls Get). Their blocks count in
        // blocksRun and nativeBlocksRun as for any native step.
        uint64_t resumes = 0;
        uint64_t runs = 0; // Run calls
    };

    // Runs compiled blocks on `kind`; an unavailable kind falls back to BackendKind::Ir. A block runs
    // with RunBlock until its nativeCompileThreshold-th run, which compiles it natively
    // (kNativeCompileThreshold; tests pass 1 for native code on the first run). worker: finalizes
    // the native compiles in the background, or nullptr for synchronous compiles (see BlockCache);
    // not owned, it must outlive the executor.
    explicit Executor(BackendKind kind = DefaultBackend(), uint32_t nativeCompileThreshold = kNativeCompileThreshold,
                      CompileWorker *worker = nullptr);

    Executor(const Executor &) = delete;
    Executor &operator=(const Executor &) = delete;

    BackendKind Backend() const {
        return m_kind;
    }

    // Steps until `target`, writing *ctx.cyclesExecuted = executed before each step. With chaining
    // (the default), a native block hands control to the next linked block itself, with the same
    // checks and the same *ctx.cyclesExecuted updates as these steps (INativeBackend::Run).
    //
    // Resume: a native step whose chain ended at the cycle limit with a next entry
    // (ExitInfo::next) records {PC, next, cache Version}. The next step, after Step's gates
    // (interrupt, delay slot, PC & 2), runs that entry without BlockCache::Get when PC and the
    // version still match and the cache has no worker results waiting; otherwise, and after any
    // other step outcome or a Flush, the record is dropped. Only self-validating blocks are
    // published, so the resumed block still checks its guest code and pages in its prologue.
    uint64 Run(ymir::sh2::SH2JitContext &ctx, uint64 executed, uint64 target) override;

    // Enables or disables block chaining in Run (tests and diagnostics; Step never chains).
    void SetChaining(bool enabled) {
        m_chaining = enabled;
    }

    // Drops all compiled blocks. When called from inside a running block (a memory access that
    // resets the CPU), the flush is deferred until the block returns and the block is aborted.
    void Flush() override;

    // Runs one compiled block, or one interpreter instruction when no block applies
    // (pending interrupt, delay slot, or unsupported first instruction; reported as retired = 1).
    // A compiled block stops before any instruction at which *ctx.cyclesExecuted + its cycles
    // would reach `target`, exactly where the interpreter's Advance loop stops.
    ExitInfo Step(ymir::sh2::SH2JitContext &ctx, uint64 target = kNoCycleTarget);

    const BlockCache &Cache() const {
        return m_cache;
    }
    // Tests and diagnostics; never while a block runs.
    BlockCache &MutableCache() {
        return m_cache;
    }
    const Stats &GetStats() const {
        return m_stats;
    }

private:
    // Step; with allowChain, a native block may chain to further blocks (Run).
    ExitInfo StepImpl(ymir::sh2::SH2JitContext &ctx, uint64 target, bool allowChain);

    // Runs a compiled block (native `code`, and a native chain, or `block` with RunBlock when
    // code.entry is nullptr) with the in-block flush deferral.
    ExitInfo RunEntry(const NativeCode &code, const Block *block, ymir::sh2::SH2JitContext &ctx, uint64 target,
                      bool allowChain);

    // Where the next step may resume (Run); entry == nullptr: nothing recorded.
    struct Resume {
        uint32_t pc = 0;
        const void *entry = nullptr;
        uint64_t version = 0;
    };

    BackendKind m_kind;
    std::unique_ptr<INativeBackend> m_native; // nullptr for BackendKind::Ir; declared before m_cache
    BlockCache m_cache;
    Stats m_stats;
    Resume m_resume;
    bool m_chaining = true;
    bool m_inBlock = false;      // a block (or chain) owned by m_cache is executing
    bool m_flushPending = false; // Flush() was requested in a block; also the block's abort flag
};

} // namespace brimir::jit
