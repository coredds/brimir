#include "jit_fuzz_programs.hpp"

#include "jit_opcode_specs.hpp"
#include "sh2_test_rig.hpp"

#include <array>
#include <random>
#include <string_view>

namespace sh2test {

namespace {

constexpr uint32_t kCode = kFuzzCode;
constexpr uint32_t kMmio = 0x22000000;
constexpr int kLength = 24; // program words before the SLEEP padding

// Encoders
uint16_t Nm(uint16_t base, uint32_t n, uint32_t m) {
    return static_cast<uint16_t>(base | (n << 8) | (m << 4));
}
uint16_t NImm(uint16_t base, uint32_t n, uint32_t imm) {
    return static_cast<uint16_t>(base | (n << 8) | (imm & 0xFF));
}
constexpr uint16_t kRts = 0x000B;
uint16_t MovR(uint32_t n, uint32_t m) { return Nm(0x6003, n, m); }
uint16_t MovI(uint32_t n, uint32_t imm) { return NImm(0xE000, n, imm); }
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

// Register roles keep every access and branch inside known memory: R0-R7 and R13-R15 data; R8-R11
// data addresses (never written); R12 branch register; GBR always holds one of the R8-R11
// addresses; PR a return target. LDC.L/LDS.L into GBR, SR, VBR and PR are restricted to keep these
// (see the Fmt::M case).
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
// register) and post-increment/pre-decrement (the base is written). The generator emits setup
// instructions right before them (`mov #imm,R0` or `mov Rbase,Rd`, plus a store for the restricted
// system-register loads, see FuzzInstr); the group is never split by a branch target or a delay slot.
bool NeedsSetup(jitspec::Addr a) {
    using jitspec::Addr;
    return a == Addr::RmR0 || a == Addr::RnR0 || a == Addr::GbrR0 || a == Addr::RnPreDec || a == Addr::RmPostInc ||
           a == Addr::MacPair;
}

// One random non-branch instruction from the compiled-opcode table, plus its setup instructions if
// it needs any (setup first, forming one group). Jitspec::Encode supplies the random fields; register fields are then
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
        // TAS @Rn: Rn is an address (read-modify-write, Rn itself is not written). STC.L/STS.L
        // @-Rn: Rn is a copy of an address register (it is written).
        if (spec.addr == Addr::RnPreDec) {
            w = WithHi(w, setupBase());
        } else {
            w = WithHi(w, spec.addr == Addr::Rn ? addrReg() : RemapDest(hi));
        }
        break;
    case Fmt::ND8:
    case Fmt::NI: w = WithHi(w, RemapDest(hi)); break;
    case Fmt::M: // LDC/LDS sources
        if (spec.addr == Addr::RmPostInc) {
            // LDC.L/LDS.L @Rm+: Rm is a copy of an address register. The loads that the register
            // roles constrain only load a value stored right before them through @-Rm (Rm ends up
            // back at the address register's value):
            //   ldc.l @Rm+,GBR: mov.l Raddr,@-Rm  (GBR = one of R8-R11, like LDC_GBR_R)
            //   ldc.l @Rm+,SR:  stc.l SR,@-Rm     (SR unchanged; LDC Rm,SR still randomizes it)
            //   ldc.l @Rm+,VBR: stc.l VBR,@-Rm    (VBR unchanged; LDC Rm,VBR still randomizes it)
            //   lds.l @Rm+,PR:  sts.l PR,@-Rm     (PR stays a return target)
            const uint32_t d = setupBase();
            if (name == "LDC_GBR_M") {
                setup.push_back(Nm(0x2006, d, addrReg())); // mov.l Raddr,@-Rd
            } else if (name == "LDC_SR_M") {
                setup.push_back(Nm(0x4003, d, 0)); // stc.l SR,@-Rd
            } else if (name == "LDC_VBR_M") {
                setup.push_back(Nm(0x4023, d, 0)); // stc.l VBR,@-Rd
            } else if (name == "LDS_PR_M") {
                setup.push_back(Nm(0x4022, d, 0)); // sts.l PR,@-Rd
            }
            w = WithHi(w, d);
        } else if (name == "LDC_GBR_R") {
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
        case Addr::MacPair: { // mac.x @Rd+,@Rd+ (n == m): both operands from one copied address
            const uint32_t d = setupBase();
            w = WithLo(WithHi(w, d), d);
            break;
        }
        default: break;
        }
        break;
    }
    setup.push_back(w);
    return setup;
}

// Non-branch opcodes, and the subset that needs no setup instruction (delay slots and the last
// program position hold only those).
struct Pools {
    std::vector<const jitspec::OpSpec *> nonBranch;
    std::vector<const jitspec::OpSpec *> singles;

    Pools() {
        for (const jitspec::OpSpec &spec : jitspec::CompiledOpcodes()) {
            // MAC.W/MAC.L (Addr::MacPair) are included: FuzzInstr emits them as `mov Rbase,Rd ;
            // mac.x @Rd+,@Rd+`, so no address register is written.
            if (spec.slotOk) {
                nonBranch.push_back(&spec);
                if (!NeedsSetup(spec.addr)) {
                    singles.push_back(&spec);
                }
            }
        }
    }
};

} // namespace

FuzzProgram MakeFuzzProgram(uint32_t seed) {
    static const Pools pools;
    const auto &nonBranch = pools.nonBranch;
    const auto &singles = pools.singles;

    FuzzProgram out;
    std::mt19937 rng(seed);
    out.busWaitEvery = (rng() & 1u) ? 3u : 0u;
    const bool absR12 = (rng() & 1u) != 0;                // JMP/JSR, else BRAF/BSRF
    const int relDisp = static_cast<int>(rng() % 13) - 6; // BRAF/BSRF: R12 = 2 * relDisp
    const auto isLdsPr = [](const jitspec::OpSpec *s) { return std::string_view(s->name) == "LDS_PR_R"; };

    // Pass 1: instructions, with branch displacements patched in pass 2 once the valid targets
    // (the first word of every instruction group) are known.
    enum class Br { None, Bt, Bf, Bts, Bfs, Bra, Bsr, Braf, Bsrf };
    std::vector<uint16_t> &program = out.words;
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
            // A setup group must fit before the end and never sits in a delay slot.
            const auto &pool = (prevDelayed || last) ? singles : nonBranch;
            const jitspec::OpSpec *spec = pool[rng() % pool.size()];
            while (!absR12 && isLdsPr(spec)) {
                spec = pool[rng() % pool.size()]; // R12 is not an absolute address here
            }
            const std::vector<uint16_t> words = FuzzInstr(*spec, rng);
            if (i + static_cast<int>(words.size()) > kLength) {
                continue; // a setup group must fit before the end: draw again
            }
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

    // A fresh rig with the program written: BaseState reads the fetch buffer from memory.
    Rig scratch;
    scratch.WriteCode(kCode, program);
    auto &state = out.state;
    state = scratch.BaseState(kCode);
    for (int r : {0, 1, 2, 3, 4, 5, 6, 7, 13, 14, 15}) {
        state.R[r] = rng();
    }
    state.R[8] = 0x26040000 + (rng() & 0xFF0u);
    state.R[9] = 0x06040100 + (rng() & 0xFF0u);
    state.R[10] = kMmio + (rng() & 0xF0u);
    state.R[11] = 0x26048000;
    state.GBR = state.R[8 + rng() % 4];
    state.R[12] = absR12 ? kCode + 2 * static_cast<uint32_t>(randomTarget()) : static_cast<uint32_t>(2 * relDisp);
    state.PR = kCode + 2 * static_cast<uint32_t>(randomTarget());
    state.SR = 0xF0 | (rng() & 1u);
    state.wbReg = static_cast<uint8_t>(rng() % 17);
    state.MACH = rng();
    state.MACL = rng();
    return out;
}

} // namespace sh2test
