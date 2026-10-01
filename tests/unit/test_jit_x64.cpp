// Brimir - SH-2 JIT x64 native backend tests
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "jit_test_backend.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/core_wrapper.hpp>
#include <brimir/jit/backend.hpp>
#include <brimir/jit/executor.hpp>

#include <memory>
#include <string>

using brimir::jit::BackendKind;
using sh2test::kSleep;
using sh2test::Rig;

namespace {

constexpr uint32_t kCode = 0x06001000;
constexpr uint16_t kAdd1_R3 = 0x7301;   // add #1,R3
constexpr uint16_t kMov_R3_R4 = 0x6433; // mov R3,R4
constexpr uint16_t kShll_R4 = 0x4400;   // shll R4

} // namespace

TEST_CASE("x64 backend: names, parsing and availability", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    CHECK(std::string(brimir::jit::BackendName(BackendKind::Ir)) == "ir");
    CHECK(std::string(brimir::jit::BackendName(BackendKind::X64)) == "x64");

    BackendKind kind = BackendKind::Ir;
    CHECK(brimir::jit::ParseBackend("x64", kind));
    CHECK(kind == BackendKind::X64);
    CHECK(brimir::jit::ParseBackend("ir", kind));
    CHECK(kind == BackendKind::Ir);
    kind = BackendKind::X64;
    CHECK_FALSE(brimir::jit::ParseBackend("arm64", kind));
    CHECK_FALSE(brimir::jit::ParseBackend("", kind));
    CHECK(kind == BackendKind::X64); // untouched on failure

    CHECK(brimir::jit::IsBackendAvailable(BackendKind::Ir));
    CHECK(brimir::jit::DefaultBackend() == BackendKind::X64);

    const auto available = sh2test::AvailableBackends();
    REQUIRE(available.size() == 2);
    CHECK(available[0] == BackendKind::Ir);
    CHECK(available[1] == BackendKind::X64);
}

TEST_CASE("x64 backend: factory", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    CHECK(brimir::jit::MakeNativeBackend(BackendKind::Ir) == nullptr);
    const auto x64 = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(x64 != nullptr);
    CHECK(x64->Kind() == BackendKind::X64);
    CHECK(x64->CodeBytes() == 0);
    x64->Reset();
    CHECK(x64->CodeBytes() == 0);
}

TEST_CASE("x64 backend: an executor on the stub runs blocks like the interpreter", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    auto ref = std::make_unique<Rig>();
    auto jit = std::make_unique<Rig>();
    brimir::jit::Executor exec{BackendKind::X64};
    CHECK(exec.Backend() == BackendKind::X64);

    const std::vector<uint16_t> program{kAdd1_R3, kMov_R3_R4, kShll_R4, static_cast<uint16_t>(kSleep)};
    ref->WriteCode(kCode, program);
    jit->WriteCode(kCode, program);
    auto state = ref->BaseState(kCode);
    state.R[3] = 0x40000000;
    state.R[4] = 0;
    ref->Load(state);
    jit->Load(state);

    const auto info = exec.Step(jit->sh2->GetJitContext());
    REQUIRE(info.retired == 3);
    uint64_t refCycles = 0;
    for (uint32_t i = 0; i < info.retired; ++i) {
        refCycles += ref->sh2->Step<false, false>();
    }
    CHECK(info.cycles == refCycles);
    const std::string diff = sh2test::DiffRigs(*ref, *jit);
    INFO(diff);
    CHECK(diff.empty());
    CHECK(jit->State().R[4] == 0x80000002u);

    const auto &stats = exec.GetStats();
    CHECK(stats.blocksRun == 1);
    CHECK(stats.compileFallbacks > 0);
    CHECK(stats.nativeBlocksRun == 0);
}

TEST_CASE("x64 backend: CoreWrapper backend selection recreates the executors", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    brimir::CoreWrapper core;
    CHECK(core.GetSH2JitBackend() == brimir::jit::DefaultBackend());
    core.SetSH2JitBackend(BackendKind::Ir);
    core.SetSH2JitEnabled(true);
    REQUIRE(core.Initialize());
    auto *saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    REQUIRE(core.GetSH2JitExecutor(true) != nullptr);
    CHECK(core.GetSH2JitExecutor(true)->Backend() == BackendKind::Ir);
    CHECK(core.GetSH2JitExecutor(false)->Backend() == BackendKind::Ir);

    core.SetSH2JitBackend(BackendKind::X64);
    CHECK(core.GetSH2JitBackend() == BackendKind::X64);
    REQUIRE(core.GetSH2JitExecutor(true) != nullptr);
    REQUIRE(core.GetSH2JitExecutor(false) != nullptr);
    CHECK(core.GetSH2JitExecutor(true)->Backend() == BackendKind::X64);
    CHECK(core.GetSH2JitExecutor(false)->Backend() == BackendKind::X64);
    CHECK(saturn->masterSH2.GetJitExecutor() == core.GetSH2JitExecutor(true));
    CHECK(saturn->slaveSH2.GetJitExecutor() == core.GetSH2JitExecutor(false));

    // With the JIT off, changing the backend drops the executors and leaves the CPUs detached.
    core.SetSH2JitEnabled(false);
    core.SetSH2JitBackend(BackendKind::Ir);
    CHECK(core.GetSH2JitExecutor(true) == nullptr);
    CHECK(saturn->masterSH2.GetJitExecutor() == nullptr);
    core.SetSH2JitEnabled(true);
    REQUIRE(core.GetSH2JitExecutor(true) != nullptr);
    CHECK(core.GetSH2JitExecutor(true)->Backend() == BackendKind::Ir);
}

TEST_CASE("x64 backend: an IR executor runs without a native backend", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    brimir::jit::Executor exec{BackendKind::Ir};
    CHECK(exec.Backend() == BackendKind::Ir);
    CHECK(exec.GetStats().compileFallbacks == 0);
}
