#include <brimir/hw/m68k/m68k_disasm.hpp>

#include "m68k_addr_modes.hpp"

#include <brimir/util/bit_ops.hpp>
#include <brimir/util/unreachable.hpp>

namespace brimir::m68k {

DisassemblyTable::DisassemblyTable() {
    for (uint32 opcode = 0; opcode < 0x10000; opcode++) {
        DisassemblyInfo &info = this->infos[opcode];

        // ---------------------------------------

        using Op = Operand;
        using Cond = Condition;

        // <op>
        auto make0 = [&](Mnemonic mnemonic) { info.mnemonic = mnemonic; };

        // <op>.b <op1>, <op2>
        auto makeOpB = [&](Mnemonic mnemonic, Op op1, Op op2 = Op::None()) {
            info.mnemonic = mnemonic;
            info.opSize = OperandSize::Byte;
            info.op1 = op1;
            info.op2 = op2;
        };

        // <op>.w <op1>, <op2>
        auto makeOpW = [&](Mnemonic mnemonic, Op op1, Op op2 = Op::None()) {
            info.mnemonic = mnemonic;
            info.opSize = OperandSize::Word;
            info.op1 = op1;
            info.op2 = op2;
        };

        // <op>.l <op1>, <op2>
        auto makeOpL = [&](Mnemonic mnemonic, Op op1, Op op2 = Op::None()) {
            info.mnemonic = mnemonic;
            info.opSize = OperandSize::Long;
            info.op1 = op1;
            info.op2 = op2;
        };

        // <op> <op1>, <op2> (implicit byte operand)
        auto makeOpBI = [&](Mnemonic mnemonic, Op op1, Op op2 = Op::None()) {
            info.mnemonic = mnemonic;
            info.opSize = OperandSize::ByteImplicit;
            info.op1 = op1;
            info.op2 = op2;
        };

        // <op> <op1>, <op2> (implicit word operand)
        auto makeOpWI = [&](Mnemonic mnemonic, Op op1, Op op2 = Op::None()) {
            info.mnemonic = mnemonic;
            info.opSize = OperandSize::WordImplicit;
            info.op1 = op1;
            info.op2 = op2;
        };

        // <op> <op1>, <op2> (implicit long operand)
        auto makeOpLI = [&](Mnemonic mnemonic, Op op1, Op op2 = Op::None()) {
            info.mnemonic = mnemonic;
            info.opSize = OperandSize::LongImplicit;
            info.op1 = op1;
            info.op2 = op2;
        };

        // <op> <op1>, <op2> (no transfers)
        auto makeOpN = [&](Mnemonic mnemonic, Op op1, Op op2 = Op::None()) {
            info.mnemonic = mnemonic;
            info.opSize = OperandSize::None;
            info.op1 = op1;
            info.op2 = op2;
        };

        // <ea>
        auto eaOp_N = [](uint8 ea) {
            const uint16 Xn = bit::extract<0, 2>(ea);
            const uint16 M = bit::extract<3, 5>(ea);
            switch (M) {
            case 0b010: return Op::AtAn_N(Xn);
            case 0b101: return Op::AtDispAn_N(Xn);
            case 0b110: return Op::AtDispAnIx_N(Xn);
            case 0b111:
                switch (Xn) {
                case 0b010: return Op::AtDispPC_N();
                case 0b011: return Op::AtDispPCIx_N();
                case 0b000: return Op::AtImmWord_N();
                case 0b001: return Op::AtImmLong_N();
                }
                break;
            }

            util::unreachable();
        };

        auto eaOp_R = [](uint8 ea, OperandSize size) {
            const uint16 Xn = bit::extract<0, 2>(ea);
            const uint16 M = bit::extract<3, 5>(ea);
            switch (M) {
            case 0b000: return Op::Dn_R(Xn);
            case 0b001: return Op::An_R(Xn);
            case 0b010: return Op::AtAn_R(Xn);
            case 0b011: return Op::AtAnPlus_R(Xn);
            case 0b100: return Op::MinusAtAn_R(Xn);
            case 0b101: return Op::AtDispAn_R(Xn);
            case 0b110: return Op::AtDispAnIx_R(Xn);
            case 0b111:
                switch (Xn) {
                case 0b010: return Op::AtDispPC_R();
                case 0b011: return Op::AtDispPCIx_R();
                case 0b000: return Op::AtImmWord_R();
                case 0b001: return Op::AtImmLong_R();
                case 0b100:
                    switch (size) {
                    case OperandSize::Byte: [[fallthrough]];
                    case OperandSize::ByteImplicit: return Op::UImm8Fetched();
                    case OperandSize::Word: [[fallthrough]];
                    case OperandSize::WordImplicit: return Op::UImm16Fetched();
                    case OperandSize::Long: [[fallthrough]];
                    case OperandSize::LongImplicit: return Op::UImm32Fetched();
                    default: break;
                    }
                    break;
                }
                break;
            }

            util::unreachable();
        };
        auto eaOp_R_B = [&](uint8 ea) { return eaOp_R(ea, OperandSize::Byte); };
        auto eaOp_R_W = [&](uint8 ea) { return eaOp_R(ea, OperandSize::Word); };
        auto eaOp_R_L = [&](uint8 ea) { return eaOp_R(ea, OperandSize::Long); };

        auto eaOp_W = [](uint8 ea) {
            const uint16 Xn = bit::extract<0, 2>(ea);
            const uint16 M = bit::extract<3, 5>(ea);
            switch (M) {
            case 0b000: return Op::Dn_W(Xn);
            case 0b001: return Op::An_W(Xn);
            case 0b010: return Op::AtAn_W(Xn);
            case 0b011: return Op::AtAnPlus_W(Xn);
            case 0b100: return Op::MinusAtAn_W(Xn);
            case 0b101: return Op::AtDispAn_W(Xn);
            case 0b110: return Op::AtDispAnIx_W(Xn);
            case 0b111:
                switch (Xn) {
                case 0b000: return Op::AtImmWord_W();
                case 0b001: return Op::AtImmLong_W();
                }
                break;
            }

            util::unreachable();
        };

        auto eaOp_RW = [](uint8 ea) {
            const uint16 Xn = bit::extract<0, 2>(ea);
            const uint16 M = bit::extract<3, 5>(ea);
            switch (M) {
            case 0b000: return Op::Dn_RW(Xn);
            case 0b001: return Op::An_RW(Xn);
            case 0b010: return Op::AtAn_RW(Xn);
            case 0b011: return Op::AtAnPlus_RW(Xn);
            case 0b100: return Op::MinusAtAn_RW(Xn);
            case 0b101: return Op::AtDispAn_RW(Xn);
            case 0b110: return Op::AtDispAnIx_RW(Xn);
            case 0b111:
                switch (Xn) {
                case 0b000: return Op::AtImmWord_RW();
                case 0b001: return Op::AtImmLong_RW();
                }
                break;
            }

            util::unreachable();
        };

        auto privileged = [&] { info.privileged = true; };
        auto cond = [&](Condition cond) { info.cond = cond; };

        // ---------------------------------------

        using enum Mnemonic;

        switch (opcode >> 12u) {
        case 0x0: {
            const uint16 ea = bit::extract<0, 5>(opcode);
            const uint16 sz = bit::extract<6, 7>(opcode);
            const uint16 Dx = bit::extract<9, 11>(opcode);

            if (opcode == 0x003C) {
                makeOpW(OrI, Op::UImm16Fetched(), Op::CCR_W());
            } else if (opcode == 0x007C) {
                makeOpW(OrI, Op::UImm16Fetched(), Op::SR_W()), privileged();
            } else if (opcode == 0x023C) {
                makeOpW(AndI, Op::UImm16Fetched(), Op::CCR_W());
            } else if (opcode == 0x027C) {
                makeOpW(AndI, Op::UImm16Fetched(), Op::SR_W()), privileged();
            } else if (opcode == 0x0A3C) {
                makeOpW(EorI, Op::UImm16Fetched(), Op::CCR_W());
            } else if (opcode == 0x0A7C) {
                makeOpW(EorI, Op::UImm16Fetched(), Op::SR_W()), privileged();
            } else if (bit::extract<3, 5>(opcode) == 0b001 && bit::extract<8>(opcode) == 1) {
                const bool szBit = bit::test<6>(opcode);
                const uint8 Ay = bit::extract<0, 2>(opcode);
                if (bit::test<7>(opcode)) {
                    if (szBit) {
                        makeOpL(MoveP, Op::Dn_R(Dx), Op::AtDispAn_W(Ay));
                    } else {
                        makeOpW(MoveP, Op::Dn_R(Dx), Op::AtDispAn_W(Ay));
                    }
                } else {
                    if (szBit) {
                        makeOpL(MoveP, Op::AtDispAn_R(Ay), Op::Dn_W(Dx));
                    } else {
                        makeOpW(MoveP, Op::AtDispAn_R(Ay), Op::Dn_W(Dx));
                    }
                }
            } else if (bit::extract<6, 8>(opcode) == 0b100) {
                if ((ea >> 3u) == 0b000) {
                    makeOpL(BTst, Op::Dn_R(Dx), Op::Dn_W(bit::extract<0, 2>(ea)));
                } else if (kValidDataAddrModes[ea]) {
                    makeOpB(BTst, Op::Dn_R(Dx), eaOp_R_B(ea));
                }
            } else if (bit::extract<6, 8>(opcode) == 0b101) {
                if ((ea >> 3u) == 0b000) {
                    makeOpL(BChg, Op::Dn_R(Dx), Op::Dn_W(bit::extract<0, 2>(ea)));
                } else if (kValidDataAlterableAddrModes[ea]) {
                    makeOpB(BChg, Op::Dn_R(Dx), eaOp_RW(ea));
                }
            } else if (bit::extract<6, 8>(opcode) == 0b110) {
                if ((ea >> 3u) == 0b000) {
                    makeOpL(BClr, Op::Dn_R(Dx), Op::Dn_W(bit::extract<0, 2>(ea)));
                } else if (kValidDataAlterableAddrModes[ea]) {
                    makeOpB(BClr, Op::Dn_R(Dx), eaOp_RW(ea));
                }
            } else if (bit::extract<6, 8>(opcode) == 0b111) {
                if ((ea >> 3u) == 0b000) {
                    makeOpL(BSet, Op::Dn_R(Dx), Op::Dn_W(bit::extract<0, 2>(ea)));
                } else if (kValidDataAlterableAddrModes[ea]) {
                    makeOpB(BSet, Op::Dn_R(Dx), eaOp_RW(ea));
                }
            } else if (bit::extract<6, 11>(opcode) == 0b100000) {
                if ((ea >> 3u) == 0b000) {
                    makeOpL(BTst, Op::UImm8Fetched(), Op::Dn_W(bit::extract<0, 2>(ea)));
                } else if (kValidDataAddrModes[ea]) {
                    makeOpB(BTst, Op::UImm8Fetched(), eaOp_R_B(ea));
                }
            } else if (bit::extract<6, 11>(opcode) == 0b100001) {
                if ((ea >> 3u) == 0b000) {
                    makeOpL(BChg, Op::UImm8Fetched(), Op::Dn_W(bit::extract<0, 2>(ea)));
                } else if (kValidDataAlterableAddrModes[ea]) {
                    makeOpB(BChg, Op::UImm8Fetched(), eaOp_RW(ea));
                }
            } else if (bit::extract<6, 11>(opcode) == 0b100010) {
                if ((ea >> 3u) == 0b000) {
                    makeOpL(BClr, Op::UImm8Fetched(), Op::Dn_W(bit::extract<0, 2>(ea)));
                } else if (kValidDataAlterableAddrModes[ea]) {
                    makeOpB(BClr, Op::UImm8Fetched(), eaOp_RW(ea));
                }
            } else if (bit::extract<6, 11>(opcode) == 0b100011) {
                if ((ea >> 3u) == 0b000) {
                    makeOpL(BSet, Op::UImm8Fetched(), Op::Dn_W(bit::extract<0, 2>(ea)));
                } else if (kValidDataAlterableAddrModes[ea]) {
                    makeOpB(BSet, Op::UImm8Fetched(), eaOp_RW(ea));
                }
            } else if (bit::extract<8, 11>(opcode) == 0b0000) {
                if (kValidDataAlterableAddrModes[ea]) {
                    switch (sz) {
                    case 0b00: makeOpB(OrI, Op::UImm8Fetched(), eaOp_RW(ea)); break;
                    case 0b01: makeOpW(OrI, Op::UImm16Fetched(), eaOp_RW(ea)); break;
                    case 0b10: makeOpL(OrI, Op::UImm32Fetched(), eaOp_RW(ea)); break;
                    }
                }
            } else if (bit::extract<8, 11>(opcode) == 0b0010) {
                if (kValidDataAlterableAddrModes[ea]) {
                    switch (sz) {
                    case 0b00: makeOpB(AndI, Op::UImm8Fetched(), eaOp_RW(ea)); break;
                    case 0b01: makeOpW(AndI, Op::UImm16Fetched(), eaOp_RW(ea)); break;
                    case 0b10: makeOpL(AndI, Op::UImm32Fetched(), eaOp_RW(ea)); break;
                    }
                }
            } else if (bit::extract<8, 11>(opcode) == 0b1010) {
                if (kValidDataAlterableAddrModes[ea]) {
                    switch (sz) {
                    case 0b00: makeOpB(EorI, Op::UImm8Fetched(), eaOp_RW(ea)); break;
                    case 0b01: makeOpW(EorI, Op::UImm16Fetched(), eaOp_RW(ea)); break;
                    case 0b10: makeOpL(EorI, Op::UImm32Fetched(), eaOp_RW(ea)); break;
                    }
                }
            } else if (bit::extract<8, 11>(opcode) == 0b0100) {
                if (kValidDataAlterableAddrModes[ea]) {
                    switch (sz) {
                    case 0b00: makeOpB(SubI, Op::UImm8Fetched(), eaOp_RW(ea)); break;
                    case 0b01: makeOpW(SubI, Op::UImm16Fetched(), eaOp_RW(ea)); break;
                    case 0b10: makeOpL(SubI, Op::UImm32Fetched(), eaOp_RW(ea)); break;
                    }
                }
            } else if (bit::extract<8, 11>(opcode) == 0b0110) {
                if (kValidDataAlterableAddrModes[ea]) {
                    switch (sz) {
                    case 0b00: makeOpB(AddI, Op::UImm8Fetched(), eaOp_RW(ea)); break;
                    case 0b01: makeOpW(AddI, Op::UImm16Fetched(), eaOp_RW(ea)); break;
                    case 0b10: makeOpL(AddI, Op::UImm32Fetched(), eaOp_RW(ea)); break;
                    }
                }
            } else if (bit::extract<8, 11>(opcode) == 0b1100) {
                if (kValidDataAddrModes[ea]) {
                    switch (sz) {
                    case 0b00: makeOpB(CmpI, Op::UImm8Fetched(), eaOp_R_B(ea)); break;
                    case 0b01: makeOpW(CmpI, Op::UImm16Fetched(), eaOp_R_W(ea)); break;
                    case 0b10: makeOpL(CmpI, Op::UImm32Fetched(), eaOp_R_L(ea)); break;
                    }
                }
            }
            break;
        }
        case 0x1: [[fallthrough]];
        case 0x2: [[fallthrough]];
        case 0x3:
            if (bit::extract<6, 8>(opcode) == 0b001) {
                const uint16 ea = bit::extract<0, 5>(opcode);

                if (kValidAddrModes[ea]) {
                    const uint16 An = bit::extract<9, 11>(opcode);
                    const uint16 size = bit::extract<12, 13>(opcode);
                    switch (size) {
                    case 0b11: makeOpW(MoveA, eaOp_R_W(ea), Op::An_W(An)); break;
                    case 0b10: makeOpL(MoveA, eaOp_R_L(ea), Op::An_W(An)); break;
                    }
                }
            } else {
                const uint16 srcEA = bit::extract<0, 5>(opcode);
                const uint16 dstEA = (bit::extract<6, 8>(opcode) << 3) | bit::extract<9, 11>(opcode);

                if (kValidDataAlterableAddrModes[dstEA] && kValidAddrModes[srcEA]) {
                    // Note the swapped bit order between word and longword moves
                    const uint16 size = bit::extract<12, 13>(opcode);
                    switch (size) {
                    case 0b01: makeOpB(Move, eaOp_R_B(srcEA), eaOp_W(dstEA)); break;
                    case 0b11: makeOpW(Move, eaOp_R_W(srcEA), eaOp_W(dstEA)); break;
                    case 0b10: makeOpL(Move, eaOp_R_L(srcEA), eaOp_W(dstEA)); break;
                    }
                }
            }
            break;
        case 0x4: {
            const uint16 ea = bit::extract<0, 5>(opcode);
            const uint16 sz = bit::extract<6, 7>(opcode);
            if (opcode == 0x4E70) {
                make0(Reset), privileged();
            } else if (opcode == 0x4E71) {
                make0(Noop);
            } else if (opcode == 0x4E72) {
                makeOpWI(Stop, Op::UImm16Fetched()), privileged();
            } else if (opcode == 0x4E73) {
                make0(RTE), privileged();
            } else if (opcode == 0x4E75) {
                make0(RTS);
            } else if (opcode == 0x4E76) {
                make0(TrapV);
            } else if (opcode == 0x4E77) {
                make0(RTR);
            } else if (opcode == 0x4AFC) {
                make0(Illegal);
            } else if (bit::extract<3, 11>(opcode) == 0b100001000) {
                const uint32 Dn = bit::extract<0, 2>(opcode);
                makeOpLI(Swap, Op::Dn_RW(Dn));
            } else if (bit::extract<3, 11>(opcode) == 0b100010000) {
                const uint32 Dn = bit::extract<0, 2>(opcode);
                makeOpWI(Ext, Op::Dn_RW(Dn));
            } else if (bit::extract<3, 11>(opcode) == 0b100011000) {
                const uint32 Dn = bit::extract<0, 2>(opcode);
                makeOpLI(Ext, Op::Dn_RW(Dn));
            } else if (bit::extract<3, 11>(opcode) == 0b111001010) {
                const uint16 An = bit::extract<0, 2>(opcode);
                makeOpW(Link, Op::An_RW(An), Op::UImm16Fetched());
            } else if (bit::extract<3, 11>(opcode) == 0b111001011) {
                const uint16 An = bit::extract<0, 2>(opcode);
                makeOpLI(Unlink, Op::An_RW(An));
            } else if (bit::extract<3, 11>(opcode) == 0b111001100) {
                const uint16 An = bit::extract<0, 2>(opcode);
                makeOpLI(Move, Op::An_R(An), Op::USP_W()), privileged();
            } else if (bit::extract<3, 11>(opcode) == 0b111001101) {
                const uint16 An = bit::extract<0, 2>(opcode);
                makeOpLI(Move, Op::USP_R(), Op::An_W(An)), privileged();
            } else if (bit::extract<4, 11>(opcode) == 0b11100100) {
                const uint8 vector = bit::extract<0, 3>(opcode);
                makeOpBI(Trap, Op::UImmEmbedded(vector));
            } else if (bit::extract<6, 11>(opcode) == 0b000011) {
                if (kValidDataAlterableAddrModes[ea]) {
                    makeOpW(Move, Op::SR_R(), eaOp_W(ea));
                }
            } else if (bit::extract<6, 11>(opcode) == 0b001011) {
                if (kValidDataAlterableAddrModes[ea]) {
                    makeOpW(Move, Op::CCR_R(), eaOp_W(ea));
                }
            } else if (bit::extract<6, 11>(opcode) == 0b010011) {
                if (kValidDataAddrModes[ea]) {
                    makeOpW(Move, eaOp_R_W(ea), Op::CCR_W());
                }
            } else if (bit::extract<6, 11>(opcode) == 0b011011) {
                if (kValidDataAddrModes[ea]) {
                    makeOpW(Move, eaOp_R_W(ea), Op::SR_W()), privileged();
                }
            } else if (bit::extract<6, 11>(opcode) == 0b100000) {
                if (kValidDataAlterableAddrModes[ea]) {
                    makeOpBI(NBCD, eaOp_RW(ea));
                }
            } else if (bit::extract<6, 11>(opcode) == 0b101011) {
                if (kValidDataAlterableAddrModes[ea]) {
                    makeOpBI(TAS, eaOp_RW(ea));
                }
            } else if (bit::extract<6, 11>(opcode) == 0b100001) {
                if (kValidControlAddrModes[ea]) {
                    makeOpLI(PEA, eaOp_N(ea));
                }
            } else if (bit::extract<6, 11>(opcode) == 0b111010) {
                if (kValidControlAddrModes[ea]) {
                    makeOpLI(JSR, eaOp_N(ea));
                }
            } else if (bit::extract<6, 11>(opcode) == 0b111011) {
                if (kValidControlAddrModes[ea]) {
                    makeOpLI(Jmp, eaOp_N(ea));
                }
            } else if (bit::extract<7, 11>(opcode) == 0b10001) {
                const bool isPredecrement = (ea >> 3u) == 0b100;
                const bool sz = bit::test<6>(opcode);
                if (isPredecrement) {
                    const uint16 An = bit::extract<0, 2>(opcode);
                    if (sz) {
                        makeOpL(MoveM, Op::RevRegList_R(), Op::MinusAtAn_W(An));
                    } else {
                        makeOpW(MoveM, Op::RevRegList_R(), Op::MinusAtAn_W(An));
                    }
                } else if (kValidControlAlterableAddrModes[ea]) {
                    if (sz) {
                        makeOpL(MoveM, Op::RegList_R(), eaOp_W(ea));
                    } else {
                        makeOpW(MoveM, Op::RegList_R(), eaOp_W(ea));
                    }
                }
            } else if (bit::extract<7, 11>(opcode) == 0b11001) {
                const bool isPostincrement = (ea >> 3u) == 0b011;
                const bool sz = bit::test<6>(opcode);
                if (isPostincrement) {
                    const uint16 An = bit::extract<0, 2>(opcode);
                    if (sz) {
                        makeOpL(MoveM, Op::AtAnPlus_R(An), Op::RegList_W());
                    } else {
                        makeOpW(MoveM, Op::AtAnPlus_R(An), Op::RegList_W());
                    }
                } else if (kValidControlAddrModes[ea]) {
                    if (sz) {
                        makeOpL(MoveM, eaOp_R_L(ea), Op::RegList_W());
                    } else {
                        makeOpW(MoveM, eaOp_R_W(ea), Op::RegList_W());
                    }
                }
            } else if (bit::extract<8, 11>(opcode) == 0b0000) {
                if (kValidDataAlterableAddrModes[ea]) {
                    switch (sz) {
                    case 0b00: makeOpB(NegX, eaOp_RW(ea)); break;
                    case 0b01: makeOpW(NegX, eaOp_RW(ea)); break;
                    case 0b10: makeOpL(NegX, eaOp_RW(ea)); break;
                    }
                }
            } else if (bit::extract<8, 11>(opcode) == 0b0010) {
                if (kValidDataAlterableAddrModes[ea]) {
                    switch (sz) {
                    case 0b00: makeOpB(Clr, eaOp_RW(ea)); break;
                    case 0b01: makeOpW(Clr, eaOp_RW(ea)); break;
                    case 0b10: makeOpL(Clr, eaOp_RW(ea)); break;
                    }
                }
            } else if (bit::extract<8, 11>(opcode) == 0b0100) {
                if (kValidDataAlterableAddrModes[ea]) {
                    switch (sz) {
                    case 0b00: makeOpB(Neg, eaOp_RW(ea)); break;
                    case 0b01: makeOpW(Neg, eaOp_RW(ea)); break;
                    case 0b10: makeOpL(Neg, eaOp_RW(ea)); break;
                    }
                }
            } else if (bit::extract<8, 11>(opcode) == 0b0110) {
                if (kValidDataAlterableAddrModes[ea]) {
                    switch (sz) {
                    case 0b00: makeOpB(Not, eaOp_RW(ea)); break;
                    case 0b01: makeOpW(Not, eaOp_RW(ea)); break;
                    case 0b10: makeOpL(Not, eaOp_RW(ea)); break;
                    }
                }
            } else if (bit::extract<8, 11>(opcode) == 0b1010) {
                if (kValidDataAlterableAddrModes[ea]) {
                    switch (sz) {
                    case 0b00: makeOpB(Tst, eaOp_R_B(ea)); break;
                    case 0b01: makeOpB(Tst, eaOp_R_W(ea)); break;
                    case 0b10: makeOpB(Tst, eaOp_R_L(ea)); break;
                    }
                }
            } else if (bit::extract<6, 8>(opcode) == 0b110) {
                if (kValidDataAddrModes[ea]) {
                    const uint16 Dn = bit::extract<9, 11>(opcode);
                    makeOpWI(Chk, eaOp_R_W(ea), Op::Dn_R(Dn));
                }
            } else if (bit::extract<6, 8>(opcode) == 0b111) {
                if (kValidControlAddrModes[ea]) {
                    const uint16 An = bit::extract<9, 11>(opcode);
                    makeOpLI(LEA, eaOp_N(ea), Op::An_W(An));
                }
            }
            break;
        }
        case 0x5: {
            const uint16 ea = bit::extract<0, 5>(opcode);
            if (bit::extract<3, 7>(opcode) == 0b11001) {
                const uint32 condNum = bit::extract<8, 11>(opcode);
                const uint32 Dn = bit::extract<0, 2>(opcode);
                makeOpWI(DBcc, Op::Dn_R(Dn), Op::WordDispPCFetched()), cond(static_cast<Cond>(condNum));
            } else if (bit::extract<6, 7>(opcode) == 0b11) {
                if (kValidDataAlterableAddrModes[ea]) {
                    const uint32 condNum = bit::extract<8, 11>(opcode);
                    makeOpBI(Scc, eaOp_W(ea)), cond(static_cast<Cond>(condNum));
                }
            } else {
                const uint16 M = bit::extract<3, 5>(opcode);
                const uint16 sz = bit::extract<6, 7>(opcode);
                const uint16 data = bit::extract<9, 11>(opcode);
                const uint32 op1 = data == 0 ? 8 : data;
                const bool isAn = M == 0b001;
                if (bit::extract<8>(opcode) == 1) {
                    if (isAn) {
                        const uint16 An = bit::extract<0, 2>(opcode);
                        switch (sz) {
                        case 0b01: makeOpW(SubQ, Op::UImmEmbedded(op1), Op::An_RW(An)); break;
                        case 0b10: makeOpL(SubQ, Op::UImmEmbedded(op1), Op::An_RW(An)); break;
                        }
                    } else if (kValidAlterableAddrModes[ea]) {
                        switch (sz) {
                        case 0b00: makeOpB(SubQ, Op::UImmEmbedded(op1), eaOp_RW(ea)); break;
                        case 0b01: makeOpW(SubQ, Op::UImmEmbedded(op1), eaOp_RW(ea)); break;
                        case 0b10: makeOpL(SubQ, Op::UImmEmbedded(op1), eaOp_RW(ea)); break;
                        }
                    }
                } else {
                    if (isAn) {
                        const uint16 An = bit::extract<0, 2>(opcode);
                        switch (sz) {
                        case 0b01: makeOpW(AddQ, Op::UImmEmbedded(op1), Op::An_RW(An)); break;
                        case 0b10: makeOpL(AddQ, Op::UImmEmbedded(op1), Op::An_RW(An)); break;
                        }
                    } else if (kValidAlterableAddrModes[ea]) {
                        switch (sz) {
                        case 0b00: makeOpB(AddQ, Op::UImmEmbedded(op1), eaOp_RW(ea)); break;
                        case 0b01: makeOpW(AddQ, Op::UImmEmbedded(op1), eaOp_RW(ea)); break;
                        case 0b10: makeOpL(AddQ, Op::UImmEmbedded(op1), eaOp_RW(ea)); break;
                        }
                    }
                }
            }
            break;
        }
        case 0x6: {
            const sint16 disp = static_cast<sint8>(bit::extract<0, 7>(opcode));
            const bool longDisp = disp == 0;
            if (longDisp) {
                switch (bit::extract<8, 11>(opcode)) {
                case 0b0000: makeOpW(BRA, Op::WordDispPCFetched()); break;
                case 0b0001: makeOpW(BSR, Op::WordDispPCFetched()); break;
                default: {
                    const uint32 condNum = bit::extract<8, 11>(opcode);
                    makeOpW(Bcc, Op::WordDispPCFetched()), cond(static_cast<Cond>(condNum));
                    break;
                }
                }
            } else {
                switch (bit::extract<8, 11>(opcode)) {
                case 0b0000: makeOpN(BRA, Op::WordDispPCEmbedded(disp)); break;
                case 0b0001: makeOpN(BSR, Op::WordDispPCEmbedded(disp)); break;
                default: {
                    const uint32 condNum = bit::extract<8, 11>(opcode);
                    makeOpN(Bcc, Op::WordDispPCEmbedded(disp)), cond(static_cast<Cond>(condNum));
                    break;
                }
                }
            }
            break;
        }
        case 0x7: {
            if (bit::extract<8>(opcode) == 0) {
                const uint8 value = bit::extract<0, 7>(opcode);
                const uint32 Dn = bit::extract<9, 11>(opcode);
                makeOpLI(MoveQ, Op::UImmEmbedded(value), Op::Dn_W(Dn));
            }
            break;
        }
        case 0x8: {
            const uint16 ea = bit::extract<0, 5>(opcode);
            const uint16 sz = bit::extract<6, 7>(opcode);
            const uint16 Dn = bit::extract<9, 11>(opcode);
            if (bit::extract<6, 8>(opcode) == 0b011) {
                if (kValidDataAddrModes[ea]) {
                    makeOpWI(DivU, eaOp_R_W(ea), Op::Dn_RW(Dn));
                }
            } else if (bit::extract<6, 8>(opcode) == 0b111) {
                if (kValidDataAddrModes[ea]) {
                    makeOpWI(DivS, eaOp_R_W(ea), Op::Dn_RW(Dn));
                }
            } else if (bit::extract<3, 8>(opcode) == 0b100000) {
                const uint16 Dy = bit::extract<0, 2>(opcode);
                const uint16 Dx = bit::extract<9, 11>(opcode);
                makeOpBI(SBCD, Op::Dn_R(Dy), Op::Dn_RW(Dx));
            } else if (bit::extract<3, 8>(opcode) == 0b100001) {
                const uint16 Ay = bit::extract<0, 2>(opcode);
                const uint16 Ax = bit::extract<9, 11>(opcode);
                makeOpBI(SBCD, Op::MinusAtAn_R(Ay), Op::MinusAtAn_RW(Ax));
            } else {
                const bool dir = bit::test<8>(opcode);
                if (dir) {
                    if (kValidMemoryAlterableAddrModes[ea]) {
                        switch (sz) {
                        case 0b00: makeOpB(Or, Op::Dn_R(Dn), eaOp_RW(ea)); break;
                        case 0b01: makeOpW(Or, Op::Dn_R(Dn), eaOp_RW(ea)); break;
                        case 0b10: makeOpL(Or, Op::Dn_R(Dn), eaOp_RW(ea)); break;
                        }
                    }
                } else {
                    if (kValidDataAddrModes[ea]) {
                        switch (sz) {
                        case 0b00: makeOpB(Or, eaOp_R_B(ea), Op::Dn_RW(Dn)); break;
                        case 0b01: makeOpW(Or, eaOp_R_W(ea), Op::Dn_RW(Dn)); break;
                        case 0b10: makeOpL(Or, eaOp_R_L(ea), Op::Dn_RW(Dn)); break;
                        }
                    }
                }
            }
            break;
        }
        case 0x9: {
            const uint16 ea = bit::extract<0, 5>(opcode);
            const uint16 sz = bit::extract<6, 7>(opcode);
            if (bit::extract<6, 7>(opcode) == 0b11) {
                if (kValidAddrModes[ea]) {
                    const uint16 An = bit::extract<9, 11>(opcode);
                    const bool szBit = bit::test<8>(opcode);
                    if (szBit) {
                        makeOpL(SubA, eaOp_R_L(ea), Op::An_RW(An));
                    } else {
                        makeOpW(SubA, eaOp_R_W(ea), Op::An_RW(An));
                    }
                }
            } else if (bit::extract<4, 5>(opcode) == 0b00 && bit::extract<8>(opcode) == 1) {
                const bool rm = bit::test<3>(opcode);
                const uint16 Ry = bit::extract<0, 2>(opcode);
                const uint16 Rx = bit::extract<9, 11>(opcode);
                if (rm) {
                    switch (sz) {
                    case 0b00: makeOpB(SubX, Op::MinusAtAn_R(Ry), Op::MinusAtAn_RW(Rx)); break;
                    case 0b01: makeOpW(SubX, Op::MinusAtAn_R(Ry), Op::MinusAtAn_RW(Rx)); break;
                    case 0b10: makeOpL(SubX, Op::MinusAtAn_R(Ry), Op::MinusAtAn_RW(Rx)); break;
                    }
                } else {
                    switch (sz) {
                    case 0b00: makeOpB(SubX, Op::Dn_R(Ry), Op::Dn_RW(Rx)); break;
                    case 0b01: makeOpW(SubX, Op::Dn_R(Ry), Op::Dn_RW(Rx)); break;
                    case 0b10: makeOpL(SubX, Op::Dn_R(Ry), Op::Dn_RW(Rx)); break;
                    }
                }
            } else {
                const bool dir = bit::test<8>(opcode);
                const uint16 Dn = bit::extract<9, 11>(opcode);
                if (dir) {
                    if (kValidMemoryAlterableAddrModes[ea]) {
                        switch (sz) {
                        case 0b00: makeOpB(Sub, Op::Dn_R(Dn), eaOp_RW(ea)); break;
                        case 0b01: makeOpW(Sub, Op::Dn_R(Dn), eaOp_RW(ea)); break;
                        case 0b10: makeOpL(Sub, Op::Dn_R(Dn), eaOp_RW(ea)); break;
                        }
                    }
                } else {
                    if (kValidAddrModes[ea]) {
                        switch (sz) {
                        case 0b00: makeOpB(Sub, eaOp_R_B(ea), Op::Dn_RW(Dn)); break;
                        case 0b01: makeOpW(Sub, eaOp_R_W(ea), Op::Dn_RW(Dn)); break;
                        case 0b10: makeOpL(Sub, eaOp_R_L(ea), Op::Dn_RW(Dn)); break;
                        }
                    }
                }
            }
            break;
        }
        case 0xA: make0(Illegal1010); break;
        case 0xB: {
            const uint16 ea = bit::extract<0, 5>(opcode);
            const uint16 sz = bit::extract<6, 7>(opcode);
            if (sz == 0b11) {
                if (kValidAddrModes[ea]) {
                    const uint16 An = bit::extract<9, 11>(opcode);
                    const bool szBit = bit::test<8>(opcode);
                    if (szBit) {
                        makeOpL(CmpA, eaOp_R_L(ea), Op::An_R(An));
                    } else {
                        makeOpW(CmpA, eaOp_R_W(ea), Op::An_R(An));
                    }
                }
            } else if (bit::extract<8>(opcode) == 0) {
                if (kValidAddrModes[ea]) {
                    const uint16 Dn = bit::extract<9, 11>(opcode);
                    switch (sz) {
                    case 0b00: makeOpB(Cmp, eaOp_R_B(ea), Op::Dn_R(Dn)); break;
                    case 0b01: makeOpW(Cmp, eaOp_R_W(ea), Op::Dn_R(Dn)); break;
                    case 0b10: makeOpL(Cmp, eaOp_R_L(ea), Op::Dn_R(Dn)); break;
                    }
                }
            } else if (bit::extract<3, 5>(opcode) == 0b001) {
                const uint16 Ay = bit::extract<0, 2>(opcode);
                const uint16 Ax = bit::extract<9, 11>(opcode);
                switch (sz) {
                case 0b00: makeOpB(CmpM, Op::AtAnPlus_R(Ay), Op::AtAnPlus_R(Ax)); break;
                case 0b01: makeOpW(CmpM, Op::AtAnPlus_R(Ay), Op::AtAnPlus_R(Ax)); break;
                case 0b10: makeOpL(CmpM, Op::AtAnPlus_R(Ay), Op::AtAnPlus_R(Ax)); break;
                }
            } else {
                if (kValidDataAlterableAddrModes[ea]) {
                    const uint16 Dn = bit::extract<9, 11>(opcode);
                    switch (sz) {
                    case 0b00: makeOpB(Eor, Op::Dn_R(Dn), eaOp_RW(ea)); break;
                    case 0b01: makeOpW(Eor, Op::Dn_R(Dn), eaOp_RW(ea)); break;
                    case 0b10: makeOpL(Eor, Op::Dn_R(Dn), eaOp_RW(ea)); break;
                    }
                }
            }
            break;
        }
        case 0xC: {
            const uint16 ea = bit::extract<0, 5>(opcode);
            const uint16 sz = bit::extract<6, 7>(opcode);
            const uint32 Ry = bit::extract<0, 2>(opcode);
            const uint32 Rx = bit::extract<9, 11>(opcode);
            if (bit::extract<3, 8>(opcode) == 0b100000) {
                makeOpBI(ABCD, Op::Dn_R(Ry), Op::Dn_RW(Rx));
            } else if (bit::extract<3, 8>(opcode) == 0b100001) {
                makeOpBI(ABCD, Op::MinusAtAn_R(Ry), Op::MinusAtAn_RW(Rx));
            } else if (bit::extract<3, 8>(opcode) == 0b101000) {
                makeOpLI(Exg, Op::Dn_RW(Ry), Op::Dn_RW(Rx));
            } else if (bit::extract<3, 8>(opcode) == 0b101001) {
                makeOpLI(Exg, Op::An_RW(Ry), Op::An_RW(Rx));
            } else if (bit::extract<3, 8>(opcode) == 0b110001) {
                makeOpLI(Exg, Op::Dn_RW(Ry), Op::An_RW(Rx));
            } else if (bit::extract<6, 8>(opcode) == 0b011) {
                if (kValidDataAddrModes[ea]) {
                    makeOpWI(MulU, eaOp_R_W(ea), Op::Dn_RW(Rx));
                }
            } else if (bit::extract<6, 8>(opcode) == 0b111) {
                if (kValidDataAddrModes[ea]) {
                    makeOpWI(MulS, eaOp_R_W(ea), Op::Dn_RW(Rx));
                }
            } else {
                const bool dir = bit::test<8>(opcode);
                const uint16 Dn = bit::extract<9, 11>(opcode);
                if (dir) {
                    if (kValidMemoryAlterableAddrModes[ea]) {
                        switch (sz) {
                        case 0b00: makeOpB(And, Op::Dn_R(Dn), eaOp_RW(ea)); break;
                        case 0b01: makeOpW(And, Op::Dn_R(Dn), eaOp_RW(ea)); break;
                        case 0b10: makeOpL(And, Op::Dn_R(Dn), eaOp_RW(ea)); break;
                        }
                    }
                } else {
                    if (kValidDataAddrModes[ea]) {
                        switch (sz) {
                        case 0b00: makeOpB(And, eaOp_R_B(ea), Op::Dn_RW(Dn)); break;
                        case 0b01: makeOpW(And, eaOp_R_W(ea), Op::Dn_RW(Dn)); break;
                        case 0b10: makeOpL(And, eaOp_R_L(ea), Op::Dn_RW(Dn)); break;
                        }
                    }
                }
            }
            break;
        }
        case 0xD: {
            const uint16 ea = bit::extract<0, 5>(opcode);
            const uint16 sz = bit::extract<6, 7>(opcode);
            if (sz == 0b11) {
                if (kValidAddrModes[ea]) {
                    const uint16 An = bit::extract<9, 11>(opcode);
                    const bool szBit = bit::test<8>(opcode);
                    if (szBit) {
                        makeOpL(AddA, eaOp_R_L(ea), Op::An_RW(An));
                    } else {
                        makeOpW(AddA, eaOp_R_W(ea), Op::An_RW(An));
                    }
                }
            } else if (bit::extract<4, 5>(opcode) == 0b00 && bit::extract<8>(opcode) == 1) {
                const bool rm = bit::test<3>(opcode);
                const uint16 Ry = bit::extract<0, 2>(opcode);
                const uint16 Rx = bit::extract<9, 11>(opcode);
                if (rm) {
                    switch (sz) {
                    case 0b00: makeOpB(AddX, Op::MinusAtAn_R(Ry), Op::MinusAtAn_RW(Rx)); break;
                    case 0b01: makeOpW(AddX, Op::MinusAtAn_R(Ry), Op::MinusAtAn_RW(Rx)); break;
                    case 0b10: makeOpL(AddX, Op::MinusAtAn_R(Ry), Op::MinusAtAn_RW(Rx)); break;
                    }
                } else {
                    switch (sz) {
                    case 0b00: makeOpB(AddX, Op::Dn_R(Ry), Op::Dn_RW(Rx)); break;
                    case 0b01: makeOpW(AddX, Op::Dn_R(Ry), Op::Dn_RW(Rx)); break;
                    case 0b10: makeOpL(AddX, Op::Dn_R(Ry), Op::Dn_RW(Rx)); break;
                    }
                }
            } else {
                const bool dir = bit::test<8>(opcode);
                const uint16 Dn = bit::extract<9, 11>(opcode);
                if (dir) {
                    if (kValidMemoryAlterableAddrModes[ea]) {
                        switch (sz) {
                        case 0b00: makeOpB(Add, Op::Dn_R(Dn), eaOp_RW(ea)); break;
                        case 0b01: makeOpW(Add, Op::Dn_R(Dn), eaOp_RW(ea)); break;
                        case 0b10: makeOpL(Add, Op::Dn_R(Dn), eaOp_RW(ea)); break;
                        }
                    }
                } else {
                    if (kValidAddrModes[ea]) {
                        switch (sz) {
                        case 0b00: makeOpB(Add, eaOp_R_B(ea), Op::Dn_RW(Dn)); break;
                        case 0b01: makeOpW(Add, eaOp_R_W(ea), Op::Dn_RW(Dn)); break;
                        case 0b10: makeOpL(Add, eaOp_R_L(ea), Op::Dn_RW(Dn)); break;
                        }
                    }
                }
            }
            break;
        }
        case 0xE:
            if (bit::extract<6, 7>(opcode) == 0b11 && bit::extract<11>(opcode) == 0) {
                const uint16 ea = bit::extract<0, 5>(opcode);
                const bool dir = bit::test<8>(opcode);
                if (kValidMemoryAlterableAddrModes[ea]) {
                    switch (bit::extract<9, 10>(opcode)) {
                    case 0b00: makeOpW(dir ? ASL : ASR, eaOp_RW(ea)); break;
                    case 0b01: makeOpW(dir ? LSL : LSR, eaOp_RW(ea)); break;
                    case 0b10: makeOpW(dir ? ROXL : ROXR, eaOp_RW(ea)); break;
                    case 0b11: makeOpW(dir ? ROL : ROR, eaOp_RW(ea)); break;
                    }
                }
            } else {
                const bool reg = bit::test<5>(opcode);
                const uint16 sz = bit::extract<6, 7>(opcode);
                const bool dir = bit::test<8>(opcode);

                if (reg) {
                    const uint16 Dy = bit::extract<0, 2>(opcode);
                    const uint16 Dx = bit::extract<9, 11>(opcode);
                    switch (bit::extract<3, 4>(opcode)) {
                    case 0b00:
                        switch (sz) {
                        case 0b00: makeOpB(dir ? ASL : ASR, Op::Dn_R(Dx), Op::Dn_RW(Dy)); break;
                        case 0b01: makeOpW(dir ? ASL : ASR, Op::Dn_R(Dx), Op::Dn_RW(Dy)); break;
                        case 0b10: makeOpL(dir ? ASL : ASR, Op::Dn_R(Dx), Op::Dn_RW(Dy)); break;
                        }
                        break;
                    case 0b01:
                        switch (sz) {
                        case 0b00: makeOpB(dir ? LSL : LSR, Op::Dn_R(Dx), Op::Dn_RW(Dy)); break;
                        case 0b01: makeOpW(dir ? LSL : LSR, Op::Dn_R(Dx), Op::Dn_RW(Dy)); break;
                        case 0b10: makeOpL(dir ? LSL : LSR, Op::Dn_R(Dx), Op::Dn_RW(Dy)); break;
                        }
                        break;
                    case 0b10:
                        switch (sz) {
                        case 0b00: makeOpB(dir ? ROXL : ROXR, Op::Dn_R(Dx), Op::Dn_RW(Dy)); break;
                        case 0b01: makeOpW(dir ? ROXL : ROXR, Op::Dn_R(Dx), Op::Dn_RW(Dy)); break;
                        case 0b10: makeOpL(dir ? ROXL : ROXR, Op::Dn_R(Dx), Op::Dn_RW(Dy)); break;
                        }
                        break;
                    case 0b11:
                        switch (sz) {
                        case 0b00: makeOpB(dir ? ROL : ROR, Op::Dn_R(Dx), Op::Dn_RW(Dy)); break;
                        case 0b01: makeOpW(dir ? ROL : ROR, Op::Dn_R(Dx), Op::Dn_RW(Dy)); break;
                        case 0b10: makeOpL(dir ? ROL : ROR, Op::Dn_R(Dx), Op::Dn_RW(Dy)); break;
                        }
                        break;
                    }
                } else {
                    const uint16 Dy = bit::extract<0, 2>(opcode);
                    uint16 shift = bit::extract<9, 11>(opcode);
                    if (shift == 0) {
                        shift = 8;
                    }
                    switch (bit::extract<3, 4>(opcode)) {
                    case 0b00:
                        switch (sz) {
                        case 0b00: makeOpB(dir ? ASL : ASR, Op::UImmEmbedded(shift), Op::Dn_RW(Dy)); break;
                        case 0b01: makeOpW(dir ? ASL : ASR, Op::UImmEmbedded(shift), Op::Dn_RW(Dy)); break;
                        case 0b10: makeOpL(dir ? ASL : ASR, Op::UImmEmbedded(shift), Op::Dn_RW(Dy)); break;
                        }
                        break;
                    case 0b01:
                        switch (sz) {
                        case 0b00: makeOpB(dir ? LSL : LSR, Op::UImmEmbedded(shift), Op::Dn_RW(Dy)); break;
                        case 0b01: makeOpW(dir ? LSL : LSR, Op::UImmEmbedded(shift), Op::Dn_RW(Dy)); break;
                        case 0b10: makeOpL(dir ? LSL : LSR, Op::UImmEmbedded(shift), Op::Dn_RW(Dy)); break;
                        }
                        break;
                    case 0b10:
                        switch (sz) {
                        case 0b00: makeOpB(dir ? ROXL : ROXR, Op::UImmEmbedded(shift), Op::Dn_RW(Dy)); break;
                        case 0b01: makeOpW(dir ? ROXL : ROXR, Op::UImmEmbedded(shift), Op::Dn_RW(Dy)); break;
                        case 0b10: makeOpL(dir ? ROXL : ROXR, Op::UImmEmbedded(shift), Op::Dn_RW(Dy)); break;
                        }
                        break;
                    case 0b11:
                        switch (sz) {
                        case 0b00: makeOpB(dir ? ROL : ROR, Op::UImmEmbedded(shift), Op::Dn_RW(Dy)); break;
                        case 0b01: makeOpW(dir ? ROL : ROR, Op::UImmEmbedded(shift), Op::Dn_RW(Dy)); break;
                        case 0b10: makeOpL(dir ? ROL : ROR, Op::UImmEmbedded(shift), Op::Dn_RW(Dy)); break;
                        }
                        break;
                    }
                }
            }
            break;
        case 0xF: make0(Illegal1111); break;
        }
    }
}

DisassembledInstruction Disassemble(std::function<uint16()> fetcher) {
    const uint16 opcode = fetcher();
    const DisassemblyInfo &opDisasm = DisassemblyTable::s_instance.infos[opcode];

    DisassembledInstruction disasm{
        .info = opDisasm,
    };
    disasm.opcodes.push_back(opcode);

    auto fetch = [&] {
        const uint16 value = fetcher();
        disasm.opcodes.push_back(value);
        return value;
    };

    auto translateOperand = [&](const Operand &op, OperandDetails &det) {
        using Type = Operand::Type;
        switch (op.type) {
        case Type::AtDispAn: det.immDisp = static_cast<sint16>(fetch()); break;
        case Type::AtDispAnIx: {
            const uint16 briefExtWord = fetch();
            det.immDisp = static_cast<sint8>(bit::extract<0, 7>(briefExtWord));
            det.ixLong = bit::test<11>(briefExtWord);
            det.ix = bit::extract<12, 15>(briefExtWord);
            break;
        }
        case Type::AtDispPC: det.immDisp = static_cast<sint16>(fetch()) - 2; break;
        case Type::AtDispPCIx: {
            const uint16 briefExtWord = fetch();
            det.immDisp = static_cast<sint8>(bit::extract<0, 7>(briefExtWord)) - 2;
            det.ixLong = bit::test<11>(briefExtWord);
            det.ix = bit::extract<12, 15>(briefExtWord);
            break;
        }
        case Type::AtImmWord: det.immDisp = static_cast<sint16>(fetch()); break;
        case Type::AtImmLong: {
            const uint32 addressHigh = fetch();
            const uint32 addressLow = fetch();
            det.immDisp = (addressHigh << 16u) | addressLow;
            break;
        }
        case Type::UImm8Fetched: det.immDisp = static_cast<uint8>(fetch()); break;
        case Type::UImm16Fetched: det.immDisp = fetch(); break;
        case Type::UImm32Fetched: {
            det.immDisp = fetch();
            det.immDisp = (det.immDisp << 16u) | fetch();
            break;
        }

        case Type::WordDispPCFetched: det.immDisp = static_cast<sint16>(fetch()) - 2; break;

        case Type::RegList: det.regList = fetch(); break;
        case Type::RevRegList: det.regList = bit::reverse(fetch()); break;

        default: break;
        }
    };

    translateOperand(opDisasm.op1, disasm.op1);
    translateOperand(opDisasm.op2, disasm.op2);

    return disasm;
}

DisassemblyTable DisassemblyTable::s_instance{};

} // namespace brimir::m68k
