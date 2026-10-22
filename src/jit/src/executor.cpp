#include <brimir/jit/executor.hpp>

#include <brimir/jit/bus_fast_path.hpp>

#if BRIMIR_JIT_HAS_X64
#include "x64/x64_factory.hpp"
#endif

#include <cassert>

namespace brimir::jit {

namespace {

BackendKind EffectiveBackend(BackendKind kind) {
    return IsBackendAvailable(kind) ? kind : BackendKind::Ir;
}

} // namespace

Executor::Executor(BackendKind kind, uint32_t nativeCompileThreshold, CompileWorker *worker)
    : m_kind(EffectiveBackend(kind))
    , m_native(MakeNativeBackend(m_kind))
    , m_cache(m_native.get(), kMaxNativeCodeBytes, nativeCompileThreshold, kMaxCachedInsts, kMaxRetiredCodeBytes,
              kMaxStaleDampShift, worker) {}

uint64 Executor::Run(ymir::sh2::SH2JitContext &ctx, uint64 executed, uint64 target) {
    ++m_stats.runs;
    while (executed < target) {
        // On-chip timers read the running count, exactly as in the interpreter loop. Chained
        // blocks store it themselves between blocks (INativeBackend::Run).
        *ctx.cyclesExecuted = executed;
        executed += StepImpl(ctx, target, m_chaining).cycles;
    }
    *ctx.cyclesExecuted = executed;
    return executed;
}

void Executor::Flush() {
    m_resume = Resume{};
    if (m_inBlock) {
        // Freeing the running block (or its native code) here would be a use-after-free; Step
        // flushes after the block or chain returns.
        m_flushPending = true;
        return;
    }
    m_cache.Flush();
}

ExitInfo Executor::Step(ymir::sh2::SH2JitContext &ctx, uint64 target) {
    return StepImpl(ctx, target, false);
}

ExitInfo Executor::StepImpl(ymir::sh2::SH2JitContext &ctx, uint64 target, bool allowChain) {
    const auto interpret = [&] {
        ++m_stats.interpreted;
        ExitInfo info;
        info.cycles = ctx.interpretOne(ctx.sh2);
        info.retired = 1;
        return info;
    };

    // The resume recorded by the previous step (Run), used at most once.
    const void *const resumeEntry = m_resume.entry;
    m_resume.entry = nullptr;

    // Interrupt entry and pending delay slots are handled by the interpreter.
    // (A chained block's prologue repeats this check and the next one; see INativeBackend::Run.)
    if (*ctx.delaySlot || (*ctx.intrPending && *ctx.intrAllow)) {
        return interpret();
    }

    // At PC & 2 the interpreter executes the opcode already in its fetch buffer, which can differ
    // from memory if the code was modified after the fetch. Only compile when they agree.
    const uint32_t pc = *ctx.PC;
    if ((pc & 2u) != 0 && static_cast<uint16_t>(*ctx.fetchedOpcodes) != PeekOpcode(ctx, pc)) {
        return interpret();
    }

    // A native block whose guest code changed (or whose code pages were remapped) since it was
    // compiled reports `stale` before doing anything: drop it, recompile and run the new block in
    // this same step. The new block is compiled against the current code, so it cannot be stale
    // again.
    //
    // Resume: while the cache version is the one recorded, the entry is still the live native code
    // of the map's block at pc (BlockCache::Version), exactly what Get would return; it is
    // self-validating, so its prologue checks the guest code, and a stale report is handled as for
    // a looked-up block. Waiting worker results must be installed by a Get first.
    int attempt = 0;
    if (resumeEntry != nullptr && m_resume.pc == pc && m_resume.version == m_cache.Version() &&
        !m_cache.HasWaitingResults()) {
        ++m_stats.resumes;
        const uint64_t version = m_resume.version;
        ExitInfo info = RunEntry(NativeCode{resumeEntry, true}, nullptr, ctx, target, allowChain);
        if (!info.stale) {
            if (info.next != nullptr) {
                m_resume = Resume{*ctx.PC, info.next, version};
            }
            return info;
        }
        ++m_stats.staleEntries;
        if (info.blocksRun > 0) {
            m_cache.Invalidate(*ctx.PC);
            info.stale = false;
            return info;
        }
        m_cache.Invalidate(pc);
        attempt = 1;
    }
    for (;; ++attempt) {
        const CachedBlock &entry = m_cache.Get(ctx, pc);
        m_stats.compileFallbacks = m_cache.CompileFallbacks();
        if (entry.block.guestInstrCount == 0) {
            return interpret();
        }
        // The cache cannot change while the block runs (a flush is deferred and bumps the version
        // after it), so a link-table entry the block reports belongs to this version.
        const uint64_t version = m_cache.Version();
        ExitInfo info = RunEntry(entry.code, &entry.block, ctx, target, allowChain);
        if (!info.stale) {
            if (info.next != nullptr) {
                m_resume = Resume{*ctx.PC, info.next, version};
            }
            return info;
        }
        ++m_stats.staleEntries;
        if (info.blocksRun > 0) {
            // A block reached by chaining was stale: the chain ran up to it, and PC is its start.
            // Drop it; the next step makes the usual checks and recompiles it.
            m_cache.Invalidate(*ctx.PC);
            info.stale = false;
            return info;
        }
        m_cache.Invalidate(pc);
        if (attempt > 0) {
            assert(false && "a freshly compiled block reported stale");
            return interpret();
        }
    }
}

ExitInfo Executor::RunEntry(const NativeCode &code, const Block *block, ymir::sh2::SH2JitContext &ctx,
                            uint64 target, bool allowChain) {
    // Same as InterpretNext on its non-interrupt path.
    *ctx.intrAllow = true;
    // Clears m_inBlock and applies a deferred flush on every exit, including an exception thrown
    // by a memory callback, so later flushes are never deferred forever. It covers a whole chain:
    // a flush requested by a read, write or refill callback aborts that block; one requested by
    // any other callback (setSR, endDelaySlot, ...) lets the block finish and ends the chain at that
    // block's exit. Either way no later block of the chain runs before the flush here.
    struct BlockScope {
        Executor &self;
        ~BlockScope() {
            self.m_inBlock = false;
            if (self.m_flushPending) {
                self.m_flushPending = false;
                self.m_cache.Flush();
            }
        }
    };
    m_flushPending = false;
    m_inBlock = true;
    const BlockScope scope{*this};
    ++m_stats.blocksRun;
    if (code.entry != nullptr) {
        // Counted before the run, so a callback exception (rethrown by Run) still counts the first
        // block; then corrected to the chain's count (0 when the first block was stale).
        ++m_stats.nativeBlocksRun;
#if BRIMIR_JIT_HAS_X64
        // The only native backend: call its Run directly.
        const ExitInfo info = RunX64(*m_native, code, ctx, target, &m_flushPending, allowChain);
#else
        const ExitInfo info = m_native->Run(code, ctx, target, &m_flushPending, allowChain);
#endif
        m_stats.blocksRun = m_stats.blocksRun - 1 + info.blocksRun;
        m_stats.nativeBlocksRun = m_stats.nativeBlocksRun - 1 + info.blocksRun;
        if (info.blocksRun > 1) {
            m_stats.chainedBlocks += info.blocksRun - 1;
        }
        return info;
    }
    assert(block != nullptr);
    return RunBlock(*block, ctx, target, &m_flushPending);
}

} // namespace brimir::jit
