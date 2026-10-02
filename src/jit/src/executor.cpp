#include <brimir/jit/executor.hpp>

#include <brimir/jit/bus_fast_path.hpp>

#include <cassert>

namespace brimir::jit {

namespace {

BackendKind EffectiveBackend(BackendKind kind) {
    return IsBackendAvailable(kind) ? kind : BackendKind::Ir;
}

} // namespace

Executor::Executor(BackendKind kind)
    : m_kind(EffectiveBackend(kind))
    , m_native(MakeNativeBackend(m_kind))
    , m_cache(m_native.get()) {}

uint64 Executor::Run(ymir::sh2::SH2JitContext &ctx, uint64 executed, uint64 target) {
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
    for (int attempt = 0;; ++attempt) {
        const CachedBlock &entry = m_cache.Get(ctx, pc);
        m_stats.compileFallbacks = m_cache.CompileFallbacks();
        if (entry.block.guestInstrCount == 0) {
            return interpret();
        }
        ExitInfo info = RunEntry(entry, ctx, target, allowChain);
        if (!info.stale) {
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

ExitInfo Executor::RunEntry(const CachedBlock &entry, ymir::sh2::SH2JitContext &ctx, uint64 target,
                            bool allowChain) {
    // Same as InterpretNext on its non-interrupt path.
    *ctx.intrAllow = true;
    // Clears m_inBlock and applies a deferred flush on every exit, including an exception thrown
    // by a memory callback, so later flushes are never deferred forever. It covers a whole chain:
    // a flush requested in any of its blocks aborts that block, which ends the chain.
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
    if (entry.code.entry != nullptr) {
        // Counted before the run, so a callback exception (rethrown by Run) still counts the first
        // block; then corrected to the chain's count (0 when the first block was stale).
        ++m_stats.nativeBlocksRun;
        const ExitInfo info = m_native->Run(entry.code, ctx, target, &m_flushPending, allowChain);
        m_stats.blocksRun = m_stats.blocksRun - 1 + info.blocksRun;
        m_stats.nativeBlocksRun = m_stats.nativeBlocksRun - 1 + info.blocksRun;
        if (info.blocksRun > 1) {
            m_stats.chainedBlocks += info.blocksRun - 1;
        }
        return info;
    }
    return RunBlock(entry.block, ctx, target, &m_flushPending);
}

} // namespace brimir::jit
