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

// Maps a supported instruction (normal or delay-slot decode) to its base opcode.
std::optional<OpcodeType> BaseOp(OpcodeType op, bool delaySlot) {
    if (!delaySlot) {
        switch (op) {
        case OpcodeType::NOP:
        case OpcodeType::MOV_R:
        case OpcodeType::MOV_I:
        case OpcodeType::MOVB_L:
        case OpcodeType::MOVL_L:
        case OpcodeType::MOVB_S:
        case OpcodeType::MOVL_S:
        case OpcodeType::MOVL_I:
        case OpcodeType::ADD:
        case OpcodeType::ADD_I:
        case OpcodeType::CMP_EQ_R:
        case OpcodeType::DT: return op;
        default: return std::nullopt;
        }
    }
    // MOVL_I is excluded in delay slots: its PC-relative base uses the delay-slot target.
    switch (op) {
    case OpcodeType::Delay_NOP: return OpcodeType::NOP;
    case OpcodeType::Delay_MOV_R: return OpcodeType::MOV_R;
    case OpcodeType::Delay_MOV_I: return OpcodeType::MOV_I;
    case OpcodeType::Delay_MOVB_L: return OpcodeType::MOVB_L;
    case OpcodeType::Delay_MOVL_L: return OpcodeType::MOVL_L;
    case OpcodeType::Delay_MOVB_S: return OpcodeType::MOVB_S;
    case OpcodeType::Delay_MOVL_S: return OpcodeType::MOVL_S;
    case OpcodeType::Delay_ADD: return OpcodeType::ADD;
    case OpcodeType::Delay_ADD_I: return OpcodeType::ADD_I;
    case OpcodeType::Delay_CMP_EQ_R: return OpcodeType::CMP_EQ_R;
    case OpcodeType::Delay_DT: return OpcodeType::DT;
    default: return std::nullopt;
    }
}

bool IsDelayedBranch(OpcodeType op) {
    return op == OpcodeType::BRA || op == OpcodeType::BTS || op == OpcodeType::BFS || op == OpcodeType::JMP ||
           op == OpcodeType::RTS;
}

// Lowers a non-branch instruction. `retiredBefore` = instructions completed before this one.
void LowerPlain(Builder &b, OpcodeType op, uint16_t instr, uint32_t pc, bool delaySlot, uint8_t retiredBefore) {
    const uint32_t n = Rn(instr);
    const uint32_t m = Rm(instr);
    const auto advance = [&] {
        if (delaySlot) {
            b.EndDelaySlot();
        }
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
    case OpcodeType::MOVL_I: {
        b.SyncCycles();
        const ValueId address = b.Const((pc & ~3u) + ((instr & 0xFFu) << 2) + 4u);
        b.AddAccessCycles(address, 4, false);
        b.SetReg(n, b.Load(address, 4, true));
        b.SetWb(static_cast<uint8_t>(n));
        break;
    }
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
    default: break; // callers only pass supported opcodes
    }
}

// Lowers the branch part of a delayed branch (the slot is lowered by the caller).
void LowerDelayedBranch(Builder &b, OpcodeType op, uint16_t instr, uint32_t pc, uint8_t retiredBefore) {
    switch (op) {
    case OpcodeType::BRA:
        b.SetupDelaySlot(b.Const(pc + Disp12x2(instr) + 4u));
        b.SetWb(kWbNone);
        b.AddCycles(2);
        break;
    case OpcodeType::BTS:
    case OpcodeType::BFS: {
        b.SetWb(kWbNone);
        const ValueId t = b.GetT();
        // Not-taken condition: BT/S is not taken when T == 0, BF/S when T == 1.
        const ValueId notTaken = op == OpcodeType::BTS ? b.CmpEq(t, b.Const(0)) : t;
        b.ExitIf(notTaken, pc + 2u, 1, false, static_cast<uint8_t>(retiredBefore + 1));
        b.SetupDelaySlot(b.Const(pc + Disp8x2(instr) + 4u));
        b.AddCycles(2);
        break;
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
    default: break;
    }
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
    const auto boundary = [&](uint32_t address, uint8_t retired) {
        if (retired > 0) {
            b.CheckBoundary(address, retired);
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
            LowerDelayedBranch(b, op, instr, pc, count);
            b.CheckBoundary(pc + 2, static_cast<uint8_t>(count + 1));
            refillIfAligned(pc + 2);
            LowerPlain(b, *slotBase, slot, pc + 2, true, static_cast<uint8_t>(count + 1));
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
