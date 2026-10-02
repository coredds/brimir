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
        // On-chip timers read the running count, exactly as in the interpreter loop.
        *ctx.cyclesExecuted = executed;
        executed += Step(ctx, target).cycles;
    }
    *ctx.cyclesExecuted = executed;
    return executed;
}

void Executor::Flush() {
    if (m_inBlock) {
        // Freeing the running block (or its native code) here would be a use-after-free; Step
        // flushes after the block returns.
        m_flushPending = true;
        return;
    }
    m_cache.Flush();
}

ExitInfo Executor::Step(ymir::sh2::SH2JitContext &ctx, uint64 target) {
    const auto interpret = [&] {
        ++m_stats.interpreted;
        ExitInfo info;
        info.cycles = ctx.interpretOne(ctx.sh2);
        info.retired = 1;
        return info;
    };

    // Interrupt entry and pending delay slots are handled by the interpreter.
    if (*ctx.delaySlot || (*ctx.intrPending && *ctx.intrAllow)) {
        return interpret();
    }

    // At PC & 2 the interpreter executes the opcode already in its fetch buffer, which can differ
    // from memory if the code was modified after the fetch. Only compile when they agree.
    const uint32_t pc = *ctx.PC;
    if ((pc & 2u) != 0 && static_cast<uint16_t>(*ctx.fetchedOpcodes) != PeekOpcode(ctx, pc)) {
        return interpret();
    }

    // A native block whose code pages were remapped since it was compiled reports `stale` before
    // doing anything: drop it, recompile and run the new block in this same step. The new block is
    // compiled against the current pages, so it cannot be stale again.
    for (int attempt = 0;; ++attempt) {
        const CachedBlock &entry = m_cache.Get(ctx, pc);
        m_stats.compileFallbacks = m_cache.CompileFallbacks();
        if (entry.block.guestInstrCount == 0) {
            return interpret();
        }
        const ExitInfo info = RunEntry(entry, ctx, target);
        if (!info.stale) {
            return info;
        }
        ++m_stats.staleEntries;
        --m_stats.blocksRun;
        --m_stats.nativeBlocksRun;
        m_cache.Invalidate(pc);
        if (attempt > 0) {
            assert(false && "a freshly compiled block reported stale");
            return interpret();
        }
    }
}

ExitInfo Executor::RunEntry(const CachedBlock &entry, ymir::sh2::SH2JitContext &ctx, uint64 target) {
    // Same as InterpretNext on its non-interrupt path.
    *ctx.intrAllow = true;
    ++m_stats.blocksRun;
    // Clears m_inBlock and applies a deferred flush on every exit, including an exception thrown
    // by a memory callback, so later flushes are never deferred forever.
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
    if (entry.code.entry != nullptr) {
        ++m_stats.nativeBlocksRun;
        return m_native->Run(entry.code, ctx, target, &m_flushPending);
    }
    return RunBlock(entry.block, ctx, target, &m_flushPending);
}

} // namespace brimir::jit
