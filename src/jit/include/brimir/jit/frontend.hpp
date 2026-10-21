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

// Whether BuildBlock(ctx, block.startPC) would still end the block where block ends, given that
// its guestOpcodes are current. To stop a block, the front end also reads the words after the
// last instruction (an unsupported instruction, or a delayed branch whose slot is unsupported or
// does not fit); they are not in guestOpcodes unless one is the tail word.
bool BlockEndIsCurrent(ymir::sh2::SH2JitContext &ctx, const Block &block);

} // namespace brimir::jit
