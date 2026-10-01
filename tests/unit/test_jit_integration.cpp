// Brimir - SH-2 JIT integration with CoreWrapper
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"

#include <brimir/core_wrapper.hpp>
#include <brimir/jit/executor.hpp>

#include <vector>

using brimir::CoreWrapper;

TEST_CASE("SH-2 JIT is off by default and toggles executors", "[jit][integration]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    REQUIRE_FALSE(core.IsSH2JitEnabled());
    REQUIRE(saturn->masterSH2.GetJitExecutor() == nullptr);

    core.SetSH2JitEnabled(true);
    REQUIRE(core.IsSH2JitEnabled());
    REQUIRE(saturn->masterSH2.GetJitExecutor() != nullptr);
    REQUIRE(saturn->slaveSH2.GetJitExecutor() != nullptr);
    REQUIRE(saturn->masterSH2.GetJitExecutor() != saturn->slaveSH2.GetJitExecutor());

    core.SetSH2JitEnabled(false);
    REQUIRE(saturn->masterSH2.GetJitExecutor() == nullptr);
    REQUIRE(saturn->slaveSH2.GetJitExecutor() == nullptr);
}

TEST_CASE("SH-2 JIT enabled before Initialize is applied", "[jit][integration]") {
    CoreWrapper core;
    core.SetSH2JitEnabled(true);
    REQUIRE(core.Initialize());
    REQUIRE(core.GetSaturn()->masterSH2.GetJitExecutor() != nullptr);
}

TEST_CASE("Frames run with the SH-2 JIT enabled", "[jit][integration]") {
    // Uses the built-in null IPL program (no BIOS loaded).
    CoreWrapper core;
    core.SetSH2JitEnabled(true);
    REQUIRE(core.Initialize());
    for (int i = 0; i < 30; ++i) {
        core.RunFrame();
    }
    REQUIRE(core.GetLastError().empty());
    const auto* master = core.GetSH2JitExecutor(true);
    REQUIRE(master != nullptr);
    REQUIRE(master->GetStats().blocksRun + master->GetStats().interpreted > 0);
}

TEST_CASE("Save state round trip with the SH-2 JIT flushes the block caches", "[jit][integration]") {
    CoreWrapper core;
    core.SetSH2JitEnabled(true);
    REQUIRE(core.Initialize());
    for (int i = 0; i < 10; ++i) {
        core.RunFrame();
    }
    std::vector<uint8_t> state(core.GetStateSize());
    REQUIRE(core.SaveState(state.data(), state.size()));
    REQUIRE(core.LoadState(state.data(), state.size()));
    REQUIRE(core.GetSH2JitExecutor(true)->Cache().Size() == 0);
    REQUIRE(core.GetSH2JitExecutor(false)->Cache().Size() == 0);
    core.RunFrame();
    REQUIRE(core.GetLastError().empty());
}
