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

    // An address constant for a `size`-byte access (opt.memory decides which areas are used).
    uint32_t AddressImm(uint8_t size) {
        const uint32_t align = ~(static_cast<uint32_t>(size) - 1u);
        uint32_t address;
        if (!m_opt.memory) {
            address = 0x06000000u | (Below(0x100000) & align); // RAM, cached area
        } else {
            switch (Below(16)) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4: address = 0x06000000u | (Below(0x100000) & align); break; // RAM, cached area
            case 5:
            case 6:
            case 7: address = 0x26000000u | (Below(0x100000) & align); break; // RAM, cache-through
            case 8:
            case 9:
            case 10:
            case 11:
            case 12:
            case 13: address = 0x22000000u | (Below(0x10000) & align); break; // MMIO
            case 14: address = 0xFFFFFE10u + (Below(16) & align); break;     // FRT registers (I/O area)
            default:
                if (Below(2) != 0) {
                    // The block's own code, through the cached, cache-through or mirror alias
                    // (known refills must notice stores there).
                    static constexpr uint32_t kAliases[] = {0x00000000u, 0x20000000u, 0x00100000u};
                    address = ((m_startPC + Below(0x80)) & align) + kAliases[Below(3)];
                } else {
                    address = 0x22000000u | Below(0x10000); // MMIO, any alignment
                }
                break;
            }
            if (Below(32) == 0) {
                address |= 1u; // misaligned (odd) for word and long accesses
            }
        }
        return address;
    }

    // An address value: a constant, or computed at run time from an existing value.
    ValueId Address(uint8_t size) {
        const uint32_t imm = AddressImm(size);
        if (Below(4) != 0) {
            return m_b.Const(imm);
        }
        // Keep the area of imm, take low bits from a live value (aligned like imm).
        const uint32_t lowMask = (imm >> 24) == 0xFF ? 0xCu : 0xFFFCu;
        const ValueId low = m_b.And(Pick(), m_b.Const(lowMask & ~(static_cast<uint32_t>(size) - 1u)));
        return m_b.Or(low, m_b.Const(imm & ~lowMask));
    }

    uint8_t Size() {
        static constexpr uint8_t kSizes[] = {1, 2, 4};
        return kSizes[Below(3)];
    }

    // Ops that call helpers out of generated code (no memory accesses besides Refill's fetch).
    void CallOp(uint32_t startPC) {
        switch (Below(7)) {
        case 0: Def(m_b.Div1(Pick(), Pick(), Below(4) == 0)); break;
        case 1: m_b.MacW(m_b.SExt16(Pick()), m_b.SExt16(Pick())); break;
        case 2: m_b.MacL(Pick(), Pick()); break;
        case 3: m_b.SetSR(Pick(), false); break;
        case 4: m_b.AddAccessCyclesRMWByte(Address(1)); break;
        default: {
            // Mostly code near the block (as the front end refills), sometimes any test address.
            const uint32_t address = Below(4) != 0 ? startPC + 4 * Below(32) : AddressImm(4);
            m_b.Refill(address);
            break;
        }
        }
    }

    // Data accesses, their cycle lookups and bus-wait exits. `pc`/`retiredBefore` describe the
    // current guest instruction (for the bus-wait exit).
    void MemoryOp(uint32_t pc, uint8_t retiredBefore) {
        const uint8_t size = Size();
        const ValueId address = Address(size);
        switch (Below(6)) {
        case 0: m_b.AddAccessCycles(address, size, Below(2) != 0); break;
        case 1: m_b.ExitIfBusWait(address, size, Below(2) != 0, pc, retiredBefore); break;
        case 2: Def(m_b.Load(address, size, Below(8) == 0)); break;
        case 3: m_b.Store(address, size, Pick()); break;
        case 4: { // a front-end style load: cycles, bus wait, load
            m_b.AddAccessCycles(address, size, false);
            m_b.ExitIfBusWait(address, size, false, pc, retiredBefore);
            Def(m_b.Load(address, size, false));
            break;
        }
        default: { // a front-end style store
            m_b.AddAccessCycles(address, size, true);
            m_b.ExitIfBusWait(address, size, true, pc, retiredBefore);
            m_b.Store(address, size, Pick());
            break;
        }
        }
    }

    std::mt19937 &m_rng;
    Builder m_b;
    std::vector<ValueId> m_pool;
    RandomIrOptions m_opt;
    uint32_t m_startPC = 0;
};

} // namespace

Block RandomBlock(std::mt19937 &rng, uint32_t startPC, const RandomIrOptions &opt) {
    Block block;
    block.startPC = startPC;
    Generator g(rng, block);
    g.m_opt = opt;
    g.m_startPC = startPC;

    const uint32_t numOps = g.Range(20, 200);
    std::vector<uint32_t> exitIfAt;
    for (uint32_t i = g.Below(4); i > 0; --i) {
        exitIfAt.push_back(g.Range(4, numOps - 4));
    }

    uint32_t retired = 1; // the first guest instruction has no boundary check before it
    // The current guest instruction is number `retired - 1`, at startPC + 2 * (retired - 1).
    const auto currentPC = [&] { return startPC + 2 * (retired - 1); };
    // One random op of any enabled kind (no exits, no boundary checks).
    const auto bodyOp = [&] {
        const uint32_t pick = g.Below(opt.calls || opt.memory ? 26 : 18);
        if (pick < 11) {
            g.ValueOp();
        } else if (pick < 15) {
            g.SinkOp();
        } else if (pick < 18) {
            g.CycleOp();
        } else if (pick < 22 && opt.memory) {
            g.MemoryOp(currentPC(), static_cast<uint8_t>(retired - 1));
        } else if (opt.calls) {
            g.CallOp(startPC);
        } else {
            g.MemoryOp(currentPC(), static_cast<uint8_t>(retired - 1));
        }
    };

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
            // Taken cycles: mostly small; rarely beyond INT32_MAX (not an imm32).
            const uint32_t takenCycles = g.Below(50) == 0 ? 0x80000000u + g.Below(0x10000) : g.Below(4);
            const bool refill = opt.calls && g.Below(2) != 0;
            const uint32_t target = refill && opt.memory && g.Below(4) == 0 ? g.AddressImm(4) : startPC + 2 * g.Below(64);
            g.m_b.ExitIf(cond, target, takenCycles, refill, static_cast<uint8_t>(retired));
            continue;
        }
        if (g.Below(20) == 0 && retired < 200) {
            g.m_b.CheckBoundary(startPC + 2 * retired, static_cast<uint8_t>(retired));
            ++retired;
            if (opt.calls && g.Below(2) != 0 && (currentPC() & 2u) == 0) {
                g.m_b.Refill(currentPC()); // as the front end does before 4-byte aligned instructions
            }
            continue;
        }
        bodyOp();
    }

    // A delayed branch: SetupDelaySlot, the slot's boundary check, the slot instruction (which may
    // include SetSR in a delay slot) and EndDelaySlot, then ExitDynamic (as frontend.cpp emits).
    const bool delaySlot = opt.calls && g.Below(3) == 0 && retired < 200;
    if (delaySlot) {
        const ValueId target = g.Below(2) != 0 ? g.m_b.Const(startPC + 2 * g.Below(64)) : g.Pick();
        g.m_b.SetupDelaySlot(target);
        g.m_b.AddCycles(2);
        g.m_b.CheckBoundary(startPC + 2 * retired, static_cast<uint8_t>(retired));
        ++retired;
        if ((currentPC() & 2u) == 0) {
            g.m_b.Refill(currentPC());
        }
        for (uint32_t i = g.Below(6); i > 0; --i) {
            bodyOp();
        }
        if (g.Below(4) == 0) {
            g.m_b.SetSR(g.Pick(), true);
        }
        g.m_b.EndDelaySlot();
        for (uint32_t i = g.Below(3); i > 0; --i) {
            g.CycleOp();
        }
    }

    // Keep a few old values live to the very end.
    for (uint32_t i = g.Below(4); i > 0; --i) {
        g.m_b.SetReg(g.Below(16), g.Pick());
    }
    if (!delaySlot && g.Below(2) != 0) {
        g.m_b.Exit(startPC + 2 * retired, static_cast<uint8_t>(retired));
    } else {
        g.m_b.ExitDynamic(static_cast<uint8_t>(retired));
    }
    block.guestInstrCount = retired;

    // Guest code and known refills (with calls, most of the time): random opcodes, an optional
    // tail word, and most refills inside them marked known with their value. The caller writes
    // guestOpcodes to memory at startPC, as the entry check guarantees for real blocks.
    if (opt.calls && g.Below(4) != 0) {
        block.hasTailWord = g.Below(2) != 0;
        const uint32_t words = retired + (block.hasTailWord ? 1u : 0u);
        for (uint32_t i = 0; i < words; ++i) {
            block.guestOpcodes.push_back(static_cast<uint16_t>(g.Below(0x10000)));
        }
        block.fetchFromArrays = g.Below(8) != 0;
        for (brimir::jit::Inst &inst : block.code) {
            if (inst.op != brimir::jit::Op::Refill || inst.imm < startPC || (inst.imm & 3u) != 0 || g.Below(4) == 0) {
                continue;
            }
            const uint32_t index = (inst.imm - startPC) / 2;
            if (index + 1 < words) {
                inst.flag = true;
                inst.imm2 = (static_cast<uint32_t>(block.guestOpcodes[index]) << 16) | block.guestOpcodes[index + 1];
            }
        }
    }
    return block;
}

} // namespace sh2test
