#include "../include/jit_x64_backend.hpp"

namespace brimir::jit {

X64CodeGen::X64CodeGen(X64CodeBuffer& buffer)
    : buffer_(buffer)
{
}

// ============================================================================
// Helper: REX prefix
// ============================================================================

void X64CodeGen::EmitRex(bool w, uint8 reg, uint8 rm) {
    // REX.W R X B
    // W = 1 for 64-bit operand size
    // R = extension of ModR/M reg field
    // X = extension of SIB index field (not used here)
    // B = extension of ModR/M r/m field
    
    uint8 rex = 0x40;
    if (w) rex |= 0x08;  // REX.W
    if (reg & 0x8) rex |= 0x04;  // REX.R
    if (rm & 0x8) rex |= 0x01;  // REX.B
    
    // Only emit if needed (64-bit or extended registers)
    if (rex != 0x40 || w) {
        buffer_.Emit8(rex);
    }
}

void X64CodeGen::EmitModRM(uint8 mod, uint8 reg, uint8 rm) {
    buffer_.Emit8((mod << 6) | ((reg & 0x7) << 3) | (rm & 0x7));
}

void X64CodeGen::EmitSIB(uint8 scale, uint8 index, uint8 base) {
    buffer_.Emit8((scale << 6) | ((index & 0x7) << 3) | (base & 0x7));
}

// ============================================================================
// MOV operations
// ============================================================================

void X64CodeGen::MovRegReg(X64Register dst, X64Register src) {
    if (dst == src) return; // Optimize away no-op
    
    // MOV dst, src (64-bit)
    // Encoding: REX.W + 89 /r
    EmitRex(true, static_cast<uint8>(src), static_cast<uint8>(dst));
    buffer_.Emit8(0x89);
    EmitModRM(0b11, static_cast<uint8>(src), static_cast<uint8>(dst));
}

void X64CodeGen::MovRegImm32(X64Register reg, sint32 imm) {
    // MOV reg, imm32 (sign-extended to 64-bit)
    // Encoding: REX.W + C7 /0 id
    EmitRex(true, 0, static_cast<uint8>(reg));
    buffer_.Emit8(0xC7);
    EmitModRM(0b11, 0, static_cast<uint8>(reg));
    buffer_.Emit32(static_cast<uint32>(imm));
}

void X64CodeGen::MovRegImm64(X64Register reg, uint64 imm) {
    // MOV reg, imm64
    // Encoding: REX.W + B8+rd iq
    EmitRex(true, 0, static_cast<uint8>(reg));
    buffer_.Emit8(0xB8 + (static_cast<uint8>(reg) & 0x7));
    buffer_.Emit64(imm);
}

void X64CodeGen::MovRegMem(X64Register reg, X64Register base, sint32 offset) {
    // MOV reg, [base + offset]
    // Encoding: REX.W + 8B /r + disp32
    EmitRex(true, static_cast<uint8>(reg), static_cast<uint8>(base));
    buffer_.Emit8(0x8B);
    
    if (offset == 0 && base != X64Register::RBP && base != X64Register::R13) {
        // [base] with no displacement
        EmitModRM(0b00, static_cast<uint8>(reg), static_cast<uint8>(base));
    } else if (offset >= -128 && offset <= 127) {
        // [base + disp8]
        EmitModRM(0b01, static_cast<uint8>(reg), static_cast<uint8>(base));
        buffer_.Emit8(static_cast<uint8>(offset));
    } else {
        // [base + disp32]
        EmitModRM(0b10, static_cast<uint8>(reg), static_cast<uint8>(base));
        buffer_.Emit32(static_cast<uint32>(offset));
    }
}

void X64CodeGen::MovMemReg(X64Register base, sint32 offset, X64Register reg) {
    // MOV [base + offset], reg
    // Encoding: REX.W + 89 /r + disp32
    EmitRex(true, static_cast<uint8>(reg), static_cast<uint8>(base));
    buffer_.Emit8(0x89);
    
    if (offset == 0 && base != X64Register::RBP && base != X64Register::R13) {
        EmitModRM(0b00, static_cast<uint8>(reg), static_cast<uint8>(base));
    } else if (offset >= -128 && offset <= 127) {
        EmitModRM(0b01, static_cast<uint8>(reg), static_cast<uint8>(base));
        buffer_.Emit8(static_cast<uint8>(offset));
    } else {
        EmitModRM(0b10, static_cast<uint8>(reg), static_cast<uint8>(base));
        buffer_.Emit32(static_cast<uint32>(offset));
    }
}

// ============================================================================
// Arithmetic operations
// ============================================================================

void X64CodeGen::AddRegReg(X64Register dst, X64Register src) {
    // ADD dst, src (64-bit)
    // Encoding: REX.W + 01 /r
    EmitRex(true, static_cast<uint8>(src), static_cast<uint8>(dst));
    buffer_.Emit8(0x01);
    EmitModRM(0b11, static_cast<uint8>(src), static_cast<uint8>(dst));
}

void X64CodeGen::AddRegImm(X64Register reg, sint32 imm) {
    // ADD reg, imm32
    // Encoding: REX.W + 81 /0 id
    EmitRex(true, 0, static_cast<uint8>(reg));
    buffer_.Emit8(0x81);
    EmitModRM(0b11, 0, static_cast<uint8>(reg));
    buffer_.Emit32(static_cast<uint32>(imm));
}

void X64CodeGen::AdcRegReg(X64Register dst, X64Register src) {
    // ADC dst, src (add with carry)
    // Encoding: REX.W + 11 /r
    EmitRex(true, static_cast<uint8>(src), static_cast<uint8>(dst));
    buffer_.Emit8(0x11);
    EmitModRM(0b11, static_cast<uint8>(src), static_cast<uint8>(dst));
}

void X64CodeGen::SubRegReg(X64Register dst, X64Register src) {
    // SUB dst, src
    // Encoding: REX.W + 29 /r
    EmitRex(true, static_cast<uint8>(src), static_cast<uint8>(dst));
    buffer_.Emit8(0x29);
    EmitModRM(0b11, static_cast<uint8>(src), static_cast<uint8>(dst));
}

void X64CodeGen::SbbRegReg(X64Register dst, X64Register src) {
    // SBB dst, src (subtract with borrow)
    // Encoding: REX.W + 19 /r
    EmitRex(true, static_cast<uint8>(src), static_cast<uint8>(dst));
    buffer_.Emit8(0x19);
    EmitModRM(0b11, static_cast<uint8>(src), static_cast<uint8>(dst));
}

void X64CodeGen::NegReg(X64Register reg) {
    // NEG reg
    // Encoding: REX.W + F7 /3
    EmitRex(true, 0, static_cast<uint8>(reg));
    buffer_.Emit8(0xF7);
    EmitModRM(0b11, 3, static_cast<uint8>(reg));
}

// ============================================================================
// Logic operations
// ============================================================================

void X64CodeGen::AndRegReg(X64Register dst, X64Register src) {
    // AND dst, src
    // Encoding: REX.W + 21 /r
    EmitRex(true, static_cast<uint8>(src), static_cast<uint8>(dst));
    buffer_.Emit8(0x21);
    EmitModRM(0b11, static_cast<uint8>(src), static_cast<uint8>(dst));
}

void X64CodeGen::OrRegReg(X64Register dst, X64Register src) {
    // OR dst, src
    // Encoding: REX.W + 09 /r
    EmitRex(true, static_cast<uint8>(src), static_cast<uint8>(dst));
    buffer_.Emit8(0x09);
    EmitModRM(0b11, static_cast<uint8>(src), static_cast<uint8>(dst));
}

void X64CodeGen::XorRegReg(X64Register dst, X64Register src) {
    // XOR dst, src
    // Encoding: REX.W + 31 /r
    EmitRex(true, static_cast<uint8>(src), static_cast<uint8>(dst));
    buffer_.Emit8(0x31);
    EmitModRM(0b11, static_cast<uint8>(src), static_cast<uint8>(dst));
}

void X64CodeGen::NotReg(X64Register reg) {
    // NOT reg
    // Encoding: REX.W + F7 /2
    EmitRex(true, 0, static_cast<uint8>(reg));
    buffer_.Emit8(0xF7);
    EmitModRM(0b11, 2, static_cast<uint8>(reg));
}

// ============================================================================
// Shift/Rotate operations
// ============================================================================

void X64CodeGen::ShlRegImm(X64Register reg, uint8 shift) {
    // SHL reg, imm
    // Encoding: REX.W + C1 /4 ib
    EmitRex(true, 0, static_cast<uint8>(reg));
    buffer_.Emit8(0xC1);
    EmitModRM(0b11, 4, static_cast<uint8>(reg));
    buffer_.Emit8(shift);
}

void X64CodeGen::ShrRegImm(X64Register reg, uint8 shift) {
    // SHR reg, imm
    // Encoding: REX.W + C1 /5 ib
    EmitRex(true, 0, static_cast<uint8>(reg));
    buffer_.Emit8(0xC1);
    EmitModRM(0b11, 5, static_cast<uint8>(reg));
    buffer_.Emit8(shift);
}

void X64CodeGen::SarRegImm(X64Register reg, uint8 shift) {
    // SAR reg, imm (arithmetic shift)
    // Encoding: REX.W + C1 /7 ib
    EmitRex(true, 0, static_cast<uint8>(reg));
    buffer_.Emit8(0xC1);
    EmitModRM(0b11, 7, static_cast<uint8>(reg));
    buffer_.Emit8(shift);
}

void X64CodeGen::RolRegImm(X64Register reg, uint8 shift) {
    // ROL reg, imm
    // Encoding: REX.W + C1 /0 ib
    EmitRex(true, 0, static_cast<uint8>(reg));
    buffer_.Emit8(0xC1);
    EmitModRM(0b11, 0, static_cast<uint8>(reg));
    buffer_.Emit8(shift);
}

void X64CodeGen::RorRegImm(X64Register reg, uint8 shift) {
    // ROR reg, imm
    // Encoding: REX.W + C1 /1 ib
    EmitRex(true, 0, static_cast<uint8>(reg));
    buffer_.Emit8(0xC1);
    EmitModRM(0b11, 1, static_cast<uint8>(reg));
    buffer_.Emit8(shift);
}

// ============================================================================
// Compare operations
// ============================================================================

void X64CodeGen::CmpRegReg(X64Register reg1, X64Register reg2) {
    // CMP reg1, reg2
    // Encoding: REX.W + 39 /r
    EmitRex(true, static_cast<uint8>(reg2), static_cast<uint8>(reg1));
    buffer_.Emit8(0x39);
    EmitModRM(0b11, static_cast<uint8>(reg2), static_cast<uint8>(reg1));
}

void X64CodeGen::CmpRegImm(X64Register reg, sint32 imm) {
    // CMP reg, imm32
    // Encoding: REX.W + 81 /7 id
    EmitRex(true, 0, static_cast<uint8>(reg));
    buffer_.Emit8(0x81);
    EmitModRM(0b11, 7, static_cast<uint8>(reg));
    buffer_.Emit32(static_cast<uint32>(imm));
}

void X64CodeGen::TestRegReg(X64Register reg1, X64Register reg2) {
    // TEST reg1, reg2
    // Encoding: REX.W + 85 /r
    EmitRex(true, static_cast<uint8>(reg2), static_cast<uint8>(reg1));
    buffer_.Emit8(0x85);
    EmitModRM(0b11, static_cast<uint8>(reg2), static_cast<uint8>(reg1));
}

// ============================================================================
// Control flow
// ============================================================================

void X64CodeGen::Jmp(sint32 offset) {
    // JMP rel32
    // Encoding: E9 cd
    buffer_.Emit8(0xE9);
    buffer_.Emit32(static_cast<uint32>(offset));
}

void X64CodeGen::JmpIf(X64Condition cond, sint32 offset) {
    // Jcc rel32
    // Encoding: 0F 80+cc cd
    buffer_.Emit8(0x0F);
    buffer_.Emit8(0x80 + static_cast<uint8>(cond));
    buffer_.Emit32(static_cast<uint32>(offset));
}

void X64CodeGen::Call(void* func_ptr) {
    // CALL via register (for function pointers)
    // MOV RAX, func_ptr
    MovRegImm64(X64Register::RAX, reinterpret_cast<uint64>(func_ptr));
    // CALL RAX
    buffer_.Emit8(0xFF);
    EmitModRM(0b11, 2, static_cast<uint8>(X64Register::RAX));
}

void X64CodeGen::Ret() {
    // RET
    // Encoding: C3
    buffer_.Emit8(0xC3);
}

// ============================================================================
// Stack operations
// ============================================================================

void X64CodeGen::Push(X64Register reg) {
    // PUSH reg
    // Encoding: 50+rd (or REX + 50+rd for extended registers)
    if (static_cast<uint8>(reg) & 0x8) {
        buffer_.Emit8(0x41); // REX.B
    }
    buffer_.Emit8(0x50 + (static_cast<uint8>(reg) & 0x7));
}

void X64CodeGen::Pop(X64Register reg) {
    // POP reg
    // Encoding: 58+rd (or REX + 58+rd for extended registers)
    if (static_cast<uint8>(reg) & 0x8) {
        buffer_.Emit8(0x41); // REX.B
    }
    buffer_.Emit8(0x58 + (static_cast<uint8>(reg) & 0x7));
}

// ============================================================================
// Special
// ============================================================================

void X64CodeGen::Nop() {
    // NOP
    // Encoding: 90
    buffer_.Emit8(0x90);
}

void X64CodeGen::Int3() {
    // INT3 (breakpoint)
    // Encoding: CC
    buffer_.Emit8(0xCC);
}

// ============================================================================
// Labels
// ============================================================================

X64CodeGen::Label X64CodeGen::CreateLabel() {
    Label label;
    label.position = 0;
    label.bound = false;
    return label;
}

void X64CodeGen::BindLabel(Label& label) {
    label.position = buffer_.GetPosition();
    label.bound = true;
    
    // Fixup all jumps to this label
    for (size_t fixup_pos : label.fixup_positions) {
        sint32 offset = static_cast<sint32>(label.position - (fixup_pos + 4));
        buffer_.Patch32(fixup_pos, static_cast<uint32>(offset));
    }
    
    label.fixup_positions.clear();
}

void X64CodeGen::JmpToLabel(const Label& label) {
    if (label.bound) {
        // Label already bound, calculate offset
        sint32 offset = static_cast<sint32>(label.position - (buffer_.GetPosition() + 5));
        Jmp(offset);
    } else {
        // Label not bound yet, emit placeholder and add fixup
        buffer_.Emit8(0xE9);
        const_cast<Label&>(label).fixup_positions.push_back(buffer_.GetPosition());
        buffer_.Emit32(0); // Placeholder
    }
}

void X64CodeGen::JmpIfToLabel(X64Condition cond, const Label& label) {
    if (label.bound) {
        sint32 offset = static_cast<sint32>(label.position - (buffer_.GetPosition() + 6));
        JmpIf(cond, offset);
    } else {
        buffer_.Emit8(0x0F);
        buffer_.Emit8(0x80 + static_cast<uint8>(cond));
        const_cast<Label&>(label).fixup_positions.push_back(buffer_.GetPosition());
        buffer_.Emit32(0); // Placeholder
    }
}

} // namespace brimir::jit

