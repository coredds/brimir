#pragma once

// Decodes guest SH-2 code into IR blocks (see design/sh2-jit.md section 5).

#include <brimir/jit/ir.hpp>

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <cstdint>

namespace brimir::jit {

constexpr uint32_t kMaxBlockInstructions = 32;
static_assert(kMaxBlockInstructions <= 255, "ExitInfo::retired and Inst::retired are uint8_t");

// Whether code at pc may be compiled: cached (0b000) and cache-through (0b001, 0b101) areas.
bool IsCompilableAddress(uint32_t pc);

// Decodes a block starting at pc. guestInstrCount == 0 means the first instruction is not
// supported and must run on the interpreter.
Block BuildBlock(ymir::sh2::SH2JitContext &ctx, uint32_t pc);

} // namespace brimir::jit
