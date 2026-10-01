#include <brimir/jit/frontend.hpp>

#include <ymir/hw/sh2/sh2_decode.hpp>

#include <cstdint>
#include <optional>

namespace brimir::jit {

namespace {

using ymir::sh2::DecodeTable;
using ymir::sh2::OpcodeType;

constexpr uint8_t kWbNone = 0xFF;
constexpr uint32_t kWbPRBit = 1u << 16;

constexpr uint32_t RegBit(uint32_t reg) {
    return 1u << reg;
}

uint32_t Rn(uint16_t instr) {
    return (instr >> 8) & 0xFu;
}

uint32_t Rm(uint16_t instr) {
    return (instr >> 4) & 0xFu;
}

uint32_t SImm8(uint16_t instr) {
    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(instr & 0xFF)));
}

uint32_t Disp8x2(uint16_t instr) {
    return SImm8(instr) << 1;
}

uint32_t Disp12x2(uint16_t instr) {
    int32_t disp = instr & 0xFFF;
    if (disp & 0x800) {
        disp -= 0x1000;
    }
    return static_cast<uint32_t>(disp) << 1;
}

// Field decoders matching sh2.cpp's DECODE_* macros (all displacements unsigned).
// DECODE_MD / DECODE_ND4: the register is in bits 7..4 (Rm(instr)); disp4 in bits 3..0.
uint32_t Disp4(uint16_t instr, uint32_t shift) {
    return static_cast<uint32_t>(instr & 0xFu) << shift;
}

// DECODE_D_U / DECODE_ND8: disp8 in bits 7..0.
uint32_t Disp8U(uint16_t instr, uint32_t shift) {
    return static_cast<uint32_t>(instr & 0xFFu) << shift;
}

// ALU, shift, compare, @(R0,GBR) logic, system-register transfer, multiply, divide-step, MAC and
// TAS opcodes (handler table sections 4, 5, 6 and 9.2-9.6); each has a Delay_ variant with
// identical semantics (LDC SR passes the delay-slot flag to SetSR, with the same net effect).
#define BRIMIR_JIT_ALU_OPS(X)                                                                                         \
    X(EXTSB) X(EXTSW) X(EXTUB) X(EXTUW) X(SWAPB) X(SWAPW) X(XTRCT) X(ADDC) X(ADDV) X(AND_R) X(AND_I) X(NEG) X(NEGC)  \
        X(NOT) X(OR_R) X(OR_I) X(ROTCL) X(ROTCR) X(ROTL) X(ROTR) X(SHAL) X(SHAR) X(SHLL) X(SHLL2) X(SHLL8)           \
            X(SHLL16) X(SHLR) X(SHLR2) X(SHLR8) X(SHLR16) X(SUB) X(SUBC) X(SUBV) X(XOR_R) X(XOR_I) X(CMP_EQ_I)       \
                X(CMP_GE) X(CMP_GT) X(CMP_HI) X(CMP_HS) X(CMP_PL) X(CMP_PZ) X(CMP_STR) X(TST_R) X(TST_I) X(CLRMAC)   \
                    X(AND_M) X(OR_M) X(XOR_M) X(TST_M) X(LDC_GBR_R) X(LDC_SR_R) X(LDC_VBR_R) X(LDS_MACH_R)           \
                        X(LDS_MACL_R) X(LDS_PR_R) X(STC_GBR_R) X(STC_SR_R) X(STC_VBR_R) X(STS_MACH_R)                \
                            X(STS_MACL_R) X(STS_PR_R) X(MUL) X(MULS) X(MULU) X(DMULS) X(DMULU) X(DIV0S)    \
                                X(DIV0U) X(DIV1) X(MACW) X(MACL) X(TAS)

// Maps a supported instruction (normal or delay-slot decode) to its base opcode.
std::optional<OpcodeType> BaseOp(OpcodeType op, bool delaySlot) {
    if (!delaySlot) {
        switch (op) {
        case OpcodeType::NOP:
        case OpcodeType::MOV_R:
        case OpcodeType::MOV_I:
        case OpcodeType::MOVB_L:
        case OpcodeType::MOVW_L:
        case OpcodeType::MOVL_L:
        case OpcodeType::MOVB_L0:
        case OpcodeType::MOVW_L0:
        case OpcodeType::MOVL_L0:
        case OpcodeType::MOVB_L4:
        case OpcodeType::MOVW_L4:
        case OpcodeType::MOVL_L4:
        case OpcodeType::MOVB_LG:
        case OpcodeType::MOVW_LG:
        case OpcodeType::MOVL_LG:
        case OpcodeType::MOVB_M:
        case OpcodeType::MOVW_M:
        case OpcodeType::MOVL_M:
        case OpcodeType::MOVB_P:
        case OpcodeType::MOVW_P:
        case OpcodeType::MOVL_P:
        case OpcodeType::MOVB_S:
        case OpcodeType::MOVW_S:
        case OpcodeType::MOVL_S:
        case OpcodeType::MOVB_S0:
        case OpcodeType::MOVW_S0:
        case OpcodeType::MOVL_S0:
        case OpcodeType::MOVB_S4:
        case OpcodeType::MOVW_S4:
        case OpcodeType::MOVL_S4:
        case OpcodeType::MOVB_SG:
        case OpcodeType::MOVW_SG:
        case OpcodeType::MOVL_SG:
        case OpcodeType::MOVW_I:
        case OpcodeType::MOVL_I:
        case OpcodeType::MOVA:
        case OpcodeType::MOVT:
        case OpcodeType::CLRT:
        case OpcodeType::SETT:
        case OpcodeType::ADD:
        case OpcodeType::ADD_I:
        case OpcodeType::CMP_EQ_R:
        case OpcodeType::DT:
#define BRIMIR_JIT_ALU_CASE(name) case OpcodeType::name:
            BRIMIR_JIT_ALU_OPS(BRIMIR_JIT_ALU_CASE)
#undef BRIMIR_JIT_ALU_CASE
            return op;
        default: return std::nullopt;
        }
    }
    switch (op) {
#define BRIMIR_JIT_ALU_DELAY_CASE(name)                                                                               \
    case OpcodeType::Delay_##name: return OpcodeType::name;
        BRIMIR_JIT_ALU_OPS(BRIMIR_JIT_ALU_DELAY_CASE)
#undef BRIMIR_JIT_ALU_DELAY_CASE
    case OpcodeType::Delay_NOP: return OpcodeType::NOP;
    case OpcodeType::Delay_MOV_R: return OpcodeType::MOV_R;
    case OpcodeType::Delay_MOV_I: return OpcodeType::MOV_I;
    case OpcodeType::Delay_MOVB_L: return OpcodeType::MOVB_L;
    case OpcodeType::Delay_MOVW_L: return OpcodeType::MOVW_L;
    case OpcodeType::Delay_MOVL_L: return OpcodeType::MOVL_L;
    case OpcodeType::Delay_MOVB_L0: return OpcodeType::MOVB_L0;
    case OpcodeType::Delay_MOVW_L0: return OpcodeType::MOVW_L0;
    case OpcodeType::Delay_MOVL_L0: return OpcodeType::MOVL_L0;
    case OpcodeType::Delay_MOVB_L4: return OpcodeType::MOVB_L4;
    case OpcodeType::Delay_MOVW_L4: return OpcodeType::MOVW_L4;
    case OpcodeType::Delay_MOVL_L4: return OpcodeType::MOVL_L4;
    case OpcodeType::Delay_MOVB_LG: return OpcodeType::MOVB_LG;
    case OpcodeType::Delay_MOVW_LG: return OpcodeType::MOVW_LG;
    case OpcodeType::Delay_MOVL_LG: return OpcodeType::MOVL_LG;
    case OpcodeType::Delay_MOVB_M: return OpcodeType::MOVB_M;
    case OpcodeType::Delay_MOVW_M: return OpcodeType::MOVW_M;
    case OpcodeType::Delay_MOVL_M: return OpcodeType::MOVL_M;
    case OpcodeType::Delay_MOVB_P: return OpcodeType::MOVB_P;
    case OpcodeType::Delay_MOVW_P: return OpcodeType::MOVW_P;
    case OpcodeType::Delay_MOVL_P: return OpcodeType::MOVL_P;
    case OpcodeType::Delay_MOVB_S: return OpcodeType::MOVB_S;
    case OpcodeType::Delay_MOVW_S: return OpcodeType::MOVW_S;
    case OpcodeType::Delay_MOVL_S: return OpcodeType::MOVL_S;
    case OpcodeType::Delay_MOVB_S0: return OpcodeType::MOVB_S0;
    case OpcodeType::Delay_MOVW_S0: return OpcodeType::MOVW_S0;
    case OpcodeType::Delay_MOVL_S0: return OpcodeType::MOVL_S0;
    case OpcodeType::Delay_MOVB_S4: return OpcodeType::MOVB_S4;
    case OpcodeType::Delay_MOVW_S4: return OpcodeType::MOVW_S4;
    case OpcodeType::Delay_MOVL_S4: return OpcodeType::MOVL_S4;
    case OpcodeType::Delay_MOVB_SG: return OpcodeType::MOVB_SG;
    case OpcodeType::Delay_MOVW_SG: return OpcodeType::MOVW_SG;
    case OpcodeType::Delay_MOVL_SG: return OpcodeType::MOVL_SG;
    case OpcodeType::Delay_MOVW_I: return OpcodeType::MOVW_I;
    case OpcodeType::Delay_MOVL_I: return OpcodeType::MOVL_I;
    case OpcodeType::Delay_MOVA: return OpcodeType::MOVA;
    case OpcodeType::Delay_MOVT: return OpcodeType::MOVT;
    case OpcodeType::Delay_CLRT: return OpcodeType::CLRT;
    case OpcodeType::Delay_SETT: return OpcodeType::SETT;
    case OpcodeType::Delay_ADD: return OpcodeType::ADD;
    case OpcodeType::Delay_ADD_I: return OpcodeType::ADD_I;
    case OpcodeType::Delay_CMP_EQ_R: return OpcodeType::CMP_EQ_R;
    case OpcodeType::Delay_DT: return OpcodeType::DT;
    default: return std::nullopt;
    }
}

#undef BRIMIR_JIT_ALU_OPS

bool IsDelayedBranch(OpcodeType op) {
    switch (op) {
    case OpcodeType::BRA:
    case OpcodeType::BTS:
    case OpcodeType::BFS:
    case OpcodeType::JMP:
    case OpcodeType::RTS:
    case OpcodeType::BSR:
    case OpcodeType::BRAF:
    case OpcodeType::BSRF:
    case OpcodeType::JSR: return true;
    default: return false;
    }
}

// System-register transfers clear m_intrFlags.allow: no interrupt is accepted before the next
// instruction (handler table section 6, "intrAllow handling").
bool ClearsIntrAllow(OpcodeType base) {
    switch (base) {
    case OpcodeType::LDC_GBR_R:
    case OpcodeType::LDC_SR_R:
    case OpcodeType::LDC_VBR_R:
    case OpcodeType::LDS_MACH_R:
    case OpcodeType::LDS_MACL_R:
    case OpcodeType::LDS_PR_R:
    case OpcodeType::STC_GBR_R:
    case OpcodeType::STC_SR_R:
    case OpcodeType::STC_VBR_R:
    case OpcodeType::STS_MACH_R:
    case OpcodeType::STS_MACL_R:
    case OpcodeType::STS_PR_R: return true;
    default: return false;
    }
}

// Lowers a non-branch instruction. `retiredBefore` = instructions completed before this one.
// In a delay slot, `slotTarget` is the branch target when it is a compile-time constant
// (kNoValue for dynamic targets: the target is then read back with GetDelayTarget).
void LowerPlain(Builder &b, OpcodeType op, uint16_t instr, uint32_t pc, bool delaySlot, uint8_t retiredBefore,
                ValueId slotTarget = kNoValue) {
    const uint32_t n = Rn(instr);
    const uint32_t m = Rm(instr);
    const auto advance = [&] {
        if (delaySlot) {
            b.EndDelaySlot();
        }
    };
    const auto stall = [&](uint32_t mask) {
        if (mask != 0) {
            b.WbStall(mask);
        }
    };
    // Loads: the access cycles, then `preMask` stall (byte forms, and MOVW_L4's first stall), then
    // the bus-wait check (word/long only), the load, optional post-increment of `incReg`, and the
    // `postMask` stall after AdvancePC (word/long forms count write-back only on success).
    const auto load = [&](ValueId address, uint8_t size, uint32_t dst, uint32_t preMask, uint32_t postMask,
                          uint32_t incReg = 16) {
        b.AddAccessCycles(address, size, false);
        stall(preMask);
        if (size != 1) {
            b.ExitIfBusWait(address, size, false, pc, retiredBefore);
        }
        ValueId value = b.Load(address, size, false);
        if (size == 1) {
            value = b.SExt8(value);
        } else if (size == 2) {
            value = b.SExt16(value);
        }
        b.SetReg(dst, value);
        if (incReg < 16 && incReg != dst) {
            b.SetReg(incReg, b.Add(address, b.Const(size)));
        }
        advance();
        stall(postMask);
        b.SetWb(static_cast<uint8_t>(dst));
    };
    // Stores: byte forms stall before the write; word/long forms check bus wait and stall after
    // AdvancePC. `decReg` receives the (pre-decremented) address after the write.
    const auto store = [&](ValueId address, uint8_t size, ValueId value, uint32_t mask, uint32_t decReg = 16) {
        b.AddAccessCycles(address, size, true);
        if (size == 1) {
            stall(mask);
        } else {
            b.ExitIfBusWait(address, size, true, pc, retiredBefore);
        }
        b.Store(address, size, value);
        if (decReg < 16) {
            b.SetReg(decReg, address);
        }
        advance();
        if (size != 1) {
            stall(mask);
        }
        b.SetWb(kWbNone);
    };
    // "ALU template" tail: AdvancePC, WritebackCycles(mask) + 1, m_wbReg = None.
    const auto aluTail = [&](uint32_t mask) {
        advance();
        b.WbStall(mask);
        b.AddCycles(1);
        b.SetWb(kWbNone);
    };
    // The delay-slot branch target (PC-relative instructions in a slot use target - 2 as their PC).
    const auto delayTarget = [&] { return slotTarget != kNoValue ? slotTarget : b.GetDelayTarget(); };
    // (PC & ~3) + disp + 4 for MOVL_I / MOVA.
    const auto alignedPcRel = [&](uint32_t disp) {
        if (!delaySlot) {
            return b.Const((pc & ~3u) + disp + 4u);
        }
        return b.Add(b.And(b.Sub(delayTarget(), b.Const(2)), b.Const(~3u)), b.Const(disp + 4u));
    };

    switch (op) {
    case OpcodeType::NOP:
        advance();
        b.SetWb(kWbNone);
        b.AddCycles(1);
        break;
    case OpcodeType::MOV_R:
        b.SetReg(n, b.GetReg(m));
        advance();
        b.WbStall(RegBit(m) | RegBit(n));
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    case OpcodeType::MOV_I:
        b.SetReg(n, b.Const(SImm8(instr)));
        advance();
        b.WbStall(RegBit(n));
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    case OpcodeType::MOVB_L: {
        b.SyncCycles();
        const ValueId address = b.GetReg(m);
        b.AddAccessCycles(address, 1, false);
        b.WbStall(RegBit(m));
        b.SetReg(n, b.SExt8(b.Load(address, 1, false)));
        advance();
        b.SetWb(static_cast<uint8_t>(n));
        break;
    }
    case OpcodeType::MOVL_L: {
        b.SyncCycles();
        const ValueId address = b.GetReg(m);
        b.AddAccessCycles(address, 4, false);
        b.ExitIfBusWait(address, 4, false, pc, retiredBefore);
        b.SetReg(n, b.Load(address, 4, false));
        advance();
        b.WbStall(RegBit(m));
        b.SetWb(static_cast<uint8_t>(n));
        break;
    }
    case OpcodeType::MOVB_S: {
        b.SyncCycles();
        const ValueId address = b.GetReg(n);
        b.AddAccessCycles(address, 1, true);
        b.WbStall(RegBit(m) | RegBit(n));
        b.Store(address, 1, b.GetReg(m));
        advance();
        b.SetWb(kWbNone);
        break;
    }
    case OpcodeType::MOVL_S: {
        b.SyncCycles();
        const ValueId address = b.GetReg(n);
        b.AddAccessCycles(address, 4, true);
        b.ExitIfBusWait(address, 4, true, pc, retiredBefore);
        b.Store(address, 4, b.GetReg(m));
        advance();
        b.WbStall(RegBit(m) | RegBit(n));
        b.SetWb(kWbNone);
        break;
    }
    case OpcodeType::MOVW_L:
        b.SyncCycles();
        load(b.GetReg(m), 2, n, 0, RegBit(m));
        break;
    case OpcodeType::MOVB_L0:
        b.SyncCycles();
        load(b.Add(b.GetReg(m), b.GetReg(0)), 1, n, RegBit(m) | RegBit(0), 0);
        break;
    case OpcodeType::MOVW_L0:
        b.SyncCycles();
        load(b.Add(b.GetReg(m), b.GetReg(0)), 2, n, 0, RegBit(m) | RegBit(0));
        break;
    case OpcodeType::MOVL_L0:
        b.SyncCycles();
        load(b.Add(b.GetReg(m), b.GetReg(0)), 4, n, 0, RegBit(m) | RegBit(0));
        break;
    case OpcodeType::MOVB_L4: // no write-back stall at all
        b.SyncCycles();
        load(b.Add(b.GetReg(m), b.Const(Disp4(instr, 0))), 1, 0, 0, 0);
        break;
    case OpcodeType::MOVW_L4: // WritebackCycles(rm) counted before the wait check and again on success
        b.SyncCycles();
        load(b.Add(b.GetReg(m), b.Const(Disp4(instr, 1))), 2, 0, RegBit(m), RegBit(m));
        break;
    case OpcodeType::MOVL_L4:
        b.SyncCycles();
        load(b.Add(b.GetReg(m), b.Const(Disp4(instr, 2))), 4, n, 0, RegBit(m));
        break;
    case OpcodeType::MOVB_LG: // GBR forms have no write-back stall
        b.SyncCycles();
        load(b.Add(b.GetGBR(), b.Const(Disp8U(instr, 0))), 1, 0, 0, 0);
        break;
    case OpcodeType::MOVW_LG:
        b.SyncCycles();
        load(b.Add(b.GetGBR(), b.Const(Disp8U(instr, 1))), 2, 0, 0, 0);
        break;
    case OpcodeType::MOVL_LG:
        b.SyncCycles();
        load(b.Add(b.GetGBR(), b.Const(Disp8U(instr, 2))), 4, 0, 0, 0);
        break;
    case OpcodeType::MOVB_P:
        b.SyncCycles();
        load(b.GetReg(m), 1, n, RegBit(m), 0, m);
        break;
    case OpcodeType::MOVW_P:
        b.SyncCycles();
        load(b.GetReg(m), 2, n, 0, RegBit(m), m);
        break;
    case OpcodeType::MOVL_P:
        b.SyncCycles();
        load(b.GetReg(m), 4, n, 0, RegBit(m), m);
        break;
    case OpcodeType::MOVW_S:
        b.SyncCycles();
        store(b.GetReg(n), 2, b.GetReg(m), RegBit(m) | RegBit(n));
        break;
    case OpcodeType::MOVB_M:
    case OpcodeType::MOVW_M:
    case OpcodeType::MOVL_M: {
        const uint8_t size = op == OpcodeType::MOVB_M ? 1 : op == OpcodeType::MOVW_M ? 2 : 4;
        b.SyncCycles();
        // R[rm] is read before R[rn] is updated: with rn == rm the original value is stored.
        const ValueId value = b.GetReg(m);
        store(b.Sub(b.GetReg(n), b.Const(size)), size, value, RegBit(m) | RegBit(n), n);
        break;
    }
    case OpcodeType::MOVB_S0:
    case OpcodeType::MOVW_S0:
    case OpcodeType::MOVL_S0: { // stalls on (rn, R0), not rm
        const uint8_t size = op == OpcodeType::MOVB_S0 ? 1 : op == OpcodeType::MOVW_S0 ? 2 : 4;
        b.SyncCycles();
        store(b.Add(b.GetReg(n), b.GetReg(0)), size, b.GetReg(m), RegBit(n) | RegBit(0));
        break;
    }
    case OpcodeType::MOVB_S4:
    case OpcodeType::MOVW_S4: { // DECODE_ND4: Rn in bits 7..4
        const uint32_t shift = op == OpcodeType::MOVB_S4 ? 0 : 1;
        const uint32_t n4 = Rm(instr);
        b.SyncCycles();
        store(b.Add(b.GetReg(n4), b.Const(Disp4(instr, shift))), static_cast<uint8_t>(1u << shift), b.GetReg(0),
              RegBit(n4) | RegBit(0));
        break;
    }
    case OpcodeType::MOVL_S4:
        b.SyncCycles();
        store(b.Add(b.GetReg(n), b.Const(Disp4(instr, 2))), 4, b.GetReg(m), RegBit(m) | RegBit(n));
        break;
    case OpcodeType::MOVB_SG:
    case OpcodeType::MOVW_SG:
    case OpcodeType::MOVL_SG: {
        const uint32_t shift = op == OpcodeType::MOVB_SG ? 0 : op == OpcodeType::MOVW_SG ? 1 : 2;
        b.SyncCycles();
        store(b.Add(b.GetGBR(), b.Const(Disp8U(instr, shift))), static_cast<uint8_t>(1u << shift), b.GetReg(0),
              RegBit(0));
        break;
    }
    case OpcodeType::MOVW_I: {
        // PC + disp + 4, no alignment; in a slot PC = delay-slot target - 2.
        const uint32_t disp = Disp8U(instr, 1);
        b.SyncCycles();
        const ValueId address =
            delaySlot ? b.Add(delayTarget(), b.Const(disp + 2u)) : b.Const(pc + disp + 4u);
        b.AddAccessCycles(address, 2, false);
        b.SetReg(n, b.SExt16(b.Load(address, 2, true)));
        advance();
        b.SetWb(static_cast<uint8_t>(n));
        break;
    }
    case OpcodeType::MOVL_I: {
        b.SyncCycles();
        const ValueId address = alignedPcRel(Disp8U(instr, 2));
        b.AddAccessCycles(address, 4, false);
        b.SetReg(n, b.Load(address, 4, true));
        advance();
        b.SetWb(static_cast<uint8_t>(n));
        break;
    }
    case OpcodeType::MOVA:
        b.SetReg(0, alignedPcRel(Disp8U(instr, 2)));
        advance();
        b.WbStall(RegBit(0));
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    case OpcodeType::MOVT:
        b.SetReg(n, b.GetT());
        advance();
        b.WbStall(RegBit(n));
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    case OpcodeType::CLRT:
    case OpcodeType::SETT: // fixed 1 cycle, no write-back stall
        b.SetT(b.Const(op == OpcodeType::SETT ? 1 : 0));
        advance();
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    case OpcodeType::ADD:
        b.SetReg(n, b.Add(b.GetReg(n), b.GetReg(m)));
        advance();
        b.WbStall(RegBit(m) | RegBit(n));
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    case OpcodeType::ADD_I:
        b.SetReg(n, b.Add(b.GetReg(n), b.Const(SImm8(instr))));
        advance();
        b.WbStall(RegBit(n));
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    case OpcodeType::CMP_EQ_R:
        b.SetT(b.CmpEq(b.GetReg(n), b.GetReg(m)));
        advance();
        b.WbStall(RegBit(m) | RegBit(n));
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    case OpcodeType::DT: {
        const ValueId dec = b.Sub(b.GetReg(n), b.Const(1));
        b.SetReg(n, dec);
        b.SetT(b.CmpEq(dec, b.Const(0)));
        advance();
        b.WbStall(RegBit(n));
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    }

    // Register-only ALU ops (handler table section 4): the result first, then AdvancePC, then
    // WritebackCycles(mask) + 1 and m_wbReg = None.
    case OpcodeType::EXTSB: b.SetReg(n, b.SExt8(b.GetReg(m))); aluTail(RegBit(m) | RegBit(n)); break;
    case OpcodeType::EXTSW: b.SetReg(n, b.SExt16(b.GetReg(m))); aluTail(RegBit(m) | RegBit(n)); break;
    case OpcodeType::EXTUB: b.SetReg(n, b.And(b.GetReg(m), b.Const(0xFF))); aluTail(RegBit(m) | RegBit(n)); break;
    case OpcodeType::EXTUW:
        b.SetReg(n, b.And(b.GetReg(m), b.Const(0xFFFF)));
        aluTail(RegBit(m) | RegBit(n));
        break;
    case OpcodeType::SWAPB: {
        const ValueId x = b.GetReg(m);
        const ValueId low = b.Or(b.And(b.Shr(x, 8), b.Const(0xFF)), b.Shl(b.And(x, b.Const(0xFF)), 8));
        b.SetReg(n, b.Or(low, b.And(x, b.Const(0xFFFF0000u))));
        aluTail(RegBit(m) | RegBit(n));
        break;
    }
    case OpcodeType::SWAPW: {
        const ValueId x = b.GetReg(m);
        b.SetReg(n, b.Or(b.Shl(x, 16), b.Shr(x, 16)));
        aluTail(RegBit(m) | RegBit(n));
        break;
    }
    case OpcodeType::XTRCT:
        b.SetReg(n, b.Or(b.Shr(b.GetReg(n), 16), b.Shl(b.GetReg(m), 16)));
        aluTail(RegBit(m) | RegBit(n));
        break;
    case OpcodeType::ADDC: {
        // tmp1 = Rn + Rm; Rn = tmp1 + T; T = (tmp0 > tmp1) || (tmp1 > Rn)
        const ValueId a = b.GetReg(n);
        const ValueId t1 = b.Add(a, b.GetReg(m));
        const ValueId r = b.Add(t1, b.GetT());
        b.SetReg(n, r);
        b.SetT(b.Or(b.CmpGtU(a, t1), b.CmpGtU(t1, r)));
        aluTail(RegBit(m) | RegBit(n));
        break;
    }
    case OpcodeType::ADDV:
    case OpcodeType::SUBV: {
        // dst/src = sign bits before; T = (src == dst [ADDV] / src != dst [SUBV]) & (sign(result) ^ dst)
        const ValueId a = b.GetReg(n);
        const ValueId c = b.GetReg(m);
        const ValueId r = op == OpcodeType::ADDV ? b.Add(a, c) : b.Sub(a, c);
        b.SetReg(n, r);
        const ValueId d = b.Shr(a, 31);
        const ValueId s = b.Shr(c, 31);
        const ValueId cond = op == OpcodeType::ADDV ? b.CmpEq(s, d) : b.Xor(s, d);
        b.SetT(b.And(cond, b.Xor(b.Shr(r, 31), d)));
        aluTail(RegBit(m) | RegBit(n));
        break;
    }
    case OpcodeType::AND_R: b.SetReg(n, b.And(b.GetReg(n), b.GetReg(m))); aluTail(RegBit(m) | RegBit(n)); break;
    case OpcodeType::OR_R: b.SetReg(n, b.Or(b.GetReg(n), b.GetReg(m))); aluTail(RegBit(m) | RegBit(n)); break;
    case OpcodeType::XOR_R: b.SetReg(n, b.Xor(b.GetReg(n), b.GetReg(m))); aluTail(RegBit(m) | RegBit(n)); break;
    case OpcodeType::AND_I: b.SetReg(0, b.And(b.GetReg(0), b.Const(instr & 0xFFu))); aluTail(RegBit(0)); break;
    case OpcodeType::OR_I: b.SetReg(0, b.Or(b.GetReg(0), b.Const(instr & 0xFFu))); aluTail(RegBit(0)); break;
    case OpcodeType::XOR_I: b.SetReg(0, b.Xor(b.GetReg(0), b.Const(instr & 0xFFu))); aluTail(RegBit(0)); break;
    case OpcodeType::NEG: b.SetReg(n, b.Sub(b.Const(0), b.GetReg(m))); aluTail(RegBit(m) | RegBit(n)); break;
    case OpcodeType::NEGC: {
        // tmp = -Rm; Rn = tmp - T; T = (0 < tmp) || (tmp < Rn)
        const ValueId tmp = b.Sub(b.Const(0), b.GetReg(m));
        const ValueId r = b.Sub(tmp, b.GetT());
        b.SetReg(n, r);
        b.SetT(b.Or(b.CmpGtU(tmp, b.Const(0)), b.CmpGtU(r, tmp)));
        aluTail(RegBit(m) | RegBit(n));
        break;
    }
    case OpcodeType::NOT: b.SetReg(n, b.Not(b.GetReg(m))); aluTail(RegBit(m) | RegBit(n)); break;
    case OpcodeType::ROTCL: {
        const ValueId x = b.GetReg(n);
        b.SetReg(n, b.Or(b.Shl(x, 1), b.GetT()));
        b.SetT(b.Shr(x, 31));
        aluTail(RegBit(n));
        break;
    }
    case OpcodeType::ROTCR: {
        const ValueId x = b.GetReg(n);
        b.SetReg(n, b.Or(b.Shr(x, 1), b.Shl(b.GetT(), 31)));
        b.SetT(b.And(x, b.Const(1)));
        aluTail(RegBit(n));
        break;
    }
    case OpcodeType::ROTL: {
        const ValueId x = b.GetReg(n);
        const ValueId msb = b.Shr(x, 31);
        b.SetT(msb);
        b.SetReg(n, b.Or(b.Shl(x, 1), msb));
        aluTail(RegBit(n));
        break;
    }
    case OpcodeType::ROTR: {
        const ValueId x = b.GetReg(n);
        b.SetT(b.And(x, b.Const(1)));
        b.SetReg(n, b.Or(b.Shr(x, 1), b.Shl(x, 31)));
        aluTail(RegBit(n));
        break;
    }
    case OpcodeType::SHAL:
    case OpcodeType::SHLL: { // byte-identical handlers
        const ValueId x = b.GetReg(n);
        b.SetT(b.Shr(x, 31));
        b.SetReg(n, b.Shl(x, 1));
        aluTail(RegBit(n));
        break;
    }
    case OpcodeType::SHAR:
    case OpcodeType::SHLR: {
        const ValueId x = b.GetReg(n);
        b.SetT(b.And(x, b.Const(1)));
        b.SetReg(n, op == OpcodeType::SHAR ? b.Sar(x, 1) : b.Shr(x, 1));
        aluTail(RegBit(n));
        break;
    }
    case OpcodeType::SHLL2: b.SetReg(n, b.Shl(b.GetReg(n), 2)); aluTail(RegBit(n)); break;
    case OpcodeType::SHLL8: b.SetReg(n, b.Shl(b.GetReg(n), 8)); aluTail(RegBit(n)); break;
    case OpcodeType::SHLL16: b.SetReg(n, b.Shl(b.GetReg(n), 16)); aluTail(RegBit(n)); break;
    case OpcodeType::SHLR2: b.SetReg(n, b.Shr(b.GetReg(n), 2)); aluTail(RegBit(n)); break;
    case OpcodeType::SHLR8: b.SetReg(n, b.Shr(b.GetReg(n), 8)); aluTail(RegBit(n)); break;
    case OpcodeType::SHLR16: b.SetReg(n, b.Shr(b.GetReg(n), 16)); aluTail(RegBit(n)); break;
    case OpcodeType::SUB: b.SetReg(n, b.Sub(b.GetReg(n), b.GetReg(m))); aluTail(RegBit(m) | RegBit(n)); break;
    case OpcodeType::SUBC: {
        // tmp1 = Rn - Rm; Rn = tmp1 - T; T = (tmp0 < tmp1) || (tmp1 < Rn)
        const ValueId a = b.GetReg(n);
        const ValueId t1 = b.Sub(a, b.GetReg(m));
        const ValueId r = b.Sub(t1, b.GetT());
        b.SetReg(n, r);
        b.SetT(b.Or(b.CmpGtU(t1, a), b.CmpGtU(r, t1)));
        aluTail(RegBit(m) | RegBit(n));
        break;
    }
    case OpcodeType::CMP_EQ_I: b.SetT(b.CmpEq(b.GetReg(0), b.Const(SImm8(instr)))); aluTail(RegBit(0)); break;
    case OpcodeType::CMP_GE: b.SetT(b.CmpGeS(b.GetReg(n), b.GetReg(m))); aluTail(RegBit(m) | RegBit(n)); break;
    case OpcodeType::CMP_GT: b.SetT(b.CmpGtS(b.GetReg(n), b.GetReg(m))); aluTail(RegBit(m) | RegBit(n)); break;
    case OpcodeType::CMP_HI: b.SetT(b.CmpGtU(b.GetReg(n), b.GetReg(m))); aluTail(RegBit(m) | RegBit(n)); break;
    case OpcodeType::CMP_HS: b.SetT(b.CmpGeU(b.GetReg(n), b.GetReg(m))); aluTail(RegBit(m) | RegBit(n)); break;
    case OpcodeType::CMP_PL: b.SetT(b.CmpGtS(b.GetReg(n), b.Const(0))); aluTail(RegBit(n)); break;
    case OpcodeType::CMP_PZ: b.SetT(b.CmpGeS(b.GetReg(n), b.Const(0))); aluTail(RegBit(n)); break;
    case OpcodeType::CMP_STR: {
        // T = 1 iff any byte of Rm ^ Rn is zero.
        const ValueId t = b.Xor(b.GetReg(m), b.GetReg(n));
        const ValueId byteMask = b.Const(0xFF);
        const ValueId zero = b.Const(0);
        const auto zeroByte = [&](uint32_t shift) {
            const ValueId v = shift == 0 ? t : b.Shr(t, shift);
            return b.CmpEq(b.And(v, byteMask), zero);
        };
        b.SetT(b.Or(b.Or(zeroByte(24), zeroByte(16)), b.Or(zeroByte(8), zeroByte(0))));
        aluTail(RegBit(m) | RegBit(n));
        break;
    }
    case OpcodeType::TST_R:
        b.SetT(b.CmpEq(b.And(b.GetReg(n), b.GetReg(m)), b.Const(0)));
        aluTail(RegBit(m) | RegBit(n));
        break;
    case OpcodeType::TST_I:
        b.SetT(b.CmpEq(b.And(b.GetReg(0), b.Const(instr & 0xFFu)), b.Const(0)));
        aluTail(RegBit(0));
        break;
    case OpcodeType::CLRMAC: // fixed 1 cycle, no write-back stall
        b.SetMACH(b.Const(0));
        b.SetMACL(b.Const(0));
        advance();
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;

    // @(R0,GBR) logic (handler table section 5): no bus-wait check, byte accesses. The access-cycle
    // size differs per handler: AND_M uint8, OR_M uint16, XOR_M uint32 (read + write terms).
    case OpcodeType::AND_M:
    case OpcodeType::OR_M:
    case OpcodeType::XOR_M: {
        const uint8_t cycleSize = op == OpcodeType::AND_M ? 1 : op == OpcodeType::OR_M ? 2 : 4;
        const ValueId imm = b.Const(instr & 0xFFu);
        b.SyncCycles();
        const ValueId address = b.Add(b.GetGBR(), b.GetReg(0));
        b.AddAccessCycles(address, cycleSize, false);
        b.AddAccessCycles(address, cycleSize, true);
        b.WbStall(RegBit(0));
        b.AddCycles(1);
        const ValueId v = b.Load(address, 1, false);
        const ValueId r = op == OpcodeType::AND_M  ? b.And(v, imm)
                          : op == OpcodeType::OR_M ? b.Or(v, imm)
                                                   : b.Xor(v, imm);
        b.Store(address, 1, r);
        advance();
        b.SetWb(kWbNone);
        break;
    }
    case OpcodeType::TST_M: {
        b.SyncCycles();
        const ValueId address = b.Add(b.GetGBR(), b.GetReg(0));
        b.AddAccessCycles(address, 1, false);
        b.WbStall(RegBit(0));
        b.AddCycles(2);
        b.SetT(b.CmpEq(b.And(b.Load(address, 1, false), b.Const(instr & 0xFFu)), b.Const(0)));
        advance();
        b.SetWb(kWbNone);
        break;
    }

    // System-register transfers (handler table section 6). LDC/LDS use DECODE_M (Rm in bits 11..8,
    // i.e. `n` here). All clear interrupt-allow before AdvancePC; BuildBlock re-enables it after
    // the next instruction's boundary check.
    case OpcodeType::LDC_GBR_R: b.SetGBR(b.GetReg(n)); b.ClearIntrAllow(); aluTail(RegBit(n)); break;
    case OpcodeType::LDC_VBR_R: b.SetVBR(b.GetReg(n)); b.ClearIntrAllow(); aluTail(RegBit(n)); break;
    case OpcodeType::LDC_SR_R: // SetSR also clears allow and recomputes pending (false in a slot)
        b.SetSR(b.GetReg(n), delaySlot);
        aluTail(RegBit(n));
        break;
    case OpcodeType::LDS_MACH_R: b.SetMACH(b.GetReg(n)); b.ClearIntrAllow(); aluTail(RegBit(n)); break;
    case OpcodeType::LDS_MACL_R: b.SetMACL(b.GetReg(n)); b.ClearIntrAllow(); aluTail(RegBit(n)); break;
    case OpcodeType::LDS_PR_R: b.SetPR(b.GetReg(n)); b.ClearIntrAllow(); aluTail(RegBit(n) | kWbPRBit); break;
    case OpcodeType::STC_GBR_R: b.SetReg(n, b.GetGBR()); b.ClearIntrAllow(); aluTail(RegBit(n)); break;
    case OpcodeType::STC_VBR_R: b.SetReg(n, b.GetVBR()); b.ClearIntrAllow(); aluTail(RegBit(n)); break;
    case OpcodeType::STC_SR_R: b.SetReg(n, b.GetSR()); b.ClearIntrAllow(); aluTail(RegBit(n)); break;
    case OpcodeType::STS_PR_R: b.SetReg(n, b.GetPR()); b.ClearIntrAllow(); aluTail(RegBit(n) | kWbPRBit); break;
    case OpcodeType::STS_MACH_R:
    case OpcodeType::STS_MACL_R: // fixed 1 cycle, no write-back stall, m_wbReg = rn (load-like)
        b.SetReg(n, op == OpcodeType::STS_MACH_R ? b.GetMACH() : b.GetMACL());
        b.ClearIntrAllow();
        advance();
        b.AddCycles(1);
        b.SetWb(static_cast<uint8_t>(n));
        break;

    // Multiplies (handler table section 9.2): no multiplier latency is modelled. MUL/DMULx take
    // WritebackCycles(rm, rn) + 3, MULS/MULU the ALU template's + 1.
    case OpcodeType::MUL:
        b.SetMACL(b.Mul(b.GetReg(m), b.GetReg(n)));
        advance();
        b.WbStall(RegBit(m) | RegBit(n));
        b.AddCycles(3);
        b.SetWb(kWbNone);
        break;
    case OpcodeType::MULS:
        b.SetMACL(b.Mul(b.SExt16(b.GetReg(m)), b.SExt16(b.GetReg(n))));
        aluTail(RegBit(m) | RegBit(n));
        break;
    case OpcodeType::MULU: {
        const ValueId lo16 = b.Const(0xFFFF);
        b.SetMACL(b.Mul(b.And(b.GetReg(m), lo16), b.And(b.GetReg(n), lo16)));
        aluTail(RegBit(m) | RegBit(n));
        break;
    }
    case OpcodeType::DMULS:
    case OpcodeType::DMULU: {
        const ValueId x = b.GetReg(m);
        const ValueId y = b.GetReg(n);
        b.SetMACL(b.Mul(x, y));
        b.SetMACH(op == OpcodeType::DMULS ? b.MulHiS(x, y) : b.MulHiU(x, y));
        advance();
        b.WbStall(RegBit(m) | RegBit(n));
        b.AddCycles(3);
        b.SetWb(kWbNone);
        break;
    }

    // Divide steps (handler table sections 9.3 and 9.4). SR.M/Q/T are written through SetSRBits
    // (SetSR would clear interrupt-allow and recompute pending).
    case OpcodeType::DIV0S: {
        // M = Rm < 0; Q = Rn < 0; T = M != Q
        const ValueId mm = b.Shr(b.GetReg(m), 31);
        const ValueId qq = b.Shr(b.GetReg(n), 31);
        b.SetSRBits(b.Or(b.Or(b.Shl(mm, 9), b.Shl(qq, 8)), b.Xor(mm, qq)), 0x301);
        aluTail(RegBit(m) | RegBit(n));
        break;
    }
    case OpcodeType::DIV0U: // M = Q = T = 0; fixed 1 cycle, no write-back stall
        b.SetSRBits(b.Const(0), 0x301);
        advance();
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    case OpcodeType::DIV1: // with n == m, Rm is read after Rn was shifted (handled by Div1Step)
        b.SetReg(n, b.Div1(b.GetReg(n), b.GetReg(m), n == m));
        aluTail(RegBit(m) | RegBit(n));
        break;

    // MAC.W / MAC.L (handler table section 9.5): @Rn is read first and Rn post-incremented, then
    // @Rm (= the incremented Rn when n == m, chosen at compile time). No bus-wait check. Cycles:
    // both read accesses + WritebackCycles(rm, rn) + 1; one SyncCycles covers both reads.
    case OpcodeType::MACW:
    case OpcodeType::MACL: {
        const uint8_t size = op == OpcodeType::MACW ? 2 : 4;
        const ValueId inc = b.Const(size);
        const auto read = [&](ValueId address) {
            b.AddAccessCycles(address, size, false);
            const ValueId value = b.Load(address, size, false);
            return size == 2 ? b.SExt16(value) : value;
        };
        b.SyncCycles();
        const ValueId a2 = b.GetReg(n);
        const ValueId op2 = read(a2);
        const ValueId a2Next = b.Add(a2, inc);
        b.SetReg(n, a2Next);
        const ValueId a1 = n == m ? a2Next : b.GetReg(m);
        const ValueId op1 = read(a1);
        b.SetReg(m, b.Add(a1, inc));
        if (size == 2) {
            b.MacW(op1, op2);
        } else {
            b.MacL(op1, op2);
        }
        advance();
        b.WbStall(RegBit(m) | RegBit(n));
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    }

    // TAS (handler table section 9.6): AccessCyclesRMWByte + WritebackCycles(rn) + 4; the read
    // bypasses the cache (the JIT's loads always do), T = (byte == 0), byte |= 0x80. No bus lock
    // and no bus-wait check; Rn is unchanged.
    case OpcodeType::TAS: {
        b.SyncCycles();
        const ValueId address = b.GetReg(n);
        b.AddAccessCyclesRMWByte(address);
        b.WbStall(RegBit(n));
        b.AddCycles(4);
        const ValueId value = b.Load(address, 1, false);
        b.SetT(b.CmpEq(value, b.Const(0)));
        b.Store(address, 1, b.Or(value, b.Const(0x80)));
        advance();
        b.SetWb(kWbNone);
        break;
    }
    default: break; // callers only pass supported opcodes
    }
}

// Lowers the branch part of a delayed branch (the slot is lowered by the caller). Returns the branch
// target when it is a compile-time constant, kNoValue for dynamic targets.
ValueId LowerDelayedBranch(Builder &b, OpcodeType op, uint16_t instr, uint32_t pc, uint8_t retiredBefore) {
    switch (op) {
    case OpcodeType::BRA: {
        const ValueId target = b.Const(pc + Disp12x2(instr) + 4u);
        b.SetupDelaySlot(target);
        b.SetWb(kWbNone);
        b.AddCycles(2);
        return target;
    }
    case OpcodeType::BTS:
    case OpcodeType::BFS: {
        b.SetWb(kWbNone);
        const ValueId t = b.GetT();
        // Not-taken condition: BT/S is not taken when T == 0, BF/S when T == 1.
        const ValueId notTaken = op == OpcodeType::BTS ? b.CmpEq(t, b.Const(0)) : t;
        b.ExitIf(notTaken, pc + 2u, 1, false, static_cast<uint8_t>(retiredBefore + 1));
        const ValueId target = b.Const(pc + Disp8x2(instr) + 4u);
        b.SetupDelaySlot(target);
        b.AddCycles(2);
        return target;
    }
    case OpcodeType::JMP: {
        const uint32_t m = Rn(instr);
        b.SetupDelaySlot(b.GetReg(m));
        b.WbStall(RegBit(m));
        b.AddCycles(2);
        b.SetWb(kWbNone);
        break;
    }
    case OpcodeType::RTS:
        b.SetupDelaySlot(b.GetPR());
        b.WbStall(kWbPRBit);
        b.AddCycles(2);
        b.SetWb(kWbNone);
        break;
    // Calls write PR before the slot runs (the slot may read it). WritebackCycles compares register
    // indices only, so it is evaluated against the old m_wbReg after PR is written.
    case OpcodeType::BSR: {
        b.SetPR(b.Const(pc + 4u));
        const ValueId target = b.Const(pc + Disp12x2(instr) + 4u);
        b.SetupDelaySlot(target);
        b.WbStall(kWbPRBit);
        b.AddCycles(2);
        b.SetWb(kWbNone);
        return target;
    }
    case OpcodeType::BRAF:
    case OpcodeType::BSRF: { // DECODE_M: Rm in bits 11..8; target = PC + Rm + 4
        const uint32_t m = Rn(instr);
        const ValueId target = b.Add(b.GetReg(m), b.Const(pc + 4u));
        uint32_t mask = RegBit(m);
        if (op == OpcodeType::BSRF) {
            b.SetPR(b.Const(pc + 4u));
            mask |= kWbPRBit;
        }
        b.SetupDelaySlot(target);
        b.WbStall(mask);
        b.AddCycles(2);
        b.SetWb(kWbNone);
        break;
    }
    case OpcodeType::JSR: {
        const uint32_t m = Rn(instr);
        const ValueId target = b.GetReg(m);
        b.SetPR(b.Const(pc + 4u));
        b.SetupDelaySlot(target);
        b.WbStall(RegBit(m) | kWbPRBit);
        b.AddCycles(2);
        b.SetWb(kWbNone);
        break;
    }
    default: break;
    }
    return kNoValue;
}

} // namespace

bool IsCompilableAddress(uint32_t pc) {
    const uint32_t partition = pc >> 29u;
    return partition == 0b000 || partition == 0b001 || partition == 0b101;
}

Block BuildBlock(ymir::sh2::SH2JitContext &ctx, uint32_t startPC) {
    Block block;
    block.startPC = startPC;
    Builder b(block);

    const auto &table = DecodeTable::s_instance;
    const auto peek = [&](uint32_t address) { return ctx.peekInstruction(ctx.sh2, address); };
    const auto refillIfAligned = [&](uint32_t address) {
        if ((address & 2u) == 0) {
            b.Refill(address);
        }
    };
    // The interpreter checks the cycle budget and pending interrupts before every instruction;
    // the first instruction of a block is covered by the executor's own checks.
    // After an instruction that cleared interrupt-allow, the boundary check of the next one cannot
    // take an interrupt; InterpretNext then sets allow = true before executing it. When the
    // allow-clearing instruction ends the block, allow stays false and the executor's pre-entry
    // check plus its `intrAllow = true` at block entry do the same.
    bool allowCleared = false;
    const auto boundary = [&](uint32_t address, uint8_t retired) {
        if (retired > 0) {
            b.CheckBoundary(address, retired);
        }
        if (allowCleared) {
            b.SetIntrAllow();
            allowCleared = false;
        }
    };

    uint32_t pc = startPC;
    uint8_t count = 0;
    while (count < kMaxBlockInstructions && IsCompilableAddress(pc)) {
        const uint16_t instr = peek(pc);
        const OpcodeType op = table.opcodes[0][instr];

        if (IsDelayedBranch(op)) {
            if (count + 2 > kMaxBlockInstructions || !IsCompilableAddress(pc + 2)) {
                break;
            }
            const uint16_t slot = peek(pc + 2);
            const auto slotBase = BaseOp(table.opcodes[1][slot], true);
            if (!slotBase) {
                break;
            }
            block.guestOpcodes.push_back(instr);
            block.guestOpcodes.push_back(slot);
            boundary(pc, count);
            refillIfAligned(pc);
            const ValueId target = LowerDelayedBranch(b, op, instr, pc, count);
            b.CheckBoundary(pc + 2, static_cast<uint8_t>(count + 1));
            refillIfAligned(pc + 2);
            LowerPlain(b, *slotBase, slot, pc + 2, true, static_cast<uint8_t>(count + 1), target);
            b.ExitDynamic(static_cast<uint8_t>(count + 2));
            block.guestInstrCount = count + 2u;
            return block;
        }

        if (op == OpcodeType::BT || op == OpcodeType::BF) {
            block.guestOpcodes.push_back(instr);
            boundary(pc, count);
            refillIfAligned(pc);
            b.SetWb(kWbNone);
            const ValueId t = b.GetT();
            const ValueId taken = op == OpcodeType::BT ? t : b.CmpEq(t, b.Const(0));
            b.ExitIf(taken, pc + Disp8x2(instr) + 4u, 3, true, static_cast<uint8_t>(count + 1));
            b.AddCycles(1);
            b.Exit(pc + 2u, static_cast<uint8_t>(count + 1));
            block.guestInstrCount = count + 1u;
            return block;
        }

        const auto base = BaseOp(op, false);
        if (!base) {
            break;
        }
        block.guestOpcodes.push_back(instr);
        boundary(pc, count);
        refillIfAligned(pc);
        LowerPlain(b, *base, instr, pc, false, count);
        allowCleared = ClearsIntrAllow(*base);
        ++count;
        pc += 2;
    }

    if (count == 0) {
        // Interpreter fallback. Remember the opcode so a code change triggers a rebuild.
        block = Block{};
        block.startPC = startPC;
        if (IsCompilableAddress(startPC)) {
            block.guestOpcodes.push_back(peek(startPC));
        }
        return block;
    }

    b.Exit(pc, count);
    block.guestInstrCount = count;
    return block;
}

} // namespace brimir::jit
