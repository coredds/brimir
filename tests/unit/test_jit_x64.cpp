// Brimir - SH-2 JIT x64 native backend tests
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "jit_random_ir.hpp"
#include "jit_test_backend.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/core_wrapper.hpp>
#include <brimir/jit/backend.hpp>
#include <brimir/jit/executor.hpp>
#include <brimir/jit/interp_backend.hpp>
#include <brimir/jit/ir.hpp>

#include <memory>
#include <random>
#include <string>
#include <vector>

using brimir::jit::BackendKind;
using brimir::jit::Block;
using brimir::jit::Builder;
using brimir::jit::ExitInfo;
using brimir::jit::kNoCycleTarget;
using brimir::jit::NativeCode;
using brimir::jit::ValueId;
using sh2test::kSleep;
using sh2test::Rig;

namespace {

constexpr uint32_t kCode = 0x06001000;
constexpr uint16_t kAdd1_R3 = 0x7301;   // add #1,R3
constexpr uint16_t kMov_R3_R4 = 0x6433; // mov R3,R4
constexpr uint16_t kShll_R4 = 0x4400;   // shll R4

// CPU state the native code can see, applied identically to several rigs.
struct CpuSetup {
    ymir::savestate::SH2SaveState state;
    bool intrPending = false;
    uint64_t cycles = 0;

    void Apply(Rig &rig) const {
        rig.Load(state);
        auto &ctx = rig.sh2->GetJitContext();
        *ctx.intrPending = intrPending;
        *ctx.cyclesExecuted = cycles;
    }
};

CpuSetup RandomCpu(const Rig &rig, std::mt19937 &rng) {
    const auto word = [&] { return static_cast<uint32_t>(rng()); };
    const auto below = [&](uint32_t n) { return std::uniform_int_distribution<uint32_t>(0, n - 1)(rng); };
    CpuSetup s;
    s.state = rig.BaseState(kCode);
    for (auto &r : s.state.R) {
        r = word();
    }
    s.state.SR = word() & 0x3F3u;
    s.state.GBR = word();
    s.state.VBR = word();
    s.state.PR = word();
    s.state.MACH = word();
    s.state.MACL = word();
    s.state.delaySlotTarget = word();
    s.state.wbReg = static_cast<uint8_t>(below(5) == 0 ? 0xFF : below(17));
    s.state.intrAllow = below(2) != 0;
    s.intrPending = below(4) == 0;
    s.cycles = below(3) == 0 ? 0 : (static_cast<uint64_t>(word()) << below(24));
    return s;
}

// Compares everything a block can change: ExitInfo, CPU state (DiffRigs) and the flags DiffRigs
// does not cover. Stops the test case at the first difference.
void RequireSameOutcome(const ExitInfo &ir, const ExitInfo &x64, const Rig &irRig, const Rig &x64Rig) {
    REQUIRE(x64.cycles == ir.cycles);
    REQUIRE(x64.retired == ir.retired);
    REQUIRE(x64.busWait == ir.busWait);
    REQUIRE(x64.aborted == ir.aborted);
    REQUIRE(x64.boundary == ir.boundary);
    const std::string diff = sh2test::DiffRigs(irRig, x64Rig);
    INFO(diff);
    REQUIRE(diff.empty());
    auto &a = irRig.sh2->GetJitContext();
    auto &b = x64Rig.sh2->GetJitContext();
    REQUIRE(*b.cyclesExecuted == *a.cyclesExecuted);
    REQUIRE(*b.intrPending == *a.intrPending);
    REQUIRE(*b.intrAllow == *a.intrAllow);
    REQUIRE(*b.PC == *a.PC);
    REQUIRE(*b.wbReg == *a.wbReg);
}

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

// Front-end blocks start with a pipeline Refill, a call out of generated code, so this block still
// falls back to RunBlock until calls are lowered.
TEST_CASE("x64 backend: an executor runs blocks it cannot compile like the interpreter", "[jit][x64]") {
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

TEST_CASE("x64 matches the IR interpreter on random blocks", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    auto irRig = std::make_unique<Rig>();
    auto x64Rig = std::make_unique<Rig>();
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);

    for (uint32_t seed = 0; seed < 2000; ++seed) {
        std::mt19937 rng(seed);
        const Block block = sh2test::RandomBlock(rng, kCode);
        INFO("seed " << seed << "\n" << brimir::jit::PrintBlock(block));
        REQUIRE(brimir::jit::VerifyBlock(block).empty());

        const CpuSetup cpu = RandomCpu(*irRig, rng);
        cpu.Apply(*irRig);
        cpu.Apply(*x64Rig);
        uint64_t target = kNoCycleTarget;
        if (rng() % 3 != 0) {
            target = cpu.cycles + rng() % 41;
        }
        INFO("entry cycles " << cpu.cycles << " target " << target << " intrPending " << cpu.intrPending
                             << " intrAllow " << cpu.state.intrAllow);

        auto &x64Ctx = x64Rig->sh2->GetJitContext();
        NativeCode code;
        REQUIRE(backend->Compile(block, x64Ctx, code));
        REQUIRE(code.entry != nullptr);
        const ExitInfo ir = brimir::jit::RunBlock(block, irRig->sh2->GetJitContext(), target);
        const ExitInfo x64 = backend->Run(code, x64Ctx, target);
        RequireSameOutcome(ir, x64, *irRig, *x64Rig);

        if (seed % 256 == 255) {
            backend->Reset(); // keeps memory bounded and exercises compiling after a reset
        }
    }
}

TEST_CASE("x64 spills", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    // 300 values, all live until the final reduction into R0-R15: far more than the host registers.
    Block block;
    block.startPC = kCode;
    block.guestInstrCount = 1;
    Builder b(block);
    std::vector<ValueId> values;
    std::mt19937 rng(1234);
    for (uint32_t i = 0; i < 300; ++i) {
        if (i < 2 || i % 3 == 0) {
            values.push_back(b.Const(static_cast<uint32_t>(rng())));
        } else {
            values.push_back(b.Add(values[i - 1], values[i - 2]));
        }
    }
    for (uint32_t r = 0; r < 16; ++r) {
        ValueId acc = values[r];
        for (uint32_t i = r + 16; i < values.size(); i += 16) {
            acc = (i / 16) % 2 != 0 ? b.Add(acc, values[i]) : b.Xor(acc, values[i]);
        }
        b.SetReg(r, acc);
    }
    b.Exit(kCode + 2, 1);
    INFO(brimir::jit::PrintBlock(block));
    REQUIRE(brimir::jit::VerifyBlock(block).empty());

    auto irRig = std::make_unique<Rig>();
    auto x64Rig = std::make_unique<Rig>();
    irRig->Load(irRig->BaseState(kCode));
    x64Rig->Load(x64Rig->BaseState(kCode));
    const ExitInfo ir = brimir::jit::RunBlock(block, irRig->sh2->GetJitContext());
    const ExitInfo x64 = sh2test::RunOnBackend(BackendKind::X64, block, x64Rig->sh2->GetJitContext());
    RequireSameOutcome(ir, x64, *irRig, *x64Rig);
}

TEST_CASE("x64 boundary at every check", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    // A straight 32-instruction block: one check before every instruction after the first.
    Block block;
    block.startPC = kCode;
    block.guestInstrCount = 32;
    Builder b(block);
    for (uint32_t i = 0; i < 32; ++i) {
        if (i > 0) {
            b.CheckBoundary(kCode + 2 * i, static_cast<uint8_t>(i));
        }
        const uint32_t reg = i % 16;
        b.SetReg(reg, b.Add(b.GetReg(reg), b.Const(i + 1)));
        b.AddCycles(1 + i % 3);
    }
    b.Exit(kCode + 64, 32);
    INFO(brimir::jit::PrintBlock(block));
    REQUIRE(brimir::jit::VerifyBlock(block).empty());

    auto irRig = std::make_unique<Rig>();
    auto x64Rig = std::make_unique<Rig>();
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    NativeCode code;
    REQUIRE(backend->Compile(block, x64Rig->sh2->GetJitContext(), code));

    for (const bool interrupt : {false, true}) {
        for (uint64_t extra = 0; extra <= 40; ++extra) {
            CpuSetup cpu;
            cpu.state = irRig->BaseState(kCode);
            cpu.state.intrAllow = true;
            cpu.intrPending = interrupt;
            cpu.cycles = 1000;
            cpu.Apply(*irRig);
            cpu.Apply(*x64Rig);
            const uint64_t target = cpu.cycles + extra;
            INFO("interrupt " << interrupt << " target entry+" << extra);
            const ExitInfo ir = brimir::jit::RunBlock(block, irRig->sh2->GetJitContext(), target);
            const ExitInfo x64 = backend->Run(code, x64Rig->sh2->GetJitContext(), target);
            RequireSameOutcome(ir, x64, *irRig, *x64Rig);
            if (interrupt) {
                CHECK(x64.boundary);
                CHECK(x64.retired == 1);
            }
        }
    }
}

TEST_CASE("x64 code is freed by Reset", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    Block block;
    block.startPC = kCode;
    block.guestInstrCount = 1;
    Builder b(block);
    b.SetReg(1, b.Add(b.GetReg(1), b.Const(1)));
    b.AddCycles(1);
    b.Exit(kCode + 2, 1);

    auto rig = std::make_unique<Rig>();
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);
    NativeCode code;
    REQUIRE(backend->Compile(block, rig->sh2->GetJitContext(), code));
    CHECK(code.entry != nullptr);
    CHECK(backend->CodeBytes() > 0);
    backend->Reset();
    CHECK(backend->CodeBytes() == 0);
}
