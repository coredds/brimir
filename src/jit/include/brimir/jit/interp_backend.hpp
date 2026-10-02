#pragma once

// IR interpreter backend: executes IR blocks directly (milestone 1 backend).

#include <brimir/jit/ir.hpp>

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <cstdint>

namespace brimir::jit {

// Cycle target meaning "no budget limit" (tests that run a single block).
constexpr uint64_t kNoCycleTarget = ~uint64_t{0};

struct ExitInfo {
    // For a native chain (INativeBackend::Run with allowChain), cycles is the total of all its
    // blocks, and the other fields describe how its last block exited.
    uint64_t cycles = 0;
    uint8_t retired = 0;  // guest instructions fully executed (the last block of a chain)
    bool busWait = false; // exited on a bus wait; the instruction at PC retries
    bool aborted = false; // stopped mid-block on an abort request; PC was not written
    bool boundary = false; // stopped before an instruction: cycle target reached or interrupt pending
    // Native code only: the block at PC no longer matches its guest code (a code page was remapped
    // or its bytes changed). That block ran nothing and wrote nothing; the executor recompiles it.
    // In a chain, the blocks before it ran normally (blocksRun > 0). RunBlock never sets it.
    bool stale = false;
    // Native code only: blocks that passed their entry checks and ran (RunBlock leaves it 0).
    uint32_t blocksRun = 0;
};

// Executes a verified, non-empty block against the live SH-2 state.
// Before every instruction after the first, the block makes the interpreter's two checks
// (CheckBoundary): *ctx.cyclesExecuted at entry plus the cycles so far must stay below `target`,
// and no interrupt may be pending and allowed; otherwise it stops there with boundary = true.
// If *abortRequested becomes true during a memory access or pipeline refill (for example a WDT
// register access that resets the CPU and flushes the cache), the block stops right there: PC is
// left untouched and the result has aborted = true with the cycles accumulated so far.
//
// Known refills (see Block in ir.hpp) store their value instead of calling refillPipeline when the
// block's code is still on array pages (checked at entry) and the run has not set codeDirty yet.
// Data accesses are classified like the x64 inline fast path, with FastArrayPointer, even though
// RunBlock performs all of them through ctx.read/ctx.write:
//   - an array-page store sets codeDirty if it overlaps the block's code in host memory (any alias);
//   - a handler write sets it, and so does a handler read outside partition 0b111.
// On-chip register reads (partition 0b111, SH2::OnChipRegRead) never write memory: they read
// registers and at most advance the FRT/WDT, which can raise an interrupt or reset the CPU (a reset
// reads the vectors and refills, and requests the abort below); none of that stores to memory.
ExitInfo RunBlock(const Block &block, ymir::sh2::SH2JitContext &ctx, uint64_t target = kNoCycleTarget,
                  const bool *abortRequested = nullptr);

} // namespace brimir::jit
