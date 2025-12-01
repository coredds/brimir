#pragma once

/**
 * @file jit_x64_backend.hpp
 * @brief x86-64 Code Generation Backend for SH-2 JIT
 * 
 * Translates platform-independent IR to native x86-64 machine code.
 * Uses System V AMD64 ABI calling convention on Windows (for consistency).
 * 
 * Register Mapping Strategy:
 * - SH-2 R0-R7: x64 RBX, R12-R15, RDI, RSI, R8
 * - SH-2 R8-R15: Memory-backed (hot registers promoted to x64)
 * - T-bit: CL or memory flag
 * - SH-2 State Pointer: RBP (base pointer to context)
 * 
 * Scratch Registers: RAX, RCX, RDX, R9-R11
 */

#include "jit_ir.hpp"
#include <brimir/core/types.hpp>
#include <vector>
#include <memory>

namespace brimir::jit {

using namespace brimir;

/// @brief x86-64 register encoding
enum class X64Register : uint8 {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3,
    RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8  = 8, R9  = 9, R10 = 10, R11 = 11,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15,
    
    // Special registers
    NONE = 255
};

/// @brief x86-64 condition codes (for conditional branches)
enum class X64Condition : uint8 {
    EQUAL = 0x4,        // ZF=1
    NOT_EQUAL = 0x5,    // ZF=0
    BELOW = 0x2,        // CF=1 (unsigned <)
    ABOVE = 0x7,        // CF=0 && ZF=0 (unsigned >)
    LESS = 0xC,         // SF != OF (signed <)
    GREATER = 0xF,      // ZF=0 && SF=OF (signed >)
    CARRY = 0x2,        // CF=1
    NOT_CARRY = 0x3     // CF=0
};

/// @brief Code buffer for emitting x86-64 instructions
class X64CodeBuffer {
public:
    X64CodeBuffer(size_t initial_capacity = 4096);
    ~X64CodeBuffer();
    
    /// @brief Get pointer to generated code
    uint8* GetCode() const { return code_; }
    
    /// @brief Get size of generated code
    size_t GetSize() const { return size_; }
    
    /// @brief Emit a single byte
    void Emit8(uint8 byte);
    
    /// @brief Emit a 16-bit value
    void Emit16(uint16 value);
    
    /// @brief Emit a 32-bit value
    void Emit32(uint32 value);
    
    /// @brief Emit a 64-bit value
    void Emit64(uint64 value);
    
    /// @brief Get current position (for fixups)
    size_t GetPosition() const { return size_; }
    
    /// @brief Patch a 32-bit value at position
    void Patch32(size_t position, uint32 value);
    
    /// @brief Reserve space for code
    void Reserve(size_t bytes);
    
    /// @brief Make code executable
    void MakeExecutable();
    
private:
    uint8* code_;
    size_t size_;
    size_t capacity_;
    bool executable_;
    
    void Grow(size_t min_capacity);
};

/// @brief x86-64 code generator
class X64CodeGen {
public:
    X64CodeGen(X64CodeBuffer& buffer);
    
    // ========================================================================
    // MOV - Move operations
    // ========================================================================
    
    /// @brief MOV reg, reg (64-bit)
    void MovRegReg(X64Register dst, X64Register src);
    
    /// @brief MOV reg, imm32 (sign-extended to 64-bit)
    void MovRegImm32(X64Register reg, sint32 imm);
    
    /// @brief MOV reg, imm64
    void MovRegImm64(X64Register reg, uint64 imm);
    
    /// @brief MOV reg, [mem] - Load from memory
    void MovRegMem(X64Register reg, X64Register base, sint32 offset = 0);
    
    /// @brief MOV [mem], reg - Store to memory
    void MovMemReg(X64Register base, sint32 offset, X64Register reg);
    
    // ========================================================================
    // Arithmetic operations
    // ========================================================================
    
    /// @brief ADD reg, reg
    void AddRegReg(X64Register dst, X64Register src);
    
    /// @brief ADD reg, imm32
    void AddRegImm(X64Register reg, sint32 imm);
    
    /// @brief ADC reg, reg (add with carry)
    void AdcRegReg(X64Register dst, X64Register src);
    
    /// @brief SUB reg, reg
    void SubRegReg(X64Register dst, X64Register src);
    
    /// @brief SBB reg, reg (subtract with borrow)
    void SbbRegReg(X64Register dst, X64Register src);
    
    /// @brief NEG reg
    void NegReg(X64Register reg);
    
    // ========================================================================
    // Logic operations
    // ========================================================================
    
    /// @brief AND reg, reg
    void AndRegReg(X64Register dst, X64Register src);
    
    /// @brief OR reg, reg
    void OrRegReg(X64Register dst, X64Register src);
    
    /// @brief XOR reg, reg
    void XorRegReg(X64Register dst, X64Register src);
    
    /// @brief NOT reg
    void NotReg(X64Register reg);
    
    // ========================================================================
    // Shift/Rotate operations
    // ========================================================================
    
    /// @brief SHL reg, imm
    void ShlRegImm(X64Register reg, uint8 shift);
    
    /// @brief SHR reg, imm
    void ShrRegImm(X64Register reg, uint8 shift);
    
    /// @brief SAR reg, imm (arithmetic shift)
    void SarRegImm(X64Register reg, uint8 shift);
    
    /// @brief ROL reg, imm (rotate left)
    void RolRegImm(X64Register reg, uint8 shift);
    
    /// @brief ROR reg, imm (rotate right)
    void RorRegImm(X64Register reg, uint8 shift);
    
    // ========================================================================
    // Compare operations
    // ========================================================================
    
    /// @brief CMP reg, reg
    void CmpRegReg(X64Register reg1, X64Register reg2);
    
    /// @brief CMP reg, imm32
    void CmpRegImm(X64Register reg, sint32 imm);
    
    /// @brief TEST reg, reg
    void TestRegReg(X64Register reg1, X64Register reg2);
    
    // ========================================================================
    // Control flow
    // ========================================================================
    
    /// @brief JMP to label (relative 32-bit offset)
    void Jmp(sint32 offset);
    
    /// @brief Jcc - Conditional jump
    void JmpIf(X64Condition cond, sint32 offset);
    
    /// @brief CALL - Call function
    void Call(void* func_ptr);
    
    /// @brief RET - Return
    void Ret();
    
    // ========================================================================
    // Stack operations
    // ========================================================================
    
    /// @brief PUSH reg
    void Push(X64Register reg);
    
    /// @brief POP reg
    void Pop(X64Register reg);
    
    // ========================================================================
    // Special
    // ========================================================================
    
    /// @brief NOP
    void Nop();
    
    /// @brief INT3 (breakpoint)
    void Int3();
    
    // ========================================================================
    // Helpers
    // ========================================================================
    
    /// @brief Get current code position
    size_t GetPosition() const { return buffer_.GetPosition(); }
    
    /// @brief Emit label for fixup
    struct Label {
        size_t position;
        bool bound;
        std::vector<size_t> fixup_positions;
    };
    
    Label CreateLabel();
    void BindLabel(Label& label);
    void JmpToLabel(const Label& label);
    void JmpIfToLabel(X64Condition cond, const Label& label);
    
private:
    X64CodeBuffer& buffer_;
    
    // REX prefix helpers
    void EmitRex(bool w, uint8 reg, uint8 rm);
    void EmitModRM(uint8 mod, uint8 reg, uint8 rm);
    void EmitSIB(uint8 scale, uint8 index, uint8 base);
};

/// @brief SH-2 to x86-64 register allocator
class X64RegisterAllocator {
public:
    X64RegisterAllocator();
    
    /// @brief Get x64 register for SH-2 register
    X64Register GetSH2Register(uint8 sh2_reg);
    
    /// @brief Allocate a temporary x64 register
    X64Register AllocTemp();
    
    /// @brief Free a temporary register
    void FreeTemp(X64Register reg);
    
    /// @brief Check if SH-2 register is in x64 register
    bool IsInRegister(uint8 sh2_reg) const;
    
    /// @brief Spill SH-2 register to memory
    void Spill(uint8 sh2_reg);
    
    /// @brief Load SH-2 register from memory
    void Load(uint8 sh2_reg);
    
private:
    // SH-2 register to x64 register mapping
    X64Register sh2_to_x64_[16];
    
    // Available temporary registers
    std::vector<X64Register> available_temps_;
    
    // In-use temporary registers
    std::vector<X64Register> used_temps_;
};

/// @brief Complete x86-64 backend
class X64Backend {
public:
    X64Backend();
    ~X64Backend();
    
    /**
     * @brief Compile an IR block to x86-64 code
     * @param block IR block to compile
     * @return Pointer to executable code, or nullptr on failure
     */
    void* Compile(const IRBlock& block);
    
    /**
     * @brief Get generated code size
     */
    size_t GetCodeSize() const;
    
    /**
     * @brief Disassemble generated code (for debugging)
     * @return Assembly listing
     */
    std::string Disassemble() const;
    
private:
    X64CodeBuffer buffer_;
    X64RegisterAllocator allocator_;
    X64CodeGen codegen_;
    
    /// @brief Emit prologue (save callee-saved registers)
    void EmitPrologue();
    
    /// @brief Emit epilogue (restore registers, return)
    void EmitEpilogue();
    
    /// @brief Compile single IR instruction to x64
    void CompileInstruction(const IRInstruction& instr);
    
    /// @brief Emit code to load SH-2 context pointer
    void EmitLoadContext();
    
    /// @brief Emit code to save SH-2 context
    void EmitSaveContext();
};

} // namespace brimir::jit

