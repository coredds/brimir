#pragma once

// Brimir SH-2 JIT: IR timing-bookkeeping pass (design/sh2-x64-performance.md, 2C item 3).

#include <brimir/jit/ir.hpp>

namespace brimir::jit {

// Folds per-instruction timing bookkeeping in a block, without changing what it does on either
// backend (RunBlock is the reference). The block cache runs it on every block after BuildBlock;
// guestOpcodes, guestInstrCount and the other block fields are unchanged. Rules:
//
// 1. Write-back folding. The m_wbReg value is tracked through the block: unknown at entry, set by
//    SetWb. A WbStall(mask) with a known value becomes AddCycles(1) if the value's bit is in mask
//    (a value <= 16), else it is removed. Every SetWb is kept (exits observe the value).
//    No op other than SetWb can write m_wbReg while the block keeps running (audit in ir_opt.cpp).
//
// 2. Interrupt-test elision. A CheckBoundary becomes cycles-only (flag set: only the budget is
//    tested) when (*intrPending && *intrAllow) is known to be false there:
//      - an earlier CheckBoundary in the block passed, or
//      - the last op that changed the interrupt state was ClearIntrAllow (allow = false) or
//        SetupDelaySlot (pending = false; SetIntrAllow after it keeps that),
//    and no op between can change *intrPending or *intrAllow. Changing ops: Load, Store, Refill,
//    AddAccessCycles, AddAccessCyclesRMWByte, ExitIfBusWait, SetSR, SetupDelaySlot, EndDelaySlot,
//    ClearIntrAllow and SetIntrAllow (every op that can run a callback with SH-2 side effects).
//    The one exception is a known Refill that both backends store inline: in a fetchFromArrays
//    block, before any Load or Store (so codeDirty is still clear), it only calls back when the
//    code is no longer on array pages. A check whose elision relies on such refills gets imm2 bit
//    kCheckNeedsInlineRefills; the backends then test interrupts too unless their known refills
//    are inline in this run (RunBlock: the code is on its array pages at entry; x64: the block has
//    the known-refill entry check).
//
// 3. Cycle merging. Consecutive AddCycles merge into the first while their sum fits 32 bits, with
//    no CheckBoundary, SyncCycles, exit or callback-capable op between; AddCycles(0) is removed.
//
// 4. SyncCycles. A SyncCycles is removed when no AddCycles, AddAccessCycles, WbStall or
//    AddAccessCyclesRMWByte ran since the previous one or since block entry (where *cyclesExecuted
//    already equals the entry count).
//
// 5. Guarded runs (design/sh2-jit-m2d.md, B1). A guarded run is a maximal sequence of at least 2
//    cycles-only CheckBoundary ops (rule 2) with only these ops between the first and the last:
//    ops that cannot change the interrupt state (not in rule 2's changing set: ALU and register
//    ops, SetSRBits, Div1, MacW, MacL, AddCycles, WbStall, SetWb, ...) and the known refills rule 2
//    treats as inline; no exit (ExitIf included), no SyncCycles. A GuardRun(maxCycles, checks,
//    needsInlineRefills) goes right before the first check: maxCycles is the sum of the AddCycles
//    immediates plus 1 per WbStall from the first check to the last, checks the run length, and
//    needsInlineRefills is set if any covered check has kCheckNeedsInlineRefills. A run closes
//    early (and the next one starts at the check that did not fit) at 255 checks or when
//    maxCycles would exceed 32 bits.
//    Exact: the covered checks test only the cycle budget (where their interrupt test relies on
//    inline refills, the guard applies only when the backend runs them inline, as the checks do);
//    the cycle count only grows inside the run, by at most maxCycles before the last check (no op
//    there adds cycles but AddCycles and WbStall; a refill callback adds none), and the target is
//    fixed while a block runs. So if cycles + maxCycles < target at the first check, every
//    covered check would pass, and skipping them changes nothing.
//    This extends the plan's rule (checks without kCheckNeedsInlineRefills, no refill inside):
//    the front end puts a known refill before every 4-byte aligned instruction, so in real code
//    every check after the first such refill relies on it, and runs without refills hardly exist.
void OptimizeBlock(Block &block);

} // namespace brimir::jit
