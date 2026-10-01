#pragma once

// Brimir: interface between the forked SH-2 and the SH-2 JIT.
// See src/core/BRIMIR_FORK.md and design/sh2-jit.md.
//
// The SH-2 fills in SH2JitContext with pointers to its live state and with
// callbacks that reuse the interpreter's own helpers with cache emulation
// disabled, so compiled code has exactly the interpreter's memory, timing and
// delay-slot behavior. The JIT library implements ISH2Executor.

#include <ymir/core/types.hpp>

namespace ymir::sh2 {

struct SH2JitContext {
    // Live CPU state of the owning SH2
    uint32 *R = nullptr; // R0..R15
    uint32 *PC = nullptr;
    uint32 *PR = nullptr;
    uint32 *GBR = nullptr;
    uint32 *VBR = nullptr;
    uint32 *SR = nullptr; // RegSR::u32, T is bit 0
    uint32 *delaySlotTarget = nullptr;
    bool *delaySlot = nullptr;
    uint8 *wbReg = nullptr; // 0x0..0xF: R0..R15, 0x10: PR, 0xFF: none
    bool *intrPending = nullptr;
    bool *intrAllow = nullptr;
    uint32 *fetchedOpcodes = nullptr; // 32-bit instruction fetch buffer

    // Cycles executed so far in the current Advance() call. On-chip timers (WDT, FRT) read it to
    // sync, so the executor must keep it current before every interpreter call and memory access.
    uint64 *cyclesExecuted = nullptr;

    void *sh2 = nullptr; // opaque owner, passed to every callback

    // Executes exactly one instruction (or interrupt entry) with the interpreter; returns cycles.
    uint64 (*interpretOne)(void *sh2) = nullptr;
    // SH2::MemRead / MemWrite with cache emulation off. size is 1, 2 or 4; reads are zero-extended.
    uint32 (*read)(void *sh2, uint32 address, uint32 size, bool instrFetch) = nullptr;
    void (*write)(void *sh2, uint32 address, uint32 size, uint32 value) = nullptr;
    // Side-effect-free instruction read, for decoding and block validation.
    uint16 (*peekInstruction)(void *sh2, uint32 address) = nullptr;
    // SH2::AccessCycles with cache emulation off.
    uint64 (*accessCycles)(void *sh2, uint32 address, uint32 size, bool write) = nullptr;
    // SH2Bus::IsBusWait.
    bool (*busWait)(void *sh2, uint32 address, uint32 size, bool write) = nullptr;
    // Loads the 32-bit instruction fetch buffer from address (SH2::RefillPipeline).
    void (*refillPipeline)(void *sh2, uint32 address) = nullptr;
    // SH2::SetupDelaySlot.
    void (*setupDelaySlot)(void *sh2, uint32 target) = nullptr;
    // SH2::AdvancePC for an instruction executed in a delay slot.
    void (*endDelaySlot)(void *sh2) = nullptr;
};

class ISH2Executor {
public:
    virtual ~ISH2Executor() = default;

    // Runs code until executed >= target and returns the new executed count.
    virtual uint64 Run(SH2JitContext &ctx, uint64 executed, uint64 target) = 0;

    // Drops all compiled code (reset, save-state load, executor attach).
    virtual void Flush() = 0;
};

} // namespace ymir::sh2
