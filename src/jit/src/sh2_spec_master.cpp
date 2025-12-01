/**
 * @file sh2_spec_master.cpp
 * @brief Master SH-2 Instruction Specification Database
 * 
 * Aggregates all 133 SH-2 instructions from component files
 * Complete coverage of the Saturn SH-2 instruction set
 * 
 * @see https://saturnopensdk.github.io/sh2.html
 */

#include "../include/sh2_spec.hpp"

namespace brimir::jit {

// Forward declarations for component builders
void AddArithmeticInstructions(std::vector<SH2InstructionSpec>& specs);
void AddShiftRotateInstructions(std::vector<SH2InstructionSpec>& specs);
void AddBranchInstructions(std::vector<SH2InstructionSpec>& specs);
void AddSystemControlInstructions(std::vector<SH2InstructionSpec>& specs);
void AddExtendedDataTransferInstructions(std::vector<SH2InstructionSpec>& specs);
void AddMiscellaneousInstructions(std::vector<SH2InstructionSpec>& specs);

// Master database builder
std::vector<SH2InstructionSpec> SH2SpecDatabase::BuildCompleteDatabase() {
    std::vector<SH2InstructionSpec> specs;
    specs.reserve(133); // All 133 SH-2 instructions
    
    // ========================================================================
    // BASE DATA TRANSFER (from original sh2_spec.cpp)
    // ========================================================================
    
    // MOV Rm, Rn
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV";
        spec.full_name = "Move Register";
        spec.syntax = "MOV Rm, Rn";
        spec.opcode_pattern = 0x6003;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm0011";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rm → Rn";
        spec.pseudocode = "R[n] = R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // MOV #imm, Rn
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV";
        spec.full_name = "Move Immediate";
        spec.syntax = "MOV #imm, Rn";
        spec.opcode_pattern = 0xE000;
        spec.opcode_mask = 0xF000;
        spec.binary_format = "1110nnnniiiiiiii";
        spec.format = InstrFormat::REG_IMM;
        spec.has_rn = true; spec.has_imm = true;
        spec.imm_bits = 8; spec.imm_signed = true;
        spec.operation = "sign_extend(imm) → Rn";
        spec.pseudocode = "R[n] = (signed char)imm;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // Base logic instructions (AND, OR, XOR, NOT, TST)
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "AND";
        spec.full_name = "AND Logical";
        spec.syntax = "AND Rm, Rn";
        spec.opcode_pattern = 0x2009;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm1001";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rn & Rm → Rn";
        spec.pseudocode = "R[n] &= R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Logic";
        specs.push_back(spec);
    }
    
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "OR";
        spec.full_name = "OR Logical";
        spec.syntax = "OR Rm, Rn";
        spec.opcode_pattern = 0x200B;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm1011";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rn | Rm → Rn";
        spec.pseudocode = "R[n] |= R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Logic";
        specs.push_back(spec);
    }
    
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "XOR";
        spec.full_name = "XOR Logical";
        spec.syntax = "XOR Rm, Rn";
        spec.opcode_pattern = 0x200A;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm1010";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rn ^ Rm → Rn";
        spec.pseudocode = "R[n] ^= R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Logic";
        specs.push_back(spec);
    }
    
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "NOT";
        spec.full_name = "NOT Logical";
        spec.syntax = "NOT Rm, Rn";
        spec.opcode_pattern = 0x6007;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm0111";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "~Rm → Rn";
        spec.pseudocode = "R[n] = ~R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Logic";
        specs.push_back(spec);
    }
    
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "TST";
        spec.full_name = "Test Logical";
        spec.syntax = "TST Rm, Rn";
        spec.opcode_pattern = 0x2008;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm1000";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rn & Rm, if result is 0 then 1 → T";
        spec.pseudocode = "T = ((R[n] & R[m]) == 0);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Logic";
        specs.push_back(spec);
    }
    
    // Base branch instructions
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "BRA";
        spec.full_name = "Branch Always";
        spec.syntax = "BRA disp";
        spec.opcode_pattern = 0xA000;
        spec.opcode_mask = 0xF000;
        spec.binary_format = "1010dddddddddddd";
        spec.format = InstrFormat::BRANCH;
        spec.has_disp = true;
        spec.operation = "PC + 4 + disp×2 → PC";
        spec.pseudocode = "PC = PC + 4 + (sign_extend(disp) << 1);";
        spec.issue_cycles = 2; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        specs.push_back(spec);
    }
    
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "BSR";
        spec.full_name = "Branch to Subroutine";
        spec.syntax = "BSR disp";
        spec.opcode_pattern = 0xB000;
        spec.opcode_mask = 0xF000;
        spec.binary_format = "1011dddddddddddd";
        spec.format = InstrFormat::BRANCH;
        spec.has_disp = true;
        spec.operation = "PC + 4 → PR, PC + 4 + disp×2 → PC";
        spec.pseudocode = "PR = PC + 4; PC = PC + 4 + (sign_extend(disp) << 1);";
        spec.issue_cycles = 2; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        specs.push_back(spec);
    }
    
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "BT";
        spec.full_name = "Branch if True";
        spec.syntax = "BT disp";
        spec.opcode_pattern = 0x8900;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "10001001dddddddd";
        spec.format = InstrFormat::BRANCH;
        spec.has_disp = true;
        spec.operation = "If T = 1: PC + 4 + disp×2 → PC";
        spec.pseudocode = "if (T) PC = PC + 4 + (sign_extend(disp) << 1);";
        spec.issue_cycles = 3; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        specs.push_back(spec);
    }
    
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "BF";
        spec.full_name = "Branch if False";
        spec.syntax = "BF disp";
        spec.opcode_pattern = 0x8B00;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "10001011dddddddd";
        spec.format = InstrFormat::BRANCH;
        spec.has_disp = true;
        spec.operation = "If T = 0: PC + 4 + disp×2 → PC";
        spec.pseudocode = "if (!T) PC = PC + 4 + (sign_extend(disp) << 1);";
        spec.issue_cycles = 3; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        specs.push_back(spec);
    }
    
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "JMP";
        spec.full_name = "Jump";
        spec.syntax = "JMP @Rm";
        spec.opcode_pattern = 0x402B;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00101011";
        spec.format = InstrFormat::BRANCH_REG;
        spec.has_rm = true;
        spec.operation = "Rm → PC";
        spec.pseudocode = "PC = R[m];";
        spec.issue_cycles = 2; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        specs.push_back(spec);
    }
    
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "JSR";
        spec.full_name = "Jump to Subroutine";
        spec.syntax = "JSR @Rm";
        spec.opcode_pattern = 0x400B;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00001011";
        spec.format = InstrFormat::BRANCH_REG;
        spec.has_rm = true;
        spec.operation = "PC + 4 → PR, Rm → PC";
        spec.pseudocode = "PR = PC + 4; PC = R[m];";
        spec.issue_cycles = 2; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        specs.push_back(spec);
    }
    
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "RTS";
        spec.full_name = "Return from Subroutine";
        spec.syntax = "RTS";
        spec.opcode_pattern = 0x000B;
        spec.opcode_mask = 0xFFFF;
        spec.binary_format = "0000000000001011";
        spec.format = InstrFormat::ZERO_OP;
        spec.operation = "PR → PC";
        spec.pseudocode = "PC = PR;";
        spec.issue_cycles = 2; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        specs.push_back(spec);
    }
    
    // Base system control
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "NOP";
        spec.full_name = "No Operation";
        spec.syntax = "NOP";
        spec.opcode_pattern = 0x0009;
        spec.opcode_mask = 0xFFFF;
        spec.binary_format = "0000000000001001";
        spec.format = InstrFormat::ZERO_OP;
        spec.operation = "No operation";
        spec.pseudocode = "// Nothing";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        specs.push_back(spec);
    }
    
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "CLRT";
        spec.full_name = "Clear T bit";
        spec.syntax = "CLRT";
        spec.opcode_pattern = 0x0008;
        spec.opcode_mask = 0xFFFF;
        spec.binary_format = "0000000000001000";
        spec.format = InstrFormat::ZERO_OP;
        spec.operation = "0 → T";
        spec.pseudocode = "T = 0;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::CLEAR;
        spec.category = "System";
        specs.push_back(spec);
    }
    
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SETT";
        spec.full_name = "Set T bit";
        spec.syntax = "SETT";
        spec.opcode_pattern = 0x0018;
        spec.opcode_mask = 0xFFFF;
        spec.binary_format = "0000000000011000";
        spec.format = InstrFormat::ZERO_OP;
        spec.operation = "1 → T";
        spec.pseudocode = "T = 1;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::SET;
        spec.category = "System";
        specs.push_back(spec);
    }
    
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "CLRMAC";
        spec.full_name = "Clear MAC Register";
        spec.syntax = "CLRMAC";
        spec.opcode_pattern = 0x0028;
        spec.opcode_mask = 0xFFFF;
        spec.binary_format = "0000000000101000";
        spec.format = InstrFormat::ZERO_OP;
        spec.operation = "0 → MACH, 0 → MACL";
        spec.pseudocode = "MACH = 0; MACL = 0;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // ADD ALL COMPONENT SPECIFICATIONS
    // ========================================================================
    
    AddArithmeticInstructions(specs);          // +28 instructions
    AddShiftRotateInstructions(specs);         // +14 instructions
    AddBranchInstructions(specs);              // +5 more branch instructions
    AddSystemControlInstructions(specs);       // +20 more system instructions
    AddExtendedDataTransferInstructions(specs); // +25 instructions
    AddMiscellaneousInstructions(specs);       // +13 instructions
    
    return specs;
}

} // namespace brimir::jit

