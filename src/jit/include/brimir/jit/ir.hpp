#pragma once

// Brimir SH-2 JIT intermediate representation. See design/sh2-jit.md section 5
// and the semantics table in design/plans/2026-09-30-sh2-jit-m1b-foundation.md.

#include <cstdint>
#include <string>
#include <vector>

namespace brimir::jit {

using ValueId = uint16_t;
constexpr ValueId kNoValue = 0xFFFF;
constexpr uint16_t kMaxValues = 4096;

enum class Op : uint8_t {
    Const,
    GetReg,
    SetReg,
    GetPR,
    GetT,
    SetT,
    Add,
    Sub,
    CmpEq,
    SExt8,
    SExt16,
    And,
    Or,
    Xor,
    Not,
    Shl, // imm: amount 1..31
    Shr, // imm: amount 1..31
    Sar, // imm: amount 1..31
    CmpGtU,
    CmpGeU,
    CmpGtS,
    CmpGeS,
    GetGBR,
    SetGBR,
    GetVBR,
    SetVBR,
    SetPR,
    GetSR,
    SetSR, // flag: delaySlot
    GetMACH,
    GetMACL,
    SetMACH,
    SetMACL,
    ClearIntrAllow,
    SetIntrAllow,
    GetDelayTarget,
    Mul,                    // low 32 bits of a * b
    MulHiS,                 // high 32 bits of sint64(a) * sint64(b)
    MulHiU,                 // high 32 bits of uint64(a) * uint64(b)
    // Side effects not visible as IR operands (backends caching SR/T/MAC in host registers must
    // flush/reload around them): SetSRBits writes SR; Div1 reads/writes SR (Q/M/T); MacW/MacL
    // read SR.S and read/write MACH/MACL.
    SetSRBits,              // imm: mask (T/S/Q/M only); SR = (SR & ~mask) | (a & mask)
    Div1,                   // dst = Div1Step(a = Rn, b = Rm, flag = n == m, SR)
    MacW,                   // MAC = MacWStep(MAC, SR.S, a, b)
    MacL,                   // MAC = MacLStep(MAC, SR.S, a, b)
    AddAccessCyclesRMWByte, // cycles += ctx.accessCyclesRMWByte(a) (TAS)
    Load,
    Store,
    AddCycles,
    AddAccessCycles,
    WbStall,
    SetWb,
    SyncCycles,
    CheckBoundary,
    Refill,
    SetupDelaySlot,
    EndDelaySlot,
    ExitIfBusWait,
    ExitIf,
    Exit,
    ExitDynamic,
};

struct Inst {
    Op op = Op::Exit;
    uint8_t size = 0;    // access size in bytes (1, 2, 4)
    bool flag = false;   // Load: instrFetch; AddAccessCycles/ExitIfBusWait: write; ExitIf: refill; SetSR: delaySlot;
                         // Div1: rmIsRn; Refill: known (imm2 is the value the fetch reads);
                         // CheckBoundary: cyclesOnly (see below)
    uint8_t retired = 0; // exit ops: guest instructions completed when the exit is taken
    ValueId dst = kNoValue;
    ValueId a = kNoValue;
    ValueId b = kNoValue;
    uint32_t imm = 0;
    uint32_t imm2 = 0; // ExitIf: cycles added when taken; known Refill: (op[imm] << 16) | op[imm + 2];
                       // CheckBoundary: kCheckNeedsInlineRefills or 0
};

// CheckBoundary(pc, retired) stops the block (PC = pc, boundary) when the cycle budget is reached
// or an interrupt is pending and allowed. With flag (cyclesOnly, set by OptimizeBlock) only the
// budget is tested; with imm2 = kCheckNeedsInlineRefills as well, the interrupt is still tested
// unless the block's known refills store their value in this run (ir_opt.hpp, rule 2).
constexpr uint32_t kCheckNeedsInlineRefills = 1;

// A known Refill (flag set) reads two words of the block's own guestOpcodes, so imm2 is the value
// the fetch returns while that code is unchanged. A backend stores imm2 to *ctx.fetchedOpcodes
// instead of calling refillPipeline only if
//   - fetchFromArrays is set and the code pages are still the array pages seen at compile time, and
//   - no data access of this block run may have written the block's code before the refill
//     ("codeDirty", RunBlock in interp_backend.cpp is the reference);
// otherwise it calls refillPipeline(imm) like an unknown Refill.
//
// Refill callbacks never set codeDirty, which is sound because the front end never emits a
// refill callback that can write memory before a known Refill: with fetchFromArrays every
// Refill inside guestOpcodes is known, and its fallback fetches from an array page (no side
// effects); the only unknown Refill (the last instruction's, when the tail word is not
// compilable) and the runtime refills (taken ExitIf, EndDelaySlot) come after every known one.
// VerifyBlock does not enforce this (hand-built and random test blocks mix them freely); a front
// end change that emits an unknown Refill before a known one must make the backends set codeDirty
// after such a callback.
struct Block {
    uint32_t startPC = 0;
    // Guest code at startPC, startPC+2, ... (check-on-entry): the guestInstrCount instruction words,
    // then the tail word when hasTailWord is set.
    std::vector<uint16_t> guestOpcodes;
    uint32_t guestInstrCount = 0; // 0: the first instruction runs on the interpreter
    // guestOpcodes ends with the word after the last instruction, which the refill at a 4-byte
    // aligned last instruction reads.
    bool hasTailWord = false;
    // At compile time every word of guestOpcodes was on an array page (FastPeek16 succeeded).
    bool fetchFromArrays = false;
    uint16_t numValues = 0;
    std::vector<Inst> code;
};

class Builder {
public:
    explicit Builder(Block &block)
        : m_block(block) {}

    ValueId Const(uint32_t value);
    ValueId GetReg(uint32_t reg);
    void SetReg(uint32_t reg, ValueId value);
    ValueId GetPR();
    ValueId GetT();
    void SetT(ValueId value);
    ValueId Add(ValueId a, ValueId b);
    ValueId Sub(ValueId a, ValueId b);
    ValueId CmpEq(ValueId a, ValueId b);
    ValueId SExt8(ValueId a);
    ValueId SExt16(ValueId a);
    ValueId And(ValueId a, ValueId b);
    ValueId Or(ValueId a, ValueId b);
    ValueId Xor(ValueId a, ValueId b);
    ValueId Not(ValueId a);
    ValueId Shl(ValueId a, uint32_t amount); // amount 1..31
    ValueId Shr(ValueId a, uint32_t amount); // logical, amount 1..31
    ValueId Sar(ValueId a, uint32_t amount); // arithmetic, amount 1..31
    ValueId CmpGtU(ValueId a, ValueId b);
    ValueId CmpGeU(ValueId a, ValueId b);
    ValueId CmpGtS(ValueId a, ValueId b);
    ValueId CmpGeS(ValueId a, ValueId b);
    ValueId GetGBR();
    void SetGBR(ValueId value);
    ValueId GetVBR();
    void SetVBR(ValueId value);
    void SetPR(ValueId value);
    ValueId GetSR();
    void SetSR(ValueId value, bool delaySlot);
    ValueId GetMACH();
    ValueId GetMACL();
    void SetMACH(ValueId value);
    void SetMACL(ValueId value);
    void ClearIntrAllow();
    void SetIntrAllow();
    ValueId GetDelayTarget();
    ValueId Mul(ValueId a, ValueId b);
    ValueId MulHiS(ValueId a, ValueId b);
    ValueId MulHiU(ValueId a, ValueId b);
    void SetSRBits(ValueId value, uint32_t mask); // mask: T/S/Q/M bits only (0x303)
    ValueId Div1(ValueId rn, ValueId rm, bool rmIsRn);
    void MacW(ValueId op1, ValueId op2);
    void MacL(ValueId op1, ValueId op2);
    void AddAccessCyclesRMWByte(ValueId address);
    ValueId Load(ValueId address, uint8_t size, bool instrFetch);
    void Store(ValueId address, uint8_t size, ValueId value);
    void AddCycles(uint32_t cycles);
    void AddAccessCycles(ValueId address, uint8_t size, bool write);
    void WbStall(uint32_t mask);
    void SetWb(uint8_t reg);
    void SyncCycles();
    void CheckBoundary(uint32_t pc, uint8_t retired, bool cyclesOnly = false, bool needsInlineRefills = false);
    void Refill(uint32_t address);
    // A Refill whose value is known (see Block): value = (op[address] << 16) | op[address + 2].
    void KnownRefill(uint32_t address, uint32_t value);
    void SetupDelaySlot(ValueId target);
    void EndDelaySlot();
    void ExitIfBusWait(ValueId address, uint8_t size, bool write, uint32_t pc, uint8_t retired);
    void ExitIf(ValueId cond, uint32_t pc, uint32_t takenCycles, bool refill, uint8_t retired);
    void Exit(uint32_t pc, uint8_t retired);
    void ExitDynamic(uint8_t retired);

private:
    ValueId NewValue();
    Inst &Emit(Op op);
    ValueId Binary(Op op, ValueId a, ValueId b);
    ValueId Unary(Op op, ValueId a, uint32_t imm = 0);
    ValueId Nullary(Op op);
    void Sink(Op op, ValueId a);

    Block &m_block;
};

const char *OpName(Op op);

// Returns an empty string if the block is well formed, otherwise a description of the first problem.
std::string VerifyBlock(const Block &block);

// Human-readable listing, used in test failure messages.
std::string PrintBlock(const Block &block);

} // namespace brimir::jit
