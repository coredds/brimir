#pragma once

// State comparison for SH-2 JIT validation (design/sh2-jit.md section 7):
// SH-2 state diffs (used by the isolated SH-2 test rig and by whole-system lockstep runs).

#include <ymir/savestate/savestate_sh2.hpp>

#include <string>

namespace brimir {

enum class SH2DiffScope {
    Cpu,               // CPU, pipeline and register-programmed on-chip state
    CpuAndPeripherals, // also timers (FRT, WDT), DMAC and the pending interrupt, which advance with time
};

// Returns "" if equal, otherwise "<label><field> differs: a=0x.. b=0x.." for the first difference.
std::string DiffSH2State(const ymir::savestate::SH2SaveState &a, const ymir::savestate::SH2SaveState &b,
                         SH2DiffScope scope, const char *label = "");

} // namespace brimir
