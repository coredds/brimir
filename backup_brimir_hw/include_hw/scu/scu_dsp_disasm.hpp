#pragma once

#include <brimir/core/types.hpp>

#include <string_view>

namespace brimir::scu {

struct SCUDSPInstruction {
    enum class Type : uint8 { Operation, MVI, DMA, JMP, LPS, BTM, END, ENDI, Invalid };
    enum class Cond : uint8 { None, NZ, NS, NZS, NC, NT0, Z, S, ZS, C, T0 };

    enum class OpSrc : uint8 { None, M0, M1, M2, M3, MC0, MC1, MC2, MC3, ALUL, ALUH, Invalid };
    enum class OpDst : uint8 { None, CT0, CT1, CT2, CT3, MC0, MC1, MC2, MC3, RX, P, RA0, WA0, LOP, TOP, Invalid };
    enum class MVIDst : uint8 { None, MC0, MC1, MC2, MC3, RX, P, RA0, WA0, LOP, PC, Invalid };

    enum class ALUOp : uint8 { NOP, AND, OR, XOR, ADD, SUB, AD2, SR, RR, SL, RL, RL8, Invalid };
    enum class XBusPOp : uint8 { NOP, MOV_MUL_P, MOV_S_P };
    enum class YBusAOp : uint8 { NOP, CLR_A, MOV_ALU_A, MOV_S_A };
    enum class D1BusOp : uint8 { NOP, MOV_SIMM_D, MOV_S_D };

    enum class DMARAMOp : uint8 { MC0, MC1, MC2, MC3, PRG, Invalid };

    Type type;

    union {
        // Valid when Type::Operation
        struct {
            ALUOp aluOp;

            XBusPOp xbusPOp; // NOP / MOV MUL,P / MOV [s],P
            bool xbusXOp;    // NOP / MOV [s],X
            OpSrc xbusSrc;   // [s]

            YBusAOp ybusAOp; // NOP / CLR A / MOV ALU,A / MOV [s],A
            bool ybusYOp;    // NOP / MOV [s],Y
            OpSrc ybusSrc;   // [s]

            D1BusOp d1BusOp; // NOP / MOV SImm, [d] / MOV [s], [d]
            union {
                sint8 imm; // SImm
                OpSrc reg; // [s]
            } d1BusSrc;
            OpDst d1BusDst; // [d]
        } operation;

        // Valid for Type::MVI
        struct {
            sint32 imm; // SImm
            MVIDst dst; // [d]
            Cond cond;  // <cond>
        } mvi;

        // Valid for Type::DMA
        struct {
            bool hold;      // DMA / DMAH
            bool toD0;      // D0,[RAM] / [RAM],D0
            bool countType; // SImm (false) / [s] (true)
            union {
                uint8 imm; // SImm value (countType==false)
                uint8 ct;  // M0-3, MC0-3 (countType==true)
            } count;
            DMARAMOp ramOp; // MC0-3, PRG
        } dma;

        // Valid for Type::JMP
        struct {
            Cond cond;    // <cond>
            uint8 target; // SImm
        } jmp;
    };
};

SCUDSPInstruction Disassemble(uint32 opcode);

std::string_view ToString(SCUDSPInstruction::Cond cond);

std::string_view ToString(SCUDSPInstruction::OpSrc opSrc);
std::string_view ToString(SCUDSPInstruction::OpDst opDst);
std::string_view ToString(SCUDSPInstruction::MVIDst mviDst);

std::string_view ToString(SCUDSPInstruction::ALUOp aluOp);
std::string_view ToString(SCUDSPInstruction::DMARAMOp dmaRamOp);

} // namespace brimir::scu
