#pragma once

#include <brimir/core/types.hpp>

#include <array>
#include <functional>
#include <vector>

namespace brimir::m68k {

enum class Mnemonic : uint8 {
    Move,   // move
    MoveA,  // movea
    MoveM,  // movem
    MoveP,  // movep
    MoveQ,  // moveq
    Clr,    // clr
    Exg,    // exg
    Ext,    // ext
    Swap,   // swap
    ABCD,   // abcd
    NBCD,   // nbcd
    SBCD,   // sbcd
    Add,    // add
    AddA,   // adda
    AddI,   // addi
    AddQ,   // addq
    AddX,   // addx
    And,    // and
    AndI,   // andi
    Eor,    // eor
    EorI,   // eori
    Neg,    // neg
    NegX,   // negx
    Not,    // not
    Or,     // or
    OrI,    // ori
    Sub,    // sub
    SubA,   // suba
    SubI,   // subi
    SubQ,   // subq
    SubX,   // subx
    DivS,   // divs
    DivU,   // divu
    MulS,   // muls
    MulU,   // mulu
    BChg,   // bchg
    BClr,   // bclr
    BSet,   // bset
    BTst,   // btst
    ASL,    // asl
    ASR,    // asr
    LSL,    // lsl
    LSR,    // lsr
    ROL,    // rol
    ROR,    // ror
    ROXL,   // roxl
    ROXR,   // roxr
    Cmp,    // cmp
    CmpA,   // cmpa
    CmpI,   // cmpi
    CmpM,   // cmpm
    Scc,    // s<cc>
    TAS,    // tas
    Tst,    // tst
    LEA,    // lea
    PEA,    // pea
    Link,   // link
    Unlink, // unlk
    BRA,    // bra
    BSR,    // bsr
    Bcc,    // b<cc>
    DBcc,   // db<cc>
    JSR,    // jsr
    Jmp,    // jmp
    RTE,    // rte
    RTR,    // rtr
    RTS,    // rts
    Chk,    // chk
    Reset,  // reset
    Stop,   // stop
    Trap,   // trap
    TrapV,  // trapv
    Noop,   // nop

    Illegal1010, // illegal instructions with bits 15-12 = 1010
    Illegal1111, // illegal instructions with bits 15-12 = 1111
    Illegal,     // any other illegal instruction, including the designated ILLEGAL instruction 0100 1010 1111 1100
};

enum class Condition : uint8 {
    T,  // always true
    F,  // always false
    HI, // !C and !Z
    LS, // C or Z
    CC, // !C
    CS, // C
    NE, // !Z
    EQ, // Z
    VC, // !V
    VS, // V
    PL, // !N
    MI, // N
    GE, // N == V
    LT, // N != V
    GT, // N == V and !Z
    LE, // N != V or Z
};

enum class OperandSize : uint8 {
    Byte,         // <op>.b
    Word,         // <op>.w
    Long,         // <op>.l
    ByteImplicit, // <op>  (implicit byte transfers, e.g. NBCD)
    WordImplicit, // <op>  (implicit word transfers, e.g. STOP)
    LongImplicit, // <op>  (implicit longword transfers, e.g. MOVE <reg>, <reg>)
    None,         // <op>  (no transfers, e.g. NOP, RESET, TRAPV, etc.)
};

struct Operand {
    enum class Type : uint8 {
        None,

        // Effective addresses
        Dn,            // Dn
        An,            // An
        AtAn,          // (An)
        AtAnPlus,      // (An)+
        MinusAtAn,     // -(An)
        AtDispAn,      // (disp,An)
        AtDispAnIx,    // (disp,An,<ix>)
        AtDispPC,      // (disp,PC)
        AtDispPCIx,    // (disp,PC,<ix>)
        AtImmWord,     // (xxx).w
        AtImmLong,     // (xxx).l
        UImmEmbedded,  // #uimm (embedded in opcode)
        UImm8Fetched,  // #uimm8 (fetched from next word)
        UImm16Fetched, // #uimm16 (fetched from next word)
        UImm32Fetched, // #uimm32 (fetched from next word)

        WordDispPCEmbedded, // #disp (embedded in opcode)
        WordDispPCFetched,  // #disp (fetched from next word)

        CCR, // CCR
        SR,  // SR
        USP, // USP

        RegList,    // <list>  (movem)
        RevRegList, // <list>  (movem predecrement - reversed list)
    };

    Type type;
    bool read, write;
    union {
        uint8 rn;    // Dn, An
        uint16 uimm; // #uimm (embedded in opcode)
        sint16 simm; // #simm (embedded in opcode)
    };

    static Operand None() {
        return {.type = Type::None};
    }

    static Operand Dn_R(uint8 rn) {
        return {.type = Type::Dn, .read = true, .write = false, .rn = rn};
    }
    static Operand Dn_W(uint8 rn) {
        return {.type = Type::Dn, .read = false, .write = true, .rn = rn};
    }
    static Operand Dn_RW(uint8 rn) {
        return {.type = Type::Dn, .read = true, .write = true, .rn = rn};
    }

    static Operand An_R(uint8 rn) {
        return {.type = Type::An, .read = true, .write = false, .rn = rn};
    }
    static Operand An_W(uint8 rn) {
        return {.type = Type::An, .read = false, .write = true, .rn = rn};
    }
    static Operand An_RW(uint8 rn) {
        return {.type = Type::An, .read = true, .write = true, .rn = rn};
    }

    static Operand AtAn_N(uint8 rn) {
        return {.type = Type::AtAn, .read = false, .write = false, .rn = rn};
    }
    static Operand AtAn_R(uint8 rn) {
        return {.type = Type::AtAn, .read = true, .write = false, .rn = rn};
    }
    static Operand AtAn_W(uint8 rn) {
        return {.type = Type::AtAn, .read = false, .write = true, .rn = rn};
    }
    static Operand AtAn_RW(uint8 rn) {
        return {.type = Type::AtAn, .read = true, .write = true, .rn = rn};
    }

    static Operand AtAnPlus_R(uint8 rn) {
        return {.type = Type::AtAnPlus, .read = true, .write = false, .rn = rn};
    }
    static Operand AtAnPlus_W(uint8 rn) {
        return {.type = Type::AtAnPlus, .read = false, .write = true, .rn = rn};
    }
    static Operand AtAnPlus_RW(uint8 rn) {
        return {.type = Type::AtAnPlus, .read = true, .write = true, .rn = rn};
    }

    static Operand MinusAtAn_R(uint8 rn) {
        return {.type = Type::MinusAtAn, .read = true, .write = false, .rn = rn};
    }
    static Operand MinusAtAn_W(uint8 rn) {
        return {.type = Type::MinusAtAn, .read = false, .write = true, .rn = rn};
    }
    static Operand MinusAtAn_RW(uint8 rn) {
        return {.type = Type::MinusAtAn, .read = true, .write = true, .rn = rn};
    }

    static Operand AtDispAn_N(uint8 rn) {
        return {.type = Type::AtDispAn, .read = false, .write = false, .rn = rn};
    }
    static Operand AtDispAn_R(uint8 rn) {
        return {.type = Type::AtDispAn, .read = true, .write = false, .rn = rn};
    }
    static Operand AtDispAn_W(uint8 rn) {
        return {.type = Type::AtDispAn, .read = false, .write = true, .rn = rn};
    }
    static Operand AtDispAn_RW(uint8 rn) {
        return {.type = Type::AtDispAn, .read = true, .write = true, .rn = rn};
    }

    static Operand AtDispAnIx_N(uint8 rn) {
        return {.type = Type::AtDispAnIx, .read = false, .write = false, .rn = rn};
    }
    static Operand AtDispAnIx_R(uint8 rn) {
        return {.type = Type::AtDispAnIx, .read = true, .write = false, .rn = rn};
    }
    static Operand AtDispAnIx_W(uint8 rn) {
        return {.type = Type::AtDispAnIx, .read = false, .write = true, .rn = rn};
    }
    static Operand AtDispAnIx_RW(uint8 rn) {
        return {.type = Type::AtDispAnIx, .read = true, .write = true, .rn = rn};
    }

    static Operand AtDispPC_N() {
        return {.type = Type::AtDispPC, .read = false, .write = false};
    }
    static Operand AtDispPC_R() {
        return {.type = Type::AtDispPC, .read = true, .write = false};
    }

    static Operand AtDispPCIx_N() {
        return {.type = Type::AtDispPCIx, .read = false, .write = false};
    }
    static Operand AtDispPCIx_R() {
        return {.type = Type::AtDispPCIx, .read = true, .write = false};
    }

    static Operand AtImmWord_N() {
        return {.type = Type::AtImmWord, .read = false, .write = false};
    }
    static Operand AtImmWord_R() {
        return {.type = Type::AtImmWord, .read = true, .write = false};
    }
    static Operand AtImmWord_W() {
        return {.type = Type::AtImmWord, .read = false, .write = true};
    }
    static Operand AtImmWord_RW() {
        return {.type = Type::AtImmWord, .read = true, .write = true};
    }

    static Operand AtImmLong_N() {
        return {.type = Type::AtImmLong, .read = false, .write = false};
    }
    static Operand AtImmLong_R() {
        return {.type = Type::AtImmLong, .read = true, .write = false};
    }
    static Operand AtImmLong_W() {
        return {.type = Type::AtImmLong, .read = false, .write = true};
    }
    static Operand AtImmLong_RW() {
        return {.type = Type::AtImmLong, .read = true, .write = true};
    }

    static Operand UImmEmbedded(uint16 uimm) {
        return {.type = Type::UImmEmbedded, .uimm = uimm};
    }
    static Operand UImm8Fetched() {
        return {.type = Type::UImm8Fetched};
    }
    static Operand UImm16Fetched() {
        return {.type = Type::UImm16Fetched};
    }
    static Operand UImm32Fetched() {
        return {.type = Type::UImm32Fetched};
    }

    static Operand WordDispPCEmbedded(sint16 imm) {
        return {.type = Type::WordDispPCEmbedded, .simm = imm};
    }
    static Operand WordDispPCFetched() {
        return {.type = Type::WordDispPCFetched};
    }

    static Operand CCR_R() {
        return {.type = Type::CCR, .read = true, .write = false};
    }
    static Operand CCR_W() {
        return {.type = Type::CCR, .read = false, .write = true};
    }

    static Operand SR_R() {
        return {.type = Type::SR, .read = true, .write = false};
    }
    static Operand SR_W() {
        return {.type = Type::SR, .read = false, .write = true};
    }

    static Operand USP_R() {
        return {.type = Type::USP, .read = true, .write = false};
    }
    static Operand USP_W() {
        return {.type = Type::USP, .read = false, .write = true};
    }

    static Operand RegList_R() {
        return {.type = Type::RegList, .read = true, .write = false};
    }
    static Operand RegList_W() {
        return {.type = Type::RegList, .read = false, .write = true};
    }

    static Operand RevRegList_R() {
        return {.type = Type::RevRegList, .read = true, .write = false};
    }
    static Operand RevRegList_W() {
        return {.type = Type::RevRegList, .read = false, .write = true};
    }
};

struct DisassemblyInfo {
    Mnemonic mnemonic = Mnemonic::Illegal;
    Condition cond = Condition::T;
    OperandSize opSize = OperandSize::None;
    bool privileged = false;
    Operand op1 = Operand::None();
    Operand op2 = Operand::None();
};

struct OperandDetails {
    sint32 immDisp; // #imm, disp
    uint16 regList; // <list> (movem)
    uint8 ix;       // <ix>: 0-7 = D0-7, 8-15 = A0-7
    bool ixLong;    // true: <ix> is longword; false: <ix> is word
};

struct DisassembledInstruction {
    const DisassemblyInfo &info;
    OperandDetails op1;
    OperandDetails op2;
    std::vector<uint16> opcodes;
};

struct DisassemblyTable {
private:
    static constexpr auto alignment = 64;

    DisassemblyTable();

public:
    static DisassemblyTable s_instance;

    alignas(alignment) std::array<DisassemblyInfo, 0x10000> infos;
};

// Disassembles M68K code, reading opcodes using the given fetcher function.
// Every invocation of the function must return successive words starting from the base opcode.
DisassembledInstruction Disassemble(std::function<uint16()> fetcher);

} // namespace brimir::m68k
