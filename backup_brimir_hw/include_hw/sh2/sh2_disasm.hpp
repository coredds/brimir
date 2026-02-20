#pragma once

#include <brimir/core/types.hpp>

#include <array>

namespace brimir::sh2 {

enum class Mnemonic : uint8 {
    NOP,     // nop
    SLEEP,   // sleep
    MOV,     // mov
    MOVA,    // mova
    MOVT,    // movt
    CLRT,    // clrt
    SETT,    // sett
    EXTU,    // extu
    EXTS,    // exts
    SWAP,    // swap
    XTRCT,   // xtrct
    LDC,     // ldc
    LDS,     // lds
    STC,     // stc
    STS,     // sts
    ADD,     // add
    ADDC,    // addc
    ADDV,    // addv
    AND,     // and
    NEG,     // neg
    NEGC,    // negc
    NOT,     // not
    OR,      // or
    ROTCL,   // rotcl
    ROTCR,   // rotcr
    ROTL,    // rotl
    ROTR,    // rotr
    SHAL,    // shal
    SHAR,    // shar
    SHLL,    // shll
    SHLL2,   // shll2
    SHLL8,   // shll8
    SHLL16,  // shll16
    SHLR,    // shlr
    SHLR2,   // shlr2
    SHLR8,   // shlr8
    SHLR16,  // shlr16
    SUB,     // sub
    SUBC,    // subc
    SUBV,    // subv
    XOR,     // xor
    DT,      // dt
    CLRMAC,  // clrmac
    MAC,     // mac
    MUL,     // mul
    MULS,    // muls
    MULU,    // mulu
    DMULS,   // dmuls
    DMULU,   // dmulu
    DIV0S,   // div0s
    DIV0U,   // div0u
    DIV1,    // div1
    CMP_EQ,  // cmp/eq
    CMP_GE,  // cmp/ge
    CMP_GT,  // cmp/gt
    CMP_HI,  // cmp/hi
    CMP_HS,  // cmp/hs
    CMP_PL,  // cmp/pl
    CMP_PZ,  // cmp/pz
    CMP_STR, // cmp/str
    TAS,     // tas
    TST,     // tst
    BF,      // bf
    BFS,     // bf/s
    BT,      // bt
    BTS,     // bt/s
    BRA,     // bra
    BRAF,    // braf
    BSR,     // bsr
    BSRF,    // bsrf
    JMP,     // jmp
    JSR,     // jsr
    TRAPA,   // trapa
    RTE,     // rte
    RTS,     // rts
    Illegal, // (illegal instruction)
};

enum class OperandSize : uint8 {
    Byte,         // <op>.b
    Word,         // <op>.w
    Long,         // <op>.l
    LongImplicit, // <op>  (reg-reg transfers)
    None,         // <op>  (no transfers, e.g. NOP, SLEEP, etc.)
};

struct Operand {
    enum class Type : uint8 {
        None,
        Imm,               // #imm
        Rn,                // Rn
        AtRn,              // @Rn
        AtRnPlus,          // @Rn+
        AtMinusRn,         // @-Rn
        AtDispRn,          // @(disp,Rn)
        AtR0Rn,            // @(R0,Rn)
        AtDispGBR,         // @(disp,GBR)
        AtR0GBR,           // @(R0,GBR)
        AtRnPC,            // @Rn [JMP, JSR]
        AtDispPC,          // @(disp,PC)
        AtDispPCWordAlign, // @(disp,PC) [PC & ~3]
        DispPC,            // disp[+PC]
        RnPC,              // Rn[+PC]
        SR,                // SR
        GBR,               // GBR
        VBR,               // VBR
        MACH,              // MACH
        MACL,              // MACL
        PR,                // PR
    };
    Type type;
    bool read, write;
    uint8 reg;
    sint32 immDisp;

    static Operand None() {
        return {.type = Type::None};
    }

    // #imm
    static Operand Imm(sint32 imm) {
        return {.type = Type::Imm, .read = false, .write = false, .immDisp = imm};
    }

    // Rn
    static Operand Rn_R(uint8 rn) {
        return {.type = Type::Rn, .read = true, .write = false, .reg = rn};
    }
    static Operand Rn_W(uint8 rn) {
        return {.type = Type::Rn, .read = false, .write = true, .reg = rn};
    }
    static Operand Rn_RW(uint8 rn) {
        return {.type = Type::Rn, .read = true, .write = true, .reg = rn};
    }

    // @Rn
    static Operand AtRn_R(uint8 rn) {
        return {.type = Type::AtRn, .read = true, .write = false, .reg = rn};
    }
    static Operand AtRn_W(uint8 rn) {
        return {.type = Type::AtRn, .read = false, .write = true, .reg = rn};
    }
    static Operand AtRn_RW(uint8 rn) {
        return {.type = Type::AtRn, .read = true, .write = true, .reg = rn};
    }

    // @Rn+
    static Operand AtRnPlus_R(uint8 rn) {
        return {.type = Type::AtRnPlus, .read = true, .write = false, .reg = rn};
    }
    static Operand AtRnPlus_W(uint8 rn) {
        return {.type = Type::AtRnPlus, .read = false, .write = true, .reg = rn};
    }

    // @-Rn
    static Operand AtMinusRn_R(uint8 rn) {
        return {.type = Type::AtMinusRn, .read = true, .write = false, .reg = rn};
    }
    static Operand AtMinusRn_W(uint8 rn) {
        return {.type = Type::AtMinusRn, .read = false, .write = true, .reg = rn};
    }

    // @(disp,Rn)
    static Operand AtDispRn_R(uint8 rn, sint32 disp) {
        return {.type = Type::AtDispRn, .read = true, .write = false, .reg = rn, .immDisp = disp};
    }
    static Operand AtDispRn_W(uint8 rn, sint32 disp) {
        return {.type = Type::AtDispRn, .read = false, .write = true, .reg = rn, .immDisp = disp};
    }

    // @(R0,Rn)
    static Operand AtR0Rn_R(uint8 rn) {
        return {.type = Type::AtR0Rn, .read = true, .write = false, .reg = rn};
    }
    static Operand AtR0Rn_W(uint8 rn) {
        return {.type = Type::AtR0Rn, .read = false, .write = true, .reg = rn};
    }

    // @(disp,GBR)
    static Operand AtDispGBR_R(sint32 disp) {
        return {.type = Type::AtDispGBR, .read = true, .write = false, .immDisp = disp};
    }
    static Operand AtDispGBR_W(sint32 disp) {
        return {.type = Type::AtDispGBR, .read = false, .write = true, .immDisp = disp};
    }

    // @(R0,GBR)
    static Operand AtR0GBR_R() {
        return {.type = Type::AtR0GBR, .read = true, .write = false};
    }
    static Operand AtR0GBR_W() {
        return {.type = Type::AtR0GBR, .read = false, .write = true};
    }
    static Operand AtR0GBR_RW() {
        return {.type = Type::AtR0GBR, .read = true, .write = true};
    }

    // @Rn [JMP, JSR]
    static Operand AtRnPC(uint8 rn) {
        return {.type = Type::AtRnPC, .read = true, .write = false, .reg = rn};
    }

    // @(disp,PC)
    static Operand AtDispPC(sint32 disp) {
        return {.type = Type::AtDispPC, .read = true, .write = false, .immDisp = disp};
    }

    // @(disp,PC) [PC & ~3]
    static Operand AtDispPCWordAlign(sint32 disp) {
        return {.type = Type::AtDispPCWordAlign, .read = true, .write = false, .immDisp = disp};
    }

    // disp[+PC]
    static Operand DispPC(sint32 disp) {
        return {.type = Type::DispPC, .read = true, .write = false, .immDisp = disp};
    }

    // Rn[+PC]
    static Operand RnPC(uint8 rn) {
        return {.type = Type::RnPC, .read = true, .write = false, .reg = rn};
    }

    // SR
    static Operand SR_R() {
        return {.type = Type::SR, .read = true, .write = false};
    }
    static Operand SR_W() {
        return {.type = Type::SR, .read = false, .write = true};
    }

    // GBR
    static Operand GBR_R() {
        return {.type = Type::GBR, .read = true, .write = false};
    }
    static Operand GBR_W() {
        return {.type = Type::GBR, .read = false, .write = true};
    }

    // VBR
    static Operand VBR_R() {
        return {.type = Type::VBR, .read = true, .write = false};
    }
    static Operand VBR_W() {
        return {.type = Type::VBR, .read = false, .write = true};
    }

    // MACH
    static Operand MACH_R() {
        return {.type = Type::MACH, .read = true, .write = false};
    }
    static Operand MACH_W() {
        return {.type = Type::MACH, .read = false, .write = true};
    }

    // MACL
    static Operand MACL_R() {
        return {.type = Type::MACL, .read = true, .write = false};
    }
    static Operand MACL_W() {
        return {.type = Type::MACL, .read = false, .write = true};
    }

    // PR
    static Operand PR_R() {
        return {.type = Type::PR, .read = true, .write = false};
    }
    static Operand PR_W() {
        return {.type = Type::PR, .read = false, .write = true};
    }
};

struct DisassembledInstruction {
    bool hasDelaySlot = false;
    bool validInDelaySlot = true;
    Mnemonic mnemonic = Mnemonic::Illegal;
    OperandSize opSize = OperandSize::None;
    Operand op1 = Operand::None();
    Operand op2 = Operand::None();
};

struct DisassemblyTable {
private:
    static constexpr auto alignment = 64;

    DisassemblyTable();

public:
    static DisassemblyTable s_instance;

    alignas(alignment) std::array<DisassembledInstruction, 0x10000> instrs;
};

const DisassembledInstruction &Disassemble(uint16 opcode);

} // namespace brimir::sh2
