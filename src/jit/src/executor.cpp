#include <brimir/jit/executor.hpp>

namespace brimir::jit {

uint64 Executor::Run(ymir::sh2::SH2JitContext &ctx, uint64 executed, uint64 target) {
    while (executed < target) {
        // On-chip timers read the running count, exactly as in the interpreter loop.
        *ctx.cyclesExecuted = executed;
        executed += Step(ctx).cycles;
    }
    *ctx.cyclesExecuted = executed;
    return executed;
}

void Executor::Flush() {
    m_cache.Flush();
}

ExitInfo Executor::Step(ymir::sh2::SH2JitContext &ctx) {
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
    if ((pc & 2u) != 0 && static_cast<uint16_t>(*ctx.fetchedOpcodes) != ctx.peekInstruction(ctx.sh2, pc)) {
        return interpret();
    }

    const Block &block = m_cache.Get(ctx, pc);
    if (block.guestInstrCount == 0) {
        return interpret();
    }

    // Same as InterpretNext on its non-interrupt path.
    *ctx.intrAllow = true;
    ++m_stats.blocksRun;
    return RunBlock(block, ctx);
}

} // namespace brimir::jit
