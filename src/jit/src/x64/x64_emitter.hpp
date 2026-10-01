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
    uint8_t stop = 0;           // set by trampolines: abort requested or exception caught
    std::exception_ptr error;   // set by trampolines: the exception a callback threw
    ExitInfo out;               // written by generated code before it returns
};
// Generated code addresses the frame with offsetof.
static_assert(std::is_standard_layout_v<X64Frame>);
static_assert(std::is_standard_layout_v<ExitInfo>);

using X64BlockFn = void (*)(X64Frame *frame);

// Trampolines: the only functions generated code calls (defined in x64_backend.cpp). Each loads
// its callback from frame->ctx at call time. None lets an exception escape: a callback exception
// is stored in frame->error, frame->stop is set and the trampoline returns 0. TrRead, TrWrite and
// TrRefill also set frame->stop when *frame->abortRequested is true after the callback. Generated
// code tests frame->stop after every trampoline except TrDiv1/TrMacW/TrMacL, and on stop takes
// RunBlock's abort exit (out.aborted, out.cycles; PC untouched). Run then rethrows frame->error.
// bool arguments are passed as uint32_t 0/1.
uint32_t TrRead(X64Frame *f, uint32_t address, uint32_t size, uint32_t instrFetch) noexcept;
void TrWrite(X64Frame *f, uint32_t address, uint32_t size, uint32_t value) noexcept;
void TrRefill(X64Frame *f, uint32_t address) noexcept;
uint64_t TrAccessCycles(X64Frame *f, uint32_t address, uint32_t size, uint32_t write) noexcept;
uint64_t TrAccessCyclesRMWByte(X64Frame *f, uint32_t address) noexcept;
uint32_t TrBusWait(X64Frame *f, uint32_t address, uint32_t size, uint32_t write) noexcept; // 0 or 1
void TrSetupDelaySlot(X64Frame *f, uint32_t target) noexcept;
void TrEndDelaySlot(X64Frame *f) noexcept;
void TrSetSR(X64Frame *f, uint32_t value, uint32_t delaySlot) noexcept;
uint32_t TrDiv1(X64Frame *f, uint32_t rn, uint32_t rm, uint32_t rmIsRn) noexcept;
void TrMacW(X64Frame *f, uint32_t op1, uint32_t op2) noexcept;
void TrMacL(X64Frame *f, uint32_t op1, uint32_t op2) noexcept;

// Emits `block` as one function `void fn(X64Frame *)` into `cc`, which must be attached to a
// CodeHolder. Guest state is addressed relative to ctx.R with offsets taken from `ctx`, so the code
// only runs against that CPU. Returns false, before emitting anything, if the block has an op this
// emitter does not lower or a state field whose offset from ctx.R does not fit in 32 bits. asmjit
// errors are reported through the CodeHolder's error handler, not by the return value.
bool EmitBlock(asmjit::x86::Compiler &cc, const Block &block, const ymir::sh2::SH2JitContext &ctx);

// True if EmitBlock lowers every op of `block` (state offsets aside). Every valid op is lowered, so
// this only rejects blocks with out-of-range op values.
bool CanEmitBlock(const Block &block);

} // namespace brimir::jit
