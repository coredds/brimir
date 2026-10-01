#pragma once

// IR interpreter backend: executes IR blocks directly (milestone 1 backend).

#include <brimir/jit/ir.hpp>

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <cstdint>

namespace brimir::jit {

struct ExitInfo {
    uint64_t cycles = 0;
    uint8_t retired = 0;  // guest instructions fully executed
    bool busWait = false; // exited on a bus wait; the instruction at PC retries
    bool aborted = false; // stopped mid-block on an abort request; PC was not written
};

// Executes a verified, non-empty block against the live SH-2 state.
// If *abortRequested becomes true during a memory access or pipeline refill (for example a WDT
// register access that resets the CPU and flushes the cache), the block stops right there: PC is
// left untouched and the result has aborted = true with the cycles accumulated so far.
ExitInfo RunBlock(const Block &block, ymir::sh2::SH2JitContext &ctx, const bool *abortRequested = nullptr);

} // namespace brimir::jit
