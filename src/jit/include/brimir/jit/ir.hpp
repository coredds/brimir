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
    bool flag = false;   // Load: instrFetch; AddAccessCycles/ExitIfBusWait: write; ExitIf: refill
    uint8_t retired = 0; // exit ops: guest instructions completed when the exit is taken
    ValueId dst = kNoValue;
    ValueId a = kNoValue;
    ValueId b = kNoValue;
    uint32_t imm = 0;
    uint32_t imm2 = 0; // ExitIf: cycles added when taken
};

struct Block {
    uint32_t startPC = 0;
    std::vector<uint16_t> guestOpcodes; // guest code at startPC, startPC+2, ... (check-on-entry)
    uint32_t guestInstrCount = 0;       // 0: the first instruction runs on the interpreter
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
    ValueId Load(ValueId address, uint8_t size, bool instrFetch);
    void Store(ValueId address, uint8_t size, ValueId value);
    void AddCycles(uint32_t cycles);
    void AddAccessCycles(ValueId address, uint8_t size, bool write);
    void WbStall(uint32_t mask);
    void SetWb(uint8_t reg);
    void SyncCycles();
    void CheckBoundary(uint32_t pc, uint8_t retired);
    void Refill(uint32_t address);
    void SetupDelaySlot(ValueId target);
    void EndDelaySlot();
    void ExitIfBusWait(ValueId address, uint8_t size, bool write, uint32_t pc, uint8_t retired);
    void ExitIf(ValueId cond, uint32_t pc, uint32_t takenCycles, bool refill, uint8_t retired);
    void Exit(uint32_t pc, uint8_t retired);
    void ExitDynamic(uint8_t retired);

private:
    ValueId NewValue();
    Inst &Emit(Op op);

    Block &m_block;
};

const char *OpName(Op op);

// Returns an empty string if the block is well formed, otherwise a description of the first problem.
std::string VerifyBlock(const Block &block);

// Human-readable listing, used in test failure messages.
std::string PrintBlock(const Block &block);

} // namespace brimir::jit
