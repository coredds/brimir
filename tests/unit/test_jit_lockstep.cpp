// Brimir - whole-system lockstep: a JIT core and an interpreter core must stay identical
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"

#include <brimir/core_wrapper.hpp>
#include <brimir/jit/executor.hpp>
#include <brimir/lockstep.hpp>

#include <cstdint>
#include <memory>
#include <string>

using brimir::CoreWrapper;

namespace {

std::unique_ptr<CoreWrapper> MakeLockstepCore(bool jit) {
    auto core = std::make_unique<CoreWrapper>();
    core->SetSH2JitEnabled(jit);
    REQUIRE(core->Initialize());
    brimir::PrepareLockstepCore(*core);
    return core;
}

} // namespace

// Control: proves the emulator itself is deterministic, so any JIT lockstep failure is the JIT's.
TEST_CASE("Lockstep control: two interpreter cores stay identical", "[lockstep]") {
    // Uses the built-in null IPL program (no BIOS loaded).
    auto a = MakeLockstepCore(false);
    auto b = MakeLockstepCore(false);
    const auto result = brimir::RunLockstep(*a, *b, 120);
    INFO("frame " << result.framesRun << ": " << result.divergence);
    REQUIRE(result.divergence.empty());
    REQUIRE(result.framesRun == 120);
}

TEST_CASE("Lockstep: JIT core matches interpreter core", "[lockstep][jit]") {
    auto jit = MakeLockstepCore(true);
    auto ref = MakeLockstepCore(false);
    const auto result = brimir::RunLockstep(*jit, *ref, 300);
    INFO("frame " << result.framesRun << ": " << result.divergence);
    REQUIRE(result.divergence.empty());
    const auto &stats = jit->GetSH2JitExecutor(true)->GetStats();
    // The null IPL runs only a few instructions before SLEEP, so this is a smoke test; still
    // require that at least one compiled block ran.
    REQUIRE(stats.blocksRun > 0);
}

TEST_CASE("CompareCores detects a difference", "[lockstep]") {
    auto a = MakeLockstepCore(false);
    auto b = MakeLockstepCore(false);
    a->RunFrame();
    b->RunFrame();
    REQUIRE(brimir::CompareCores(*a, *b).empty());
    b->GetSaturn()->mem.WRAMHigh[0x123] ^= 0xFF;
    REQUIRE(brimir::CompareCores(*a, *b).find("WRAMHigh") != std::string::npos);
}

TEST_CASE("CompareCores detects differences outside the SH-2s and WRAM", "[lockstep]") {
    auto a = MakeLockstepCore(false);
    auto b = MakeLockstepCore(false);
    a->RunFrame();
    b->RunFrame();
    REQUIRE(brimir::CompareCores(*a, *b).empty());

    // VDP VRAM is not visible to the SH-2 diff or WRAM comparison. VDP::Probe has no VDP2 VRAM
    // writer, so this pokes one byte of VDP1 VRAM, which is equally VDP-only state.
    auto &probe = b->GetSaturn()->VDP.GetProbe();
    probe.VDP1WriteVRAM<uint8_t>(0x100, 0x5A);
    const std::string diff = brimir::CompareCores(*a, *b);
    INFO(diff);
    REQUIRE(diff.find("vdp state differs") != std::string::npos);
}
