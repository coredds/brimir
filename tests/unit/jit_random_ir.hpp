#pragma once

// Random verified IR blocks for backend-vs-backend tests (RunBlock against a native backend).

#include <brimir/jit/ir.hpp>

#include <cstdint>
#include <random>

namespace sh2test {

struct RandomIrOptions {
    // Reserved for the ops that call out of generated code (calls) and access memory (memory).
    // Both default to false; with both false the generator uses exactly the ops of milestone 2B
    // Task 2: state, ALU, cycles, CheckBoundary, ExitIf without refill, Exit and ExitDynamic.
    // The ops behind these flags are added together with their native lowering.
    bool calls = false;
    bool memory = false;
};

// A block that passes VerifyBlock, with 20-200 ops over the allowed set. Values are drawn from a
// pool of every live value, often old ones, so many stay live at once (forcing register spills).
// CheckBoundary markers appear at increasing retired counts, with AddCycles/WbStall/SetWb between
// them; 0-3 ExitIf on random conditions; and a final Exit or ExitDynamic.
brimir::jit::Block RandomBlock(std::mt19937 &rng, uint32_t startPC, const RandomIrOptions &opt = {});

} // namespace sh2test
