#pragma once

// IR -> x86-64 lowering of one block with asmjit's x86::Compiler (design/sh2-jit-m2.md section 4.3).
// Internal to src/jit/src/x64/.

#include <brimir/jit/interp_backend.hpp> // ExitInfo
#include <brimir/jit/ir.hpp>
#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <asmjit/x86.h>

#include <cstdint>
#include <exception>
#include <type_traits>

namespace brimir::jit {

// The one argument of a generated block function. Run fills the inputs, the generated code writes
// `out` before it returns.
struct X64Frame {
    ymir::sh2::SH2JitContext *ctx;
    uint64_t limit;             // boundary when cycles >= limit: target >= entry ? target - entry : 0
    uint64_t entryCycles;       // *ctx->cyclesExecuted at entry
    const bool *abortRequested; // never null (points to a static false when the caller passed nullptr)
    uint8_t stop = 0;           // set by trampolines (Task 3)
    std::exception_ptr error;   // set by trampolines (Task 3)
    ExitInfo out;               // written by generated code before it returns
};
// Generated code addresses the frame with offsetof.
static_assert(std::is_standard_layout_v<X64Frame>);
static_assert(std::is_standard_layout_v<ExitInfo>);

using X64BlockFn = void (*)(X64Frame *frame);

// Emits `block` as one function `void fn(X64Frame *)` into `cc`, which must be attached to a
// CodeHolder. Guest state is addressed relative to ctx.R with offsets taken from `ctx`, so the code
// only runs against that CPU. Returns false, before emitting anything, if the block has an op this
// emitter does not lower or a state field whose offset from ctx.R does not fit in 32 bits. asmjit
// errors are reported through the CodeHolder's error handler, not by the return value.
bool EmitBlock(asmjit::x86::Compiler &cc, const Block &block, const ymir::sh2::SH2JitContext &ctx);

// True if EmitBlock lowers every op of `block` (state offsets aside).
bool CanEmitBlock(const Block &block);

} // namespace brimir::jit
