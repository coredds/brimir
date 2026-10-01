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
// so frames are produced on the emulation thread, the RTC is switched from the host clock to
// the virtual (emulated-time) clock so both cores read the same date and time, and, if no game
// has loaded a backup RAM image yet, a formatted in-memory 32 KiB internal backup RAM is created
// (otherwise the area reads 0xFF and drops writes, and BIOS-only runs would not exercise it).
void PrepareLockstepCore(CoreWrapper &core);

// Compares the emulated state of two cores after the same frames, in this order: both SH-2s (CPU
// and on-chip peripherals, field by field), the slave SH-2 enable flag, low and high work RAM, the
// 32 KiB internal backup RAM, the cartridge type and, for a backup memory cartridge, its contents
// (read through the Saturn object, without touching CoreWrapper's SRAM sync state), the save state
// of every other subsystem (scheduler, system, SCU, SMPC, VDP, SCSP, the CD block -- HLE or
// SH-1/YGR/CD drive/DRAM when LLE -- and the spillover counters), and the last output frame.
// Returns "" if identical, otherwise the first difference ("a" is the first core) in one of two
// formats: "<field> differs: a=0x.. b=0x.." for named fields (SH-2 registers, "WRAMLow[0xNNNNN]",
// "internal backup RAM[0xNNNN]", "cartridge backup RAM[0xNNNN]", "cdblockLLE", the spillover and
// fractional cycle counters), or "<subsystem> state differs (byte offset N)" for the byte-compared
// subsystem save states. Frame and audio mismatches use their own messages ("frame row N differs").
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
