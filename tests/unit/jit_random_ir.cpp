#include "jit_random_ir.hpp"

#include <vector>

namespace sh2test {

using brimir::jit::Block;
using brimir::jit::Builder;
using brimir::jit::ValueId;

namespace {

class Generator {
public:
    Generator(std::mt19937 &rng, Block &block)
        : m_rng(rng)
        , m_b(block) {}

    uint32_t Below(uint32_t n) { // uniform in [0, n)
        return std::uniform_int_distribution<uint32_t>(0, n - 1)(m_rng);
    }
    uint32_t Range(uint32_t lo, uint32_t hi) { // uniform in [lo, hi]
        return std::uniform_int_distribution<uint32_t>(lo, hi)(m_rng);
    }
    uint32_t Word() {
        switch (Below(4)) {
        case 0: return Below(16); // small values hit the sign/zero edge cases of compares and shifts
        case 1: return 0xFFFFFFFFu - Below(16);
        case 2: return (Below(2) != 0 ? 0x80000000u : 0x7FFFFFFFu) ^ Below(4);
        default: return static_cast<uint32_t>(m_rng());
        }
    }

    // An existing value: often an old one, so live ranges overlap and the allocator has to spill.
    ValueId Pick() {
        const auto n = static_cast<uint32_t>(m_pool.size());
        switch (Below(4)) {
        case 0: return m_pool[Below((n + 3) / 4)];              // oldest quarter
        case 1: return m_pool[n - 1 - Below(n < 4 ? n : 4)];    // the last few
        default: return m_pool[Below(n)];
        }
    }

    void Def(ValueId v) {
        m_pool.push_back(v);
    }

    // Defines one new value.
    void ValueOp() {
        if (m_pool.size() < 2 || Below(8) == 0) {
            Def(Below(2) != 0 ? m_b.Const(Word()) : m_b.GetReg(Below(16)));
            return;
        }
        const ValueId a = Pick();
        const ValueId b = Pick();
        switch (Below(30)) {
        case 0: Def(m_b.Const(Word())); break;
        case 1: Def(m_b.GetReg(Below(16))); break;
        case 2: Def(m_b.GetPR()); break;
        case 3: Def(m_b.GetT()); break;
        case 4: Def(m_b.GetGBR()); break;
        case 5: Def(m_b.GetVBR()); break;
        case 6: Def(m_b.GetSR()); break;
        case 7: Def(Below(2) != 0 ? m_b.GetMACH() : m_b.GetMACL()); break;
        case 8: Def(m_b.GetDelayTarget()); break;
        case 9: Def(m_b.Add(a, b)); break;
        case 10: Def(m_b.Sub(a, b)); break;
        case 11: Def(m_b.And(a, b)); break;
        case 12: Def(m_b.Or(a, b)); break;
        case 13: Def(m_b.Xor(a, b)); break;
        case 14: Def(m_b.Not(a)); break;
        case 15: Def(m_b.Shl(a, Range(1, 31))); break;
        case 16: Def(m_b.Shr(a, Range(1, 31))); break;
        case 17: Def(m_b.Sar(a, Range(1, 31))); break;
        case 18: Def(m_b.SExt8(a)); break;
        case 19: Def(m_b.SExt16(a)); break;
        case 20: Def(m_b.CmpEq(a, Below(4) == 0 ? a : b)); break;
        case 21: Def(m_b.CmpGtU(a, b)); break;
        case 22: Def(m_b.CmpGeU(a, Below(4) == 0 ? a : b)); break;
        case 23: Def(m_b.CmpGtS(a, b)); break;
        case 24: Def(m_b.CmpGeS(a, Below(4) == 0 ? a : b)); break;
        case 25: Def(m_b.Mul(a, b)); break;
        case 26: Def(m_b.MulHiS(a, b)); break;
        case 27: Def(m_b.MulHiU(a, b)); break;
        case 28: Def(m_b.Add(a, m_b.Const(Word()))); break;
        default: Def(m_b.Sub(a, b)); break;
        }
    }

    // Writes guest state from an existing value (or changes interrupt-allow).
    void SinkOp() {
        const ValueId a = Pick();
        switch (Below(11)) {
        case 0:
        case 1:
        case 2: m_b.SetReg(Below(16), a); break;
        case 3: m_b.SetPR(a); break;
        case 4: m_b.SetT(Below(2) != 0 ? a : m_b.CmpGtU(a, Pick())); break;
        case 5: m_b.SetGBR(a); break;
        case 6: m_b.SetVBR(a); break;
        case 7: Below(2) != 0 ? m_b.SetMACH(a) : m_b.SetMACL(a); break;
        case 8: Below(2) != 0 ? m_b.ClearIntrAllow() : m_b.SetIntrAllow(); break;
        case 9: {
            static constexpr uint32_t kMasks[] = {0x001, 0x002, 0x100, 0x200, 0x301, 0x303, 0x003, 0x300};
            m_b.SetSRBits(a, kMasks[Below(8)]);
            break;
        }
        default: m_b.SetReg(Below(16), a); break;
        }
    }

    void CycleOp() {
        switch (Below(5)) {
        case 0:
        case 1: {
            // Mostly small; rarely beyond INT32_MAX, which an imm32 add cannot encode.
            const uint32_t n = Below(50) == 0 ? 0x80000000u + Below(0x10000) : Below(5);
            m_b.AddCycles(n);
            break;
        }
        case 2: m_b.WbStall(Below(3) == 0 ? static_cast<uint32_t>(m_rng()) : 1u << Below(17)); break;
        case 3: m_b.SetWb(static_cast<uint8_t>(Below(4) == 0 ? 0xFF : Below(17))); break;
        default: m_b.SyncCycles(); break;
        }
    }

    std::mt19937 &m_rng;
    Builder m_b;
    std::vector<ValueId> m_pool;
};

} // namespace

Block RandomBlock(std::mt19937 &rng, uint32_t startPC, const RandomIrOptions &opt) {
    (void)opt; // see RandomIrOptions: no op behind calls/memory exists yet
    Block block;
    block.startPC = startPC;
    Generator g(rng, block);

    const uint32_t numOps = g.Range(20, 200);
    std::vector<uint32_t> exitIfAt;
    for (uint32_t i = g.Below(4); i > 0; --i) {
        exitIfAt.push_back(g.Range(4, numOps - 4));
    }

    uint32_t retired = 1; // the first guest instruction has no boundary check before it
    g.Def(g.m_b.GetReg(g.Below(16))); // Pick() needs a non-empty pool
    while (block.code.size() + 1 < numOps) {
        const auto pos = static_cast<uint32_t>(block.code.size());
        bool exitIfNow = false;
        for (uint32_t &at : exitIfAt) {
            if (at != 0 && pos >= at) {
                at = 0;
                exitIfNow = true;
            }
        }
        if (exitIfNow) {
            ValueId cond = g.Pick();
            if (g.Below(4) != 0) {
                const ValueId other = g.Pick();
                cond = g.Below(2) != 0 ? g.m_b.CmpGtU(cond, other) : g.m_b.CmpGeS(cond, other);
            }
            g.m_b.ExitIf(cond, startPC + 2 * g.Below(64), g.Below(4), false, static_cast<uint8_t>(retired));
            continue;
        }
        const uint32_t pick = g.Below(20);
        if (pick < 11) {
            g.ValueOp();
        } else if (pick < 15) {
            g.SinkOp();
        } else if (pick < 18) {
            g.CycleOp();
        } else if (retired < 200) {
            g.m_b.CheckBoundary(startPC + 2 * retired, static_cast<uint8_t>(retired));
            ++retired;
        }
    }
    // Keep a few old values live to the very end.
    for (uint32_t i = g.Below(4); i > 0; --i) {
        g.m_b.SetReg(g.Below(16), g.Pick());
    }
    if (g.Below(2) != 0) {
        g.m_b.Exit(startPC + 2 * retired, static_cast<uint8_t>(retired));
    } else {
        g.m_b.ExitDynamic(static_cast<uint8_t>(retired));
    }
    block.guestInstrCount = retired;
    return block;
}

} // namespace sh2test
