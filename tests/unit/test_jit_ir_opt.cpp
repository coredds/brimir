// Brimir - SH-2 JIT IR timing-bookkeeping pass tests (ir_opt.hpp)
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "jit_random_ir.hpp"
#include "jit_test_backend.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/jit/backend.hpp>
#include <brimir/jit/interp_backend.hpp>
#include <brimir/jit/ir.hpp>
#include <brimir/jit/ir_opt.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

using brimir::jit::BackendKind;
using brimir::jit::Block;
using brimir::jit::Builder;
using brimir::jit::ExitInfo;
using brimir::jit::Inst;
using brimir::jit::kNoCycleTarget;
using brimir::jit::Op;
using brimir::jit::ValueId;
using sh2test::Rig;

namespace {

constexpr uint32_t kCode = 0x06001000;
constexpr uint32_t kMmioCode = 0x22000100; // code on a handler page (no array)

// One line per op: the op name, plus the fields the pass reads or writes.
std::string Describe(const Inst &in) {
    switch (in.op) {
    case Op::AddCycles: return "AddCycles " + std::to_string(in.imm);
    case Op::WbStall: return "WbStall " + std::to_string(in.imm);
    case Op::SetWb: return "SetWb " + std::to_string(in.imm);
    case Op::CheckBoundary:
        if (!in.flag) {
            return "CheckBoundary full";
        }
        return (in.imm2 & brimir::jit::kCheckNeedsInlineRefills) != 0 ? "CheckBoundary cyclesOnly inlineRefills"
                                                                       : "CheckBoundary cyclesOnly";
    default: return brimir::jit::OpName(in.op);
    }
}

std::vector<std::string> Describe(const Block &block) {
    std::vector<std::string> out;
    for (const Inst &in : block.code) {
        out.push_back(Describe(in));
    }
    return out;
}

// Builds a block with `body` and a final Exit, optimizes it and returns the optimized ops. The
// optimized block must still verify.
std::vector<std::string> Optimized(const std::function<void(Builder &)> &body, uint32_t guestInstrCount = 8,
                                   const std::function<void(Block &)> &setup = {}) {
    Block block;
    block.startPC = kCode;
    block.guestInstrCount = guestInstrCount;
    if (setup) {
        setup(block);
    }
    Builder b(block);
    body(b);
    b.Exit(kCode + 2 * guestInstrCount, static_cast<uint8_t>(guestInstrCount));
    INFO(brimir::jit::PrintBlock(block));
    REQUIRE(brimir::jit::VerifyBlock(block).empty());
    brimir::jit::OptimizeBlock(block);
    INFO(brimir::jit::PrintBlock(block));
    REQUIRE(brimir::jit::VerifyBlock(block).empty());
    return Describe(block);
}

using Ops = std::vector<std::string>;

} // namespace

// ---- Rule 1: write-back folding ----

TEST_CASE("IR opt: a known write-back value folds WbStall", "[jit][iropt]") {
    struct Case {
        uint8_t wb;
        uint32_t mask;
        bool stalls;
    };
    for (const Case c : {Case{3, 1u << 3, true}, Case{3, 1u << 4, false}, Case{0, 1, true}, Case{16, 1u << 16, true},
                         Case{0xFF, 0xFFFFFFFFu, false}, Case{17, 0xFFFFFFFFu, false}, Case{15, 0xFFFF7FFFu, false}}) {
        INFO("wb " << unsigned(c.wb) << " mask " << c.mask);
        const Ops ops = Optimized([&](Builder &b) {
            b.SetWb(c.wb);
            b.WbStall(c.mask);
        });
        if (c.stalls) {
            CHECK(ops == Ops{"SetWb " + std::to_string(c.wb), "AddCycles 1", "Exit"});
        } else {
            CHECK(ops == Ops{"SetWb " + std::to_string(c.wb), "Exit"});
        }
    }
}

TEST_CASE("IR opt: an unknown write-back value at entry keeps WbStall", "[jit][iropt]") {
    const Ops ops = Optimized([](Builder &b) {
        b.WbStall(0x8);
        b.SetWb(3);
        b.WbStall(0x8);
    });
    CHECK(ops == Ops{"WbStall 8", "SetWb 3", "AddCycles 1", "Exit"});
}

TEST_CASE("IR opt: the write-back value survives callbacks (none writes m_wbReg)", "[jit][iropt]") {
    const Ops ops = Optimized([](Builder &b) {
        b.SetWb(2);
        const ValueId address = b.Const(0x06000100);
        b.AddAccessCycles(address, 4, false);
        b.ExitIfBusWait(address, 4, false, kCode, 0);
        const ValueId value = b.Load(address, 4, false);
        b.Store(address, 4, value);
        b.SetSR(value, false);
        b.Refill(kCode);
        b.SetupDelaySlot(value);
        b.WbStall(1u << 2);
    });
    CHECK(ops == Ops{"SetWb 2", "Const", "AddAccessCycles", "ExitIfBusWait", "Load", "Store", "SetSR", "Refill",
                     "SetupDelaySlot", "AddCycles 1", "Exit"});
}

TEST_CASE("IR opt: the last SetWb decides", "[jit][iropt]") {
    const Ops ops = Optimized([](Builder &b) {
        b.SetWb(1);
        b.SetWb(0xFF);
        b.WbStall(0x2);
        b.SetWb(5);
        b.WbStall(0x20);
    });
    CHECK(ops == Ops{"SetWb 1", "SetWb 255", "SetWb 5", "AddCycles 1", "Exit"});
}

// ---- Rule 2: interrupt-test elision ----

TEST_CASE("IR opt: a check after a full check with no changing op tests only cycles", "[jit][iropt]") {
    // Every op between the checks: plain state, ALU, the pure helpers (Div1, MacW: no SH-2
    // callback), cycles, a sync, a write-back value and a not-taken ExitIf.
    const Ops ops = Optimized([](Builder &b) {
        b.CheckBoundary(kCode + 2, 1);
        const ValueId r1 = b.GetReg(1);
        const ValueId one = b.Const(1);
        b.SetReg(1, b.Add(r1, one));
        b.SetT(b.GetT());
        b.SetSRBits(b.GetSR(), 0x301);
        const ValueId r2 = b.GetReg(2);
        b.SetMACL(b.Mul(r2, r1));
        b.MacW(r1, r2);
        b.SetReg(6, b.Div1(r1, r2, false));
        b.AddCycles(1);
        b.SyncCycles();
        b.SetWb(3);
        b.ExitIf(b.GetT(), kCode + 0x40, 3, true, 2);
        b.CheckBoundary(kCode + 4, 2);
        b.CheckBoundary(kCode + 6, 3);
    });
    CHECK(ops == Ops{"CheckBoundary full",
                     "GetReg",
                     "Const",
                     "Add",
                     "SetReg",
                     "GetT",
                     "SetT",
                     "GetSR",
                     "SetSRBits",
                     "GetReg",
                     "Mul",
                     "SetMACL",
                     "MacW",
                     "Div1",
                     "SetReg",
                     "AddCycles 1",
                     "SyncCycles",
                     "SetWb 3",
                     "GetT",
                     "ExitIf",
                     "CheckBoundary cyclesOnly",
                     "CheckBoundary cyclesOnly",
                     "Exit"});
}

TEST_CASE("IR opt: the first check of a block is a full test", "[jit][iropt]") {
    const Ops ops = Optimized([](Builder &b) {
        b.SetReg(1, b.GetReg(2));
        b.CheckBoundary(kCode + 2, 1);
    });
    CHECK(ops == Ops{"GetReg", "SetReg", "CheckBoundary full", "Exit"});
}

TEST_CASE("IR opt: every op that can change the interrupt state keeps the next test", "[jit][iropt]") {
    struct Case {
        const char *name;
        std::function<void(Builder &)> op;
    };
    const std::vector<Case> cases{
        {"Load", [](Builder &b) { b.Load(b.Const(0x06000100), 4, false); }},
        {"Store", [](Builder &b) { b.Store(b.Const(0x06000100), 4, b.Const(1)); }},
        {"Refill", [](Builder &b) { b.Refill(kCode + 4); }},
        {"AddAccessCycles", [](Builder &b) { b.AddAccessCycles(b.Const(0x06000100), 4, false); }},
        {"AddAccessCyclesRMWByte", [](Builder &b) { b.AddAccessCyclesRMWByte(b.Const(0x06000100)); }},
        {"ExitIfBusWait", [](Builder &b) { b.ExitIfBusWait(b.Const(0x22000000), 4, false, kCode + 2, 1); }},
        {"SetSR", [](Builder &b) { b.SetSR(b.Const(0), false); }},
        {"EndDelaySlot", [](Builder &b) { b.EndDelaySlot(); }},
        {"SetIntrAllow", [](Builder &b) { b.SetIntrAllow(); }},
    };
    for (const Case &c : cases) {
        INFO(c.name);
        const Ops ops = Optimized([&](Builder &b) {
            b.CheckBoundary(kCode + 2, 1);
            c.op(b);
            b.CheckBoundary(kCode + 4, 2);
        });
        REQUIRE(ops.size() >= 3);
        CHECK(ops[0] == "CheckBoundary full");
        CHECK(ops[ops.size() - 2] == "CheckBoundary full");
    }
}

TEST_CASE("IR opt: ClearIntrAllow as the last changing op allows a cycles-only check", "[jit][iropt]") {
    // Before the first check of the block, and until the next changing op.
    CHECK(Optimized([](Builder &b) {
              b.ClearIntrAllow();
              b.CheckBoundary(kCode + 2, 1);
              b.CheckBoundary(kCode + 4, 2);
              b.SetIntrAllow();
              b.CheckBoundary(kCode + 6, 3);
          }) == Ops{"ClearIntrAllow", "CheckBoundary cyclesOnly", "CheckBoundary cyclesOnly", "SetIntrAllow",
                    "CheckBoundary full", "Exit"});
    // A changing op after it.
    CHECK(Optimized([](Builder &b) {
              b.ClearIntrAllow();
              b.Load(b.Const(0x06000100), 4, false);
              b.CheckBoundary(kCode + 2, 1);
          }) == Ops{"ClearIntrAllow", "Const", "Load", "CheckBoundary full", "Exit"});
    // A changing op before it.
    CHECK(Optimized([](Builder &b) {
              b.Load(b.Const(0x06000100), 4, false);
              b.ClearIntrAllow();
              b.CheckBoundary(kCode + 2, 1);
          }) == Ops{"Const", "Load", "ClearIntrAllow", "CheckBoundary cyclesOnly", "Exit"});
}

TEST_CASE("IR opt: SetupDelaySlot clears pending, so the slot's check tests only cycles", "[jit][iropt]") {
    CHECK(Optimized([](Builder &b) {
              const ValueId target = b.GetReg(3);
              b.SetupDelaySlot(target);
              b.WbStall(0x8);
              b.AddCycles(2);
              b.SetWb(0xFF);
              b.CheckBoundary(kCode + 2, 1);
              b.SetIntrAllow(); // allow only: pending stays false
              b.CheckBoundary(kCode + 4, 2);
              b.EndDelaySlot();
              b.CheckBoundary(kCode + 6, 3);
          }) == Ops{"GetReg", "SetupDelaySlot", "WbStall 8", "AddCycles 2", "SetWb 255", "CheckBoundary cyclesOnly",
                    "SetIntrAllow", "CheckBoundary cyclesOnly", "EndDelaySlot", "CheckBoundary full", "Exit"});
}

TEST_CASE("IR opt: a known refill keeps the elision only while it is inline", "[jit][iropt]") {
    const std::vector<uint16_t> code{0x0009, 0x0009, 0x0009, 0x0009, 0x0009, 0x0009, 0x0009, 0x0009};
    const uint32_t value = (0x0009u << 16) | 0x0009u;
    const auto withCode = [&](bool arrays) {
        return [&code, arrays](Block &block) {
            block.guestOpcodes = code;
            block.fetchFromArrays = arrays;
        };
    };
    // On array pages with no data access before it: the elision relies on the refill being
    // inline, which the backends check (or they test interrupts).
    CHECK(Optimized(
              [&](Builder &b) {
                  b.CheckBoundary(kCode + 2, 1);
                  b.SetReg(1, b.Const(1));
                  b.CheckBoundary(kCode + 4, 2);
                  b.KnownRefill(kCode + 4, value);
                  b.CheckBoundary(kCode + 6, 3);
                  b.CheckBoundary(kCode + 8, 4);
                  b.ClearIntrAllow();
                  b.CheckBoundary(kCode + 10, 5);
              },
              8, withCode(true)) == Ops{"CheckBoundary full", "Const", "SetReg", "CheckBoundary cyclesOnly", "Refill",
                                        "CheckBoundary cyclesOnly inlineRefills",
                                        "CheckBoundary cyclesOnly inlineRefills", "ClearIntrAllow",
                                        "CheckBoundary cyclesOnly", "Exit"});
    // Not fetchFromArrays: the refill always calls back.
    CHECK(Optimized(
              [&](Builder &b) {
                  b.CheckBoundary(kCode + 2, 1);
                  b.KnownRefill(kCode + 4, value);
                  b.CheckBoundary(kCode + 4, 2);
              },
              8, withCode(false)) == Ops{"CheckBoundary full", "Refill", "CheckBoundary full", "Exit"});
    // A data access earlier in the block may set codeDirty: the refill may call back.
    CHECK(Optimized(
              [&](Builder &b) {
                  b.Load(b.Const(0x06000100), 4, false);
                  b.CheckBoundary(kCode + 2, 1);
                  b.KnownRefill(kCode + 4, value);
                  b.CheckBoundary(kCode + 4, 2);
              },
              8, withCode(true)) == Ops{"Const", "Load", "CheckBoundary full", "Refill", "CheckBoundary full", "Exit"});
    // A full check after the refill does not rely on it.
    CHECK(Optimized(
              [&](Builder &b) {
                  b.CheckBoundary(kCode + 2, 1);
                  b.KnownRefill(kCode + 4, value);
                  b.SetIntrAllow();
                  b.CheckBoundary(kCode + 4, 2);
                  b.CheckBoundary(kCode + 6, 3);
              },
              8, withCode(true)) == Ops{"CheckBoundary full", "Refill", "SetIntrAllow", "CheckBoundary full",
                                        "CheckBoundary cyclesOnly", "Exit"});
}

// ---- Rule 3: cycle merging ----

TEST_CASE("IR opt: consecutive AddCycles merge", "[jit][iropt]") {
    CHECK(Optimized([](Builder &b) {
              b.AddCycles(1);
              b.SetReg(1, b.GetReg(2));
              b.WbStall(0x4); // unknown value: adds 0 or 1 cycle, observes nothing
              b.SetWb(0xFF);
              b.AddCycles(2);
              b.AddCycles(0);
          }) == Ops{"AddCycles 3", "GetReg", "SetReg", "WbStall 4", "SetWb 255", "Exit"});
    // The ALU tail after a known write-back value: stall + 1.
    CHECK(Optimized([](Builder &b) {
              b.SetWb(2);
              b.CheckBoundary(kCode + 2, 1);
              b.WbStall(0x4);
              b.AddCycles(1);
              b.SetWb(0xFF);
          }) == Ops{"SetWb 2", "CheckBoundary full", "AddCycles 2", "SetWb 255", "Exit"});
    CHECK(Optimized([](Builder &b) { b.AddCycles(0); }) == Ops{"Exit"});
}

TEST_CASE("IR opt: cycles never move across checks, syncs, exits or callbacks", "[jit][iropt]") {
    struct Case {
        const char *name;
        std::function<void(Builder &)> op;
    };
    const std::vector<Case> cases{
        {"CheckBoundary", [](Builder &b) { b.CheckBoundary(kCode + 2, 1); }},
        {"SyncCycles", [](Builder &b) { b.SyncCycles(); }},
        {"ExitIf", [](Builder &b) { b.ExitIf(b.GetT(), kCode + 0x40, 1, false, 1); }},
        {"ExitIfBusWait", [](Builder &b) { b.ExitIfBusWait(b.Const(0x22000000), 4, false, kCode, 0); }},
        {"Load", [](Builder &b) { b.Load(b.Const(0x06000100), 4, false); }},
        {"Store", [](Builder &b) { b.Store(b.Const(0x06000100), 4, b.Const(1)); }},
        {"Refill", [](Builder &b) { b.Refill(kCode + 4); }},
        {"AddAccessCycles", [](Builder &b) { b.AddAccessCycles(b.Const(0x06000100), 4, false); }},
        {"AddAccessCyclesRMWByte", [](Builder &b) { b.AddAccessCyclesRMWByte(b.Const(0x06000100)); }},
        {"SetSR", [](Builder &b) { b.SetSR(b.Const(0), false); }},
        {"SetupDelaySlot", [](Builder &b) { b.SetupDelaySlot(b.Const(kCode)); }},
        {"EndDelaySlot", [](Builder &b) { b.EndDelaySlot(); }},
        {"Div1", [](Builder &b) { b.SetReg(1, b.Div1(b.GetReg(1), b.GetReg(2), false)); }},
        {"MacW", [](Builder &b) { b.MacW(b.GetReg(1), b.GetReg(2)); }},
        {"MacL", [](Builder &b) { b.MacL(b.GetReg(1), b.GetReg(2)); }},
    };
    for (const Case &c : cases) {
        INFO(c.name);
        const Ops ops = Optimized([&](Builder &b) {
            b.AddCycles(1);
            c.op(b);
            b.AddCycles(2);
        });
        REQUIRE(ops.size() >= 3);
        CHECK(ops.front() == "AddCycles 1");
        CHECK(ops[ops.size() - 2] == "AddCycles 2");
    }
}

TEST_CASE("IR opt: merged cycles stay 32-bit", "[jit][iropt]") {
    CHECK(Optimized([](Builder &b) {
              b.AddCycles(0xFFFFFFF0u);
              b.AddCycles(0x10);
              b.AddCycles(0x0F);
          }) == Ops{"AddCycles 4294967280", "AddCycles 31", "Exit"});
    CHECK(Optimized([](Builder &b) {
              b.AddCycles(0xFFFFFFF0u);
              b.AddCycles(0x0F);
          }) == Ops{"AddCycles 4294967295", "Exit"});
}

// ---- Rule 4: SyncCycles ----

TEST_CASE("IR opt: a SyncCycles with no cycles since the last one is dropped", "[jit][iropt]") {
    // At entry *cyclesExecuted is already entryCycles + 0.
    CHECK(Optimized([](Builder &b) {
              b.SyncCycles();
              const ValueId address = b.Const(0x06000100);
              b.Load(address, 4, false);
              b.SyncCycles();
              b.ExitIf(b.GetT(), kCode + 0x40, 3, false, 1);
              b.CheckBoundary(kCode + 2, 1);
              b.SyncCycles();
          }) == Ops{"Const", "Load", "GetT", "ExitIf", "CheckBoundary full", "Exit"});
    struct Case {
        const char *name;
        std::function<void(Builder &)> op;
    };
    const std::vector<Case> cases{
        {"AddCycles", [](Builder &b) { b.AddCycles(1); }},
        {"AddAccessCycles", [](Builder &b) { b.AddAccessCycles(b.Const(0x06000100), 4, false); }},
        {"WbStall", [](Builder &b) { b.WbStall(0x1); }}, // unknown value
        {"AddAccessCyclesRMWByte", [](Builder &b) { b.AddAccessCyclesRMWByte(b.Const(0x06000100)); }},
    };
    for (const Case &c : cases) {
        INFO(c.name);
        const Ops ops = Optimized([&](Builder &b) {
            c.op(b);
            b.SyncCycles();
            c.op(b);
            b.SyncCycles();
        });
        int syncs = 0;
        for (const std::string &op : ops) {
            syncs += op == "SyncCycles" ? 1 : 0;
        }
        CHECK(syncs == 2);
    }
    // A WbStall folded away adds nothing.
    CHECK(Optimized([](Builder &b) {
              b.AddCycles(1);
              b.SyncCycles();
              b.SetWb(0xFF);
              b.WbStall(0x1);
              b.SyncCycles();
          }) == Ops{"AddCycles 1", "SyncCycles", "SetWb 255", "Exit"});
}

// ---- Backends ----

namespace {

// Set by the hooked refillPipeline below: the refill raises an interrupt.
bool *g_pendingOnRefill = nullptr;
void (*g_refill)(void *, uint32_t) = nullptr;

void RefillRaisingInterrupt(void *sh2, uint32_t address) {
    g_refill(sh2, address);
    *g_pendingOnRefill = true;
}

} // namespace

TEST_CASE("IR opt: a known refill that calls back keeps the interrupt test on every backend", "[jit][iropt]") {
    // check 1 (full) ; known refill ; check 2 (cycles only while the refill is inline). The hooked
    // refill callback raises an interrupt, so whenever it runs check 2 must stop the block.
    const auto makeBlock = [](uint32_t startPC) {
        Block block;
        block.startPC = startPC;
        block.guestInstrCount = 4;
        block.guestOpcodes = {0x7301, 0x7301, 0x7301, 0x7301};
        block.fetchFromArrays = true; // as compiled; the code may since have moved off the arrays
        Builder b(block);
        b.CheckBoundary(startPC + 2, 1);
        b.KnownRefill(startPC + 4, (0x7301u << 16) | 0x7301u);
        b.CheckBoundary(startPC + 4, 2);
        b.SetReg(3, b.Add(b.GetReg(3), b.Const(1)));
        b.AddCycles(1);
        b.Exit(startPC + 8, 4);
        return block;
    };
    for (const uint32_t startPC : {kCode, kMmioCode}) {
        const Block block = makeBlock(startPC);
        REQUIRE(brimir::jit::VerifyBlock(block).empty());
        Block optimized = block;
        brimir::jit::OptimizeBlock(optimized);
        INFO(brimir::jit::PrintBlock(optimized));
        REQUIRE(Describe(optimized)[2] == "CheckBoundary cyclesOnly inlineRefills");
        for (const BackendKind kind : sh2test::AvailableBackends()) {
            INFO("startPC " << startPC << " backend " << brimir::jit::BackendName(kind));
            auto refRig = std::make_unique<Rig>();
            auto optRig = std::make_unique<Rig>();
            ExitInfo results[2];
            Rig *rigs[2] = {refRig.get(), optRig.get()};
            for (int i = 0; i < 2; ++i) {
                Rig &rig = *rigs[i];
                rig.WriteCode(kCode, block.guestOpcodes);
                for (uint32_t w = 0; w < block.guestOpcodes.size(); ++w) {
                    rig.mmio.data[(kMmioCode & 0xFFFF) + 2 * w] = static_cast<uint8_t>(block.guestOpcodes[w] >> 8);
                    rig.mmio.data[(kMmioCode & 0xFFFF) + 2 * w + 1] = static_cast<uint8_t>(block.guestOpcodes[w]);
                }
                auto state = rig.BaseState(kCode);
                state.intrAllow = true;
                rig.Load(state);
                auto &ctx = rig.sh2->GetJitContext();
                *ctx.intrPending = false;
                g_refill = ctx.refillPipeline;
                g_pendingOnRefill = ctx.intrPending;
                ctx.refillPipeline = &RefillRaisingInterrupt;
                // The reference runs the original block on RunBlock.
                results[i] = i == 0 ? brimir::jit::RunBlock(block, ctx)
                                    : sh2test::RunOnBackend(kind, optimized, ctx);
                ctx.refillPipeline = g_refill;
            }
            CHECK(results[1].cycles == results[0].cycles);
            CHECK(results[1].retired == results[0].retired);
            CHECK(results[1].boundary == results[0].boundary);
            const std::string diff = sh2test::DiffRigs(*refRig, *optRig);
            INFO(diff);
            CHECK(diff.empty());
            CHECK(*optRig->sh2->GetJitContext().intrPending == *refRig->sh2->GetJitContext().intrPending);
            // On RAM the refill is inline (no callback, no interrupt); on MMIO it calls back.
            CHECK(results[0].boundary == (startPC == kMmioCode));
            CHECK(results[0].retired == (startPC == kMmioCode ? 2 : 4));
        }
    }
}

namespace {

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

void RequireSameOutcome(const ExitInfo &ref, const ExitInfo &opt, const Rig &refRig, const Rig &optRig) {
    REQUIRE(opt.cycles == ref.cycles);
    REQUIRE(opt.retired == ref.retired);
    REQUIRE(opt.busWait == ref.busWait);
    REQUIRE(opt.aborted == ref.aborted);
    REQUIRE(opt.boundary == ref.boundary);
    const std::string diff = sh2test::DiffRigs(refRig, optRig, true);
    INFO(diff);
    REQUIRE(diff.empty());
    auto &a = refRig.sh2->GetJitContext();
    auto &b = optRig.sh2->GetJitContext();
    REQUIRE(*b.cyclesExecuted == *a.cyclesExecuted);
    REQUIRE(*b.intrPending == *a.intrPending);
    REQUIRE(*b.intrAllow == *a.intrAllow);
    REQUIRE(*b.PC == *a.PC);
    REQUIRE(*b.wbReg == *a.wbReg);
    REQUIRE(*b.delaySlot == *a.delaySlot);
    REQUIRE(optRig.State().fetchedOpcodes == refRig.State().fetchedOpcodes);
}

// An external interrupt device on the MMIO page: every read, write and bus-wait query sets the IRL
// level from address bits 4..1, which raises or lowers intrPending against SR.ILevel (as SCU
// interrupts reach the SH-2 through bus handlers). So any memory op may change the interrupt state.
void SetIrlFromAddress(Rig &rig, uint32_t address) {
    const auto level = static_cast<uint8_t>((address >> 1) & 0xFu);
    rig.sh2->CbExtIntr(level, static_cast<uint8_t>(0x40u + level));
}

void IrlOnWrite(Rig &rig, uint32_t address) {
    SetIrlFromAddress(rig, address);
}

void IrlOnAccess(Rig &rig, char, uint32_t address) {
    SetIrlFromAddress(rig, address);
}

} // namespace

// The pass must not change what a block does: RunBlock(original) against the optimized block on
// RunBlock and on the native backend, from identical random states. Half of the runs have the IRL
// device on the MMIO page.
TEST_CASE("IR opt: optimized random blocks run like the originals", "[jit][iropt]") {
    const auto backend = brimir::jit::MakeNativeBackend(BackendKind::X64); // null without x64
    auto refRig = std::make_unique<Rig>();
    auto irRig = std::make_unique<Rig>();
    auto x64Rig = std::make_unique<Rig>();
    uint64_t opsBefore = 0;
    uint64_t opsAfter = 0;
    uint64_t cyclesOnly = 0;
    uint64_t foldedStalls = 0;

    for (int variant = 0; variant < 4; ++variant) {
        sh2test::RandomIrOptions opt;
        opt.calls = (variant & 1) != 0;
        opt.memory = (variant & 2) != 0;
        for (uint32_t seed = 0; seed < 2000; ++seed) {
            std::mt19937 rng(seed * 4 + static_cast<uint32_t>(variant));
            const Block block = sh2test::RandomBlock(rng, kCode, opt);
            Block optimized = block;
            brimir::jit::OptimizeBlock(optimized);
            INFO("variant " << variant << " seed " << seed << "\noriginal:\n"
                            << brimir::jit::PrintBlock(block) << "optimized:\n"
                            << brimir::jit::PrintBlock(optimized));
            REQUIRE(brimir::jit::VerifyBlock(optimized).empty());
            REQUIRE(optimized.guestOpcodes == block.guestOpcodes);
            REQUIRE(optimized.guestInstrCount == block.guestInstrCount);
            opsBefore += block.code.size();
            opsAfter += optimized.code.size();
            uint64_t stallsBefore = 0;
            uint64_t stallsAfter = 0;
            for (const Inst &in : block.code) {
                stallsBefore += in.op == Op::WbStall ? 1 : 0;
            }
            for (const Inst &in : optimized.code) {
                stallsAfter += in.op == Op::WbStall ? 1 : 0;
                cyclesOnly += in.op == Op::CheckBoundary && in.flag ? 1 : 0;
            }
            foldedStalls += stallsBefore - stallsAfter;

            const CpuSetup cpu = RandomCpu(*refRig, rng);
            uint64_t target = kNoCycleTarget;
            switch (rng() % 4) {
            case 0: break;
            case 1: target = cpu.cycles - std::min<uint64_t>(cpu.cycles, 1 + rng() % 4); break;
            default: target = cpu.cycles + rng() % 41; break;
            }
            const uint32_t busWaitEvery = opt.memory ? static_cast<uint32_t>(rng() % 3 == 0 ? 0 : 2 + rng() % 2) : 0;
            const bool irl = (seed & 1) != 0;
            for (Rig *rig : {refRig.get(), irRig.get(), x64Rig.get()}) {
                rig->sh2->CbExtIntr(0, 0); // no IRL from the previous block
                rig->WriteCode(kCode, block.guestOpcodes);
                cpu.Apply(*rig);
                rig->mmio.busWaitEvery = busWaitEvery;
                rig->mmio.busWaitQueries = 0;
                rig->mmio.log.clear();
                rig->mmio.onWrite = irl ? &IrlOnWrite : nullptr;
                rig->mmio.onAccess = irl ? &IrlOnAccess : nullptr;
            }
            INFO("entry cycles " << cpu.cycles << " target " << target << " intrPending " << cpu.intrPending
                                 << " intrAllow " << cpu.state.intrAllow << " wbReg " << unsigned(cpu.state.wbReg));

            const ExitInfo ref = brimir::jit::RunBlock(block, refRig->sh2->GetJitContext(), target);
            const ExitInfo ir = brimir::jit::RunBlock(optimized, irRig->sh2->GetJitContext(), target);
            RequireSameOutcome(ref, ir, *refRig, *irRig);
            if (backend != nullptr) {
                INFO("x64");
                brimir::jit::NativeCode code;
                REQUIRE(backend->Compile(optimized, x64Rig->sh2->GetJitContext(), code));
                const ExitInfo x64 = backend->Run(code, x64Rig->sh2->GetJitContext(), target);
                RequireSameOutcome(ref, x64, *refRig, *x64Rig);
                if (seed % 256 == 255) {
                    backend->Reset();
                }
            }
        }
    }
    INFO("ops " << opsBefore << " -> " << opsAfter << ", cycles-only checks " << cyclesOnly << ", folded stalls "
                << foldedStalls);
    CHECK(opsAfter < opsBefore);
    CHECK(cyclesOnly > 0);
    CHECK(foldedStalls > 0);
}
