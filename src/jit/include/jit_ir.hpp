#pragma once

/**
 * @file jit_ir.hpp
 * @brief Intermediate Representation for SH-2 JIT
 * 
 * Platform-independent IR that represents SH-2 operations.
 * Translated from SH-2 instructions and compiled to native code.
 */

#include <brimir/core/types.hpp>
#include <vector>
#include <memory>
#include <string>

namespace brimir::jit {

using namespace brimir;

/// @brief IR operation types
enum class IROp : uint8 {
    // Register Operations
    MOV_REG,        ///< Move register to register
    MOV_IMM,        ///< Move immediate to register
    
    // Arithmetic
    ADD,            ///< Add
    ADDI,           ///< Add immediate
    ADDC,           ///< Add with carry
    SUB,            ///< Subtract
    SUBC,           ///< Subtract with carry/borrow
    NEG,            ///< Negate
    
    // Logic
    AND,            ///< Logical AND
    OR,             ///< Logical OR
    XOR,            ///< Logical XOR
    NOT,            ///< Logical NOT
    
    // Shift/Rotate
    SHLL,           ///< Shift left logical
    SHLR,           ///< Shift right logical
    SHAR,           ///< Shift right arithmetic
    ROTL,           ///< Rotate left
    ROTR,           ///< Rotate right
    
    // Compare
    CMP_EQ,         ///< Compare equal
    CMP_GE,         ///< Compare greater or equal (signed)
    CMP_GT,         ///< Compare greater than (signed)
    CMP_HI,         ///< Compare higher (unsigned)
    CMP_HS,         ///< Compare higher or same (unsigned)
    
    // Memory
    LOAD8,          ///< Load 8-bit
    LOAD16,         ///< Load 16-bit
    LOAD32,         ///< Load 32-bit
    STORE8,         ///< Store 8-bit
    STORE16,        ///< Store 16-bit
    STORE32,        ///< Store 32-bit
    
    // Control Flow
    BRANCH,         ///< Unconditional branch
    BRANCH_COND,    ///< Conditional branch (uses T-bit)
    CALL,           ///< Function call (JSR/BSR)
    RETURN,         ///< Return (RTS)
    
    // Special
    NOP,            ///< No operation
    TRAP,           ///< Software trap
    SET_T,          ///< Set T-bit
    CLR_T,          ///< Clear T-bit
    
    // Block Control
    EXIT_BLOCK,     ///< Exit compiled block, return to dispatcher
    UPDATE_CYCLES,  ///< Update cycle counter
};

/// @brief IR operand types
enum class IROperandType : uint8 {
    NONE,           ///< No operand
    REG,            ///< SH-2 register (R0-R15)
    IMM,            ///< Immediate value
    ADDR,           ///< Memory address (for branches)
    SR_FLAG,        ///< Status register flag (T, S, etc.)
};

/// @brief IR operand
struct IROperand {
    IROperandType type = IROperandType::NONE;
    
    union {
        uint8 reg;      ///< Register number (0-15)
        sint32 imm;     ///< Immediate value (sign-extended)
        uint32 addr;    ///< Address (for branches)
        uint8 flag;     ///< SR flag index
    };
    
    IROperand() : type(IROperandType::NONE), imm(0) {}
    
    static IROperand Reg(uint8 r) {
        IROperand op;
        op.type = IROperandType::REG;
        op.reg = r;
        return op;
    }
    
    static IROperand Imm(sint32 i) {
        IROperand op;
        op.type = IROperandType::IMM;
        op.imm = i;
        return op;
    }
    
    static IROperand Addr(uint32 a) {
        IROperand op;
        op.type = IROperandType::ADDR;
        op.addr = a;
        return op;
    }
    
    static IROperand Flag(uint8 f) {
        IROperand op;
        op.type = IROperandType::SR_FLAG;
        op.flag = f;
        return op;
    }
};

/// @brief Single IR instruction
struct IRInstruction {
    IROp op;                    ///< Operation
    IROperand dst;              ///< Destination operand
    IROperand src1;             ///< Source operand 1
    IROperand src2;             ///< Source operand 2
    uint8 cycles;               ///< Cycle count for this instruction
    uint32 sh2_addr;            ///< Original SH-2 address (for debugging)
    
    IRInstruction(IROp op_, IROperand dst_ = IROperand(), 
                  IROperand src1_ = IROperand(), IROperand src2_ = IROperand(),
                  uint8 cycles_ = 1, uint32 addr_ = 0)
        : op(op_), dst(dst_), src1(src1_), src2(src2_), 
          cycles(cycles_), sh2_addr(addr_) {}
    
    /// @brief Get human-readable string for debugging
    std::string ToString() const;
};

/// @brief Basic block in IR form
struct IRBlock {
    uint32 start_addr;                      ///< Starting SH-2 address
    uint32 end_addr;                        ///< Ending SH-2 address (exclusive)
    std::vector<IRInstruction> instructions; ///< IR instructions
    uint32 total_cycles;                    ///< Total cycle count for block
    
    /// @brief Exit type of this block
    enum class ExitType {
        SEQUENTIAL,     ///< Falls through to next instruction
        BRANCH,         ///< Unconditional branch to target
        CONDITIONAL,    ///< Conditional branch (T-bit)
        DYNAMIC,        ///< Dynamic branch (register indirect)
        RETURN          ///< Function return
    };
    
    ExitType exit_type = ExitType::SEQUENTIAL;
    uint32 branch_target = 0;           ///< Target address (if known)
    bool has_delay_slot = false;        ///< Does last branch have delay slot?
    
    IRBlock(uint32 start) : start_addr(start), end_addr(start), total_cycles(0) {}
    
    /// @brief Add instruction to block
    void Add(const IRInstruction& instr) {
        instructions.push_back(instr);
        total_cycles += instr.cycles;
        end_addr += 2; // SH-2 instructions are 2 bytes
    }
    
    /// @brief Get instruction count
    size_t Size() const { return instructions.size(); }
    
    /// @brief Check if block is empty
    bool Empty() const { return instructions.empty(); }
    
    /// @brief Get human-readable dump
    std::string ToString() const;
};

/// @brief Live register analysis for optimization
struct LiveRanges {
    /// @brief Which registers are live (read before written) at block entry
    uint16 live_in = 0;
    
    /// @brief Which registers are live (written and used later) at block exit
    uint16 live_out = 0;
    
    /// @brief Per-instruction live register mask
    std::vector<uint16> per_instr;
    
    /// @brief Check if register is live at instruction i
    bool IsLive(size_t instr_index, uint8 reg) const {
        if (instr_index >= per_instr.size()) return false;
        return (per_instr[instr_index] & (1 << reg)) != 0;
    }
};

/// @brief Block optimization metadata
struct BlockMetadata {
    uint32 execution_count = 0;     ///< How many times block was executed
    uint32 compilation_count = 0;   ///< How many times block was compiled
    LiveRanges live_ranges;         ///< Live register analysis
    bool needs_recompile = false;   ///< Needs recompilation (e.g., profiling data changed)
};

} // namespace brimir::jit

