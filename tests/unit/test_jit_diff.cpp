// Brimir - SH-2 JIT vs interpreter differential tests (per instruction)
// Licensed under GPL-3.0
//
// Every test runs the same code on two identical isolated SH-2s: one through
// the JIT executor, one through the interpreter, and requires identical state,
// memory, bus-wait query sequence and cycle totals.

#include "catch_amalgamated.hpp"
#include "jit_opcode_specs.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/jit/executor.hpp>

#include <array>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using sh2test::kSleep;
using sh2test::Rig;

namespace {

constexpr uint32_t kCode = 0x06001000;
constexpr uint32_t kTarget = kCode + 0x100; // branch target area, filled with SLEEP
constexpr uint32_t kMmio = 0x22000000;

// Encoders
uint16_t Nm(uint16_t base, uint32_t n, uint32_t m) {
    return static_cast<uint16_t>(base | (n << 8) | (m << 4));
}
uint16_t NImm(uint16_t base, uint32_t n, uint32_t imm) {
    return static_cast<uint16_t>(base | (n << 8) | (imm & 0xFF));
}
constexpr uint16_t kNop = 0x0009;
constexpr uint16_t kRts = 0x000B;
uint16_t MovR(uint32_t n, uint32_t m) { return Nm(0x6003, n, m); }
uint16_t MovI(uint32_t n, uint32_t imm) { return NImm(0xE000, n, imm); }
uint16_t MovBL(uint32_t n, uint32_t m) { return Nm(0x6000, n, m); }
uint16_t MovLL(uint32_t n, uint32_t m) { return Nm(0x6002, n, m); }
uint16_t MovBS(uint32_t n, uint32_t m) { return Nm(0x2000, n, m); }
uint16_t MovLS(uint32_t n, uint32_t m) { return Nm(0x2002, n, m); }
uint16_t MovLI(uint32_t n, uint32_t disp) { return NImm(0xD000, n, disp); }
uint16_t Add(uint32_t n, uint32_t m) { return Nm(0x300C, n, m); }
uint16_t AddI(uint32_t n, uint32_t imm) { return NImm(0x7000, n, imm); }
uint16_t CmpEq(uint32_t n, uint32_t m) { return Nm(0x3000, n, m); }
uint16_t Dt(uint32_t n) { return static_cast<uint16_t>(0x4010 | (n << 8)); }
uint16_t Jmp(uint32_t m) { return static_cast<uint16_t>(0x402B | (m << 8)); }
uint16_t Bt(uint32_t d) { return static_cast<uint16_t>(0x8900 | (d & 0xFF)); }
uint16_t Bf(uint32_t d) { return static_cast<uint16_t>(0x8B00 | (d & 0xFF)); }
uint16_t Bts(uint32_t d) { return static_cast<uint16_t>(0x8D00 | (d & 0xFF)); }
uint16_t Bfs(uint32_t d) { return static_cast<uint16_t>(0x8F00 | (d & 0xFF)); }
uint16_t Bra(uint32_t d) { return static_cast<uint16_t>(0xA000 | (d & 0xFFF)); }
uint16_t Bsr(uint32_t d) { return static_cast<uint16_t>(0xB000 | (d & 0xFFF)); }
uint16_t Braf(uint32_t m) { return static_cast<uint16_t>(0x0023 | (m << 8)); }
uint16_t Bsrf(uint32_t m) { return static_cast<uint16_t>(0x0003 | (m << 8)); }
uint16_t Jsr(uint32_t m) { return static_cast<uint16_t>(0x400B | (m << 8)); }

std::string Hex(const std::vector<uint16_t> &words) {
    std::string out;
    char buf[8];
    for (uint16_t w : words) {
        std::snprintf(buf, sizeof(buf), "%04X ", w);
        out += buf;
    }
    return out;
}

struct Pair {
    std::unique_ptr<Rig> ref = std::make_unique<Rig>();
    std::unique_ptr<Rig> jit = std::make_unique<Rig>();
    brimir::jit::Executor exec;
    bool lastStepMatched = true; // whether the most recent Step() found identical cycles and state

    void WriteCode(uint32_t address, const std::vector<uint16_t> &words) {
        ref->WriteCode(address, words);
        jit->WriteCode(address, words);
    }
    void Write32(uint32_t address, uint32_t value) {
        ref->Write32(address, value);
        jit->Write32(address, value);
    }
    void SetBusWaitEvery(uint32_t every) {
        ref->mmio.busWaitEvery = every;
        jit->mmio.busWaitEvery = every;
    }
    void Load(const ymir::savestate::SH2SaveState &state) {
        ref->Load(state);
        jit->Load(state);
    }

    // One executor step on the JIT rig and the equivalent interpreter steps on the reference rig.
    brimir::jit::ExitInfo Step() {
        const auto info = exec.Step(jit->sh2->GetJitContext());
        uint64_t refCycles = 0;
        for (uint32_t i = 0; i < info.retired; ++i) {
            refCycles += ref->sh2->Step<false, false>();
        }
        if (info.busWait) {
            refCycles += ref->sh2->Step<false, false>();
        }
        CHECK(info.cycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*ref, *jit);
        INFO(diff);
        CHECK(diff.empty());
        lastStepMatched = info.cycles == refCycles && diff.empty();
        return info;
    }
};

// Random register file; memory-op base registers are pointed at valid data afterwards.
std::array<uint32_t, 16> RandomRegs(std::mt19937 &rng) {
    std::array<uint32_t, 16> regs{};
    for (auto &r : regs) {
        r = rng();
    }
    return regs;
}

// A data address in cache-through RAM, cached RAM, or MMIO, aligned to `align`.
uint32_t RandomDataAddress(std::mt19937 &rng, uint32_t align) {
    const uint32_t offset = (rng() & 0xFFF0u) | (rng() & 0xFu & ~(align - 1));
    switch (rng() % 3) {
    case 0: return 0x26040000 + offset;
    case 1: return 0x06040000 + offset;
    default: return kMmio + offset;
    }
}

uint8_t RandomWb(std::mt19937 &rng) {
    const uint32_t pick = rng() % 19;
    return pick < 17 ? static_cast<uint8_t>(pick) : uint8_t{0xFF};
}

enum class Kind { Nop, MovR, MovI, MovBL, MovLL, MovBS, MovLS, MovLI, Add, AddI, CmpEq, Dt };
constexpr Kind kAllKinds[] = {Kind::Nop, Kind::MovR, Kind::MovI, Kind::MovBL, Kind::MovLL, Kind::MovBS,
                              Kind::MovLS, Kind::MovLI, Kind::Add, Kind::AddI, Kind::CmpEq, Kind::Dt};

// Builds one instance of `kind` with random operands and fixes up registers used as addresses.
uint16_t MakeInstr(Kind kind, std::mt19937 &rng, std::array<uint32_t, 16> &regs) {
    const uint32_t n = rng() % 16;
    const uint32_t m = rng() % 16;
    switch (kind) {
    case Kind::Nop: return kNop;
    case Kind::MovR: return MovR(n, m);
    case Kind::MovI: return MovI(n, rng());
    case Kind::MovBL: regs[m] = RandomDataAddress(rng, 1); return MovBL(n, m);
    case Kind::MovLL: regs[m] = RandomDataAddress(rng, 4); return MovLL(n, m);
    case Kind::MovBS: regs[n] = RandomDataAddress(rng, 1); return MovBS(n, m);
    case Kind::MovLS: regs[n] = RandomDataAddress(rng, 4); return MovLS(n, m);
    case Kind::MovLI: return MovLI(n, rng());
    case Kind::Add: return Add(n, m);
    case Kind::AddI: return AddI(n, rng());
    case Kind::CmpEq: return CmpEq(n, m);
    case Kind::Dt: return Dt(n);
    }
    return kNop;
}

void FillTargetArea(Pair &p) {
    p.WriteCode(kTarget - 0x10, std::vector<uint16_t>(0x40, static_cast<uint16_t>(kSleep)));
}

// Random-program generation (fuzz test). Register roles keep every access and branch inside known
// memory: R0-R7 and R13-R15 data; R8-R11 data addresses (never written); R12 branch register; GBR
// always holds one of the R8-R11 addresses.
uint32_t RemapDest(uint32_t r) {
    return (r >= 8 && r <= 12) ? r - 8 : r;
}
uint16_t WithHi(uint16_t w, uint32_t r) { // bits 11..8
    return static_cast<uint16_t>((w & ~0x0F00u) | (r << 8));
}
uint16_t WithLo(uint16_t w, uint32_t r) { // bits 7..4
    return static_cast<uint16_t>((w & ~0x00F0u) | (r << 4));
}

// Address forms whose base cannot be one of the fixed R8-R11 addresses: R0-indexed (R0 is a data
// register) and post-increment/pre-decrement (the base is written). The generator emits a setup
// instruction right before them (`mov #imm,R0` or `mov Rbase,Rd`); the pair is never split by a
// branch target or a delay slot.
bool NeedsSetup(jitspec::Addr a) {
    using jitspec::Addr;
    return a == Addr::RmR0 || a == Addr::RnR0 || a == Addr::GbrR0 || a == Addr::RnPreDec || a == Addr::RmPostInc;
}

// One random non-branch instruction from the compiled-opcode table, plus its setup instruction if
// it needs one (setup first). Jitspec::Encode supplies the random fields; register fields are then
// constrained to the roles above (its register fixups target single-instruction tests and are
// discarded here).
std::vector<uint16_t> FuzzInstr(const jitspec::OpSpec &spec, std::mt19937 &rng) {
    using jitspec::Addr;
    using jitspec::Fmt;
    std::array<uint32_t, 16> scratchRegs{};
    uint32_t scratchGbr = 0;
    uint16_t w = jitspec::Encode(spec, rng, scratchRegs, scratchGbr);
    const auto addrReg = [&] { return 8 + rng() % 4; };
    const std::string_view name = spec.name;
    const uint32_t hi = (w >> 8) & 0xFu;
    std::vector<uint16_t> setup;
    const auto setupR0 = [&] {
        // R0 = sign-extended imm8 (-128..127), aligned to the access size.
        const uint32_t imm = rng() & 0xFFu & ~(static_cast<uint32_t>(spec.size) - 1);
        setup.push_back(MovI(0, imm));
    };
    const auto setupBase = [&] { // Rd = copy of an address register; returns d
        const uint32_t d = RemapDest(rng() % 16);
        const uint32_t base = addrReg();
        setup.push_back(MovR(d, base));
        return d;
    };

    switch (spec.fmt) {
    case Fmt::Z:
    case Fmt::D: break; // MOVA (R0) and GBR-relative forms: GBR is always an address register
    case Fmt::I:
        if (spec.addr == Addr::GbrR0) {
            setupR0();
        }
        break;
    case Fmt::N:
    case Fmt::ND8:
    case Fmt::NI: w = WithHi(w, RemapDest(hi)); break;
    case Fmt::M: // LDC/LDS sources
        if (name == "LDC_GBR_R") {
            w = WithHi(w, addrReg());
        } else if (name == "LDS_PR_R") {
            w = WithHi(w, 12); // R12 holds an absolute program address (callers ensure it)
        }
        break;
    case Fmt::MD: w = WithLo(w, addrReg()); break;  // @(disp,Rm) -> R0
    case Fmt::ND4: w = WithLo(w, addrReg()); break; // R0 -> @(disp,Rn), Rn in bits 7..4
    case Fmt::NM:
    case Fmt::NMD:
        switch (spec.addr) {
        case Addr::None: w = WithHi(w, RemapDest(hi)); break;
        case Addr::Rm:
        case Addr::RmDisp: w = WithLo(WithHi(w, RemapDest(hi)), addrReg()); break;
        case Addr::Rn:
        case Addr::RnDisp: w = WithHi(w, addrReg()); break;
        case Addr::RmR0:
            setupR0();
            w = WithLo(WithHi(w, RemapDest(hi)), addrReg());
            break;
        case Addr::RnR0:
            setupR0();
            w = WithHi(w, addrReg());
            break;
        case Addr::RnPreDec: w = WithHi(w, setupBase()); break;
        case Addr::RmPostInc: {
            const uint32_t d = setupBase();
            w = WithLo(WithHi(w, RemapDest(hi)), d);
            break;
        }
        default: break;
        }
        break;
    }
    setup.push_back(w);
    return setup;
}

} // namespace

TEST_CASE("JIT matches the interpreter for each supported instruction", "[jit][diff]") {
    Pair p;
    std::mt19937 rng(0x5EED0001);
    for (Kind kind : kAllKinds) {
        for (int iter = 0; iter < 200; ++iter) {
            const uint32_t pc = kCode + (rng() & 1u) * 2; // exercise both fetch-buffer halves
            auto regs = RandomRegs(rng);
            const uint16_t instr = MakeInstr(kind, rng, regs);
            p.WriteCode(pc, {instr, static_cast<uint16_t>(kSleep)});

            auto state = p.ref->BaseState(pc);
            state.R = regs;
            state.SR = 0xF0 | (rng() & 1u);
            state.wbReg = RandomWb(rng);
            state.PR = rng();
            p.Load(state);

            INFO("instr " << Hex({instr}) << " at PC " << std::hex << pc);
            const auto info = p.Step();
            CHECK(info.retired == 1);
        }
    }
}

TEST_CASE("JIT matches the interpreter for instructions in delay slots", "[jit][diff]") {
    Pair p;
    std::mt19937 rng(0x5EED0002);
    const Kind slotKinds[] = {Kind::Nop, Kind::MovR, Kind::MovI, Kind::MovBL, Kind::MovLL, Kind::MovBS,
                              Kind::MovLS, Kind::Add, Kind::AddI, Kind::CmpEq, Kind::Dt};
    enum class Branch { Bra, Jmp, Rts, Bts, Bfs };
    for (Branch branch : {Branch::Bra, Branch::Jmp, Branch::Rts, Branch::Bts, Branch::Bfs}) {
        for (Kind kind : slotKinds) {
            for (int iter = 0; iter < 40; ++iter) {
                // Slot stores may hit the target area (when the JMP register aliases the store's
                // base register); restore it so every iteration branches onto SLEEP.
                FillTargetArea(p);
                auto regs = RandomRegs(rng);
                const uint16_t slot = MakeInstr(kind, rng, regs);
                // Displacements reach kTarget from kCode: (0x100 - 4) / 2 = 126.
                uint16_t br = 0;
                uint32_t pr = rng();
                switch (branch) {
                case Branch::Bra: br = Bra(126); break;
                case Branch::Bts: br = Bts(126); break;
                case Branch::Bfs: br = Bfs(126); break;
                case Branch::Jmp: {
                    const uint32_t m = rng() % 16;
                    regs[m] = kTarget;
                    br = Jmp(m);
                    break;
                }
                case Branch::Rts: pr = kTarget; br = kRts; break;
                }
                p.WriteCode(kCode, {br, slot, static_cast<uint16_t>(kSleep)});

                auto state = p.ref->BaseState(kCode);
                state.R = regs;
                state.SR = 0xF0 | (rng() & 1u);
                state.wbReg = RandomWb(rng);
                state.PR = pr;
                p.Load(state);

                INFO("branch/slot " << Hex({br, slot}));
                p.Step(); // branch (+ slot when taken)
                p.Step(); // not-taken BT/S, BF/S: the slot instruction as a normal instruction
            }
        }
    }
}

TEST_CASE("JIT matches the interpreter for branches at unaligned positions and targets", "[jit][diff]") {
    // A branch at kCode + 2 puts its slot on a 4-byte boundary (the slot fetch refills); a target
    // with bit 1 set makes the delay-slot end refill.
    Pair p;
    enum class Branch { Bra, Bts, Bf, Jmp, Rts };
    const uint16_t slots[] = {AddI(5, 0x13), MovLL(7, 6), MovLS(8, 7)};
    for (uint32_t pc : {kCode, kCode + 2}) {
        for (uint32_t target : {kTarget, kTarget + 2}) {
            const uint32_t disp = (target - pc - 4) / 2;
            for (Branch branch : {Branch::Bra, Branch::Bts, Branch::Bf, Branch::Jmp, Branch::Rts}) {
                for (uint16_t slot : slots) {
                    FillTargetArea(p);
                    uint16_t br = 0;
                    switch (branch) {
                    case Branch::Bra: br = Bra(disp); break;
                    case Branch::Bts: br = Bts(disp); break; // taken (T = 1 below)
                    case Branch::Bf: br = Bf(disp); break;   // taken (T = 0)
                    case Branch::Jmp: br = Jmp(9); break;
                    case Branch::Rts: br = kRts; break;
                    }
                    // Code is written before BaseState so the fetch buffer matches memory.
                    p.WriteCode(kCode, {kNop, kNop});
                    p.WriteCode(pc, {br, slot, static_cast<uint16_t>(kSleep)});

                    auto state = p.ref->BaseState(pc);
                    state.R[6] = 0x06040000;
                    state.R[7] = 0x89ABCDEF;
                    state.R[8] = 0x06040010;
                    state.R[9] = target;
                    state.SR = 0xF0;
                    state.wbReg = 7;
                    if (branch == Branch::Bts) {
                        state.SR |= 1;
                    }
                    state.PR = target;
                    p.Load(state);

                    INFO("branch/slot " << Hex({br, slot}) << " at PC " << std::hex << pc << " target "
                                        << target);
                    p.Step();
                    p.Step();
                    p.Step();
                    const uint32_t endPc = p.jit->State().PC;
                    CHECK(endPc >= kTarget - 0x10);
                    CHECK(endPc < kTarget + 0x70);
                }
            }
        }
    }
}

TEST_CASE("JIT matches the interpreter for BT and BF", "[jit][diff]") {
    Pair p;
    FillTargetArea(p);
    for (uint32_t t = 0; t < 2; ++t) {
        for (uint16_t br : {Bt(126), Bf(126), Bt(0xFE), Bf(0xFE)}) {
            p.WriteCode(kCode, {br, static_cast<uint16_t>(kSleep)});
            auto state = p.ref->BaseState(kCode);
            state.SR = 0xF0 | t;
            state.wbReg = 4;
            p.Load(state);
            INFO("branch " << Hex({br}) << " T=" << t);
            const auto info = p.Step();
            CHECK(info.retired == 1);
        }
    }
}

TEST_CASE("Bus-wait retries match the interpreter", "[jit][diff]") {
    for (uint32_t every : {1u, 2u, 3u}) {
        Pair p;
        FillTargetArea(p);
        p.SetBusWaitEvery(every);
        const std::vector<std::vector<uint16_t>> programs = {
            {MovLL(1, 2), static_cast<uint16_t>(kSleep)},
            {MovLS(2, 1), static_cast<uint16_t>(kSleep)},
            {AddI(3, 1), MovLL(1, 2), AddI(3, 1), static_cast<uint16_t>(kSleep)},
            {Bra(126), MovLL(1, 2), static_cast<uint16_t>(kSleep)},
            {Bra(126), MovLS(2, 1), static_cast<uint16_t>(kSleep)},
        };
        for (const auto &program : programs) {
            p.WriteCode(kCode, program);
            auto state = p.ref->BaseState(kCode);
            state.R[1] = 0x12345678;
            state.R[2] = kMmio + 0x40;
            state.wbReg = 2;
            p.Load(state);
            INFO("program " << Hex(program) << " busWaitEvery " << every);
            for (int step = 0; step < 6; ++step) {
                p.Step();
            }
        }
    }
}

TEST_CASE("JIT recompiles a block when its guest code changes", "[jit][diff]") {
    Pair p;
    p.WriteCode(kCode, {AddI(0, 1), static_cast<uint16_t>(kSleep)});
    auto state = p.ref->BaseState(kCode);
    p.Load(state);
    p.Step();
    REQUIRE(p.exec.Cache().Compiles() == 1);

    p.WriteCode(kCode, {AddI(0, 2), static_cast<uint16_t>(kSleep)});
    p.Load(state);
    p.Step();
    REQUIRE(p.exec.Cache().Invalidations() == 1);
    REQUIRE(p.exec.Cache().Compiles() == 2);
    REQUIRE(p.jit->State().R[0] == state.R[0] + 2);
}

TEST_CASE("Unsupported instructions fall back to the interpreter", "[jit][diff]") {
    Pair p;
    // mulu.w R2,R1 (unsupported) ; add #1,R0 ; sleep
    p.WriteCode(kCode, {0x212E, AddI(0, 1), static_cast<uint16_t>(kSleep)});
    auto state = p.ref->BaseState(kCode);
    state.R[1] = 7;
    state.R[2] = 9;
    p.Load(state);
    p.Step();
    REQUIRE(p.exec.GetStats().interpreted == 1);
    p.Step();
    REQUIRE(p.exec.GetStats().blocksRun == 1);
}

TEST_CASE("Delayed branch with an unsupported slot ends the block before the branch", "[jit][diff]") {
    Pair p;
    FillTargetArea(p);
    // add #1,R0 ; bra kTarget ; mulu.w R2,R1 (unsupported in the slot)
    p.WriteCode(kCode, {AddI(0, 1), Bra(125), 0x212E});
    p.Load(p.ref->BaseState(kCode));
    const auto first = p.Step();
    REQUIRE(first.retired == 1);
    p.Step(); // bra via the interpreter
    p.Step(); // slot via the interpreter (delay slot pending)
}

TEST_CASE("Blocks are capped at the maximum length", "[jit][diff]") {
    Pair p;
    std::vector<uint16_t> program(40, kNop);
    program.push_back(static_cast<uint16_t>(kSleep));
    p.WriteCode(kCode, program);
    p.Load(p.ref->BaseState(kCode));
    const auto info = p.Step();
    REQUIRE(info.retired == brimir::jit::kMaxBlockInstructions);
}

TEST_CASE("Executor::Run executes until the cycle target", "[jit][diff]") {
    Pair p;
    // add #1,R0 ; bra kCode ; nop  (infinite loop, 4 cycles per iteration)
    p.WriteCode(kCode, {AddI(0, 1), 0xAFFD, kNop});
    p.Load(p.ref->BaseState(kCode));
    auto &ctx = p.jit->sh2->GetJitContext();
    const uint64 executed = p.exec.Run(ctx, 0, 100);
    REQUIRE(executed >= 100);
    REQUIRE(executed < 100 + 4);
}

TEST_CASE("A stale fetch buffer at PC & 2 runs on the interpreter", "[jit][diff]") {
    Pair p;
    p.WriteCode(kCode, {kNop, AddI(0, 1), static_cast<uint16_t>(kSleep)});
    auto state = p.ref->BaseState(kCode + 2);
    state.fetchedOpcodes = (uint32_t{kNop} << 16) | kNop; // memory at kCode+2 now holds add #1,R0
    p.Load(state);
    p.Step();
    REQUIRE(p.exec.GetStats().interpreted == 1);
    REQUIRE(p.jit->State().R[0] == state.R[0]); // the buffered NOP ran, not the add
}

TEST_CASE("On-chip timer reads see the same cycle counts as the interpreter", "[jit][diff]") {
    auto ref = std::make_unique<Rig>();
    auto jit = std::make_unique<Rig>();
    brimir::jit::Executor exec;
    // loop: add #1,R4 ; add #1,R4 ; mov.b @R1,R2 (FRC byte) ; add R2,R3 ; bra loop ; nop
    // The FRC read is mid-block, after two cycle-consuming instructions, so it only sees the
    // interpreter's count if the block syncs the cycle counter before the access.
    // bra at offset 8 -> disp = (0 - 8 - 4) / 2 = -6 = 0xFFA.
    const std::vector<uint16_t> loop = {AddI(4, 1), AddI(4, 1), MovBL(2, 1), Add(3, 2), 0xAFFA, kNop};
    ref->WriteCode(kCode, loop);
    jit->WriteCode(kCode, loop);
    auto state = ref->BaseState(kCode);
    state.R[1] = 0xFFFFFE12;
    state.R[3] = 0;
    ref->Load(state);
    jit->Load(state);
    jit->sh2->SetJitExecutor(&exec);

    // With instruction-exact boundaries, both stop at the same instruction for the same target.
    const uint64 jitCycles = jit->sh2->Advance<false, false>(50000);
    const uint64 refCycles = ref->sh2->Advance<false, false>(50000);
    REQUIRE(refCycles == jitCycles);
    const std::string diff = sh2test::DiffRigs(*ref, *jit, true);
    INFO(diff);
    REQUIRE(diff.empty());
    CHECK(jit->State().R[3] != 0u); // the timer actually advanced
}

// Random programs of every compiled instruction (jitspec::CompiledOpcodes()) plus all branch kinds,
// with delay slots, loops, calls, MMIO and bus waits. Register roles (see FuzzInstr) keep execution
// and data accesses inside known memory: R0-R7, R13-R15 data; R8-R11 data addresses (never
// written); GBR one of R8-R11; R12 the branch register; PR a return target. R12 is per program
// either an absolute program address (JMP/JSR, and LDS PR,R12) or a fixed displacement
// (BRAF/BSRF: target = PC + 4 + R12).
TEST_CASE("JIT matches the interpreter on random programs", "[jit][diff][fuzz]") {
    constexpr int kPrograms = 300;
    constexpr int kLength = 24;
    constexpr int kSteps = 80;
    // Coverage lower bounds, ~65% of the values measured with all 300 programs passing
    // (steps=15527 blocksRun=15378 interpreted=149 compiles=1134, i.e. 99% of steps
    // in blocks).
    constexpr uint64_t kMinBlocksRun = 10000;
    constexpr uint64_t kMinBlockPercent = 64;
    bool diverged = false;
    uint64_t totalSteps = 0;
    uint64_t totalBlocksRun = 0;
    uint64_t totalInterpreted = 0;
    uint64_t totalCompiles = 0;

    // Non-branch opcodes, and the subset that needs no setup instruction (delay slots and the last
    // program position hold only those).
    std::vector<const jitspec::OpSpec *> nonBranch;
    std::vector<const jitspec::OpSpec *> singles;
    for (const jitspec::OpSpec &spec : jitspec::CompiledOpcodes()) {
        if (spec.slotOk) {
            nonBranch.push_back(&spec);
            if (!NeedsSetup(spec.addr)) {
                singles.push_back(&spec);
            }
        }
    }

    enum class Br { None, Bt, Bf, Bts, Bfs, Bra, Bsr, Braf, Bsrf };
    for (int prog = 0; prog < kPrograms; ++prog) {
        const uint32_t seed = 0xF0220000u + static_cast<uint32_t>(prog);
        std::mt19937 rng(seed);
        Pair p;
        p.SetBusWaitEvery((rng() & 1u) ? 3u : 0u);
        const bool absR12 = (rng() & 1u) != 0;                         // JMP/JSR, else BRAF/BSRF
        const int relDisp = static_cast<int>(rng() % 13) - 6;          // BRAF/BSRF: R12 = 2 * relDisp
        const auto isLdsPr = [](const jitspec::OpSpec *s) { return std::string_view(s->name) == "LDS_PR_R"; };

        // Pass 1: instructions, with branch displacements patched in pass 2 once the valid targets
        // (every instruction except the second of a setup pair) are known.
        std::vector<uint16_t> program;
        std::vector<Br> branchKind;
        std::vector<bool> pairSecond;
        bool prevDelayed = false;
        while (static_cast<int>(program.size()) < kLength) {
            const int i = static_cast<int>(program.size());
            const bool last = i == kLength - 1;
            uint32_t pick = rng() % 16;
            if (prevDelayed && pick >= 12) {
                pick = rng() % 12; // delay slots never hold branches
            } else if (last && pick >= 13) {
                pick = rng() % 13; // no delayed branch last: its slot would be SLEEP
            }
            bool delayed = false;
            if (pick < 12) {
                // A setup pair must fit before the end and never sits in a delay slot.
                const auto &pool = (prevDelayed || last) ? singles : nonBranch;
                const jitspec::OpSpec *spec = pool[rng() % pool.size()];
                while (!absR12 && isLdsPr(spec)) {
                    spec = pool[rng() % pool.size()]; // R12 is not an absolute address here
                }
                const std::vector<uint16_t> words = FuzzInstr(*spec, rng);
                for (size_t k = 0; k < words.size(); ++k) {
                    program.push_back(words[k]);
                    branchKind.push_back(Br::None);
                    pairSecond.push_back(k > 0);
                }
            } else {
                Br kind = Br::None;
                uint16_t word = 0;
                const uint32_t sub = rng();
                switch (pick) {
                case 12: kind = (sub & 1u) ? Br::Bt : Br::Bf; break;
                case 13: kind = (sub & 1u) ? Br::Bts : Br::Bfs; break;
                case 14: kind = (sub & 1u) ? Br::Bra : Br::Bsr; break;
                default: // register branches through R12, or RTS
                    switch (sub % 3) {
                    case 0:
                        word = absR12 ? Jmp(12) : Braf(12);
                        kind = absR12 ? Br::None : Br::Braf;
                        break;
                    case 1:
                        word = absR12 ? Jsr(12) : Bsrf(12);
                        kind = absR12 ? Br::None : Br::Bsrf;
                        break;
                    default: word = kRts; break;
                    }
                    break;
                }
                program.push_back(word);
                branchKind.push_back(kind);
                pairSecond.push_back(false);
                delayed = pick >= 13;
            }
            prevDelayed = delayed;
        }

        // Pass 2: branch displacements.
        std::vector<int> targets;
        for (int i = 0; i < kLength; ++i) {
            if (!pairSecond[i]) {
                targets.push_back(i);
            }
        }
        const auto randomTarget = [&] { return targets[rng() % targets.size()]; };
        for (int i = 0; i < kLength; ++i) {
            Br kind = branchKind[i];
            if (kind == Br::Braf || kind == Br::Bsrf) {
                const int t = i + 2 + relDisp;
                if (t >= 0 && t < kLength && !pairSecond[t]) {
                    continue; // the fixed R12 displacement lands on a valid target
                }
                kind = kind == Br::Braf ? Br::Bra : Br::Bsr; // out of range here: use the disp12 form
            }
            if (kind == Br::None) {
                continue;
            }
            const uint32_t disp = static_cast<uint32_t>(randomTarget() - i - 2);
            switch (kind) {
            case Br::Bt: program[i] = Bt(disp); break;
            case Br::Bf: program[i] = Bf(disp); break;
            case Br::Bts: program[i] = Bts(disp); break;
            case Br::Bfs: program[i] = Bfs(disp); break;
            case Br::Bra: program[i] = Bra(disp); break;
            case Br::Bsr: program[i] = Bsr(disp); break;
            default: break;
            }
        }
        for (int i = 0; i < 8; ++i) {
            program.push_back(static_cast<uint16_t>(kSleep));
        }
        p.WriteCode(kCode, program);

        auto state = p.ref->BaseState(kCode);
        for (int r : {0, 1, 2, 3, 4, 5, 6, 7, 13, 14, 15}) {
            state.R[r] = rng();
        }
        state.R[8] = 0x26040000 + (rng() & 0xFF0u);
        state.R[9] = 0x06040100 + (rng() & 0xFF0u);
        state.R[10] = kMmio + (rng() & 0xF0u);
        state.R[11] = 0x26048000;
        state.GBR = state.R[8 + rng() % 4];
        state.R[12] = absR12 ? kCode + 2 * static_cast<uint32_t>(randomTarget())
                             : static_cast<uint32_t>(2 * relDisp);
        state.PR = kCode + 2 * static_cast<uint32_t>(randomTarget());
        state.SR = 0xF0 | (rng() & 1u);
        state.wbReg = static_cast<uint8_t>(rng() % 17);
        state.MACH = rng();
        state.MACL = rng();
        p.Load(state);

        INFO("seed 0x" << std::hex << seed << " program " << Hex(program));
        bool failed = false;
        for (int step = 0; step < kSteps; ++step) {
            INFO("step " << std::dec << step);
            p.Step();
            ++totalSteps;
            if (!p.lastStepMatched) {
                failed = true; // Pair::Step already reported the difference
                break;
            }
            if (p.ref->State().sleep && p.jit->State().sleep) {
                break; // both asleep: remaining steps would only re-run SLEEP
            }
        }
        totalBlocksRun += p.exec.GetStats().blocksRun;
        totalInterpreted += p.exec.GetStats().interpreted;
        totalCompiles += p.exec.Cache().Compiles();
        if (failed) {
            diverged = true;
            break; // stop at the first failing program
        }
    }

    // The premise of the test: most steps ran compiled blocks rather than the interpreter.
    // Compile attempts include empty fallback blocks, so totalCompiles is informational only.
    WARN("fuzz coverage: steps=" << totalSteps << " blocksRun=" << totalBlocksRun
                                 << " interpreted=" << totalInterpreted << " compiles=" << totalCompiles);
    if (!diverged) {
        // A divergence already failed the test; don't bury it under threshold failures.
        CHECK(totalBlocksRun >= kMinBlocksRun);
        CHECK(totalBlocksRun * 100 >= totalSteps * kMinBlockPercent);
    }
}

// With instruction-exact boundaries, Advance() through the JIT must stop at exactly the same
// instruction as the interpreter for any cycle target, including between a branch and its slot.
TEST_CASE("JIT Advance matches interpreter Advance for every cycle target", "[jit][diff][exact]") {
    // loop: mov.l @R8,R1 ; add R1,R2 ; mov.l R2,@R9 ; dt R3 ; bf/s loop ; add #1,R4 ; bra loop ; nop
    const std::vector<uint16_t> loop = {MovLL(1, 8), Add(2, 1), MovLS(9, 2), Dt(3),
                                        Bfs(0xFA),   AddI(4, 1), Bra(0xFF8), kNop};
    bool sawDelaySlotStop = false;
    for (uint32_t target = 1; target <= 400; ++target) {
        Pair p;
        p.SetBusWaitEvery(target % 3 == 0 ? 2u : 0u);
        p.WriteCode(kCode, loop);
        p.Write32(0x06040000, 0x01020304);
        auto state = p.ref->BaseState(kCode);
        state.R[3] = 3;
        state.R[8] = 0x26040000;
        state.R[9] = (target & 1u) ? kMmio + 0x10 : 0x26040010;
        p.Load(state);
        p.jit->sh2->SetJitExecutor(&p.exec);

        const uint64 refCycles = p.ref->sh2->Advance<false, false>(target);
        const uint64 jitCycles = p.jit->sh2->Advance<false, false>(target);
        INFO("target " << target);
        REQUIRE(jitCycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*p.ref, *p.jit, true);
        INFO(diff);
        REQUIRE(diff.empty());
        sawDelaySlotStop = sawDelaySlotStop || p.jit->State().delaySlot;
    }
    CHECK(sawDelaySlotStop); // some target stopped between bf/s and its slot
}

// Same sweep with a non-delayed BF that ends a multi-instruction block, so the boundary check
// before it decides whether a target between cmp/eq and bf stops there. BF is taken on most
// iterations and falls through when R5 reaches R6.
TEST_CASE("JIT Advance matches interpreter Advance for every cycle target with BT/BF", "[jit][diff][exact]") {
    // loop: add #1,R5 ; cmp/eq R6,R5 ; bf loop ; mov #0,R5 ; bt loop ; nop
    // bf at offset 4 -> disp = (0 - 4 - 4) / 2 = -4; bt at offset 8 -> disp = (0 - 8 - 4) / 2 = -6.
    // T stays 1 after the fall-through, so bt is always taken; mov #0,R5 restarts the count.
    const std::vector<uint16_t> loop = {AddI(5, 1), CmpEq(5, 6), Bf(0xFC), MovI(5, 0), Bt(0xFA), kNop};
    bool sawBeforeBranchStop = false;
    for (uint32_t target = 1; target <= 400; ++target) {
        Pair p;
        p.WriteCode(kCode, loop);
        auto state = p.ref->BaseState(kCode);
        state.R[5] = 0;
        state.R[6] = 3;
        p.Load(state);
        p.jit->sh2->SetJitExecutor(&p.exec);

        const uint64 refCycles = p.ref->sh2->Advance<false, false>(target);
        const uint64 jitCycles = p.jit->sh2->Advance<false, false>(target);
        INFO("target " << target);
        REQUIRE(jitCycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*p.ref, *p.jit, true);
        INFO(diff);
        REQUIRE(diff.empty());
        sawBeforeBranchStop = sawBeforeBranchStop || p.jit->State().PC == kCode + 4;
    }
    CHECK(sawBeforeBranchStop); // some target stopped right before bf
}

// A 32/32 division by zero (write to DVDNT) raises the DIVU overflow interrupt synchronously,
// inside the store. The interpreter takes it before the next instruction; so must the JIT.
TEST_CASE("Interrupts raised inside a block are taken at the same instruction", "[jit][diff][exact]") {
    constexpr uint32_t kVbr = 0x06008000;
    constexpr uint32_t kVector = 0x50; // not 0x40, the reset vector of IRL
    constexpr uint32_t kHandler = 0x06009000;
    constexpr uint32_t kStack = 0x0600F000;
    // Every cycle target up to 60, so Advance also stops mid-block, right after the store and inside
    // the exception entry; plus the original 200. The handler's SLEEP is reached from target
    // 14 on (measured; the plan estimated about 40); from there all the end-state assertions apply.
    constexpr uint32_t kHandlerTarget = 14;
    std::vector<uint32_t> targets;
    for (uint32_t target = 1; target <= 60; ++target) {
        targets.push_back(target);
    }
    targets.push_back(200);
    for (const uint32_t target : targets) {
        INFO("target " << target);
        Pair p;
        for (Rig *rig : {p.ref.get(), p.jit.get()}) {
            rig->Write32(kVbr + kVector * 4, kHandler);
            rig->WriteCode(kHandler, {static_cast<uint16_t>(kSleep)});
            auto &ctx = rig->sh2->GetJitContext();
            ctx.write(ctx.sh2, 0xFFFFFF00, 4, 0);       // DVSR = 0: the next division overflows
            ctx.write(ctx.sh2, 0xFFFFFF08, 4, 0x2);     // DVCR.OVFIE = 1
            ctx.write(ctx.sh2, 0xFFFFFF0C, 4, kVector); // VCRDIV: vector number
            ctx.write(ctx.sh2, 0xFFFFFEE2, 1, 0xF0);    // IPRA: DIVU interrupt level 15
        }
        // mov.l R1,@R2 (R2 = DVDNT) ; add #1,R3 ; add #1,R3 ; sleep
        p.WriteCode(kCode, {MovLS(2, 1), AddI(3, 1), AddI(3, 1), static_cast<uint16_t>(kSleep)});
        auto state = p.ref->BaseState(kCode);
        // Interrupt mask 14: blocks IRL, which RecalcInterrupts (called by the DVCR write) always
        // raises at its reset level 1, but lets the level-15 DIVU interrupt through.
        state.SR = 0xE0;
        state.VBR = kVbr;
        state.R[1] = 1234;
        state.R[2] = 0xFFFFFF04;
        state.R[3] = 0;
        state.R[15] = kStack; // stack for exception entry
        p.Load(state);
        p.jit->sh2->SetJitExecutor(&p.exec);
        REQUIRE_FALSE(*p.ref->sh2->GetJitContext().intrPending); // nothing pending before the store

        const uint64 refCycles = p.ref->sh2->Advance<false, false>(target);
        const uint64 jitCycles = p.jit->sh2->Advance<false, false>(target);
        REQUIRE(jitCycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*p.ref, *p.jit, true);
        INFO(diff);
        REQUIRE(diff.empty());
        REQUIRE(p.ref->State().R[3] == 0u); // the adds never ran: the interrupt comes first
        REQUIRE(p.ref->State().sleep == (target >= kHandlerTarget));
        if (target >= kHandlerTarget) {
            REQUIRE(p.ref->Read32(kStack - 8) == kCode + 2); // stacked PC: taken right after the store
            REQUIRE(p.exec.GetStats().blocksRun >= 1);       // the store ran in a compiled block
        }
    }
}

namespace {

constexpr uint32_t kIntrVbr = 0x06008000;
constexpr uint32_t kIntrVector = 0x50;
constexpr uint32_t kIntrHandler = 0x06009000;
constexpr uint32_t kIntrStack = 0x0600F000;

// On both rigs: a DIVU overflow interrupt (level 15, vector kIntrVector) is already raised, and its
// handler is a SLEEP. With SR mask 15 it is not pending until an LDC SR unmasks it.
void RaiseDivuOverflow(Pair &p) {
    for (Rig *rig : {p.ref.get(), p.jit.get()}) {
        rig->Write32(kIntrVbr + kIntrVector * 4, kIntrHandler);
        rig->WriteCode(kIntrHandler, {static_cast<uint16_t>(kSleep)});
        auto &ctx = rig->sh2->GetJitContext();
        ctx.write(ctx.sh2, 0xFFFFFF00, 4, 0);           // DVSR = 0
        ctx.write(ctx.sh2, 0xFFFFFF08, 4, 0x2);         // DVCR.OVFIE = 1
        ctx.write(ctx.sh2, 0xFFFFFF0C, 4, kIntrVector); // VCRDIV
        ctx.write(ctx.sh2, 0xFFFFFEE2, 1, 0xF0);        // IPRA: DIVU level 15
        ctx.write(ctx.sh2, 0xFFFFFF04, 4, 1234);        // DVDNT: divide by zero -> overflow raised
    }
}

} // namespace

// LDC Rm,SR that unmasks a pending interrupt: the interpreter still executes the next instruction
// (interrupt-allow is cleared for one instruction) and takes the interrupt before the one after.
TEST_CASE("Interrupts unmasked by LDC SR are taken one instruction later", "[jit][diff][exact]") {
    constexpr uint32_t kVbr = 0x06008000;
    constexpr uint32_t kVector = 0x50;
    constexpr uint32_t kHandler = 0x06009000;
    Pair p;
    for (Rig *rig : {p.ref.get(), p.jit.get()}) {
        rig->Write32(kVbr + kVector * 4, kHandler);
        rig->WriteCode(kHandler, {static_cast<uint16_t>(kSleep)});
        auto &ctx = rig->sh2->GetJitContext();
        ctx.write(ctx.sh2, 0xFFFFFF00, 4, 0);       // DVSR = 0
        ctx.write(ctx.sh2, 0xFFFFFF08, 4, 0x2);     // DVCR.OVFIE = 1
        ctx.write(ctx.sh2, 0xFFFFFF0C, 4, kVector); // VCRDIV
        ctx.write(ctx.sh2, 0xFFFFFEE2, 1, 0xF0);    // IPRA: DIVU level 15
        ctx.write(ctx.sh2, 0xFFFFFF04, 4, 1234);    // DVDNT: divide by zero -> overflow raised
    }
    // ldc R5,SR (R5 = 0: unmask) ; add #1,R3 ; add #1,R3 ; add #1,R3 ; sleep
    p.WriteCode(kCode, {0x450E, AddI(3, 1), AddI(3, 1), AddI(3, 1), static_cast<uint16_t>(kSleep)});
    auto state = p.ref->BaseState(kCode);
    state.SR = 0xF0; // mask 15: the level-15 DIVU interrupt is not pending yet
    state.VBR = kVbr;
    state.R[3] = 0;
    state.R[5] = 0;
    state.R[15] = 0x0600F000;
    p.Load(state);
    REQUIRE_FALSE(*p.jit->sh2->GetJitContext().intrPending);
    p.jit->sh2->SetJitExecutor(&p.exec);

    const uint64 refCycles = p.ref->sh2->Advance<false, false>(200);
    const uint64 jitCycles = p.jit->sh2->Advance<false, false>(200);
    REQUIRE(jitCycles == refCycles);
    const std::string diff = sh2test::DiffRigs(*p.ref, *p.jit, true);
    INFO(diff);
    REQUIRE(diff.empty());
    REQUIRE(p.ref->State().R[3] == 1u); // exactly one add ran before the interrupt
    REQUIRE(p.exec.GetStats().blocksRun > 0);
    // ldc and the adds share one compiled block, so the in-block interrupt-allow rule is exercised.
    REQUIRE(brimir::jit::BuildBlock(p.jit->sh2->GetJitContext(), kCode).guestInstrCount >= 2u);
}

// Chained allow-clearing instructions: ldc R6,GBR runs right after ldc R5,SR (allow is cleared) and
// clears allow again, so the first add also runs and the interrupt is taken before the second add.
TEST_CASE("Interrupts unmasked by LDC SR wait for a chain of allow-clearing instructions", "[jit][diff][exact]") {
    Pair p;
    RaiseDivuOverflow(p);
    // ldc R5,SR (unmask) ; ldc R6,GBR ; add #1,R3 ; add #1,R3 ; add #1,R3 ; sleep
    p.WriteCode(kCode, {0x450E, 0x461E, AddI(3, 1), AddI(3, 1), AddI(3, 1), static_cast<uint16_t>(kSleep)});
    auto state = p.ref->BaseState(kCode);
    state.SR = 0xF0; // mask 15: the level-15 DIVU interrupt is not pending yet
    state.VBR = kIntrVbr;
    state.R[3] = 0;
    state.R[5] = 0;
    state.R[6] = 0x12345678;
    state.R[15] = kIntrStack;
    p.Load(state);
    REQUIRE_FALSE(*p.jit->sh2->GetJitContext().intrPending);
    p.jit->sh2->SetJitExecutor(&p.exec);

    const uint64 refCycles = p.ref->sh2->Advance<false, false>(200);
    const uint64 jitCycles = p.jit->sh2->Advance<false, false>(200);
    REQUIRE(jitCycles == refCycles);
    const std::string diff = sh2test::DiffRigs(*p.ref, *p.jit, true);
    INFO(diff);
    REQUIRE(diff.empty());
    REQUIRE(p.ref->State().GBR == 0x12345678u);
    REQUIRE(p.ref->State().R[3] == 1u);                        // ldc SR, ldc GBR, one add, interrupt
    REQUIRE(p.ref->State().sleep);                             // the handler ran
    REQUIRE(p.ref->Read32(kIntrStack - 8) == kCode + 6);       // stacked PC: the second add
    REQUIRE(p.exec.GetStats().blocksRun > 0);
    // Both ldc and the adds share one compiled block, so the in-block rule is what is exercised.
    REQUIRE(brimir::jit::BuildBlock(p.jit->sh2->GetJitContext(), kCode).guestInstrCount >= 3u);
}

// The instruction after ldc R5,SR is a delayed branch: the branch and its slot run (a delay slot
// never takes an interrupt; the slot's PC update re-evaluates pending), and the interrupt is taken
// at the branch target, before its first instruction.
TEST_CASE("Interrupts unmasked by LDC SR before a delayed branch are taken at the target", "[jit][diff][exact]") {
    constexpr uint32_t kBranchTarget = kCode + 0x20;
    Pair p;
    RaiseDivuOverflow(p);
    // kCode: ldc R5,SR ; bra kBranchTarget ; add #1,R3 (slot) ; add #1,R3 ; sleep
    // bra at kCode + 2: disp = (0x20 - 2 - 4) / 2 = 13.
    p.WriteCode(kCode, {0x450E, Bra(13), AddI(3, 1), AddI(3, 1), static_cast<uint16_t>(kSleep)});
    // kBranchTarget: add #1,R4 ; add #1,R4 ; sleep
    p.WriteCode(kBranchTarget, {AddI(4, 1), AddI(4, 1), static_cast<uint16_t>(kSleep)});
    auto state = p.ref->BaseState(kCode);
    state.SR = 0xF0;
    state.VBR = kIntrVbr;
    state.R[3] = 0;
    state.R[4] = 0;
    state.R[5] = 0;
    state.R[15] = kIntrStack;
    p.Load(state);
    REQUIRE_FALSE(*p.jit->sh2->GetJitContext().intrPending);
    p.jit->sh2->SetJitExecutor(&p.exec);

    const uint64 refCycles = p.ref->sh2->Advance<false, false>(200);
    const uint64 jitCycles = p.jit->sh2->Advance<false, false>(200);
    REQUIRE(jitCycles == refCycles);
    const std::string diff = sh2test::DiffRigs(*p.ref, *p.jit, true);
    INFO(diff);
    REQUIRE(diff.empty());
    REQUIRE(p.ref->State().R[3] == 1u);                      // the slot ran
    REQUIRE(p.ref->State().R[4] == 0u);                      // nothing at the target ran
    REQUIRE(p.ref->State().sleep);                           // the handler ran
    REQUIRE(p.ref->Read32(kIntrStack - 8) == kBranchTarget); // stacked PC: the branch target
    REQUIRE(p.exec.GetStats().blocksRun > 0);
    // ldc, bra and the slot form one compiled block.
    REQUIRE(brimir::jit::BuildBlock(p.jit->sh2->GetJitContext(), kCode).guestInstrCount == 3u);
}
