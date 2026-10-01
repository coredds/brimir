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

#include <algorithm>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <utility>
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
// does not cover. Stops the test case at the first difference. comparePeripherals also compares
// timers, DMAC and interrupts (only meaningful when both rigs went through identical steps).
void RequireSameOutcome(const ExitInfo &ir, const ExitInfo &x64, const Rig &irRig, const Rig &x64Rig,
                        bool comparePeripherals = false) {
    REQUIRE(x64.cycles == ir.cycles);
    REQUIRE(x64.retired == ir.retired);
    REQUIRE(x64.busWait == ir.busWait);
    REQUIRE(x64.aborted == ir.aborted);
    REQUIRE(x64.boundary == ir.boundary);
    const std::string diff = sh2test::DiffRigs(irRig, x64Rig, comparePeripherals);
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

// The backend compiles every block the front end produces (no fallbacks), so the executor runs this
// one natively.
TEST_CASE("x64 backend: an executor runs native blocks like the interpreter", "[jit][x64]") {
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
    CHECK(stats.compileFallbacks == 0);
    CHECK(stats.nativeBlocksRun == 1);
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

namespace {

// Runs 2000 random blocks on RunBlock and on the x64 backend, from identical random CPU states,
// and requires identical outcomes.
void CompareRandomBlocks(const sh2test::RandomIrOptions &opt) {
    auto irRig = std::make_unique<Rig>();
    auto x64Rig = std::make_unique<Rig>();
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);

    for (uint32_t seed = 0; seed < 2000; ++seed) {
        std::mt19937 rng(seed);
        const Block block = sh2test::RandomBlock(rng, kCode, opt);
        INFO("seed " << seed << "\n" << brimir::jit::PrintBlock(block));
        REQUIRE(brimir::jit::VerifyBlock(block).empty());

        const CpuSetup cpu = RandomCpu(*irRig, rng);
        cpu.Apply(*irRig);
        cpu.Apply(*x64Rig);
        uint64_t target = kNoCycleTarget;
        switch (rng() % 4) {
        case 0: break;
        case 1: target = cpu.cycles - std::min<uint64_t>(cpu.cycles, 1 + rng() % 4); break; // below entry
        default: target = cpu.cycles + rng() % 41; break;
        }
        // Bus-wait answers depend on the query count, so both rigs restart it identically.
        const uint32_t busWaitEvery = opt.memory ? static_cast<uint32_t>(rng() % 3 == 0 ? 0 : 2 + rng() % 2) : 0;
        for (Rig *rig : {irRig.get(), x64Rig.get()}) {
            rig->mmio.busWaitEvery = busWaitEvery;
            rig->mmio.busWaitQueries = 0;
            rig->mmio.log.clear();
        }
        INFO("entry cycles " << cpu.cycles << " target " << target << " intrPending " << cpu.intrPending
                             << " intrAllow " << cpu.state.intrAllow << " busWaitEvery " << busWaitEvery);

        auto &x64Ctx = x64Rig->sh2->GetJitContext();
        NativeCode code;
        REQUIRE(backend->Compile(block, x64Ctx, code));
        REQUIRE(code.entry != nullptr);
        const ExitInfo ir = brimir::jit::RunBlock(block, irRig->sh2->GetJitContext(), target);
        const ExitInfo x64 = backend->Run(code, x64Ctx, target);
        // Both rigs ran exactly the same steps, so the peripherals (the FRT the generator touches)
        // must match too.
        RequireSameOutcome(ir, x64, *irRig, *x64Rig, true);
        REQUIRE(x64Rig->State().fetchedOpcodes == irRig->State().fetchedOpcodes);
        REQUIRE(*x64Ctx.delaySlot == *irRig->sh2->GetJitContext().delaySlot);

        if (seed % 256 == 255) {
            backend->Reset(); // keeps memory bounded and exercises compiling after a reset
        }
    }
}

} // namespace

TEST_CASE("x64 matches the IR interpreter on random blocks", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    CompareRandomBlocks({});
}

TEST_CASE("x64 matches IR on random blocks with calls and memory", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    sh2test::RandomIrOptions opt;
    opt.calls = true;
    opt.memory = true;
    CompareRandomBlocks(opt);
}

TEST_CASE("x64 matches IR on random blocks with calls only", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    sh2test::RandomIrOptions opt;
    opt.calls = true;
    CompareRandomBlocks(opt);
}

TEST_CASE("x64 matches IR on random blocks with memory only", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    sh2test::RandomIrOptions opt;
    opt.memory = true;
    CompareRandomBlocks(opt);
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
    // A straight 32-instruction block: one check before every instruction after the first. It
    // starts by clearing interrupt-allow; with allowAt = k > 0 it allows interrupts again right
    // before check k, so a pending interrupt can only stop the block there.
    const auto makeBlock = [](uint32_t allowAt) {
        Block block;
        block.startPC = kCode;
        block.guestInstrCount = 32;
        Builder b(block);
        b.ClearIntrAllow();
        for (uint32_t i = 0; i < 32; ++i) {
            if (i > 0) {
                if (i == allowAt) {
                    b.SetIntrAllow();
                }
                b.CheckBoundary(kCode + 2 * i, static_cast<uint8_t>(i));
            }
            const uint32_t reg = i % 16;
            b.SetReg(reg, b.Add(b.GetReg(reg), b.Const(i + 1)));
            b.AddCycles(1 + i % 3);
        }
        b.Exit(kCode + 64, 32);
        return block;
    };

    auto irRig = std::make_unique<Rig>();
    auto x64Rig = std::make_unique<Rig>();
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);

    // allowAt 0: interrupts stay disallowed, only the cycle target stops the block.
    for (uint32_t allowAt = 0; allowAt < 32; ++allowAt) {
        const Block block = makeBlock(allowAt);
        INFO(brimir::jit::PrintBlock(block));
        REQUIRE(brimir::jit::VerifyBlock(block).empty());
        NativeCode code;
        REQUIRE(backend->Compile(block, x64Rig->sh2->GetJitContext(), code));

        for (const bool interrupt : {false, true}) {
            for (uint64_t extra = 0; extra <= 41; ++extra) {
                CpuSetup cpu;
                cpu.state = irRig->BaseState(kCode);
                cpu.state.intrAllow = true;
                cpu.intrPending = interrupt;
                cpu.cycles = 1000;
                cpu.Apply(*irRig);
                cpu.Apply(*x64Rig);
                const uint64_t target = extra == 41 ? kNoCycleTarget : cpu.cycles + extra;
                INFO("allowAt " << allowAt << " interrupt " << interrupt << " target entry+" << extra);
                const ExitInfo ir = brimir::jit::RunBlock(block, irRig->sh2->GetJitContext(), target);
                const ExitInfo x64 = backend->Run(code, x64Rig->sh2->GetJitContext(), target);
                RequireSameOutcome(ir, x64, *irRig, *x64Rig);
                if (interrupt && allowAt > 0) {
                    CHECK(x64.boundary);
                    CHECK(x64.retired <= allowAt);
                    if (target == kNoCycleTarget) {
                        CHECK(x64.retired == allowAt);
                    }
                }
                if (target == kNoCycleTarget && (!interrupt || allowAt == 0)) {
                    CHECK_FALSE(x64.boundary);
                    CHECK(x64.retired == 32);
                }
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

TEST_CASE("x64 Compile fails cleanly on a context with a null state pointer", "[jit][x64]") {
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
    NativeCode good;
    REQUIRE(backend->Compile(block, rig->sh2->GetJitContext(), good));
    const size_t bytes = backend->CodeBytes();
    REQUIRE(bytes > 0);

    using Ctx = ymir::sh2::SH2JitContext;
    const std::vector<std::pair<const char *, void (*)(Ctx &)>> fields{
        {"R", [](Ctx &c) { c.R = nullptr; }},
        {"PC", [](Ctx &c) { c.PC = nullptr; }},
        {"PR", [](Ctx &c) { c.PR = nullptr; }},
        {"GBR", [](Ctx &c) { c.GBR = nullptr; }},
        {"VBR", [](Ctx &c) { c.VBR = nullptr; }},
        {"SR", [](Ctx &c) { c.SR = nullptr; }},
        {"MACL", [](Ctx &c) { c.MACL = nullptr; }},
        {"MACH", [](Ctx &c) { c.MACH = nullptr; }},
        {"delaySlotTarget", [](Ctx &c) { c.delaySlotTarget = nullptr; }},
        {"wbReg", [](Ctx &c) { c.wbReg = nullptr; }},
        {"intrPending", [](Ctx &c) { c.intrPending = nullptr; }},
        {"intrAllow", [](Ctx &c) { c.intrAllow = nullptr; }},
        {"cyclesExecuted", [](Ctx &c) { c.cyclesExecuted = nullptr; }},
    };
    for (const auto &[name, clear] : fields) {
        INFO("null " << name);
        Ctx ctx = rig->sh2->GetJitContext();
        clear(ctx);
        static const int sentinel = 0;
        NativeCode out;
        out.entry = &sentinel;
        CHECK_FALSE(backend->Compile(block, ctx, out));
        CHECK(out.entry == nullptr);
        CHECK(backend->CodeBytes() == bytes);
    }
}

TEST_CASE("x64 values live across calls", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint32_t kMmio = 0x22000100;
    Block block;
    block.startPC = kCode;
    block.guestInstrCount = 1;
    Builder b(block);
    std::mt19937 rng(77);
    std::vector<ValueId> values;
    for (uint32_t i = 0; i < 40; ++i) {
        const ValueId x = b.Add(b.GetReg(i % 16), b.Const(static_cast<uint32_t>(rng())));
        values.push_back(i % 3 == 0 ? b.Mul(x, b.Const(static_cast<uint32_t>(rng()) | 1u)) : b.Xor(x, b.Const(i)));
    }
    // Two calls with all 40 values live: a callback load from MMIO and SetSR.
    b.SyncCycles();
    const ValueId address = b.Const(kMmio);
    b.AddAccessCycles(address, 4, false);
    const ValueId loaded = b.Load(address, 4, false);
    b.SetSR(b.GetReg(5), false);
    for (uint32_t r = 0; r < 16; ++r) {
        ValueId acc = loaded;
        for (uint32_t i = r; i < values.size(); i += 16) {
            acc = b.Add(acc, values[i]);
        }
        b.SetReg(r, acc);
    }
    b.SetMACL(b.Xor(values[39], values[0]));
    b.AddCycles(1);
    b.Exit(kCode + 2, 1);
    INFO(brimir::jit::PrintBlock(block));
    REQUIRE(brimir::jit::VerifyBlock(block).empty());

    auto irRig = std::make_unique<Rig>();
    auto x64Rig = std::make_unique<Rig>();
    CpuSetup cpu = RandomCpu(*irRig, rng);
    cpu.intrPending = false;
    for (Rig *rig : {irRig.get(), x64Rig.get()}) {
        cpu.Apply(*rig);
        rig->mmio.data[kMmio & 0xFFFF] = 0xDE;
        rig->mmio.data[(kMmio & 0xFFFF) + 1] = 0xAD;
        rig->mmio.data[(kMmio & 0xFFFF) + 2] = 0xBE;
        rig->mmio.data[(kMmio & 0xFFFF) + 3] = 0xEF;
    }
    const ExitInfo ir = brimir::jit::RunBlock(block, irRig->sh2->GetJitContext());
    const ExitInfo x64 = sh2test::RunOnBackend(BackendKind::X64, block, x64Rig->sh2->GetJitContext());
    RequireSameOutcome(ir, x64, *irRig, *x64Rig);
    REQUIRE(x64Rig->mmio.log.size() == 1);
    CHECK(x64Rig->mmio.log[0].kind == 'R');
    CHECK(x64Rig->mmio.log[0].value == 0xDEADBEEFu);
}

namespace {

constexpr uint32_t kAbortMmio = 0x22000200;

// Hooked callbacks that request an abort when they touch the MMIO page, like a WDT access that
// resets the CPU in the middle of a block.
bool g_abort = false;
uint32 (*g_origRead)(void *, uint32, uint32, bool) = nullptr;
void (*g_origWrite)(void *, uint32, uint32, uint32) = nullptr;
void (*g_origRefill)(void *, uint32) = nullptr;

bool IsMmio(uint32 address) {
    return (address >> 24) == 0x22;
}

uint32 AbortingRead(void *sh2, uint32 address, uint32 size, bool instrFetch) {
    const uint32 value = g_origRead(sh2, address, size, instrFetch);
    g_abort = g_abort || IsMmio(address);
    return value;
}

void AbortingWrite(void *sh2, uint32 address, uint32 size, uint32 value) {
    g_origWrite(sh2, address, size, value);
    g_abort = g_abort || IsMmio(address);
}

void AbortingRefill(void *sh2, uint32 address) {
    g_origRefill(sh2, address);
    g_abort = g_abort || IsMmio(address);
}

uint32 ThrowingRead(void *sh2, uint32 address, uint32 size, bool instrFetch) {
    if (IsMmio(address)) {
        throw std::runtime_error("bus fault reading MMIO");
    }
    return g_origRead(sh2, address, size, instrFetch);
}

} // namespace

TEST_CASE("x64 aborts after each memory kind", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    enum class Kind { Load, Store, Refill, ExitIfRefill };
    for (const Kind kind : {Kind::Load, Kind::Store, Kind::Refill, Kind::ExitIfRefill}) {
        INFO("kind " << static_cast<int>(kind));
        Block block;
        block.startPC = kCode;
        block.guestInstrCount = 1;
        Builder b(block);
        b.SetReg(1, b.Const(0x1111));
        b.AddCycles(3);
        b.SyncCycles();
        const ValueId address = b.Const(kAbortMmio);
        switch (kind) {
        case Kind::Load: b.SetReg(2, b.Load(address, 4, false)); break;
        case Kind::Store: b.Store(address, 4, b.Const(0xA5A5A5A5)); break;
        case Kind::Refill: b.Refill(kAbortMmio); break;
        case Kind::ExitIfRefill: b.ExitIf(b.Const(1), kAbortMmio, 5, true, 1); break;
        }
        // Nothing from here on may run.
        b.SetReg(3, b.Const(0x3333));
        b.SetT(b.Const(1));
        b.Store(b.Const(0x06040000), 4, b.Const(0x55555555));
        b.SetSR(b.Const(0), false);
        b.AddCycles(7);
        b.Exit(kCode + 2, 1);
        INFO(brimir::jit::PrintBlock(block));
        REQUIRE(brimir::jit::VerifyBlock(block).empty());

        auto irRig = std::make_unique<Rig>();
        auto x64Rig = std::make_unique<Rig>();
        ExitInfo results[2];
        Rig *rigs[2] = {irRig.get(), x64Rig.get()};
        for (int i = 0; i < 2; ++i) {
            Rig &rig = *rigs[i];
            rig.Load(rig.BaseState(kCode));
            auto &ctx = rig.sh2->GetJitContext();
            g_origRead = ctx.read;
            g_origWrite = ctx.write;
            g_origRefill = ctx.refillPipeline;
            ctx.read = AbortingRead;
            ctx.write = AbortingWrite;
            ctx.refillPipeline = AbortingRefill;
            g_abort = false;
            results[i] = sh2test::RunOnBackend(i == 0 ? BackendKind::Ir : BackendKind::X64, block, ctx,
                                               kNoCycleTarget, &g_abort);
            ctx.read = g_origRead;
            ctx.write = g_origWrite;
            ctx.refillPipeline = g_origRefill;
            CHECK(g_abort);
        }
        const ExitInfo &x64 = results[1];
        RequireSameOutcome(results[0], x64, *irRig, *x64Rig);
        CHECK(x64.aborted);
        CHECK(x64.retired == 0);
        CHECK(x64.cycles == (kind == Kind::ExitIfRefill ? 8u : 3u));
        const auto st = x64Rig->State();
        CHECK(st.PC == kCode);
        CHECK(st.R[1] == 0x1111u);
        CHECK(st.R[3] != 0x3333u);
        CHECK(x64Rig->Read32(0x06040000) != 0x55555555u);
        CHECK(st.SR == irRig->BaseState(kCode).SR);
    }
}

TEST_CASE("x64 propagates callback exceptions", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    constexpr uint32_t kMmio = 0x22040000;

    SECTION("Run rethrows and leaves the same state as RunBlock") {
        Block block;
        block.startPC = kCode;
        block.guestInstrCount = 1;
        Builder b(block);
        b.SetReg(1, b.Const(0x1111));
        b.AddCycles(3);
        b.SyncCycles();
        b.SetReg(2, b.Load(b.Const(kMmio), 4, false));
        b.SetReg(3, b.Const(0x3333));
        b.AddCycles(1);
        b.Exit(kCode + 2, 1);
        REQUIRE(brimir::jit::VerifyBlock(block).empty());

        auto irRig = std::make_unique<Rig>();
        auto x64Rig = std::make_unique<Rig>();
        Rig *rigs[2] = {irRig.get(), x64Rig.get()};
        for (int i = 0; i < 2; ++i) {
            Rig &rig = *rigs[i];
            rig.Load(rig.BaseState(kCode));
            auto &ctx = rig.sh2->GetJitContext();
            g_origRead = ctx.read;
            ctx.read = ThrowingRead;
            bool caught = false;
            try {
                sh2test::RunOnBackend(i == 0 ? BackendKind::Ir : BackendKind::X64, block, ctx);
            } catch (const std::runtime_error &e) {
                caught = true;
                CHECK(typeid(e) == typeid(std::runtime_error));
                CHECK(std::string(e.what()) == "bus fault reading MMIO");
            }
            ctx.read = g_origRead;
            INFO("backend " << i);
            CHECK(caught);
        }
        const std::string diff = sh2test::DiffRigs(*irRig, *x64Rig);
        INFO(diff);
        CHECK(diff.empty());
        CHECK(x64Rig->State().R[1] == 0x1111u);
        CHECK(x64Rig->State().R[3] != 0x3333u);
        CHECK(x64Rig->State().PC == kCode);
    }

    SECTION("Executor::Step rethrows and flushes normally afterwards") {
        constexpr uint16_t kMovL_R1_R2 = 0x6212; // mov.l @R1,R2
        const std::vector<uint16_t> program{kMovL_R1_R2, kAdd1_R3, kAdd1_R3, static_cast<uint16_t>(kSleep)};
        auto irRig = std::make_unique<Rig>();
        auto x64Rig = std::make_unique<Rig>();
        brimir::jit::Executor irExec{BackendKind::Ir};
        brimir::jit::Executor x64Exec{BackendKind::X64};
        Rig *rigs[2] = {irRig.get(), x64Rig.get()};
        brimir::jit::Executor *execs[2] = {&irExec, &x64Exec};
        for (int i = 0; i < 2; ++i) {
            Rig &rig = *rigs[i];
            rig.WriteCode(kCode, program);
            auto state = rig.BaseState(kCode);
            state.R[1] = kMmio;
            state.R[2] = 0;
            state.R[3] = 0;
            rig.Load(state);
            auto &ctx = rig.sh2->GetJitContext();
            g_origRead = ctx.read;
            ctx.read = ThrowingRead;
            bool caught = false;
            try {
                execs[i]->Step(ctx);
            } catch (const std::runtime_error &e) {
                caught = true;
                CHECK(typeid(e) == typeid(std::runtime_error));
                CHECK(std::string(e.what()) == "bus fault reading MMIO");
            }
            ctx.read = g_origRead;
            INFO("backend " << i);
            CHECK(caught);
        }
        CHECK(x64Exec.GetStats().nativeBlocksRun == 1);
        CHECK(x64Exec.GetStats().compileFallbacks == 0);
        {
            const std::string diff = sh2test::DiffRigs(*irRig, *x64Rig);
            INFO(diff);
            CHECK(diff.empty());
        }

        // The block is no longer running: a flush applies at once.
        REQUIRE(x64Exec.Cache().Size() == 1);
        x64Exec.Flush();
        CHECK(x64Exec.Cache().Size() == 0);

        // The next step recompiles and runs the block natively.
        const auto next = x64Exec.Step(x64Rig->sh2->GetJitContext());
        CHECK_FALSE(next.aborted);
        CHECK(next.retired == 3);
        CHECK(x64Rig->State().R[3] == 2u);
        CHECK(x64Rig->State().PC == kCode + 6);
        CHECK(x64Exec.GetStats().nativeBlocksRun == 2);
    }
}

TEST_CASE("x64 inline memory matches IR", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    auto irRig = std::make_unique<Rig>();
    auto x64Rig = std::make_unique<Rig>();
    auto irRom = std::make_unique<sh2test::FastRom>();
    auto x64Rom = std::make_unique<sh2test::FastRom>();
    sh2test::MapFastPathTestPages(*irRig, *irRom);
    sh2test::MapFastPathTestPages(*x64Rig, *x64Rom);
    const sh2test::FastRom romBefore = *irRom;
    REQUIRE(*x64Rom == romBefore);
    for (Rig *rig : {irRig.get(), x64Rig.get()}) {
        for (uint32_t i = 0; i < 0x40; ++i) {
            rig->Write32(sh2test::kFastRamOffset - 0x80 + i * 4, 0x01020304u * (i + 1) ^ 0xA5C3E1F7u);
        }
        for (uint32_t i = 0; i < 0x100; ++i) {
            rig->mmio.data[(sh2test::kFastMmioOffset + i - 0x80) & 0xFFFF] = static_cast<uint8_t>(i * 13 + 7);
        }
    }
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64);
    REQUIRE(backend != nullptr);

    enum class Kind { Load, LoadFetch, Store, CyclesRead, CyclesWrite, BusWaitRead, BusWaitWrite, Count };
    uint32_t runs = 0;
    // A context without the page table compiles every access to its callback.
    for (const bool pageTable : {true, false}) {
        for (const uint32_t address : sh2test::FastPathTestAddresses()) {
            for (const uint8_t size : {uint8_t{1}, uint8_t{2}, uint8_t{4}}) {
                for (int k = 0; k < static_cast<int>(Kind::Count); ++k) {
                    const auto kind = static_cast<Kind>(k);
                    Block block;
                    block.startPC = kCode;
                    block.guestInstrCount = 1;
                    Builder b(block);
                    b.SetReg(1, b.Const(0x1111));
                    b.AddCycles(2);
                    b.SyncCycles();
                    // The address is computed at run time half of the time.
                    const ValueId addr =
                        (address & 1) != 0 ? b.Add(b.GetReg(5), b.Const(address)) : b.Const(address);
                    switch (kind) {
                    case Kind::Load: b.SetReg(2, b.Load(addr, size, false)); break;
                    case Kind::LoadFetch: b.SetReg(2, b.Load(addr, size, true)); break;
                    case Kind::Store: b.Store(addr, size, b.Const(0xA1B2C3D4u ^ address)); break;
                    case Kind::CyclesRead: b.AddAccessCycles(addr, size, false); break;
                    case Kind::CyclesWrite: b.AddAccessCycles(addr, size, true); break;
                    case Kind::BusWaitRead: b.ExitIfBusWait(addr, size, false, kCode, 0); break;
                    case Kind::BusWaitWrite: b.ExitIfBusWait(addr, size, true, kCode, 0); break;
                    case Kind::Count: break;
                    }
                    b.SetReg(3, b.Const(0x3333));
                    b.AddCycles(1);
                    b.Exit(kCode + 2, 1);
                    REQUIRE(brimir::jit::VerifyBlock(block).empty());

                    for (const uint32_t busWaitEvery : {0u, 1u}) {
                        INFO("page table " << pageTable << " address 0x" << std::hex << address << std::dec
                                           << " size " << int{size} << " kind " << k << " busWaitEvery "
                                           << busWaitEvery);
                        auto state = irRig->BaseState(kCode);
                        state.R[2] = 0xDEADBEEF;
                        state.R[5] = 0; // address = R5 + Const(address)
                        for (Rig *rig : {irRig.get(), x64Rig.get()}) {
                            rig->Load(state);
                            rig->mmio.busWaitEvery = busWaitEvery;
                            rig->mmio.busWaitQueries = 0;
                            rig->mmio.log.clear();
                        }
                        ymir::sh2::SH2JitContext x64Ctx = x64Rig->sh2->GetJitContext();
                        if (!pageTable) {
                            x64Ctx.bus.pages = nullptr;
                        }
                        NativeCode code;
                        REQUIRE(backend->Compile(block, x64Ctx, code));
                        const ExitInfo ir = brimir::jit::RunBlock(block, irRig->sh2->GetJitContext());
                        const ExitInfo x64 = backend->Run(code, x64Ctx, kNoCycleTarget);
                        RequireSameOutcome(ir, x64, *irRig, *x64Rig, true);
                        REQUIRE(*irRom == romBefore);
                        REQUIRE(*x64Rom == romBefore);
                        if (++runs % 512 == 0) {
                            backend->Reset();
                        }
                    }
                }
            }
        }
    }
    CHECK(runs == 2u * 128u * 3u * 7u * 2u);
}

namespace {

// Each throws an exception naming the callback.
uint64 ThrowAccessCycles(void *, uint32, uint32, bool) {
    throw std::runtime_error("accessCycles");
}
uint64 ThrowAccessCyclesRMWByte(void *, uint32) {
    throw std::runtime_error("accessCyclesRMWByte");
}
bool ThrowBusWait(void *, uint32, uint32, bool) {
    throw std::runtime_error("busWait");
}
void ThrowSetSR(void *, uint32, bool) {
    throw std::runtime_error("setSR");
}
void ThrowSetupDelaySlot(void *, uint32) {
    throw std::runtime_error("setupDelaySlot");
}
void ThrowEndDelaySlot(void *) {
    throw std::runtime_error("endDelaySlot");
}
uint32 ThrowRead(void *, uint32, uint32, bool) {
    throw std::runtime_error("read");
}
void ThrowWrite(void *, uint32, uint32, uint32) {
    throw std::runtime_error("write");
}
void ThrowRefill(void *, uint32) {
    throw std::runtime_error("refillPipeline");
}

} // namespace

TEST_CASE("x64 stops after an exception from every callback", "[jit][x64]") {
    if (!brimir::jit::IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend");
    }
    using Ctx = ymir::sh2::SH2JitContext;
    constexpr uint32_t kMmio = 0x22000200;
    struct Row {
        const char *name;
        void (*hook)(Ctx &);
        void (*emit)(Builder &);
    };
    // accessCycles is inline for every address while the context has the page table, so that row
    // runs on a context without it (the fallback that still calls the callback).
    const std::vector<Row> rows{
        {"accessCycles",
         [](Ctx &c) {
             c.accessCycles = ThrowAccessCycles;
             c.bus.pages = nullptr;
         },
         [](Builder &b) { b.AddAccessCycles(b.Const(kMmio), 4, false); }},
        {"accessCyclesRMWByte", [](Ctx &c) { c.accessCyclesRMWByte = ThrowAccessCyclesRMWByte; },
         [](Builder &b) { b.AddAccessCyclesRMWByte(b.Const(kMmio)); }},
        {"busWait", [](Ctx &c) { c.busWait = ThrowBusWait; },
         [](Builder &b) { b.ExitIfBusWait(b.Const(kMmio), 4, false, kCode, 0); }},
        {"setSR", [](Ctx &c) { c.setSR = ThrowSetSR; }, [](Builder &b) { b.SetSR(b.Const(0), false); }},
        {"setupDelaySlot", [](Ctx &c) { c.setupDelaySlot = ThrowSetupDelaySlot; },
         [](Builder &b) { b.SetupDelaySlot(b.Const(kCode + 0x100)); }},
        {"endDelaySlot", [](Ctx &c) { c.endDelaySlot = ThrowEndDelaySlot; },
         [](Builder &b) {
             b.SetupDelaySlot(b.Const(kCode + 0x100));
             b.EndDelaySlot();
         }},
        {"read", [](Ctx &c) { c.read = ThrowRead; }, [](Builder &b) { b.SetReg(2, b.Load(b.Const(kMmio), 4, false)); }},
        {"write", [](Ctx &c) { c.write = ThrowWrite; },
         [](Builder &b) { b.Store(b.Const(kMmio), 4, b.Const(0x55)); }},
        {"refillPipeline", [](Ctx &c) { c.refillPipeline = ThrowRefill; }, [](Builder &b) { b.Refill(kCode + 4); }},
    };

    for (const Row &row : rows) {
        INFO("callback " << row.name);
        Block block;
        block.startPC = kCode;
        block.guestInstrCount = 1;
        Builder b(block);
        b.SetReg(1, b.Const(0x1111));
        b.AddCycles(3);
        b.SyncCycles();
        row.emit(b);
        // Nothing from here on may run.
        b.SetReg(3, b.Const(0x3333));
        b.SetT(b.Const(1));
        b.Store(b.Const(0x06040000), 4, b.Const(0x55555555));
        b.AddCycles(1);
        b.Exit(kCode + 2, 1);
        INFO(brimir::jit::PrintBlock(block));
        REQUIRE(brimir::jit::VerifyBlock(block).empty());

        auto irRig = std::make_unique<Rig>();
        auto x64Rig = std::make_unique<Rig>();
        Rig *rigs[2] = {irRig.get(), x64Rig.get()};
        for (int i = 0; i < 2; ++i) {
            Rig &rig = *rigs[i];
            rig.Load(rig.BaseState(kCode));
            rig.mmio.busWaitEvery = 0;
            Ctx ctx = rig.sh2->GetJitContext(); // a copy: the hooks do not outlive this run
            row.hook(ctx);
            bool caught = false;
            try {
                sh2test::RunOnBackend(i == 0 ? BackendKind::Ir : BackendKind::X64, block, ctx);
            } catch (const std::runtime_error &e) {
                caught = true;
                CHECK(typeid(e) == typeid(std::runtime_error));
                CHECK(std::string(e.what()) == row.name);
            }
            INFO("backend " << i);
            CHECK(caught);
        }
        const std::string diff = sh2test::DiffRigs(*irRig, *x64Rig, true);
        INFO(diff);
        CHECK(diff.empty());
        const auto st = x64Rig->State();
        CHECK(st.R[1] == 0x1111u);
        CHECK(st.R[3] != 0x3333u);
        CHECK((st.SR & 1u) == (irRig->BaseState(kCode).SR & 1u));
        CHECK(x64Rig->Read32(0x06040000) != 0x55555555u);
        CHECK(*x64Rig->sh2->GetJitContext().cyclesExecuted == *irRig->sh2->GetJitContext().cyclesExecuted);
    }
}
