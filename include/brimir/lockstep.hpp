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

class CoreWrapper;

// Prepares a core for lockstep runs (call after Initialize, and again after LoadGame, which turns
// threaded VDP rendering back on): threaded VDP rendering is turned off
// so frames are produced on the emulation thread, and the RTC is switched from the host clock to
// the virtual (emulated-time) clock so both cores read the same date and time.
void PrepareLockstepCore(CoreWrapper &core);

// Compares the emulated state of two cores after the same frames, in this order: both SH-2s (CPU
// and on-chip peripherals, field by field), the slave SH-2 enable flag, low and high work RAM, the
// save state of every other subsystem (scheduler, system, SCU, SMPC, VDP, SCSP, the CD block -- HLE
// or SH-1/YGR/CD drive/DRAM when LLE -- and the spillover counters; reported per subsystem as
// "<subsystem> state differs (byte offset N)"), and the last output frame.
// Returns "" if identical, otherwise the first difference ("a" is the first core).
std::string CompareCores(CoreWrapper &a, CoreWrapper &b);

struct LockstepResult {
    int framesRun = 0;      // frames run on both cores (including the diverging one)
    std::string divergence; // "" if the cores stayed identical
};

// Runs both cores frame by frame; after every frame drains and compares both cores' audio samples
// ("audio sample count differs" / "audio differs at sample N"), then calls CompareCores. Stops at the
// first difference.
LockstepResult RunLockstep(CoreWrapper &a, CoreWrapper &b, int frames);

} // namespace brimir
