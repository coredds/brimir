#pragma once

// Random verified IR blocks for backend-vs-backend tests (RunBlock against a native backend).

#include <brimir/jit/ir.hpp>

#include <cstdint>
#include <random>

namespace sh2test {

struct RandomIrOptions {
    // With both false the generator uses exactly the ops of milestone 2B Task 2: state, ALU,
    // cycles, CheckBoundary, ExitIf without refill, Exit and ExitDynamic.
    //
    // calls: adds the ops that call helpers out of generated code, except memory accesses: Div1,
    // MacW, MacL, SetSR, Refill, AddAccessCyclesRMWByte, ExitIf with refill, and (sometimes) one
    // SetupDelaySlot ... EndDelaySlot pair right before a final ExitDynamic, as the front end
    // emits delayed branches.
    //
    // memory: adds Load, Store, AddAccessCycles and ExitIfBusWait on addresses in the test rig's
    // RAM (cached 0x06xxxxxx and cache-through 0x26xxxxxx), its MMIO page (0x22xxxxxx) and, rarely,
    // the FRT registers (0xFFFFFE10-0xFFFFFE1F) or misaligned. With calls, Refill and the RMW-cycle
    // lookups also use these addresses.
    bool calls = false;
    bool memory = false;
};

// A block that passes VerifyBlock, with 20-200 ops over the allowed set. Values are drawn from a
// pool of every live value, often old ones, so many stay live at once (forcing register spills).
// CheckBoundary markers appear at increasing retired counts, with AddCycles/WbStall/SetWb between
// them; 0-3 ExitIf on random conditions; and a final Exit or ExitDynamic.
brimir::jit::Block RandomBlock(std::mt19937 &rng, uint32_t startPC, const RandomIrOptions &opt = {});

} // namespace sh2test
