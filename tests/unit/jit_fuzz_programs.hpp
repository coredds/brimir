#pragma once

// Brimir - random SH-2 programs for the JIT fuzz tests (test_jit_diff.cpp, test_jit_x64.cpp).
// Licensed under GPL-3.0

#include <ymir/savestate/savestate_sh2.hpp>

#include <cstdint>
#include <vector>

namespace sh2test {

constexpr uint32_t kFuzzCode = 0x06001000; // where a fuzz program is written and starts

struct FuzzProgram {
    std::vector<uint16_t> words; // the program at kFuzzCode, followed by 8 SLEEPs
    uint32_t busWaitEvery = 0;   // the rigs' Mmio::busWaitEvery
    // Start state: a fresh rig's BaseState(kFuzzCode) (the program written) with random registers
    // in the generator's register roles (jit_fuzz_programs.cpp).
    ymir::savestate::SH2SaveState state;
};

// Program number `seed` of the random-program fuzz test: straight-line groups of compiled
// non-branch opcodes, BT/BF, delayed branches (displacement and register forms) and RTS, with
// every access and branch inside known memory. The same seed always yields the same program.
FuzzProgram MakeFuzzProgram(uint32_t seed);

} // namespace sh2test
