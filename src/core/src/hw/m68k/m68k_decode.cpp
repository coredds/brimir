#include <brimir/hw/m68k/m68k.hpp>

#include "m68k_addr_modes.hpp"

#include <brimir/util/bit_ops.hpp>

namespace brimir::m68k {

DecodeTable BuildDecodeTable() {
    DecodeTable table{};
    table.opcodeTypes.fill(OpcodeType::Illegal);

    for (uint32 instr = 0; instr < 0x10000; instr++) {
        auto &opcode = table.opcodeTypes[instr];

        auto legalIf = [](OpcodeType type, bool cond) { return cond ? type : OpcodeType::Illegal; };

        switch (instr >> 12u) {
        case 0x0: {
            const uint16 ea = bit::extract<0, 5>(instr);
            const uint16 sz = bit::extract<6, 7>(instr);
            if (instr == 0x003C) {
                opcode = OpcodeType::OrI_CCR;
            } else if (instr == 0x007C) {
                opcode = OpcodeType::OrI_SR;
            } else if (instr == 0x023C) {
                opcode = OpcodeType::AndI_CCR;
            } else if (instr == 0x027C) {
                opcode = OpcodeType::AndI_SR;
            } else if (instr == 0x0A3C) {
                opcode = OpcodeType::EorI_CCR;
            } else if (instr == 0x0A7C) {
                opcode = OpcodeType::EorI_SR;
            } else if (bit::extract<3, 5>(instr) == 0b001 && bit::extract<8>(instr) == 1) {
                const bool szBit = bit::test<6>(instr);
                if (bit::test<7>(instr)) {
                    opcode = szBit ? OpcodeType::MoveP_Dx_Ay_L : OpcodeType::MoveP_Dx_Ay_W;
                } else {
                    opcode = szBit ? OpcodeType::MoveP_Ay_Dx_L : OpcodeType::MoveP_Ay_Dx_W;
                }
            } else if (bit::extract<6, 8>(instr) == 0b100) {
                if ((ea >> 3u) == 0b000) {
                    opcode = OpcodeType::BTst_R_Dn;
                } else {
                    opcode = legalIf(OpcodeType::BTst_R_EA, kValidDataAddrModes[ea]);
                }
            } else if (bit::extract<6, 8>(instr) == 0b101) {
                if ((ea >> 3u) == 0b000) {
                    opcode = OpcodeType::BChg_R_Dn;
                } else {
                    opcode = legalIf(OpcodeType::BChg_R_EA, kValidDataAlterableAddrModes[ea]);
                }
            } else if (bit::extract<6, 8>(instr) == 0b110) {
                if ((ea >> 3u) == 0b000) {
                    opcode = OpcodeType::BClr_R_Dn;
                } else {
                    opcode = legalIf(OpcodeType::BClr_R_EA, kValidDataAlterableAddrModes[ea]);
                }
            } else if (bit::extract<6, 8>(instr) == 0b111) {
                if ((ea >> 3u) == 0b000) {
                    opcode = OpcodeType::BSet_R_Dn;
                } else {
                    opcode = legalIf(OpcodeType::BSet_R_EA, kValidDataAlterableAddrModes[ea]);
                }
            } else if (bit::extract<6, 11>(instr) == 0b100000) {
                if ((ea >> 3u) == 0b000) {
                    opcode = OpcodeType::BTst_I_Dn;
                } else {
                    opcode = legalIf(OpcodeType::BTst_I_EA, kValidDataAddrModes[ea]);
                }
            } else if (bit::extract<6, 11>(instr) == 0b100001) {
                if ((ea >> 3u) == 0b000) {
                    opcode = OpcodeType::BChg_I_Dn;
                } else {
                    opcode = legalIf(OpcodeType::BChg_I_EA, kValidDataAlterableAddrModes[ea]);
                }
            } else if (bit::extract<6, 11>(instr) == 0b100010) {
                if ((ea >> 3u) == 0b000) {
                    opcode = OpcodeType::BClr_I_Dn;
                } else {
                    opcode = legalIf(OpcodeType::BClr_I_EA, kValidDataAlterableAddrModes[ea]);
                }
            } else if (bit::extract<6, 11>(instr) == 0b100011) {
                if ((ea >> 3u) == 0b000) {
                    opcode = OpcodeType::BSet_I_Dn;
                } else {
                    opcode = legalIf(OpcodeType::BSet_I_EA, kValidDataAlterableAddrModes[ea]);
                }
            } else if (bit::extract<8, 11>(instr) == 0b0000) {
                switch (sz) {
                case 0b00: opcode = legalIf(OpcodeType::OrI_EA_B, kValidDataAlterableAddrModes[ea]); break;
                case 0b01: opcode = legalIf(OpcodeType::OrI_EA_W, kValidDataAlterableAddrModes[ea]); break;
                case 0b10: opcode = legalIf(OpcodeType::OrI_EA_L, kValidDataAlterableAddrModes[ea]); break;
                }
            } else if (bit::extract<8, 11>(instr) == 0b0010) {
                switch (sz) {
                case 0b00: opcode = legalIf(OpcodeType::AndI_EA_B, kValidDataAlterableAddrModes[ea]); break;
                case 0b01: opcode = legalIf(OpcodeType::AndI_EA_W, kValidDataAlterableAddrModes[ea]); break;
                case 0b10: opcode = legalIf(OpcodeType::AndI_EA_L, kValidDataAlterableAddrModes[ea]); break;
                }
            } else if (bit::extract<8, 11>(instr) == 0b1010) {
                switch (sz) {
                case 0b00: opcode = legalIf(OpcodeType::EorI_EA_B, kValidDataAlterableAddrModes[ea]); break;
                case 0b01: opcode = legalIf(OpcodeType::EorI_EA_W, kValidDataAlterableAddrModes[ea]); break;
                case 0b10: opcode = legalIf(OpcodeType::EorI_EA_L, kValidDataAlterableAddrModes[ea]); break;
                }
            } else if (bit::extract<8, 11>(instr) == 0b0100) {
                switch (sz) {
                case 0b00: opcode = legalIf(OpcodeType::SubI_B, kValidDataAlterableAddrModes[ea]); break;
                case 0b01: opcode = legalIf(OpcodeType::SubI_W, kValidDataAlterableAddrModes[ea]); break;
                case 0b10: opcode = legalIf(OpcodeType::SubI_L, kValidDataAlterableAddrModes[ea]); break;
                }
            } else if (bit::extract<8, 11>(instr) == 0b0110) {
                switch (sz) {
                case 0b00: opcode = legalIf(OpcodeType::AddI_B, kValidDataAlterableAddrModes[ea]); break;
                case 0b01: opcode = legalIf(OpcodeType::AddI_W, kValidDataAlterableAddrModes[ea]); break;
                case 0b10: opcode = legalIf(OpcodeType::AddI_L, kValidDataAlterableAddrModes[ea]); break;
                }
            } else if (bit::extract<8, 11>(instr) == 0b1100) {
                switch (sz) {
                case 0b00: opcode = legalIf(OpcodeType::CmpI_B, kValidDataAddrModes[ea]); break;
                case 0b01: opcode = legalIf(OpcodeType::CmpI_W, kValidDataAddrModes[ea]); break;
                case 0b10: opcode = legalIf(OpcodeType::CmpI_L, kValidDataAddrModes[ea]); break;
                }
            }
            break;
        }
        case 0x1: [[fallthrough]];
        case 0x2: [[fallthrough]];
        case 0x3:
            if (bit::extract<6, 8>(instr) == 0b001) {
                const uint16 size = bit::extract<12, 13>(instr);
                const uint16 ea = bit::extract<0, 5>(instr);

                switch (size) {
                case 0b11: opcode = OpcodeType::MoveA_W; break;
                case 0b10: opcode = OpcodeType::MoveA_L; break;
                }
                opcode = legalIf(opcode, kValidAddrModes[ea]);
            } else {
                const uint16 size = bit::extract<12, 13>(instr);
                const uint16 srcEA = bit::extract<0, 5>(instr);
                const uint16 dstEA = (bit::extract<6, 8>(instr) << 3) | bit::extract<9, 11>(instr);

                // Note the swapped bit order between word and longword moves
                switch (size) {
                case 0b01: opcode = OpcodeType::Move_EA_EA_B; break;
                case 0b11: opcode = OpcodeType::Move_EA_EA_W; break;
                case 0b10: opcode = OpcodeType::Move_EA_EA_L; break;
                }
                opcode = legalIf(opcode, kValidDataAlterableAddrModes[dstEA] && kValidAddrModes[srcEA]);
            }
            break;
        case 0x4: {
            const uint16 ea = bit::extract<0, 5>(instr);
            const uint16 sz = bit::extract<6, 7>(instr);
            if (instr == 0x4E70) {
                opcode = OpcodeType::Reset;
            } else if (instr == 0x4E71) {
                opcode = OpcodeType::Noop;
            } else if (instr == 0x4E72) {
                opcode = OpcodeType::Stop;
            } else if (instr == 0x4E73) {
                opcode = OpcodeType::RTE;
            } else if (instr == 0x4E75) {
                opcode = OpcodeType::RTS;
            } else if (instr == 0x4E76) {
                opcode = OpcodeType::TrapV;
            } else if (instr == 0x4E77) {
                opcode = OpcodeType::RTR;
            } else if (instr == 0x4AFC) {
                opcode = OpcodeType::Illegal;
            } else if (bit::extract<3, 11>(instr) == 0b100001000) {
                opcode = OpcodeType::Swap;
            } else if (bit::extract<3, 11>(instr) == 0b100010000) {
                opcode = OpcodeType::Ext_W;
            } else if (bit::extract<3, 11>(instr) == 0b100011000) {
                opcode = OpcodeType::Ext_L;
            } else if (bit::extract<3, 11>(instr) == 0b111001010) {
                opcode = OpcodeType::Link;
            } else if (bit::extract<3, 11>(instr) == 0b111001011) {
                opcode = OpcodeType::Unlink;
            } else if (bit::extract<3, 11>(instr) == 0b111001100) {
                opcode = OpcodeType::Move_An_USP;
            } else if (bit::extract<3, 11>(instr) == 0b111001101) {
                opcode = OpcodeType::Move_USP_An;
            } else if (bit::extract<4, 11>(instr) == 0b11100100) {
                opcode = OpcodeType::Trap;
            } else if (bit::extract<6, 11>(instr) == 0b000011) {
                opcode = legalIf(OpcodeType::Move_SR_EA, kValidDataAlterableAddrModes[ea]);
            } else if (bit::extract<6, 11>(instr) == 0b001011) {
                opcode = legalIf(OpcodeType::Move_CCR_EA, kValidDataAlterableAddrModes[ea]);
            } else if (bit::extract<6, 11>(instr) == 0b010011) {
                opcode = legalIf(OpcodeType::Move_EA_CCR, kValidDataAddrModes[ea]);
            } else if (bit::extract<6, 11>(instr) == 0b011011) {
                opcode = legalIf(OpcodeType::Move_EA_SR, kValidDataAddrModes[ea]);
            } else if (bit::extract<6, 11>(instr) == 0b100000) {
                opcode = legalIf(OpcodeType::NBCD, kValidDataAlterableAddrModes[ea]);
            } else if (bit::extract<6, 11>(instr) == 0b101011) {
                opcode = legalIf(OpcodeType::TAS, kValidDataAlterableAddrModes[ea]);
            } else if (bit::extract<6, 11>(instr) == 0b100001) {
                opcode = legalIf(OpcodeType::PEA, kValidControlAddrModes[ea]);
            } else if (bit::extract<6, 11>(instr) == 0b111010) {
                opcode = legalIf(OpcodeType::JSR, kValidControlAddrModes[ea]);
            } else if (bit::extract<6, 11>(instr) == 0b111011) {
                opcode = legalIf(OpcodeType::Jmp, kValidControlAddrModes[ea]);
            } else if (bit::extract<7, 11>(instr) == 0b10001) {
                const bool isPredecrement = (ea >> 3u) == 0b100;
                const bool sz = bit::test<6>(instr);
                if (isPredecrement) {
                    opcode = sz ? OpcodeType::MoveM_Rs_PD_L : OpcodeType::MoveM_Rs_PD_W;
                } else {
                    opcode = legalIf(sz ? OpcodeType::MoveM_Rs_EA_L : OpcodeType::MoveM_Rs_EA_W,
                                     kValidControlAlterableAddrModes[ea]);
                }
            } else if (bit::extract<7, 11>(instr) == 0b11001) {
                const bool isPostincrement = (ea >> 3u) == 0b011;
                const bool sz = bit::test<6>(instr);
                if (isPostincrement) {
                    opcode = sz ? OpcodeType::MoveM_PI_Rs_L : OpcodeType::MoveM_PI_Rs_W;
                } else {
                    const uint16 Xn = bit::extract<0, 2>(instr);
                    const uint16 M = bit::extract<3, 5>(instr);
                    const bool isProgramAccess = M == 7 && (Xn == 2 || Xn == 3);
                    if (isProgramAccess) {
                        opcode = legalIf(sz ? OpcodeType::MoveM_EA_Rs_C_L : OpcodeType::MoveM_EA_Rs_C_W,
                                         kValidControlAddrModes[ea]);
                    } else {
                        opcode = legalIf(sz ? OpcodeType::MoveM_EA_Rs_D_L : OpcodeType::MoveM_EA_Rs_D_W,
                                         kValidControlAddrModes[ea]);
                    }
                }
            } else if (bit::extract<8, 11>(instr) == 0b0000) {
                switch (sz) {
                case 0b00: opcode = legalIf(OpcodeType::NegX_B, kValidDataAlterableAddrModes[ea]); break;
                case 0b01: opcode = legalIf(OpcodeType::NegX_W, kValidDataAlterableAddrModes[ea]); break;
                case 0b10: opcode = legalIf(OpcodeType::NegX_L, kValidDataAlterableAddrModes[ea]); break;
                }
            } else if (bit::extract<8, 11>(instr) == 0b0010) {
                switch (sz) {
                case 0b00: opcode = legalIf(OpcodeType::Clr_B, kValidDataAlterableAddrModes[ea]); break;
                case 0b01: opcode = legalIf(OpcodeType::Clr_W, kValidDataAlterableAddrModes[ea]); break;
                case 0b10: opcode = legalIf(OpcodeType::Clr_L, kValidDataAlterableAddrModes[ea]); break;
                }
            } else if (bit::extract<8, 11>(instr) == 0b0100) {
                switch (sz) {
                case 0b00: opcode = legalIf(OpcodeType::Neg_B, kValidDataAlterableAddrModes[ea]); break;
                case 0b01: opcode = legalIf(OpcodeType::Neg_W, kValidDataAlterableAddrModes[ea]); break;
                case 0b10: opcode = legalIf(OpcodeType::Neg_L, kValidDataAlterableAddrModes[ea]); break;
                }
            } else if (bit::extract<8, 11>(instr) == 0b0110) {
                switch (sz) {
                case 0b00: opcode = legalIf(OpcodeType::Not_B, kValidDataAlterableAddrModes[ea]); break;
                case 0b01: opcode = legalIf(OpcodeType::Not_W, kValidDataAlterableAddrModes[ea]); break;
                case 0b10: opcode = legalIf(OpcodeType::Not_L, kValidDataAlterableAddrModes[ea]); break;
                }
            } else if (bit::extract<8, 11>(instr) == 0b1010) {
                switch (sz) {
                case 0b00: opcode = legalIf(OpcodeType::Tst_B, kValidDataAlterableAddrModes[ea]); break;
                case 0b01: opcode = legalIf(OpcodeType::Tst_W, kValidDataAlterableAddrModes[ea]); break;
                case 0b10: opcode = legalIf(OpcodeType::Tst_L, kValidDataAlterableAddrModes[ea]); break;
                }
            } else if (bit::extract<6, 8>(instr) == 0b110) {
                opcode = legalIf(OpcodeType::Chk, kValidDataAddrModes[ea]);
            } else if (bit::extract<6, 8>(instr) == 0b111) {
                opcode = legalIf(OpcodeType::LEA, kValidControlAddrModes[ea]);
            }
            break;
        }
        case 0x5: {
            const uint16 ea = bit::extract<0, 5>(instr);
            if (bit::extract<3, 7>(instr) == 0b11001) {
                opcode = OpcodeType::DBcc;
            } else if (bit::extract<6, 7>(instr) == 0b11) {
                opcode = legalIf(OpcodeType::Scc, kValidDataAlterableAddrModes[ea]);
            } else {
                const uint16 M = bit::extract<3, 5>(instr);
                const uint16 sz = bit::extract<6, 7>(instr);
                const bool isAn = M == 0b001;
                if (bit::extract<8>(instr) == 1) {
                    if (isAn) {
                        switch (sz) {
                        case 0b01: opcode = OpcodeType::SubQ_An_W; break;
                        case 0b10: opcode = OpcodeType::SubQ_An_L; break;
                        }
                    } else {
                        switch (sz) {
                        case 0b00: opcode = legalIf(OpcodeType::SubQ_EA_B, kValidAlterableAddrModes[ea]); break;
                        case 0b01: opcode = legalIf(OpcodeType::SubQ_EA_W, kValidAlterableAddrModes[ea]); break;
                        case 0b10: opcode = legalIf(OpcodeType::SubQ_EA_L, kValidAlterableAddrModes[ea]); break;
                        }
                    }
                } else {
                    if (isAn) {
                        switch (sz) {
                        case 0b01: opcode = OpcodeType::AddQ_An_W; break;
                        case 0b10: opcode = OpcodeType::AddQ_An_L; break;
                        }
                    } else {
                        switch (sz) {
                        case 0b00: opcode = legalIf(OpcodeType::AddQ_EA_B, kValidAlterableAddrModes[ea]); break;
                        case 0b01: opcode = legalIf(OpcodeType::AddQ_EA_W, kValidAlterableAddrModes[ea]); break;
                        case 0b10: opcode = legalIf(OpcodeType::AddQ_EA_L, kValidAlterableAddrModes[ea]); break;
                        }
                    }
                }
            }
            break;
        }
        case 0x6:
            switch (bit::extract<8, 11>(instr)) {
            case 0b0000: opcode = OpcodeType::BRA; break;
            case 0b0001: opcode = OpcodeType::BSR; break;
            default: opcode = OpcodeType::Bcc; break;
            }
            break;
        case 0x7: opcode = legalIf(OpcodeType::MoveQ, bit::extract<8>(instr) == 0); break;
        case 0x8: {
            const uint16 ea = bit::extract<0, 5>(instr);
            const uint16 sz = bit::extract<6, 7>(instr);
            if (bit::extract<6, 8>(instr) == 0b011) {
                opcode = legalIf(OpcodeType::DivU, kValidDataAddrModes[ea]);
            } else if (bit::extract<6, 8>(instr) == 0b111) {
                opcode = legalIf(OpcodeType::DivS, kValidDataAddrModes[ea]);
            } else if (bit::extract<3, 8>(instr) == 0b100000) {
                opcode = OpcodeType::SBCD_R;
            } else if (bit::extract<3, 8>(instr) == 0b100001) {
                opcode = OpcodeType::SBCD_M;
            } else {
                const bool dir = bit::test<8>(instr);
                if (dir) {
                    switch (sz) {
                    case 0b00: opcode = legalIf(OpcodeType::Or_Dn_EA_B, kValidMemoryAlterableAddrModes[ea]); break;
                    case 0b01: opcode = legalIf(OpcodeType::Or_Dn_EA_W, kValidMemoryAlterableAddrModes[ea]); break;
                    case 0b10: opcode = legalIf(OpcodeType::Or_Dn_EA_L, kValidMemoryAlterableAddrModes[ea]); break;
                    }
                } else {
                    switch (sz) {
                    case 0b00: opcode = legalIf(OpcodeType::Or_EA_Dn_B, kValidDataAddrModes[ea]); break;
                    case 0b01: opcode = legalIf(OpcodeType::Or_EA_Dn_W, kValidDataAddrModes[ea]); break;
                    case 0b10: opcode = legalIf(OpcodeType::Or_EA_Dn_L, kValidDataAddrModes[ea]); break;
                    }
                }
            }
            break;
        }
        case 0x9: {
            const uint16 ea = bit::extract<0, 5>(instr);
            const uint16 sz = bit::extract<6, 7>(instr);
            if (bit::extract<6, 7>(instr) == 0b11) {
                const bool szBit = bit::test<8>(instr);
                opcode = legalIf(szBit ? OpcodeType::SubA_L : OpcodeType::SubA_W, kValidAddrModes[ea]);
            } else if (bit::extract<4, 5>(instr) == 0b00 && bit::extract<8>(instr) == 1) {
                const bool rm = bit::test<3>(instr);
                if (rm) {
                    switch (sz) {
                    case 0b00: opcode = OpcodeType::SubX_M_B; break;
                    case 0b01: opcode = OpcodeType::SubX_M_W; break;
                    case 0b10: opcode = OpcodeType::SubX_M_L; break;
                    }
                } else {
                    switch (sz) {
                    case 0b00: opcode = OpcodeType::SubX_R_B; break;
                    case 0b01: opcode = OpcodeType::SubX_R_W; break;
                    case 0b10: opcode = OpcodeType::SubX_R_L; break;
                    }
                }
            } else {
                const bool dir = bit::test<8>(instr);
                if (dir) {
                    switch (sz) {
                    case 0b00: opcode = legalIf(OpcodeType::Sub_Dn_EA_B, kValidMemoryAlterableAddrModes[ea]); break;
                    case 0b01: opcode = legalIf(OpcodeType::Sub_Dn_EA_W, kValidMemoryAlterableAddrModes[ea]); break;
                    case 0b10: opcode = legalIf(OpcodeType::Sub_Dn_EA_L, kValidMemoryAlterableAddrModes[ea]); break;
                    }
                } else {
                    switch (sz) {
                    case 0b00: opcode = legalIf(OpcodeType::Sub_EA_Dn_B, kValidAddrModes[ea]); break;
                    case 0b01: opcode = legalIf(OpcodeType::Sub_EA_Dn_W, kValidAddrModes[ea]); break;
                    case 0b10: opcode = legalIf(OpcodeType::Sub_EA_Dn_L, kValidAddrModes[ea]); break;
                    }
                }
            }
            break;
        }
        case 0xA: opcode = OpcodeType::Illegal1010; break;
        case 0xB: {
            const uint16 ea = bit::extract<0, 5>(instr);
            const uint16 sz = bit::extract<6, 7>(instr);
            if (sz == 0b11) {
                const bool szBit = bit::test<8>(instr);
                opcode = legalIf(szBit ? OpcodeType::CmpA_L : OpcodeType::CmpA_W, kValidAddrModes[ea]);
            } else if (bit::extract<8>(instr) == 0) {
                switch (sz) {
                case 0b00: opcode = legalIf(OpcodeType::Cmp_B, kValidAddrModes[ea]); break;
                case 0b01: opcode = legalIf(OpcodeType::Cmp_W, kValidAddrModes[ea]); break;
                case 0b10: opcode = legalIf(OpcodeType::Cmp_L, kValidAddrModes[ea]); break;
                }
            } else if (bit::extract<3, 5>(instr) == 0b001) {
                switch (sz) {
                case 0b00: opcode = OpcodeType::CmpM_B; break;
                case 0b01: opcode = OpcodeType::CmpM_W; break;
                case 0b10: opcode = OpcodeType::CmpM_L; break;
                }
            } else {
                switch (sz) {
                case 0b00: opcode = legalIf(OpcodeType::Eor_Dn_EA_B, kValidDataAlterableAddrModes[ea]); break;
                case 0b01: opcode = legalIf(OpcodeType::Eor_Dn_EA_W, kValidDataAlterableAddrModes[ea]); break;
                case 0b10: opcode = legalIf(OpcodeType::Eor_Dn_EA_L, kValidDataAlterableAddrModes[ea]); break;
                }
            }
            break;
        }
        case 0xC: {
            const uint16 ea = bit::extract<0, 5>(instr);
            const uint16 sz = bit::extract<6, 7>(instr);
            if (bit::extract<3, 8>(instr) == 0b100000) {
                opcode = OpcodeType::ABCD_R;
            } else if (bit::extract<3, 8>(instr) == 0b100001) {
                opcode = OpcodeType::ABCD_M;
            } else if (bit::extract<3, 8>(instr) == 0b101000) {
                opcode = OpcodeType::Exg_Dn_Dn;
            } else if (bit::extract<3, 8>(instr) == 0b101001) {
                opcode = OpcodeType::Exg_An_An;
            } else if (bit::extract<3, 8>(instr) == 0b110001) {
                opcode = OpcodeType::Exg_Dn_An;
            } else if (bit::extract<6, 8>(instr) == 0b011) {
                opcode = legalIf(OpcodeType::MulU, kValidDataAddrModes[ea]);
            } else if (bit::extract<6, 8>(instr) == 0b111) {
                opcode = legalIf(OpcodeType::MulS, kValidDataAddrModes[ea]);
            } else {
                const bool dir = bit::test<8>(instr);
                if (dir) {
                    switch (sz) {
                    case 0b00: opcode = legalIf(OpcodeType::And_Dn_EA_B, kValidMemoryAlterableAddrModes[ea]); break;
                    case 0b01: opcode = legalIf(OpcodeType::And_Dn_EA_W, kValidMemoryAlterableAddrModes[ea]); break;
                    case 0b10: opcode = legalIf(OpcodeType::And_Dn_EA_L, kValidMemoryAlterableAddrModes[ea]); break;
                    }
                } else {
                    switch (sz) {
                    case 0b00: opcode = legalIf(OpcodeType::And_EA_Dn_B, kValidDataAddrModes[ea]); break;
                    case 0b01: opcode = legalIf(OpcodeType::And_EA_Dn_W, kValidDataAddrModes[ea]); break;
                    case 0b10: opcode = legalIf(OpcodeType::And_EA_Dn_L, kValidDataAddrModes[ea]); break;
                    }
                }
            }
            break;
        }
        case 0xD: {
            const uint16 ea = bit::extract<0, 5>(instr);
            const uint16 sz = bit::extract<6, 7>(instr);
            if (sz == 0b11) {
                const bool szBit = bit::test<8>(instr);
                opcode = legalIf(szBit ? OpcodeType::AddA_L : OpcodeType::AddA_W, kValidAddrModes[ea]);
            } else if (bit::extract<4, 5>(instr) == 0b00 && bit::extract<8>(instr) == 1) {
                const bool rm = bit::test<3>(instr);
                if (rm) {
                    switch (sz) {
                    case 0b00: opcode = OpcodeType::AddX_M_B; break;
                    case 0b01: opcode = OpcodeType::AddX_M_W; break;
                    case 0b10: opcode = OpcodeType::AddX_M_L; break;
                    }
                } else {
                    switch (sz) {
                    case 0b00: opcode = OpcodeType::AddX_R_B; break;
                    case 0b01: opcode = OpcodeType::AddX_R_W; break;
                    case 0b10: opcode = OpcodeType::AddX_R_L; break;
                    }
                }
            } else {
                const bool dir = bit::test<8>(instr);
                if (dir) {
                    switch (sz) {
                    case 0b00: opcode = legalIf(OpcodeType::Add_Dn_EA_B, kValidMemoryAlterableAddrModes[ea]); break;
                    case 0b01: opcode = legalIf(OpcodeType::Add_Dn_EA_W, kValidMemoryAlterableAddrModes[ea]); break;
                    case 0b10: opcode = legalIf(OpcodeType::Add_Dn_EA_L, kValidMemoryAlterableAddrModes[ea]); break;
                    }
                } else {
                    switch (sz) {
                    case 0b00: opcode = legalIf(OpcodeType::Add_EA_Dn_B, kValidAddrModes[ea]); break;
                    case 0b01: opcode = legalIf(OpcodeType::Add_EA_Dn_W, kValidAddrModes[ea]); break;
                    case 0b10: opcode = legalIf(OpcodeType::Add_EA_Dn_L, kValidAddrModes[ea]); break;
                    }
                }
            }
            break;
        }
        case 0xE:
            if (bit::extract<6, 7>(instr) == 0b11 && bit::extract<11>(instr) == 0) {
                const uint16 ea = bit::extract<0, 5>(instr);
                const bool dir = bit::test<8>(instr);
                switch (bit::extract<9, 10>(instr)) {
                case 0b00: opcode = dir ? OpcodeType::ASL_M : OpcodeType::ASR_M; break;
                case 0b01: opcode = dir ? OpcodeType::LSL_M : OpcodeType::LSR_M; break;
                case 0b10: opcode = dir ? OpcodeType::ROXL_M : OpcodeType::ROXR_M; break;
                case 0b11: opcode = dir ? OpcodeType::ROL_M : OpcodeType::ROR_M; break;
                }
                opcode = legalIf(opcode, kValidMemoryAlterableAddrModes[ea]);
            } else {
                const bool reg = bit::test<5>(instr);
                const uint16 sz = bit::extract<6, 7>(instr);
                const bool dir = bit::test<8>(instr);

                switch (bit::extract<3, 4>(instr)) {
                case 0b00:
                    switch (sz) {
                    case 0b00:
                        opcode = reg ? (dir ? OpcodeType::ASL_R_B : OpcodeType::ASR_R_B)
                                     : (dir ? OpcodeType::ASL_I_B : OpcodeType::ASR_I_B);
                        break;
                    case 0b01:
                        opcode = reg ? (dir ? OpcodeType::ASL_R_W : OpcodeType::ASR_R_W)
                                     : (dir ? OpcodeType::ASL_I_W : OpcodeType::ASR_I_W);
                        break;
                    case 0b10:
                        opcode = reg ? (dir ? OpcodeType::ASL_R_L : OpcodeType::ASR_R_L)
                                     : (dir ? OpcodeType::ASL_I_L : OpcodeType::ASR_I_L);
                        break;
                    }
                    break;
                case 0b01:
                    switch (sz) {
                    case 0b00:
                        opcode = reg ? (dir ? OpcodeType::LSL_R_B : OpcodeType::LSR_R_B)
                                     : (dir ? OpcodeType::LSL_I_B : OpcodeType::LSR_I_B);
                        break;
                    case 0b01:
                        opcode = reg ? (dir ? OpcodeType::LSL_R_W : OpcodeType::LSR_R_W)
                                     : (dir ? OpcodeType::LSL_I_W : OpcodeType::LSR_I_W);
                        break;
                    case 0b10:
                        opcode = reg ? (dir ? OpcodeType::LSL_R_L : OpcodeType::LSR_R_L)
                                     : (dir ? OpcodeType::LSL_I_L : OpcodeType::LSR_I_L);
                        break;
                    }
                    break;
                case 0b10:
                    switch (sz) {
                    case 0b00:
                        opcode = reg ? (dir ? OpcodeType::ROXL_R_B : OpcodeType::ROXR_R_B)
                                     : (dir ? OpcodeType::ROXL_I_B : OpcodeType::ROXR_I_B);
                        break;
                    case 0b01:
                        opcode = reg ? (dir ? OpcodeType::ROXL_R_W : OpcodeType::ROXR_R_W)
                                     : (dir ? OpcodeType::ROXL_I_W : OpcodeType::ROXR_I_W);
                        break;
                    case 0b10:
                        opcode = reg ? (dir ? OpcodeType::ROXL_R_L : OpcodeType::ROXR_R_L)
                                     : (dir ? OpcodeType::ROXL_I_L : OpcodeType::ROXR_I_L);
                        break;
                    }
                    break;
                case 0b11:
                    switch (sz) {
                    case 0b00:
                        opcode = reg ? (dir ? OpcodeType::ROL_R_B : OpcodeType::ROR_R_B)
                                     : (dir ? OpcodeType::ROL_I_B : OpcodeType::ROR_I_B);
                        break;
                    case 0b01:
                        opcode = reg ? (dir ? OpcodeType::ROL_R_W : OpcodeType::ROR_R_W)
                                     : (dir ? OpcodeType::ROL_I_W : OpcodeType::ROR_I_W);
                        break;
                    case 0b10:
                        opcode = reg ? (dir ? OpcodeType::ROL_R_L : OpcodeType::ROR_R_L)
                                     : (dir ? OpcodeType::ROL_I_L : OpcodeType::ROR_I_L);
                        break;
                    }
                    break;
                }
            }
            break;
        case 0xF: opcode = OpcodeType::Illegal1111; break;
        }
    }

    return table;
}

DecodeTable g_decodeTable = BuildDecodeTable();

} // namespace brimir::m68k
