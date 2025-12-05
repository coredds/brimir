#include <brimir/hw/scu/scu_dsp_disasm.hpp>

#include <brimir/util/bit_ops.hpp>
#include <brimir/util/unreachable.hpp>

namespace brimir::scu {

static SCUDSPInstruction::OpSrc TranslateOpSource(uint32 src) {
    using enum SCUDSPInstruction::OpSrc;
    switch (src) {
    case 0b0000: return M0;
    case 0b0001: return M1;
    case 0b0010: return M2;
    case 0b0011: return M3;
    case 0b0100: return MC0;
    case 0b0101: return MC1;
    case 0b0110: return MC2;
    case 0b0111: return MC3;
    case 0b1001: return ALUL;
    case 0b1010: return ALUH;
    default: return Invalid;
    }
}

static SCUDSPInstruction::OpDst TranslateOpDestination(uint32 dst) {
    using enum SCUDSPInstruction::OpDst;
    switch (dst) {
    case 0b0000: return MC0;
    case 0b0001: return MC1;
    case 0b0010: return MC2;
    case 0b0011: return MC3;
    case 0b0100: return RX;
    case 0b0101: return P;
    case 0b0110: return RA0;
    case 0b0111: return WA0;
    case 0b1010: return LOP;
    case 0b1011: return TOP;
    case 0b1100: return CT0;
    case 0b1101: return CT1;
    case 0b1110: return CT2;
    case 0b1111: return CT3;
    default: return Invalid;
    }
}

static SCUDSPInstruction::Cond TranslateCondition(uint32 cond) {
    using enum SCUDSPInstruction::Cond;
    switch (cond) {
    case 0b000001: return NZ;
    case 0b000010: return NS;
    case 0b000011: return NZS;
    case 0b000100: return NC;
    case 0b001000: return NT0;
    case 0b100001: return Z;
    case 0b100010: return S;
    case 0b100011: return ZS;
    case 0b100100: return C;
    case 0b101000: return T0;
    default: return None;
    };
}

static SCUDSPInstruction DisasembleOperation(uint32 opcode) {
    SCUDSPInstruction instr{.type = SCUDSPInstruction::Type::Operation, .operation = {}};

    // ALU
    {
        using enum SCUDSPInstruction::ALUOp;
        switch (bit::extract<26, 29>(opcode)) {
        case 0b0000: instr.operation.aluOp = NOP; break;
        case 0b0001: instr.operation.aluOp = AND; break;
        case 0b0010: instr.operation.aluOp = OR; break;
        case 0b0011: instr.operation.aluOp = XOR; break;
        case 0b0100: instr.operation.aluOp = ADD; break;
        case 0b0101: instr.operation.aluOp = SUB; break;
        case 0b0110: instr.operation.aluOp = AD2; break;
        case 0b1000: instr.operation.aluOp = SR; break;
        case 0b1001: instr.operation.aluOp = RR; break;
        case 0b1010: instr.operation.aluOp = SL; break;
        case 0b1011: instr.operation.aluOp = RL; break;
        case 0b1111: instr.operation.aluOp = RL8; break;
        default: instr.operation.aluOp = Invalid;
        }
    }

    // X-Bus
    {
        using enum SCUDSPInstruction::XBusPOp;
        switch (bit::extract<23, 24>(opcode)) {
        case 0b10: instr.operation.xbusPOp = MOV_MUL_P; break;
        case 0b11: instr.operation.xbusPOp = MOV_S_P; break;
        default: instr.operation.xbusPOp = NOP; break;
        }
    }
    instr.operation.xbusXOp = bit::test<25>(opcode);
    instr.operation.xbusSrc = TranslateOpSource(bit::extract<20, 22>(opcode));

    // Y-Bus
    {
        using enum SCUDSPInstruction::YBusAOp;
        switch (bit::extract<17, 18>(opcode)) {
        case 0b01: instr.operation.ybusAOp = CLR_A; break;
        case 0b10: instr.operation.ybusAOp = MOV_ALU_A; break;
        case 0b11: instr.operation.ybusAOp = MOV_S_A; break;
        default: instr.operation.ybusAOp = NOP; break;
        }
    }
    instr.operation.ybusYOp = bit::test<19>(opcode);
    instr.operation.ybusSrc = TranslateOpSource(bit::extract<14, 16>(opcode));

    // D1-Bus
    {
        using enum SCUDSPInstruction::D1BusOp;
        switch (bit::extract<12, 13>(opcode)) {
        case 0b01:
            instr.operation.d1BusOp = MOV_SIMM_D;
            instr.operation.d1BusSrc.imm = bit::extract_signed<0, 7>(opcode);
            break;
        case 0b11:
            instr.operation.d1BusOp = MOV_S_D;
            instr.operation.d1BusSrc.reg = TranslateOpSource(bit::extract<0, 3>(opcode));
            break;
        default: instr.operation.d1BusOp = NOP; break;
        }
        instr.operation.d1BusDst = TranslateOpDestination(bit::extract<8, 11>(opcode));
    }

    return instr;
}

static SCUDSPInstruction DisasembleLoadImm(uint32 opcode) {
    SCUDSPInstruction instr{.type = SCUDSPInstruction::Type::MVI, .mvi = {}};
    if (bit::test<25>(opcode)) {
        instr.mvi.cond = TranslateCondition(bit::extract<19, 24>(opcode));
        instr.mvi.imm = bit::extract_signed<0, 18>(opcode);
    } else {
        instr.mvi.cond = SCUDSPInstruction::Cond::None;
        instr.mvi.imm = bit::extract_signed<0, 24>(opcode);
    }

    {
        using enum SCUDSPInstruction::MVIDst;
        switch (bit::extract<26, 29>(opcode)) {
        case 0b0000: instr.mvi.dst = MC0; break;
        case 0b0001: instr.mvi.dst = MC1; break;
        case 0b0010: instr.mvi.dst = MC2; break;
        case 0b0011: instr.mvi.dst = MC3; break;
        case 0b0100: instr.mvi.dst = RX; break;
        case 0b0101: instr.mvi.dst = P; break;
        case 0b0110: instr.mvi.dst = RA0; break;
        case 0b0111: instr.mvi.dst = WA0; break;
        case 0b1010: instr.mvi.dst = LOP; break;
        case 0b1100: instr.mvi.dst = PC; break;
        default: instr.mvi.dst = Invalid; break;
        }
    }

    return instr;
}

static SCUDSPInstruction DisassembleDMA(uint32 opcode) {
    SCUDSPInstruction instr{.type = SCUDSPInstruction::Type::DMA, .dma = {}};
    instr.dma.toD0 = bit::test<12>(opcode);
    instr.dma.hold = bit::test<14>(opcode);
    instr.dma.countType = bit::test<13>(opcode);
    if (instr.dma.countType) {
        instr.dma.count.ct = bit::extract<0, 3>(opcode);
    } else {
        instr.dma.count.imm = bit::extract<0, 7>(opcode);
    }

    {
        using enum SCUDSPInstruction::DMARAMOp;
        switch (bit::extract<8, 10>(opcode)) {
        case 0: instr.dma.ramOp = MC0; break;
        case 1: instr.dma.ramOp = MC1; break;
        case 2: instr.dma.ramOp = MC2; break;
        case 3: instr.dma.ramOp = MC3; break;
        case 4: instr.dma.ramOp = instr.dma.toD0 ? Invalid : PRG; break;
        default: instr.dma.ramOp = Invalid; break;
        }
    }
    return instr;
}

static SCUDSPInstruction DisassembleJump(uint32 opcode) {
    SCUDSPInstruction instr{.type = SCUDSPInstruction::Type::JMP, .jmp = {}};
    instr.jmp.cond =
        bit::test<25>(opcode) ? TranslateCondition(bit::extract<19, 24>(opcode)) : SCUDSPInstruction::Cond::None;
    instr.jmp.target = bit::extract<0, 7>(opcode);
    return instr;
}

static SCUDSPInstruction DisassembleLoop(uint32 opcode) {
    if (bit::test<27>(opcode)) {
        return {.type = SCUDSPInstruction::Type::LPS};
    } else {
        return {.type = SCUDSPInstruction::Type::BTM};
    }
}

static SCUDSPInstruction DisassembleEnd(uint32 opcode) {
    if (bit::test<27>(opcode)) {
        return {.type = SCUDSPInstruction::Type::ENDI};
    } else {
        return {.type = SCUDSPInstruction::Type::END};
    }
}

static SCUDSPInstruction DisasembleSpecial(uint32 opcode) {
    const uint32 cmdSubcategory = bit::extract<28, 29>(opcode);
    switch (cmdSubcategory) {
    case 0b00: return DisassembleDMA(opcode);
    case 0b01: return DisassembleJump(opcode);
    case 0b10: return DisassembleLoop(opcode);
    case 0b11: return DisassembleEnd(opcode);
    }

    util::unreachable();
}

SCUDSPInstruction Disassemble(uint32 opcode) {
    const uint32 cmdCategory = bit::extract<30, 31>(opcode);
    switch (cmdCategory) {
    case 0b00: return DisasembleOperation(opcode);
    case 0b10: return DisasembleLoadImm(opcode);
    case 0b11: return DisasembleSpecial(opcode);
    }

    return SCUDSPInstruction{.type = SCUDSPInstruction::Type::Invalid};
}

std::string_view ToString(SCUDSPInstruction::Cond cond) {
    using enum SCUDSPInstruction::Cond;
    switch (cond) {
    case None: return "";
    case NZ: return "NZ";
    case NS: return "NS";
    case NZS: return "NZS";
    case NC: return "NC";
    case NT0: return "NT0";
    case Z: return "Z";
    case S: return "S";
    case ZS: return "ZS";
    case C: return "C";
    case T0: return "T0";
    default: return "";
    }
}

std::string_view ToString(SCUDSPInstruction::OpSrc opSrc) {
    using enum SCUDSPInstruction::OpSrc;
    switch (opSrc) {
    case None: return "";
    case M0: return "M0";
    case M1: return "M1";
    case M2: return "M2";
    case M3: return "M3";
    case MC0: return "MC0";
    case MC1: return "MC1";
    case MC2: return "MC2";
    case MC3: return "MC3";
    case ALUL: return "ALUL";
    case ALUH: return "ALUH";
    case Invalid: return "(?)";
    default: return "";
    }
}

std::string_view ToString(SCUDSPInstruction::OpDst opDst) {
    using enum SCUDSPInstruction::OpDst;
    switch (opDst) {
    case None: return "";
    case CT0: return "CT0";
    case CT1: return "CT1";
    case CT2: return "CT2";
    case CT3: return "CT3";
    case MC0: return "MC0";
    case MC1: return "MC1";
    case MC2: return "MC2";
    case MC3: return "MC3";
    case RX: return "RX";
    case P: return "P";
    case RA0: return "RA0";
    case WA0: return "WA0";
    case LOP: return "LOP";
    case TOP: return "TOP";
    case Invalid: return "(?)";
    default: return "";
    }
}

std::string_view ToString(SCUDSPInstruction::MVIDst mviDst) {
    using enum SCUDSPInstruction::MVIDst;
    switch (mviDst) {
    case None: return "";
    case MC0: return "MC0";
    case MC1: return "MC1";
    case MC2: return "MC2";
    case MC3: return "MC3";
    case RX: return "RX";
    case P: return "P";
    case RA0: return "RA0";
    case WA0: return "WA0";
    case LOP: return "LOP";
    case PC: return "PC";
    case Invalid: return "(?)";
    default: return "";
    }
}

std::string_view ToString(SCUDSPInstruction::ALUOp aluOp) {
    using enum SCUDSPInstruction::ALUOp;
    switch (aluOp) {
    case NOP: return "";
    case AND: return "AND";
    case OR: return "OR";
    case XOR: return "XOR";
    case ADD: return "ADD";
    case SUB: return "SUB";
    case AD2: return "AD2";
    case SR: return "SR";
    case RR: return "RR";
    case SL: return "SL";
    case RL: return "RL";
    case RL8: return "RL8";
    case Invalid: return "(?)";
    default: return "";
    }
}

std::string_view ToString(SCUDSPInstruction::DMARAMOp dmaRamOp) {
    using enum SCUDSPInstruction::DMARAMOp;
    switch (dmaRamOp) {
    case MC0: return "MC0";
    case MC1: return "MC1";
    case MC2: return "MC2";
    case MC3: return "MC3";
    case PRG: return "PRG";
    case Invalid: return "(?)";
    default: return "";
    }
}

} // namespace brimir::scu
