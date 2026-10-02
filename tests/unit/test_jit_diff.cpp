// Brimir - SH-2 JIT vs interpreter differential tests (per instruction)
// Licensed under GPL-3.0
//
// Every test runs the same code on two identical isolated SH-2s: one through
// the JIT executor, one through the interpreter, and requires identical state,
// memory, bus-wait query sequence and cycle totals.

#include "catch_amalgamated.hpp"
#include "jit_fuzz_programs.hpp"
#include "jit_test_backend.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/jit/executor.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using sh2test::kSleep;
using sh2test::Rig;

namespace {

constexpr uint32_t kCode = 0x06001000;
constexpr uint32_t kTarget = kCode + 0x100; // branch target area, filled with SLEEP
constexpr uint32_t kMmio = 0x22000000;
static_assert(kCode == sh2test::kFuzzCode, "the fuzz programs are written at kCode");

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
    brimir::jit::Executor exec{sh2test::TestBackend(), sh2test::kNativeOnFirstRun};
    bool lastStepMatched = true; // whether the most recent Step() found identical cycles and state

    Pair() = default;
    explicit Pair(brimir::jit::BackendKind kind)
        : exec{kind, sh2test::kNativeOnFirstRun} {}

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
    // TRAPA is never compiled (milestone 2A leaves TRAPA/RTE/SLEEP to the interpreter).
    constexpr uint32_t kVbr = 0x06008000;
    constexpr uint32_t kHandler = 0x06009000;
    Pair p;
    // trapa #0x20 (unsupported) -> handler: add #1,R0 ; sleep
    p.WriteCode(kCode, {0xC320, static_cast<uint16_t>(kSleep)});
    p.WriteCode(kHandler, {AddI(0, 1), static_cast<uint16_t>(kSleep)});
    p.Write32(kVbr + 0x20 * 4, kHandler);
    auto state = p.ref->BaseState(kCode);
    state.VBR = kVbr;
    state.R[15] = 0x0600F000;
    p.Load(state);
    p.Step();
    REQUIRE(p.exec.GetStats().interpreted == 1);
    p.Step();
    REQUIRE(p.exec.GetStats().blocksRun == 1);
}

TEST_CASE("Delayed branch with an unsupported slot ends the block before the branch", "[jit][diff]") {
    Pair p;
    FillTargetArea(p);
    // add #1,R0 ; bra kTarget ; sleep (SLEEP is never compiled, also not in a slot)
    p.WriteCode(kCode, {AddI(0, 1), Bra(125), static_cast<uint16_t>(kSleep)});
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
    brimir::jit::Executor exec{sh2test::TestBackend(), sh2test::kNativeOnFirstRun};
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
// written); GBR one of R8-R11 (LDC GBR,Rm and LDC.L @Rm+,GBR only load one of them); R12 the
// branch register; PR a return target (LDS.L @Rm+,PR only reloads PR). R12 is per program either
// an absolute program address (JMP/JSR, and LDS PR,R12) or a fixed displacement (BRAF/BSRF:
// target = PC + 4 + R12). The JIT side is always the IR executor; when the x64 backend is
// available, a third rig runs the program on an x64 executor and is compared against the IR rig.
TEST_CASE("JIT matches the interpreter on random programs", "[jit][diff][fuzz]") {
    constexpr int kPrograms = 300;
    constexpr int kSteps = 80;
    // Coverage lower bounds. Measured with all 300 programs passing (milestone 2A, MAC and the
    // restricted system-register loads included): steps=15284 blocksRun=15142 interpreted=142
    // compiles=1048, i.e. 99% of steps in blocks. kMinBlocksRun is ~65% of the measured count;
    // kMinBlockPercent is 90, leaving room for generator changes but failing if a regression sends
    // a meaningful share of steps back to the interpreter. The x64 rig (milestone 2B) measured
    // blocksRun=15142 nativeBlocksRun=15142 compileFallbacks=0 interpreted=142: exactly the IR rig's
    // counts, which the checks below require.
    constexpr uint64_t kMinBlocksRun = 9800;
    constexpr uint64_t kMinBlockPercent = 90;
    bool diverged = false;
    uint64_t totalSteps = 0;
    uint64_t totalBlocksRun = 0;
    uint64_t totalInterpreted = 0;
    uint64_t totalCompiles = 0;

    // Third rig: when the x64 backend is available, an x64 executor runs the same program and must
    // match the IR executor (Pair::exec, always BackendKind::Ir here) after every step: same ExitInfo,
    // same state, memory, bus-wait query sequence and peripherals.
    const bool withX64 = brimir::jit::IsBackendAvailable(brimir::jit::BackendKind::X64);
    uint64_t x64BlocksRun = 0;
    uint64_t x64NativeBlocksRun = 0;
    uint64_t x64CompileFallbacks = 0;
    uint64_t x64Interpreted = 0;

    for (int prog = 0; prog < kPrograms; ++prog) {
        const uint32_t seed = 0xF0220000u + static_cast<uint32_t>(prog);
        Pair p{brimir::jit::BackendKind::Ir};
        std::unique_ptr<Rig> x64Rig;
        std::unique_ptr<brimir::jit::Executor> x64Exec;
        if (withX64) {
            x64Rig = std::make_unique<Rig>();
            x64Exec = std::make_unique<brimir::jit::Executor>(brimir::jit::BackendKind::X64, sh2test::kNativeOnFirstRun);
        }
        const sh2test::FuzzProgram fuzz = sh2test::MakeFuzzProgram(seed);
        const std::vector<uint16_t> &program = fuzz.words;
        p.SetBusWaitEvery(fuzz.busWaitEvery);
        p.WriteCode(kCode, program);
        p.Load(fuzz.state);
        if (x64Rig) {
            x64Rig->mmio.busWaitEvery = fuzz.busWaitEvery;
            x64Rig->WriteCode(kCode, program);
            x64Rig->Load(fuzz.state);
        }

        INFO("seed 0x" << std::hex << seed << " program " << Hex(program));
        bool failed = false;
        for (int step = 0; step < kSteps; ++step) {
            INFO("step " << std::dec << step);
            const brimir::jit::ExitInfo irInfo = p.Step();
            ++totalSteps;
            if (!p.lastStepMatched) {
                failed = true; // Pair::Step already reported the difference
                break;
            }
            if (x64Exec) {
                const brimir::jit::ExitInfo x64Info = x64Exec->Step(x64Rig->sh2->GetJitContext());
                INFO("x64 vs IR");
                CHECK(x64Info.cycles == irInfo.cycles);
                CHECK(x64Info.retired == irInfo.retired);
                CHECK(x64Info.busWait == irInfo.busWait);
                CHECK(x64Info.aborted == irInfo.aborted);
                CHECK(x64Info.boundary == irInfo.boundary);
                const std::string diff = sh2test::DiffRigs(*p.jit, *x64Rig, true);
                INFO(diff);
                CHECK(diff.empty());
                const bool sameExit = x64Info.cycles == irInfo.cycles && x64Info.retired == irInfo.retired &&
                                      x64Info.busWait == irInfo.busWait && x64Info.aborted == irInfo.aborted &&
                                      x64Info.boundary == irInfo.boundary;
                if (!sameExit || !diff.empty()) {
                    failed = true;
                    break;
                }
            }
            if (p.ref->State().sleep && p.jit->State().sleep) {
                break; // both asleep: remaining steps would only re-run SLEEP
            }
        }
        totalBlocksRun += p.exec.GetStats().blocksRun;
        totalInterpreted += p.exec.GetStats().interpreted;
        totalCompiles += p.exec.Cache().Compiles();
        if (x64Exec) {
            x64BlocksRun += x64Exec->GetStats().blocksRun;
            x64NativeBlocksRun += x64Exec->GetStats().nativeBlocksRun;
            x64CompileFallbacks += x64Exec->GetStats().compileFallbacks;
            x64Interpreted += x64Exec->GetStats().interpreted;
        }
        if (failed) {
            diverged = true;
            break; // stop at the first failing program
        }
    }

    // The premise of the test: most steps ran compiled blocks rather than the interpreter.
    // Compile attempts include empty fallback blocks, so totalCompiles is informational only.
    WARN("fuzz coverage: steps=" << totalSteps << " blocksRun=" << totalBlocksRun
                                 << " interpreted=" << totalInterpreted << " compiles=" << totalCompiles);
    if (withX64) {
        WARN("fuzz x64 coverage: blocksRun=" << x64BlocksRun << " nativeBlocksRun=" << x64NativeBlocksRun
                                             << " compileFallbacks=" << x64CompileFallbacks
                                             << " interpreted=" << x64Interpreted);
    }
    if (!diverged) {
        // A divergence already failed the test; don't bury it under threshold failures.
        CHECK(totalBlocksRun >= kMinBlocksRun);
        CHECK(totalBlocksRun * 100 >= totalSteps * kMinBlockPercent);
        if (withX64) {
            // The x64 rig ran the same steps as the IR rig, every block natively.
            CHECK(x64CompileFallbacks == 0);
            CHECK(x64BlocksRun == totalBlocksRun);
            CHECK(x64NativeBlocksRun == x64BlocksRun);
            CHECK(x64Interpreted == totalInterpreted);
        }
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

// Handler table section 9.4: full 64/32 division sequences (dividend R2:R0, divisor R1). The
// 65-instruction sequence spans several 32-instruction blocks; Advance runs in small chunks so it
// also stops inside blocks.
TEST_CASE("Division sequences match the interpreter", "[jit][diff][exact]") {
    constexpr uint16_t kDiv0u = 0x0019;
    const auto div0s = [](uint32_t n, uint32_t m) { return Nm(0x2007, n, m); };
    const auto div1 = [](uint32_t n, uint32_t m) { return Nm(0x3004, n, m); };
    const auto rotcl = [](uint32_t n) { return static_cast<uint16_t>(0x4024 | (n << 8)); };
    static constexpr uint32_t kInputs[] = {0, 1, 7, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678};

    for (const bool isSigned : {false, true}) {
        // unsigned: div0u ; 32x (rotcl R0 ; div1 R1,R2) ; sleep
        // signed:   div0s R1,R2 ; 32x (rotcl R0 ; div1 R1,R2) ; sleep
        std::vector<uint16_t> program = {isSigned ? div0s(2, 1) : kDiv0u};
        for (int i = 0; i < 32; ++i) {
            program.push_back(rotcl(0));
            program.push_back(div1(2, 1));
        }
        program.push_back(static_cast<uint16_t>(kSleep));

        uint32_t combo = 0;
        for (const uint32_t dividend : kInputs) {
            for (const uint32_t high : kInputs) {
                for (const uint32_t divisor : kInputs) {
                    ++combo;
                    INFO((isSigned ? "signed" : "unsigned") << " R2:R0=" << std::hex << high << ":" << dividend
                                                            << " R1=" << divisor);
                    Pair p;
                    p.WriteCode(kCode, program);
                    auto state = p.ref->BaseState(kCode);
                    state.R[0] = dividend;
                    state.R[1] = divisor;
                    state.R[2] = high;
                    // Start from dirty T/Q/M so DIV0U/DIV0S must clear or set them.
                    state.SR = (state.SR & ~0x301u) | (combo & 1u) | ((combo & 6u) << 7);
                    p.Load(state);
                    p.jit->sh2->SetJitExecutor(&p.exec);

                    const uint64 chunk = 5 + combo % 13;
                    for (int iter = 0; iter < 64; ++iter) {
                        const uint64 refCycles = p.ref->sh2->Advance<false, false>(chunk);
                        const uint64 jitCycles = p.jit->sh2->Advance<false, false>(chunk);
                        INFO("chunk " << chunk << " iter " << iter);
                        REQUIRE(jitCycles == refCycles);
                        const std::string diff = sh2test::DiffRigs(*p.ref, *p.jit, true);
                        INFO(diff);
                        REQUIRE(diff.empty());
                        if (p.ref->State().sleep) {
                            break;
                        }
                    }
                    REQUIRE(p.ref->State().sleep);
                    REQUIRE(p.exec.GetStats().blocksRun >= 3); // compiled, across several blocks
                }
            }
        }
    }
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

// LDC Rm,SR in a delay slot unmasks the pending interrupt. A delay slot never takes an interrupt,
// and the slot itself clears interrupt-allow for the next instruction, so the interpreter runs the
// first instruction at the branch target before taking the interrupt.
TEST_CASE("LDC SR in a delay slot unmasks a pending interrupt", "[jit][diff][exact]") {
    constexpr uint32_t kBranchTarget = kCode + 0x20;
    Pair p;
    RaiseDivuOverflow(p);
    // kCode: bra kBranchTarget ; ldc R5,SR (slot) ; add #1,R3 ; sleep
    // bra at kCode: disp = (0x20 - 0 - 4) / 2 = 14.
    p.WriteCode(kCode, {Bra(14), 0x450E, AddI(3, 1), static_cast<uint16_t>(kSleep)});
    // kBranchTarget: add #1,R4 ; add #1,R4 ; add #1,R4 ; sleep
    p.WriteCode(kBranchTarget, {AddI(4, 1), AddI(4, 1), AddI(4, 1), static_cast<uint16_t>(kSleep)});
    auto state = p.ref->BaseState(kCode);
    state.SR = 0xF0; // mask 15: the level-15 DIVU interrupt is not pending yet
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
    REQUIRE(p.ref->State().R[3] == 0u); // the fall-through add never ran
    REQUIRE(p.ref->State().sleep);      // the handler ran
    // Interpreter: one instruction at the target runs (the slot's LDC SR cleared interrupt-allow),
    // then the interrupt is taken.
    REQUIRE(p.ref->State().R[4] == 1u);
    REQUIRE(p.ref->Read32(kIntrStack - 8) == kBranchTarget + 2); // stacked PC: after the first add
    REQUIRE(p.exec.GetStats().blocksRun > 0);
    // bra and its LDC SR slot form one compiled block.
    REQUIRE(brimir::jit::BuildBlock(p.jit->sh2->GetJitContext(), kCode).guestInstrCount == 2u);
}

// Handler table section 9.7 and section 9.x notes 7 and 10: ldc.l @R5+,SR loading 0 from RAM unmasks
// the pending interrupt; interrupt-allow is cleared, so exactly one following instruction runs.
TEST_CASE("LDC.L SR unmasking a pending interrupt", "[jit][diff][exact]") {
    constexpr uint32_t kSrData = 0x26040000;
    Pair p;
    RaiseDivuOverflow(p);
    p.Write32(kSrData, 0);
    // ldc.l @R5+,SR ; add #1,R3 ; add #1,R3 ; add #1,R3 ; sleep
    p.WriteCode(kCode, {0x4507, AddI(3, 1), AddI(3, 1), AddI(3, 1), static_cast<uint16_t>(kSleep)});
    auto state = p.ref->BaseState(kCode);
    state.SR = 0xF0; // mask 15: the level-15 DIVU interrupt is not pending yet
    state.VBR = kIntrVbr;
    state.R[3] = 0;
    state.R[5] = kSrData;
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
    REQUIRE(p.ref->State().R[5] == kSrData + 4);           // post-incremented
    REQUIRE(p.ref->State().R[3] == 1u);                    // exactly one add ran before the interrupt
    REQUIRE(p.ref->State().sleep);                         // the handler ran
    REQUIRE(p.ref->Read32(kIntrStack - 8) == kCode + 4);   // stacked PC: the second add
    REQUIRE(p.exec.GetStats().blocksRun > 0);
    // ldc.l and the adds share one compiled block, so the in-block interrupt-allow rule is exercised.
    REQUIRE(brimir::jit::BuildBlock(p.jit->sh2->GetJitContext(), kCode).guestInstrCount >= 2u);
}

// The same in a delay slot: the slot never takes an interrupt (EndDelaySlot recomputes pending with
// the new SR), and allow stays cleared for the first instruction at the target.
TEST_CASE("LDC.L SR in a delay slot unmasks a pending interrupt", "[jit][diff][exact]") {
    constexpr uint32_t kBranchTarget = kCode + 0x20;
    constexpr uint32_t kSrData = 0x06040010;
    Pair p;
    RaiseDivuOverflow(p);
    p.Write32(kSrData, 0);
    // kCode: bra kBranchTarget ; ldc.l @R5+,SR (slot) ; add #1,R3 ; sleep
    p.WriteCode(kCode, {Bra(14), 0x4507, AddI(3, 1), static_cast<uint16_t>(kSleep)});
    // kBranchTarget: add #1,R4 ; add #1,R4 ; add #1,R4 ; sleep
    p.WriteCode(kBranchTarget, {AddI(4, 1), AddI(4, 1), AddI(4, 1), static_cast<uint16_t>(kSleep)});
    auto state = p.ref->BaseState(kCode);
    state.SR = 0xF0;
    state.VBR = kIntrVbr;
    state.R[3] = 0;
    state.R[4] = 0;
    state.R[5] = kSrData;
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
    REQUIRE(p.ref->State().R[5] == kSrData + 4);
    REQUIRE(p.ref->State().R[3] == 0u); // the fall-through add never ran
    REQUIRE(p.ref->State().sleep);      // the handler ran
    REQUIRE(p.ref->State().R[4] == 1u); // one instruction at the target, then the interrupt
    REQUIRE(p.ref->Read32(kIntrStack - 8) == kBranchTarget + 2);
    REQUIRE(p.exec.GetStats().blocksRun > 0);
    REQUIRE(brimir::jit::BuildBlock(p.jit->sh2->GetJitContext(), kCode).guestInstrCount == 2u);
}

// Handler table section 9.7 and section 9.x note 8: lds.l @R1+,PR leaves m_wbReg = PR, so the RTS
// right after it pays the WB(PR) stall. Every cycle target also stops after the first instruction.
TEST_CASE("LDS.L PR write-back stall", "[jit][diff][exact]") {
    constexpr uint32_t kPrData = 0x26040020;
    constexpr uint8_t kWbPR = 0x10;
    bool sawWbPR = false;
    for (uint32_t target = 1; target <= 30; ++target) {
        INFO("target " << target);
        Pair p;
        FillTargetArea(p);
        p.Write32(kPrData, kTarget);
        // lds.l @R1+,PR ; rts ; nop
        p.WriteCode(kCode, {0x4126, kRts, kNop, static_cast<uint16_t>(kSleep)});
        auto state = p.ref->BaseState(kCode);
        state.R[1] = kPrData;
        state.wbReg = 0xFF;
        p.Load(state);
        p.jit->sh2->SetJitExecutor(&p.exec);

        const uint64 refCycles = p.ref->sh2->Advance<false, false>(target);
        const uint64 jitCycles = p.jit->sh2->Advance<false, false>(target);
        REQUIRE(jitCycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*p.ref, *p.jit, true);
        INFO(diff);
        REQUIRE(diff.empty());
        if (p.ref->State().PC == kCode + 2) { // stopped right after lds.l
            REQUIRE(p.ref->State().wbReg == kWbPR);
            REQUIRE(p.jit->State().wbReg == kWbPR);
            REQUIRE(p.ref->State().PR == kTarget);
            REQUIRE(p.ref->State().R[1] == kPrData + 4);
            sawWbPR = true;
        }
        if (target == 30) {
            REQUIRE(p.ref->State().sleep); // returned to the SLEEP-filled target area
            REQUIRE(p.exec.GetStats().blocksRun > 0);
        }
    }
    CHECK(sawWbPR);
}

namespace {

// Writes a big-endian value of `size` bytes (1, 2 or 4) at `address` on both rigs: RAM for the
// 0x06/0x26 areas, the MMIO page for 0x22.
void PokeBoth(Pair &p, uint32_t address, uint32_t size, uint32_t value) {
    for (Rig *rig : {p.ref.get(), p.jit.get()}) {
        for (uint32_t i = 0; i < size; ++i) {
            const uint32_t a = (address & ~(size - 1)) + i;
            const auto byte = static_cast<uint8_t>(value >> (8 * (size - 1 - i)));
            if (((a >> 24) & 0x7u) == 0x2u) { // bus 0x2000000-0x3FFFFFF: MMIO page
                rig->mmio.data[a & 0xFFFFu] = byte;
            } else {
                (*rig->ram)[a & (sh2test::kRamSize - 1)] = byte;
            }
        }
    }
}

uint8_t PeekByte(const Rig &rig, uint32_t address) {
    if (((address >> 24) & 0x7u) == 0x2u) {
        return rig.mmio.data[address & 0xFFFFu];
    }
    return (*rig.ram)[address & (sh2test::kRamSize - 1)];
}

} // namespace

// Handler table section 9.5 and section 9.x notes 1-3: MAC.W / MAC.L saturation and wrap edges,
// with S = 0 and S = 1, n == m, overlapping and misaligned operand addresses, and operands in
// cached RAM, cache-through RAM and MMIO.
TEST_CASE("MAC saturation edges match the interpreter", "[jit][diff][exact]") {
    struct MacCase {
        uint32_t mach, macl, op1, op2; // op1 = @Rm, op2 = @Rn (raw memory values)
    };
    // MAC.W: op values are 16-bit (sign-extended by the instruction).
    static constexpr MacCase kMacW[] = {
        {0x00000000, 0x7FFFFFFE, 0x0001, 0x0001}, // sum exactly 0x7FFFFFFF: no saturation
        {0x00000000, 0x7FFFFFFF, 0x0001, 0x0001}, // one past: saturate, MACH |= 1
        {0x00000000, 0x80000001, 0xFFFF, 0x0001}, // sum exactly -0x80000000: no saturation
        {0x00000000, 0x80000000, 0xFFFF, 0x0001}, // one below: saturate negative
        {0x00000001, 0x7FFFFFFF, 0x0002, 0x0003}, // MACH bit 0 already set
        {0xFFFF0000, 0x7FFFFFFF, 0x0001, 0x0001}, // negative MACH, saturation: not sign-updated
        {0xFFFF0000, 0x00000005, 0xFFFF, 0x0007}, // negative MACH, no saturation
        {0x12345678, 0x40000000, 0x8000, 0x8000}, // 0x8000 x 0x8000 = 2^30 -> 0x80000000
        {0x12345678, 0x3FFFFFFF, 0x8000, 0x8000}, // -> 0x7FFFFFFF exactly
        {0xFFFFFFFF, 0xFFFFFFFF, 0x0001, 0x0001}, // S=0: 64-bit wrap to 0
        {0x00000000, 0xFFFFFFFF, 0x0001, 0x0001}, // S=0: carry into MACH
        {0x00000005, 0x00000000, 0xFFFF, 0x0001}, // S=0: negative product borrows from MACH
        {0x80000000, 0x00000000, 0x7FFF, 0x8000}, // mixed signs
    };
    // MAC.L: MAC = MACH:MACL, saturation range is the signed 48-bit range.
    static constexpr MacCase kMacL[] = {
        {0x00007FFF, 0xFFFFFFFE, 0x00000001, 0x00000001}, // sum 0x00007FFFFFFFFFFF: no saturation
        {0x00007FFF, 0xFFFFFFFF, 0x00000001, 0x00000001}, // +1: saturate positive
        {0xFFFF8000, 0x00000001, 0xFFFFFFFF, 0x00000001}, // sum 0xFFFF800000000000: no saturation
        {0xFFFF8000, 0x00000000, 0xFFFFFFFF, 0x00000001}, // -1: saturate negative
        {0x00008000, 0x00000000, 0x00000000, 0xFFFFFFFF}, // out of range, zero product, negative operand
        {0xFFFF0000, 0x00000000, 0x00000000, 0x00000005}, // out of range below, product sign positive
        {0x00000000, 0x00000000, 0x80000000, 0x80000000}, // max product 2^62
        {0x00000000, 0x00000000, 0x80000000, 0x7FFFFFFF}, // most negative product
        {0xFFFFFFFF, 0xFFFFFFFF, 0x00000001, 0x00000001}, // S=0: full 64-bit wrap
        {0x00001234, 0x89ABCDEF, 0x12345678, 0xFEDCBA98}, // in range, mixed signs
    };
    // Operand layouts: where Rn / Rm point. Rm == Rn register (n == m) puts op2 at A and op1 at
    // A + size; overlapping (n != m, same address) reads op2 twice.
    enum class Layout { Distinct, SameReg, Overlap, Mmio, Misaligned };
    constexpr uint32_t kRn = 4;
    constexpr uint32_t kRm = 5;

    for (const bool isLong : {false, true}) {
        const uint32_t size = isLong ? 4 : 2;
        const auto cases = isLong ? std::span<const MacCase>(kMacL) : std::span<const MacCase>(kMacW);
        bool sawMacWSaturation = false;
        for (const MacCase &c : cases) {
            for (const uint32_t s : {0u, 1u}) {
                for (const Layout layout :
                     {Layout::Distinct, Layout::SameReg, Layout::Overlap, Layout::Mmio, Layout::Misaligned}) {
                    Pair p;
                    uint32_t n = kRn;
                    uint32_t m = kRm;
                    uint32_t addrN = 0x26040000;
                    uint32_t addrM = 0x06040100;
                    switch (layout) {
                    case Layout::Distinct: break;
                    case Layout::SameReg:
                        m = n;
                        addrN = 0x26040200;
                        addrM = addrN + size;
                        break;
                    case Layout::Overlap: addrM = addrN = 0x26040300; break;
                    case Layout::Mmio: addrN = kMmio + 0x40; addrM = 0x26040400; break;
                    case Layout::Misaligned: addrN = 0x26040501; addrM = 0x06040603; break;
                    }
                    PokeBoth(p, addrN, size, c.op2);
                    if (layout != Layout::Overlap) {
                        PokeBoth(p, addrM, size, c.op1);
                    }
                    const uint16_t mac = Nm(isLong ? 0x000F : 0x400F, n, m);
                    p.WriteCode(kCode, {mac, static_cast<uint16_t>(kSleep)});
                    auto state = p.ref->BaseState(kCode);
                    state.R[n] = addrN;
                    if (m != n) {
                        state.R[m] = layout == Layout::Overlap ? addrN : addrM;
                    }
                    state.SR = 0xF0 | (s << 1);
                    state.MACH = c.mach;
                    state.MACL = c.macl;
                    state.wbReg = static_cast<uint8_t>(layout == Layout::Distinct ? n : 0xFF);
                    p.Load(state);
                    p.jit->sh2->SetJitExecutor(&p.exec);

                    INFO((isLong ? "mac.l" : "mac.w") << " S=" << s << " layout " << static_cast<int>(layout)
                                                      << std::hex << " MACH=" << c.mach << " MACL=" << c.macl
                                                      << " op1=" << c.op1 << " op2=" << c.op2);
                    const uint64 refCycles = p.ref->sh2->Advance<false, false>(40);
                    const uint64 jitCycles = p.jit->sh2->Advance<false, false>(40);
                    REQUIRE(jitCycles == refCycles);
                    const std::string diff = sh2test::DiffRigs(*p.ref, *p.jit, true);
                    INFO(diff);
                    REQUIRE(diff.empty());
                    REQUIRE(p.ref->State().sleep);
                    REQUIRE(p.exec.GetStats().blocksRun >= 1); // the MAC ran compiled
                    const auto end = p.ref->State();
                    if (n == m) {
                        REQUIRE(end.R[n] == addrN + 2 * size);
                    } else {
                        REQUIRE(end.R[n] == state.R[n] + size);
                        REQUIRE(end.R[m] == state.R[m] + size);
                    }
                    if (!isLong && s == 1 && layout == Layout::Distinct && end.MACH != c.mach) {
                        sawMacWSaturation = true; // only saturation changes MACH with S = 1
                    }
                }
            }
        }
        if (!isLong) {
            CHECK(sawMacWSaturation);
        }
    }
}

// Handler table section 9.6 and section 9.x note 6: TAS on each value class, in cached RAM,
// cache-through RAM and MMIO (AccessCyclesRMWByte differs per partition), with and without a
// write-back stall on Rn.
TEST_CASE("TAS matches the interpreter for every partition", "[jit][diff][exact]") {
    struct Expect {
        uint8_t before, after;
        uint32_t t;
    };
    static constexpr Expect kValues[] = {{0x00, 0x80, 1}, {0x80, 0x80, 0}, {0x7F, 0xFF, 0}};
    static constexpr uint32_t kAddresses[] = {0x06040011, 0x26040022, kMmio + 0x33};
    constexpr uint32_t kRn = 3;
    for (const Expect &e : kValues) {
        for (const uint32_t address : kAddresses) {
            for (const uint8_t wb : {uint8_t{0xFF}, static_cast<uint8_t>(kRn)}) {
                Pair p;
                PokeBoth(p, address, 1, e.before);
                p.WriteCode(kCode, {static_cast<uint16_t>(0x401B | (kRn << 8)), static_cast<uint16_t>(kSleep)});
                auto state = p.ref->BaseState(kCode);
                state.R[kRn] = address;
                state.SR = 0xF0 | (e.t ^ 1u); // start with T opposite to the expected result
                state.wbReg = wb;
                p.Load(state);
                p.jit->sh2->SetJitExecutor(&p.exec);

                INFO("tas.b @R3 at " << std::hex << address << " byte " << uint32_t{e.before} << " wbReg "
                                     << uint32_t{wb});
                const uint64 refCycles = p.ref->sh2->Advance<false, false>(40);
                const uint64 jitCycles = p.jit->sh2->Advance<false, false>(40);
                REQUIRE(jitCycles == refCycles);
                const std::string diff = sh2test::DiffRigs(*p.ref, *p.jit, true);
                INFO(diff);
                REQUIRE(diff.empty());
                REQUIRE(p.ref->State().sleep);
                REQUIRE(p.exec.GetStats().blocksRun >= 1); // the TAS ran compiled
                REQUIRE(PeekByte(*p.ref, address) == e.after);
                REQUIRE((p.ref->State().SR & 1u) == e.t);
                REQUIRE(p.ref->State().R[kRn] == address); // Rn unchanged
            }
        }
    }
}

// ---- Fetch buffer: known refills (milestone 2C, design/sh2-x64-performance.md item 1) ----

TEST_CASE("Front end records the tail word, array fetches and known refills", "[jit][diff]") {
    Pair p;
    // Four instructions: the last one (kCode+6) is unaligned, so no refill reads past the block.
    p.WriteCode(kCode, {AddI(3, 1), AddI(3, 2), AddI(3, 3), AddI(3, 4), static_cast<uint16_t>(kSleep)});
    {
        const brimir::jit::Block block = brimir::jit::BuildBlock(p.jit->sh2->GetJitContext(), kCode);
        INFO(brimir::jit::PrintBlock(block));
        REQUIRE(brimir::jit::VerifyBlock(block).empty());
        REQUIRE(block.guestInstrCount == 4u);
        CHECK_FALSE(block.hasTailWord);
        CHECK(block.guestOpcodes.size() == 4u);
        CHECK(block.fetchFromArrays);
        uint32_t known = 0;
        for (const brimir::jit::Inst &inst : block.code) {
            if (inst.op == brimir::jit::Op::Refill) {
                CHECK(inst.flag);
                CHECK(inst.imm2 == p.jit->Read32(inst.imm));
                ++known;
            }
        }
        CHECK(known == 2u); // kCode and kCode+4
    }
    // From kCode+2: the last instruction (kCode+8) is aligned, so its refill reads the tail word.
    p.WriteCode(kCode + 8, {AddI(3, 5), static_cast<uint16_t>(kSleep)});
    {
        const brimir::jit::Block block = brimir::jit::BuildBlock(p.jit->sh2->GetJitContext(), kCode + 2);
        INFO(brimir::jit::PrintBlock(block));
        REQUIRE(brimir::jit::VerifyBlock(block).empty());
        REQUIRE(block.guestInstrCount == 4u);
        CHECK(block.hasTailWord);
        REQUIRE(block.guestOpcodes.size() == 5u);
        CHECK(block.guestOpcodes.back() == kSleep);
        uint32_t known = 0;
        for (const brimir::jit::Inst &inst : block.code) {
            if (inst.op == brimir::jit::Op::Refill) {
                CHECK(inst.flag);
                CHECK(inst.imm2 == p.jit->Read32(inst.imm));
                ++known;
            }
        }
        CHECK(known == 2u); // kCode+4 and kCode+8 (with the tail word)
    }
    // A taken BT/BF refill reads the target, outside the block: it stays a runtime refill.
    p.WriteCode(kCode, {CmpEq(1, 1), Bt(0x10), static_cast<uint16_t>(kSleep)});
    {
        const brimir::jit::Block block = brimir::jit::BuildBlock(p.jit->sh2->GetJitContext(), kCode);
        REQUIRE(brimir::jit::VerifyBlock(block).empty());
        for (const brimir::jit::Inst &inst : block.code) {
            if (inst.op == brimir::jit::Op::ExitIf) {
                CHECK(inst.flag); // refill at the taken target, at run time
            }
        }
    }
}

namespace {

// Runs Advance(target) on fresh rigs for every target in [1, maxTarget], on every backend, and
// requires identical state, memory, peripherals and cycles. Returns whether some run stopped at
// stopPC (so the fetch buffer after that refill was compared).
bool AdvanceSweep(const std::vector<uint16_t> &program, const ymir::savestate::SH2SaveState &base,
                  void (*setup)(Pair &), uint32_t maxTarget, uint32_t stopPC) {
    bool sawStop = false;
    for (const auto kind : sh2test::AvailableBackends()) {
        for (uint32_t target = 1; target <= maxTarget; ++target) {
            Pair p{kind};
            if (setup != nullptr) {
                setup(p);
            }
            p.WriteCode(kCode, program);
            auto state = base;
            state.fetchedOpcodes = p.ref->Read32(kCode);
            p.Load(state);
            p.jit->sh2->SetJitExecutor(&p.exec);
            const uint64 refCycles = p.ref->sh2->Advance<false, false>(target);
            const uint64 jitCycles = p.jit->sh2->Advance<false, false>(target);
            INFO("backend " << brimir::jit::BackendName(kind) << " target " << target);
            REQUIRE(jitCycles == refCycles);
            const std::string diff = sh2test::DiffRigs(*p.ref, *p.jit, true);
            INFO(diff);
            REQUIRE(diff.empty());
            REQUIRE(p.exec.GetStats().blocksRun >= 1);
            sawStop = sawStop || p.jit->State().PC == stopPC;
        }
    }
    return sawStop;
}

// R4-R8 hold the same value, so the old and the new opcodes below (moves between them) have the
// same effect: a block that ran an overwritten instruction from its compiled copy (the accepted
// deviation of design/sh2-jit.md section 6.5) still matches the interpreter, and only the fetch
// buffer can differ.
constexpr uint16_t kOld0 = 0x6663; // mov R6,R6 at kCode+4
constexpr uint16_t kOld1 = 0x6773; // mov R7,R7 at kCode+6
constexpr uint16_t kNew0 = 0x6483; // mov R8,R4
constexpr uint16_t kNew1 = 0x6873; // mov R7,R8

ymir::savestate::SH2SaveState SmcState(const Rig &rig, uint32_t r1, uint32_t r2) {
    auto state = rig.BaseState(kCode);
    state.R[1] = r1;
    state.R[2] = r2;
    state.R[3] = 0;
    for (uint32_t r = 4; r <= 8; ++r) {
        state.R[r] = 0x5555AAAA;
    }
    return state;
}

} // namespace

// A store over the next instruction pair, then the refill that reads it. The refill must read the
// new words (codeDirty), through every alias of the code: the cached address, the cache-through
// address 0x26001000 and the RAM mirror 0x06101000. Targets stop at every boundary, so the fetch
// buffer is compared right after the refill at kCode+4 (stop at kCode+6).
TEST_CASE("Self-modifying store before a refill", "[jit][diff][exact]") {
    struct Store {
        uint16_t opcode;
        uint32_t offset; // address offset from kCode
        uint32_t value;
    };
    const Store stores[] = {
        {MovLS(1, 2), 4, (uint32_t{kNew0} << 16) | kNew1}, // mov.l R2,@R1 over both words
        {Nm(0x2001, 1, 2), 6, kNew1},                       // mov.w R2,@R1 over the second word
        {MovBS(1, 2), 6, kNew1 >> 8},                       // mov.b R2,@R1 over its high byte
    };
    for (const Store &s : stores) {
        for (const uint32_t alias : {0x00000000u, 0x20000000u, 0x00100000u}) {
            INFO("store " << std::hex << s.opcode << " alias " << alias);
            const std::vector<uint16_t> program{s.opcode,       MovR(5, 5),  kOld0, kOld1, AddI(3, 1),
                                                static_cast<uint16_t>(kSleep)};
            Rig scratch;
            const auto base = SmcState(scratch, kCode + alias + s.offset, s.value);
            CHECK(AdvanceSweep(program, base, nullptr, 40, kCode + 6));
        }
    }
}

namespace {

// A device whose register write also rewrites the code at kCode+4 (on both rigs alike).
void PokeCodeOnWrite(Rig &rig, uint32_t address) {
    (void)address;
    rig.WriteCode(kCode + 4, {kNew0, kNew1});
}

void InstallPokeCodeOnWrite(Pair &p) {
    p.ref->mmio.onWrite = PokeCodeOnWrite;
    p.jit->mmio.onWrite = PokeCodeOnWrite;
}

} // namespace

// A write callback (a handler write) that modifies the block's code: the refill after it must
// read memory (codeDirty after every handler write).
TEST_CASE("Write callback that modifies code", "[jit][diff][exact]") {
    const std::vector<uint16_t> program{MovLS(1, 2), MovR(5, 5), kOld0, kOld1, AddI(3, 1),
                                        static_cast<uint16_t>(kSleep)};
    Rig scratch;
    const auto base = SmcState(scratch, kMmio + 0x40, 0x12345678);
    CHECK(AdvanceSweep(program, base, InstallPokeCodeOnWrite, 40, kCode + 6));
}

// The page holding a compiled block is remapped to another array with the same code (tail word
// included, so the entry check by opcode passes) but different data. The x64 block checks its
// page at entry, reports stale once and is recompiled; the IR backend checks the pages itself
// and runs the block. Both match the interpreter.
TEST_CASE("Remapped code page", "[jit][diff]") {
    constexpr uint32_t kData = 0x06002000; // same 64 KiB page as the code
    for (const auto kind : sh2test::AvailableBackends()) {
        INFO("backend " << brimir::jit::BackendName(kind));
        // Declared before the rigs: their buses point at these pages until the rigs are gone.
        std::array<std::unique_ptr<std::array<uint8_t, 0x10000>>, 2> pages;
        Pair p{kind};
        // add #1,R3 ; mov.l @R9,R2 ; add #1,R3 ; sleep (tail word of the 3-instruction block)
        p.WriteCode(kCode, {AddI(3, 1), MovLL(2, 9), AddI(3, 1), static_cast<uint16_t>(kSleep)});
        p.Write32(kData, 0x11111111);
        auto state = p.ref->BaseState(kCode);
        state.R[3] = 0;
        state.R[9] = kData;
        p.Load(state);
        REQUIRE(p.Step().retired == 3);
        REQUIRE(p.exec.Cache().Compiles() == 1);
        REQUIRE(p.jit->State().R[2] == 0x11111111u);

        // Remap bus 0x6000000-0x600FFFF (SH-2 0x06000000) on both rigs to a copy with new data.
        Rig *rigs[2] = {p.ref.get(), p.jit.get()};
        for (int i = 0; i < 2; ++i) {
            pages[i] = std::make_unique<std::array<uint8_t, 0x10000>>();
            std::copy_n(rigs[i]->ram->begin(), 0x10000, pages[i]->begin());
            (*pages[i])[0x2000] = 0x22; // kData & 0xFFFF, big-endian 0x22222222
            (*pages[i])[0x2001] = 0x22;
            (*pages[i])[0x2002] = 0x22;
            (*pages[i])[0x2003] = 0x22;
            rigs[i]->bus.MapArray(0x6000000, 0x600FFFF, *pages[i], true);
        }
        p.Load(state);
        REQUIRE(p.Step().retired == 3);
        CHECK(p.jit->State().R[2] == 0x22222222u);
        const bool native = kind != brimir::jit::BackendKind::Ir;
        CHECK(p.exec.GetStats().staleEntries == (native ? 1u : 0u));
        CHECK(p.exec.Cache().Compiles() == (native ? 2u : 1u));
        CHECK(p.exec.Cache().Invalidations() == (native ? 1u : 0u));
        CHECK(p.exec.GetStats().blocksRun == 2u);

        // The recompiled block is current: no further stale entries.
        p.Load(state);
        REQUIRE(p.Step().retired == 3);
        CHECK(p.exec.GetStats().staleEntries == (native ? 1u : 0u));
        CHECK(p.exec.Cache().Compiles() == (native ? 2u : 1u));
    }
}

namespace {

// A 64 KiB page served by read/write handlers over its own bytes (big-endian), with peeks over the
// same bytes. Counts the handler reads (instruction fetches and data loads).
struct HandlerPage {
    std::array<uint8_t, 0x10000> data{};
    uint32_t reads = 0;
};

uint32_t PageRead(HandlerPage &page, uint32_t address, uint32_t size, bool count) {
    if (count) {
        ++page.reads;
    }
    const uint32_t off = address & 0xFFFFu & ~(size - 1);
    uint32_t value = 0;
    for (uint32_t i = 0; i < size; ++i) {
        value = (value << 8) | page.data[off + i];
    }
    return value;
}

void PageWrite(HandlerPage &page, uint32_t address, uint32_t size, uint32_t value) {
    const uint32_t off = address & 0xFFFFu & ~(size - 1);
    for (uint32_t i = 0; i < size; ++i) {
        page.data[off + i] = static_cast<uint8_t>(value >> (8 * (size - 1 - i)));
    }
}

void MapHandlerPage(Rig &rig, HandlerPage &page, uint32_t start) {
    const uint32_t end = start + 0xFFFF;
    rig.bus.MapNormal(
        start, end, &page,
        [](uint32_t a, void *c) -> uint8_t { return static_cast<uint8_t>(PageRead(*static_cast<HandlerPage *>(c), a, 1, true)); },
        [](uint32_t a, void *c) -> uint16_t { return static_cast<uint16_t>(PageRead(*static_cast<HandlerPage *>(c), a, 2, true)); },
        [](uint32_t a, void *c) -> uint32_t { return PageRead(*static_cast<HandlerPage *>(c), a, 4, true); },
        [](uint32_t a, uint8_t v, void *c) { PageWrite(*static_cast<HandlerPage *>(c), a, 1, v); },
        [](uint32_t a, uint16_t v, void *c) { PageWrite(*static_cast<HandlerPage *>(c), a, 2, v); },
        [](uint32_t a, uint32_t v, void *c) { PageWrite(*static_cast<HandlerPage *>(c), a, 4, v); });
    rig.bus.MapSideEffectFree(
        start, end, &page,
        [](uint32_t a, void *c) -> uint8_t { return static_cast<uint8_t>(PageRead(*static_cast<HandlerPage *>(c), a, 1, false)); },
        [](uint32_t a, void *c) -> uint16_t { return static_cast<uint16_t>(PageRead(*static_cast<HandlerPage *>(c), a, 2, false)); },
        [](uint32_t a, void *c) -> uint32_t { return PageRead(*static_cast<HandlerPage *>(c), a, 4, false); });
}

} // namespace

// The page holding a compiled block (known refills, fetchFromArrays) is remapped from an array to a
// handler page with the same bytes. The IR backend's entry re-check of the pages fails, so it calls
// refillPipeline instead of storing known values; the x64 block reports stale once and is
// recompiled without fetchFromArrays. Both make exactly the interpreter's handler reads.
TEST_CASE("Code page remapped from an array to a handler page", "[jit][diff]") {
    constexpr uint32_t kData = 0x06002000; // same 64 KiB page as the code
    for (const auto kind : sh2test::AvailableBackends()) {
        INFO("backend " << brimir::jit::BackendName(kind));
        // Declared before the rigs: their buses point at these pages until the rigs are gone.
        std::array<std::unique_ptr<HandlerPage>, 2> pages;
        Pair p{kind};
        // add #1,R3 ; mov.l @R9,R2 ; add #1,R3 ; sleep (tail word of the 3-instruction block)
        p.WriteCode(kCode, {AddI(3, 1), MovLL(2, 9), AddI(3, 1), static_cast<uint16_t>(kSleep)});
        p.Write32(kData, 0x11111111);
        auto state = p.ref->BaseState(kCode);
        state.R[3] = 0;
        state.R[9] = kData;
        p.Load(state);
        REQUIRE(p.Step().retired == 3);
        REQUIRE(p.exec.Cache().Compiles() == 1);
        REQUIRE(brimir::jit::BuildBlock(p.jit->sh2->GetJitContext(), kCode).fetchFromArrays);

        // Remap bus 0x6000000-0x600FFFF (SH-2 0x06000000) on both rigs to handlers over a copy.
        Rig *rigs[2] = {p.ref.get(), p.jit.get()};
        for (int i = 0; i < 2; ++i) {
            pages[i] = std::make_unique<HandlerPage>();
            std::copy_n(rigs[i]->ram->begin(), 0x10000, pages[i]->data.begin());
            PageWrite(*pages[i], kData, 4, 0x22222222);
            MapHandlerPage(*rigs[i], *pages[i], 0x6000000);
        }
        p.Load(state);
        REQUIRE(p.Step().retired == 3);
        CHECK(p.jit->State().R[2] == 0x22222222u);
        CHECK(pages[0]->reads >= 3u);                // two refills and the load
        CHECK(pages[1]->reads == pages[0]->reads);   // no known refill value was used
        CHECK_FALSE(brimir::jit::BuildBlock(p.jit->sh2->GetJitContext(), kCode).fetchFromArrays);
        const bool native = kind != brimir::jit::BackendKind::Ir;
        // The cached block itself: x64 recompiled it without fetchFromArrays, as native code that is
        // off array pages and so not self-validating (the cache checks it). The IR block is the
        // original one; RunBlock re-checks the pages at every entry.
        const brimir::jit::CachedBlock *cached = p.exec.Cache().Find(kCode);
        REQUIRE(cached != nullptr);
        CHECK(cached->block.fetchFromArrays == !native);
        CHECK_FALSE(cached->code.selfValidating);
        CHECK((cached->code.entry != nullptr) == native);
        CHECK(p.exec.GetStats().staleEntries == (native ? 1u : 0u));
        CHECK(p.exec.Cache().Compiles() == (native ? 2u : 1u));
        CHECK(p.exec.Cache().Invalidations() == (native ? 1u : 0u));
        CHECK(p.exec.GetStats().blocksRun == 2u);

        // Again: the block is current (validated through the peek callback), no stale entry.
        p.Load(state);
        REQUIRE(p.Step().retired == 3);
        CHECK(pages[1]->reads == pages[0]->reads);
        CHECK(p.exec.GetStats().staleEntries == (native ? 1u : 0u));
        CHECK(p.exec.Cache().Compiles() == (native ? 2u : 1u));
    }
}