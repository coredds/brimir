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
// the virtual (emulated-time) clock so both cores read the same date and time.
void PrepareLockstepCore(CoreWrapper &core);

// Starts two lockstep cores from identical emulated state. Call once, after both cores are fully set
// up (Initialize, PrepareLockstepCore, content/BIOS loading, any test workload installation) and
// before the first lockstep frame. Saves src's full Saturn state (heap-allocated, zero-filled first
// so padding is deterministic) and loads it into dst and back into src (so both cores go through the
// same LoadState side effects; SH2::LoadState also flushes each core's SH-2 JIT). The internal and
// cartridge backup RAM images are outside the save state; bytes that differ are copied from src to
// dst when both images exist with the same size.
//
// Why: some upstream Ymir members are never initialized (see design/sh2-jit.md section 7.2), so two
// freshly constructed cores can hold different heap garbage in fields that CompareCores sees through
// the save state. This hides that garbage at the start. State that is uninitialized and NOT
// serialized is not reset by this; if it affects emulation, the cores diverge and the divergence is
// reported later through the serialized state it influences.
//
// Returns false if either core is not initialized or a LoadState fails.
bool SyncLockstepCores(CoreWrapper &src, CoreWrapper &dst);

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
