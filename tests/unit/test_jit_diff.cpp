// Brimir - SH-2 JIT vs interpreter differential tests (per instruction)
// Licensed under GPL-3.0
//
// Every test runs the same code on two identical isolated SH-2s: one through
// the JIT executor, one through the interpreter, and requires identical state,
// memory, bus-wait query sequence and cycle totals.

#include "catch_amalgamated.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/jit/executor.hpp>

#include <array>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
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

    // The JIT stops on a block boundary; that point is also an instruction boundary for the
    // interpreter, so advancing the interpreter to the same count lands on the same state.
    const uint64 jitCycles = jit->sh2->Advance<false, false>(50000);
    const uint64 refCycles = ref->sh2->Advance<false, false>(jitCycles);
    REQUIRE(refCycles == jitCycles);
    const std::string diff = sh2test::DiffRigs(*ref, *jit);
    INFO(diff);
    REQUIRE(diff.empty());
    CHECK(jit->State().R[3] != 0u); // the timer actually advanced
}

// Random programs of supported instructions, including branches, delay slots, loops, MMIO and
// bus waits. Register roles keep execution inside the program: R0-R7 data, R8-R11 data
// addresses (never written), R12 jump target, PR return target.
TEST_CASE("JIT matches the interpreter on random programs", "[jit][diff][fuzz]") {
    constexpr int kPrograms = 300;
    constexpr int kLength = 24;
    constexpr int kSteps = 80;

    for (int prog = 0; prog < kPrograms; ++prog) {
        const uint32_t seed = 0xF0220000u + static_cast<uint32_t>(prog);
        std::mt19937 rng(seed);
        Pair p;
        p.SetBusWaitEvery((rng() & 1u) ? 3u : 0u);

        const auto dataReg = [&] { return rng() % 8; };
        const auto addrReg = [&] { return 8 + rng() % 4; };
        // Displacement, in instructions, from instruction i to a random instruction of the program.
        const auto targetDisp = [&](int i) {
            return static_cast<uint32_t>(static_cast<int>(rng() % kLength) - i - 2);
        };

        std::vector<uint16_t> program;
        bool prevDelayed = false;
        for (int i = 0; i < kLength; ++i) {
            uint32_t pick = rng() % 16;
            if (prevDelayed && pick >= 12) {
                pick = rng() % 12; // delay slots never hold branches
            }
            uint16_t instr = kNop;
            switch (pick) {
            case 0: instr = kNop; break;
            case 1: instr = MovR(dataReg(), dataReg()); break;
            case 2: instr = MovI(dataReg(), rng()); break;
            case 3: instr = MovBL(dataReg(), addrReg()); break;
            case 4: instr = MovLL(dataReg(), addrReg()); break;
            case 5: instr = MovBS(addrReg(), dataReg()); break;
            case 6: instr = MovLS(addrReg(), dataReg()); break;
            case 7: instr = MovLI(dataReg(), rng() % 16); break;
            case 8: instr = Add(dataReg(), dataReg()); break;
            case 9: instr = AddI(dataReg(), rng()); break;
            case 10: instr = CmpEq(dataReg(), dataReg()); break;
            case 11: instr = Dt(dataReg()); break;
            case 12: instr = (rng() & 1u) ? Bt(targetDisp(i)) : Bf(targetDisp(i)); break;
            case 13: instr = (rng() & 1u) ? Bts(targetDisp(i)) : Bfs(targetDisp(i)); break;
            case 14: instr = Bra(targetDisp(i)); break;
            default: instr = (rng() & 1u) ? Jmp(12) : kRts; break;
            }
            program.push_back(instr);
            prevDelayed = pick >= 13;
        }
        for (int i = 0; i < 8; ++i) {
            program.push_back(static_cast<uint16_t>(kSleep));
        }
        p.WriteCode(kCode, program);

        auto state = p.ref->BaseState(kCode);
        for (int r = 0; r < 8; ++r) {
            state.R[r] = rng();
        }
        state.R[8] = 0x26040000 + (rng() & 0xFF0u);
        state.R[9] = 0x06040100 + (rng() & 0xFF0u);
        state.R[10] = kMmio + (rng() & 0xF0u);
        state.R[11] = 0x26048000;
        state.R[12] = kCode + 2 * (rng() % kLength);
        state.PR = kCode + 2 * (rng() % kLength);
        state.SR = 0xF0 | (rng() & 1u);
        state.wbReg = static_cast<uint8_t>(rng() % 17);
        p.Load(state);

        INFO("seed 0x" << std::hex << seed << " program " << Hex(program));
        for (int step = 0; step < kSteps; ++step) {
            p.Step();
            if (!sh2test::DiffRigs(*p.ref, *p.jit).empty()) {
                break; // Pair::Step already reported the difference
            }
        }
    }
}
