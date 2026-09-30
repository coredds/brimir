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
};

// Executes a verified, non-empty block against the live SH-2 state.
ExitInfo RunBlock(const Block &block, ymir::sh2::SH2JitContext &ctx);

} // namespace brimir::jit
