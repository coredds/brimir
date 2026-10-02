#pragma once

// IR -> x86-64 lowering of one block with asmjit's x86::Compiler (design/sh2-jit-m2.md section 4.3).
// Internal to src/jit/src/x64/.

#include <brimir/jit/interp_backend.hpp> // ExitInfo
#include <brimir/jit/ir.hpp>
#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <asmjit/x86.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <type_traits>

namespace brimir::jit {

// The one argument of a generated block function, shared by all blocks of a chain. Run fills the
// inputs; the generated code writes `out` before it returns or chains.
struct X64Frame {
    ymir::sh2::SH2JitContext *ctx;
    uint64_t limit;             // boundary when cycles >= limit: target >= entry ? target - entry : 0
    uint64_t entryCycles;       // *ctx->cyclesExecuted when the chain started
    const bool *abortRequested; // never null (points to a static false when the caller passed nullptr)
    uint8_t stop = 0;           // set by trampolines: abort requested or exception caught
    uint8_t codeDirty = 0;      // set by generated code: a data access may have written the block's code
    uint8_t allowChain = 0;     // input: chainable exits may enter the next linked block
    uint8_t chained = 0;        // set by generated code: the running block was entered by chaining
    std::exception_ptr error;   // set by trampolines: the exception a callback threw
    // Written by generated code. out.cycles counts from the start of the chain: each block starts
    // from it, so the cycle limit and SyncCycles stay relative to limit and entryCycles.
    ExitInfo out;
};
// Generated code addresses the frame with offsetof.
static_assert(std::is_standard_layout_v<X64Frame>);
static_assert(std::is_standard_layout_v<ExitInfo>);

// A block function returns the entry of the block to run next (chaining) or nullptr to return to
// the caller. X64Backend::Run calls it in a loop.
using X64BlockFn = const void *(*)(X64Frame *frame);

// Link table: direct-mapped by PC, owned by X64Backend; generated code embeds its address. An
// entry matches only its exact pc; an empty slot is {0, nullptr} (a match then returns nullptr).
struct X64LinkSlot {
    uint32_t pc;
    const void *entry;
};
static_assert(sizeof(X64LinkSlot) == 16 && offsetof(X64LinkSlot, entry) == 8);
constexpr uint32_t kLinkSlots = 16384;
constexpr uint32_t LinkIndex(uint32_t pc) {
    return (pc >> 1) & (kLinkSlots - 1);
}

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
// Debug check: a block was chained to while a delay slot was pending, which no chainable exit
// allows (it asserts; the block then returns to the executor, which interprets the slot).
void TrChainedInDelaySlot(X64Frame *f) noexcept;

// Emits `block` as one function `const void *fn(X64Frame *)` (X64BlockFn) into `cc`, which must be
// attached to a CodeHolder. Guest state is addressed relative to ctx.R with offsets taken from
// `ctx`, so the code only runs against that CPU. Returns false, before emitting anything, if the
// block has an op this emitter does not lower or a state field whose offset from ctx.R does not fit
// in 32 bits. asmjit errors are reported through the CodeHolder's error handler, not by the return
// value.
//
// Self-validating blocks and chaining (design/sh2-x64-performance.md, 2C item 2): when the block's
// guestOpcodes are all on array pages (inline bus), ctx has delaySlot, fetchedOpcodes and
// intcPendingLevel, and `links` is given, selfValidating is set and the prologue, in this order:
//   1. checks that every code page still has its compile-time array and that the code bytes in
//      host memory still equal guestOpcodes; otherwise it returns out.stale and writes nothing else;
//   2. when frame->chained, repeats Executor::Step's checks: returns nullptr (to the executor) if
//      an interrupt is pending and allowed, or, at PC & 2, if the fetch buffer's low half is not
//      the first opcode; then sets *intrAllow = true (a pending delay slot calls
//      TrChainedInDelaySlot and returns);
//   3. counts the block in out.blocksRun.
// A static Exit, a taken ExitIf and ExitDynamic then chain when frame->allowChain: after writing PC
// and out as usual they return nullptr if *abortRequested (a flush requested by a callback that
// did not stop the block), store *cyclesExecuted = entryCycles + cycles, return nullptr if
// cycles >= limit, and otherwise return the entry of the link slot whose pc equals PC (nullptr on a
// miss), setting frame->chained. Boundary, bus-wait, abort and stale exits never chain.
//
// With ctx.bus describing a page table, Load/Store/AddAccessCycles/ExitIfBusWait take the inline
// bus fast path (bus_fast_path.hpp) and call their trampolines only for handler or unmapped pages
// and partitions that do not reach the bus. The table's contents are read at run time; its address
// is embedded in the code. An inline access does not test *abortRequested (RunBlock's Load/Store
// do), because it runs no callback. A flush requested by a callback that does not stop the block
// (TrSetSR, TrEndDelaySlot, TrSetupDelaySlot, TrAccessCycles, TrAccessCyclesRMWByte, TrBusWait) is
// supported only up to the block's exit, where ChainExit ends the chain: an inline array-page
// Load/Store later in the same block would not abort, while RunBlock's would. This cannot happen
// today, and the backend relies on it (design/sh2-jit.md section 6.5): in production only
// SH2::Reset flushes inside a block, and only from read/write callbacks (a watchdog reset through
// an on-chip register access), which TrRead/TrWrite/TrRefill report by stopping the block.
//
// Fetch buffer (design/sh2-x64-performance.md, 2C item 1), with the inline bus and ctx.fetchedOpcodes:
//   - known refills (ir.hpp, Block) store their value, unless frame->codeDirty is set; the data
//     accesses before the last known refill set codeDirty exactly as RunBlock classifies them. A
//     block with known refills first checks that every code page still has the array pointer it
//     had at compile time; if not it returns out.stale and writes nothing else.
//   - the refills of a taken ExitIf and of EndDelaySlot are inline loads on array pages and call
//     the trampoline otherwise.
// SetupDelaySlot and EndDelaySlot are inline (SH2::SetupDelaySlot, SH2::AdvancePC<..., true> with
// cache emulation off) when ctx has delaySlot, fetchedOpcodes and intcPendingLevel; EndDelaySlot
// calls TrEndDelaySlot for a target with bit 1 set off array pages.
//
// Register cache (design/sh2-x64-performance.md, 2C item 4): R0-R15 and SR are kept in virtual
// registers inside the block (rules in x64_emitter.cpp, "Guest register cache"). Memory holds
// them, as RunBlock would have written them, whenever generated code calls a trampoline that runs
// a callback (TrDiv1/TrMacW/TrMacL, which read only SR, get SR), returns or chains, and at every
// CheckBoundary; between those points it may be behind. This relies on no callback writing
// R0-R15, and on SR being written only by setSR (and by Div1Step; the cache reloads SR after
// both). The read/write/refill callbacks reach MemRead/MemWrite, whose on-chip register and device
// side effects change only peripheral and interrupt state (interrupt recomputation reads SR.ILevel,
// so SR is stored before every call), except a watchdog reset (SH2::Reset writes R, SR and PC),
// which flushes the executor and so stops the block at that callback (TrRead/TrWrite/TrRefill)
// before any cached value is used or stored again.
bool EmitBlock(asmjit::x86::Compiler &cc, const Block &block, const ymir::sh2::SH2JitContext &ctx,
               const X64LinkSlot *links, bool &selfValidating);

// True if EmitBlock lowers every op of `block` (state offsets aside). Every valid op is lowered, so
// this only rejects blocks with out-of-range op values.
bool CanEmitBlock(const Block &block);

} // namespace brimir::jit
