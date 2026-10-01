// Brimir - table-driven SH-2 JIT vs interpreter differential tests
// Licensed under GPL-3.0
//
// Every opcode in jitspec::CompiledOpcodes() is run with random operands on two identical
// isolated SH-2s, one through the JIT executor and one through the interpreter; state, memory,
// the MMIO access log and cycle totals must match.

#include "catch_amalgamated.hpp"
#include "jit_opcode_specs.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/jit/executor.hpp>

#include <array>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

using jitspec::OpSpec;
using sh2test::kSleep;
using sh2test::Rig;

namespace {

constexpr uint32_t kCode = 0x06001000;
constexpr uint32_t kTarget = kCode + 0x100; // branch target area, filled with SLEEP
constexpr uint16_t kNop = 0x0009;
constexpr uint16_t kRts = 0x000B;
constexpr uint16_t kSleepOp = static_cast<uint16_t>(kSleep);

uint16_t Bra(uint32_t d) { return static_cast<uint16_t>(0xA000 | (d & 0xFFF)); }
uint16_t Bts(uint32_t d) { return static_cast<uint16_t>(0x8D00 | (d & 0xFF)); }
uint16_t Jmp(uint32_t m) { return static_cast<uint16_t>(0x402B | (m << 8)); }
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

std::array<uint32_t, 16> RandomRegs(std::mt19937 &rng) {
    std::array<uint32_t, 16> regs{};
    for (auto &r : regs) {
        r = rng();
    }
    return regs;
}

uint8_t RandomWb(std::mt19937 &rng) {
    const uint32_t pick = rng() % 19;
    return pick < 17 ? static_cast<uint8_t>(pick) : uint8_t{0xFF};
}

void FillTargetArea(Pair &p) {
    p.WriteCode(kTarget - 0x10, std::vector<uint16_t>(0x40, kSleepOp));
}

// Random data after the test code, so PC-relative loads (MOVW_I/MOVL_I reach up to 0x400 bytes past
// PC, or past the delay-slot target) read distinct values and a wrong base address shows up.
void FillLiteralPool(Pair &p, std::mt19937 &rng) {
    std::vector<uint16_t> pool(0x300);
    for (auto &w : pool) {
        w = static_cast<uint16_t>(rng());
    }
    p.WriteCode(kCode + 8, pool);
}

// State for one random instance: registers and GBR from Encode, random T/Q/M, wbReg, PR and MAC.
ymir::savestate::SH2SaveState RandomState(Pair &p, std::mt19937 &rng, uint32_t pc,
                                          const std::array<uint32_t, 16> &regs, uint32_t gbr) {
    auto state = p.ref->BaseState(pc);
    state.R = regs;
    state.GBR = gbr;
    state.SR = 0xF0 | (rng() & 0x301u); // random T, Q and M (DIV0S/DIV0U/DIV1 use Q and M)
    state.wbReg = RandomWb(rng);
    state.PR = rng();
    state.MACH = rng();
    state.MACL = rng();
    return state;
}

} // namespace

TEST_CASE("Every compiled opcode matches the interpreter", "[jit][diff][opcodes]") {
    const auto specs = jitspec::CompiledOpcodes();
    for (size_t index = 0; index < specs.size(); ++index) {
        const OpSpec &spec = specs[index];
        if (!spec.slotOk) {
            continue; // branches: covered by the delay-slot test and test_jit_diff.cpp
        }
        Pair p;
        std::mt19937 rng(0x0DE50000u + static_cast<uint32_t>(index));
        bool failed = false;
        for (int iter = 0; iter < 150 && !failed; ++iter) {
            const uint32_t pc = kCode + (rng() & 1u) * 2; // exercise both fetch-buffer halves
            auto regs = RandomRegs(rng);
            uint32_t gbr = rng();
            const uint16_t instr = jitspec::Encode(spec, rng, regs, gbr);
            FillLiteralPool(p, rng);
            p.WriteCode(pc, {instr, kSleepOp});
            p.Load(RandomState(p, rng, pc, regs, gbr));

            INFO(spec.name << " instr " << Hex({instr}) << " at PC " << std::hex << pc << " iter " << std::dec
                           << iter);
            const uint64_t blocksBefore = p.exec.GetStats().blocksRun;
            const auto info = p.Step();
            CHECK(info.retired == 1);
            CHECK(p.exec.GetStats().blocksRun > blocksBefore); // compiled, not interpreted
            failed = !p.lastStepMatched || info.retired != 1 || p.exec.GetStats().blocksRun == blocksBefore;
        }
    }
}

TEST_CASE("ALU edge values match the interpreter", "[jit][diff][opcodes]") {
    // Handler table section 4: random registers rarely hit carry, overflow and byte-equality edges.
    static constexpr const char *kAluOps[] = {
        "MOVT",  "CLRT",   "SETT",   "EXTSB",  "EXTSW", "EXTUB", "EXTUW",  "SWAPB",    "SWAPW",  "XTRCT",
        "ADDC",  "ADDV",   "AND_R",  "AND_I",  "NEG",   "NEGC",  "NOT",    "OR_R",     "OR_I",   "ROTCL",
        "ROTCR", "ROTL",   "ROTR",   "SHAL",   "SHAR",  "SHLL",  "SHLL2",  "SHLL8",    "SHLL16", "SHLR",
        "SHLR2", "SHLR8",  "SHLR16", "SUB",    "SUBC",  "SUBV",  "XOR_R",  "XOR_I",    "CMP_EQ_I", "CMP_GE",
        "CMP_GT", "CMP_HI", "CMP_HS", "CMP_PL", "CMP_PZ", "CMP_STR", "TST_R", "TST_I",  "CLRMAC",
        // Handler table sections 9.2-9.4: multiplies and divide steps.
        "MUL",   "MULS",   "MULU",   "DMULS",  "DMULU", "DIV0S", "DIV0U", "DIV1",
    };
    // DIV0S/DIV0U/DIV1 read or write SR.Q and SR.M: run every (T, Q, M) combination for them.
    const auto usesQM = [](const std::string &name) { return name.rfind("DIV", 0) == 0; };
    // CMP/STR sets T when any byte lane of Rn ^ Rm is zero. Pairs that make exactly one lane equal:
    //   lane 1: 0x7FFFFFFF ^ 0xFF00FF00 = 0x80FF00FF;  lane 2: 0x00000001 ^ 0xFF00FF00 = 0xFF00FF01
    //   lane 3: 0x12345678 ^ 0x12FFFFFF = 0x00CBA987;  lane 0: 0x12345678 ^ 0xFFFFFF78 = 0xEDCBA900
    // (bits 31..24 are lane 3). kImms has one entry per kValues entry (indexed by the same bi).
    // MULS/MULU edges: 0x8000 / 0xFFFF in the low half, with dirty upper halves (0xABCD8000,
    // 0x5555FFFF) that the 16-bit multiplies must ignore.
    static constexpr uint32_t kValues[] = {0,          1,          0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
                                           0x0000FFFF, 0xFF00FF00, 0x12345678, 0x12FFFFFF, 0xFFFFFF78,
                                           0x00008000, 0xABCD8000, 0x5555FFFF};
    static constexpr uint32_t kImms[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0xF0, 0x0F, 0x78, 0x12, 0x34, 0x55, 0xAA, 0xC3};
    static_assert(std::size(kImms) == std::size(kValues));

    const auto specs = jitspec::CompiledOpcodes();
    for (size_t opIndex = 0; opIndex < std::size(kAluOps); ++opIndex) {
        const std::string name = kAluOps[opIndex];
        const OpSpec *spec = nullptr;
        for (const OpSpec &s : specs) {
            if (name == s.name) {
                spec = &s;
            }
        }
        INFO("opcode " << name);
        REQUIRE(spec != nullptr);

        Pair p;
        std::mt19937 rng(0x0DE80000u + static_cast<uint32_t>(opIndex));
        bool failed = false;
        // (a, b) pairs plus an aliased pass (b = -1: Rn == Rm) for two-register forms.
        for (size_t ai = 0; ai < std::size(kValues) && !failed; ++ai) {
            for (int bi = -1; bi < static_cast<int>(std::size(kValues)) && !failed; ++bi) {
                if (bi < 0 && spec->fmt != jitspec::Fmt::NM) {
                    continue;
                }
                if (bi > 0 && (spec->fmt == jitspec::Fmt::N || spec->fmt == jitspec::Fmt::Z)) {
                    continue; // single-operand forms: only `a` matters
                }
                // srBits: bit 0 = T, bit 1 = Q, bit 2 = M.
                const uint32_t srCombos = usesQM(name) ? 8u : 2u;
                for (uint32_t srBits = 0; srBits < srCombos && !failed; ++srBits) {
                    const uint32_t t = srBits & 1u;
                    const uint32_t qm = (srBits >> 1) << 8; // Q = bit 8, M = bit 9
                    auto regs = RandomRegs(rng);
                    const uint32_t a = kValues[ai];
                    const uint32_t n = rng() % 16;
                    uint32_t m = (n + 1 + rng() % 15) % 16;
                    uint32_t word = spec->base;
                    switch (spec->fmt) {
                    case jitspec::Fmt::Z: break;
                    case jitspec::Fmt::N: word |= n << 8; regs[n] = a; break;
                    case jitspec::Fmt::NM:
                        if (bi < 0) {
                            m = n;
                        }
                        word |= (n << 8) | (m << 4);
                        regs[m] = bi < 0 ? a : kValues[bi];
                        regs[n] = a;
                        break;
                    case jitspec::Fmt::I:
                        word |= kImms[bi < 0 ? 0 : bi];
                        regs[0] = a;
                        break;
                    default: FAIL("unexpected format for " << name); break;
                    }
                    const uint16_t instr = static_cast<uint16_t>(word);
                    const uint32_t pc = kCode + (rng() & 1u) * 2;
                    p.WriteCode(pc, {instr, kSleepOp});
                    auto state = RandomState(p, rng, pc, regs, rng());
                    state.SR = 0xF0 | qm | t;
                    p.Load(state);

                    std::string operands;
                    char buf[48];
                    switch (spec->fmt) {
                    case jitspec::Fmt::I: std::snprintf(buf, sizeof(buf), " R0=%X", regs[0]); break;
                    case jitspec::Fmt::N: std::snprintf(buf, sizeof(buf), " Rn=%X", regs[n]); break;
                    case jitspec::Fmt::NM: std::snprintf(buf, sizeof(buf), " Rn=%X Rm=%X", regs[n], regs[m]); break;
                    default: buf[0] = '\0'; break; // Z: no register operands
                    }
                    operands = buf;
                    INFO(name << " instr " << Hex({instr}) << operands << " T=" << t << " Q=" << ((qm >> 8) & 1u)
                              << " M=" << (qm >> 9));
                    const uint64_t blocksBefore = p.exec.GetStats().blocksRun;
                    const auto info = p.Step();
                    CHECK(info.retired == 1);
                    CHECK(p.exec.GetStats().blocksRun > blocksBefore);
                    failed = !p.lastStepMatched || info.retired != 1 || p.exec.GetStats().blocksRun == blocksBefore;
                }
            }
        }
    }
}

TEST_CASE("Every slot-capable opcode matches the interpreter in a delay slot", "[jit][diff][opcodes]") {
    enum class Branch { Bra, BtsTaken, BtsNotTaken, Jmp, Rts, Bsr, Braf, Bsrf, Jsr };
    const auto specs = jitspec::CompiledOpcodes();
    for (size_t index = 0; index < specs.size(); ++index) {
        const OpSpec &spec = specs[index];
        if (!spec.slotOk) {
            continue;
        }
        Pair p;
        std::mt19937 rng(0x0DE60000u + static_cast<uint32_t>(index));
        bool failed = false;
        for (Branch branch : {Branch::Bra, Branch::BtsTaken, Branch::BtsNotTaken, Branch::Jmp, Branch::Rts,
                              Branch::Bsr, Branch::Braf, Branch::Bsrf, Branch::Jsr}) {
            for (int iter = 0; iter < 20 && !failed; ++iter) {
                // Slot stores may hit the target area; restore it so every iteration branches onto SLEEP.
                FillLiteralPool(p, rng);
                FillTargetArea(p);
                // Branch position and target parity vary the PC-relative bases of MOVA/MOVW_I/MOVL_I
                // and the fetch-buffer refills.
                const uint32_t pc = kCode + (rng() & 1u) * 2;
                const uint32_t target = kTarget + (rng() & 1u) * 2;
                const uint32_t disp = (target - pc - 4) / 2;
                auto regs = RandomRegs(rng);
                uint32_t gbr = rng();
                const uint16_t slot = jitspec::Encode(spec, rng, regs, gbr);
                uint16_t br = 0;
                uint32_t pr = rng();
                uint32_t t = rng() & 1u;
                switch (branch) {
                case Branch::Bra: br = Bra(disp); break;
                case Branch::BtsTaken: br = Bts(disp); t = 1; break;
                case Branch::BtsNotTaken: br = Bts(disp); t = 0; break;
                case Branch::Jmp: {
                    const uint32_t m = rng() % 16;
                    regs[m] = target;
                    br = Jmp(m);
                    break;
                }
                case Branch::Rts: pr = target; br = kRts; break;
                case Branch::Bsr: br = Bsr(disp); break;
                case Branch::Braf:
                case Branch::Bsrf: {
                    const uint32_t m = rng() % 16;
                    regs[m] = target - pc - 4; // target = PC + Rm + 4
                    br = branch == Branch::Braf ? Braf(m) : Bsrf(m);
                    break;
                }
                case Branch::Jsr: {
                    const uint32_t m = rng() % 16;
                    regs[m] = target;
                    br = Jsr(m);
                    break;
                }
                }
                // Code is written before BaseState so the fetch buffer matches memory.
                p.WriteCode(kCode, {kNop, kNop});
                p.WriteCode(pc, {br, slot, kSleepOp});

                auto state = RandomState(p, rng, pc, regs, gbr);
                state.SR = 0xF0 | (state.SR & 0x300u) | t; // keep the random Q and M
                state.PR = pr;
                p.Load(state);

                INFO(spec.name << " branch/slot " << Hex({br, slot}) << " at PC " << std::hex << pc << " target "
                               << target << " iter " << std::dec << iter);
                const uint64_t blocksBefore = p.exec.GetStats().blocksRun;
                p.Step(); // branch (+ slot when taken)
                const bool first = p.lastStepMatched;
                CHECK(p.exec.GetStats().blocksRun > blocksBefore); // branch and slot compiled together
                p.Step(); // not-taken BT/S: the slot instruction as a normal instruction
                failed = !first || !p.lastStepMatched;
            }
        }
    }
}

TEST_CASE("Bus-wait retries match for every word and long access", "[jit][diff][opcodes]") {
    const auto specs = jitspec::CompiledOpcodes();
    for (size_t index = 0; index < specs.size(); ++index) {
        const OpSpec &spec = specs[index];
        if (spec.size < 2 || spec.addr == jitspec::Addr::None) {
            continue; // PC-relative loads never check bus wait and cannot be pointed at MMIO
        }
        for (uint32_t every : {1u, 2u, 3u}) {
            Pair p;
            FillTargetArea(p);
            p.SetBusWaitEvery(every);
            std::mt19937 rng(0x0DE70000u + static_cast<uint32_t>(index) * 4 + every);
            bool failed = false;
            for (int iter = 0; iter < 8 && !failed; ++iter) {
                auto regs = RandomRegs(rng);
                uint32_t gbr = rng();
                const uint16_t instr = jitspec::Encode(spec, rng, regs, gbr, true);
                // Odd instances run the access in a BRA delay slot.
                const bool inSlot = (iter & 1) != 0;
                std::vector<uint16_t> program;
                if (inSlot) {
                    program = {Bra((kTarget - kCode - 4) / 2), instr, kSleepOp};
                } else {
                    program = {instr, kSleepOp};
                }
                FillTargetArea(p);
                p.WriteCode(kCode, program);
                p.Load(RandomState(p, rng, kCode, regs, gbr));

                INFO(spec.name << " program " << Hex(program) << " busWaitEvery " << every << " iter " << iter);
                for (int step = 0; step < 6 && !failed; ++step) {
                    p.Step();
                    failed = !p.lastStepMatched;
                }
            }
        }
    }
}
