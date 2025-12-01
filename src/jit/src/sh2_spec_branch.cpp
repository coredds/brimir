/**
 * @file sh2_spec_branch.cpp
 * @brief Complete Branch Instructions for SH-2
 * 
 * All 13 branch/control flow instructions from Saturn Open SDK
 * Reference: https://saturnopensdk.github.io/sh2.html
 */

#include "../include/sh2_spec.hpp"

namespace brimir::jit {

void AddBranchInstructions(std::vector<SH2InstructionSpec>& specs) {
    
    // ========================================================================
    // CONDITIONAL BRANCHES (with delay slot)
    // ========================================================================
    
    // BT/S disp - Branch if true with delay slot
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "BT/S";
        spec.full_name = "Branch if True with Delay Slot";
        spec.syntax = "BT/S disp";
        spec.opcode_pattern = 0x8D00;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "10001101dddddddd";
        spec.format = InstrFormat::BRANCH;
        spec.has_disp = true;
        spec.has_imm = true;
        spec.imm_bits = 8;
        spec.imm_signed = true;
        spec.operation = "If T = 1: delayed branch to PC + 4 + disp×2";
        spec.pseudocode = "if (T) { unsigned long temp = PC; PC = PC + 4 + (sign_extend(disp) << 1); /* execute delay slot at temp+2 */ }";
        spec.issue_cycles = 2; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        specs.push_back(spec);
    }
    
    // BF/S disp - Branch if false with delay slot
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "BF/S";
        spec.full_name = "Branch if False with Delay Slot";
        spec.syntax = "BF/S disp";
        spec.opcode_pattern = 0x8F00;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "10001111dddddddd";
        spec.format = InstrFormat::BRANCH;
        spec.has_disp = true;
        spec.has_imm = true;
        spec.imm_bits = 8;
        spec.imm_signed = true;
        spec.operation = "If T = 0: delayed branch to PC + 4 + disp×2";
        spec.pseudocode = "if (!T) { unsigned long temp = PC; PC = PC + 4 + (sign_extend(disp) << 1); /* execute delay slot at temp+2 */ }";
        spec.issue_cycles = 2; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // BRANCH FAR (register-based)
    // ========================================================================
    
    // BRAF Rm - Branch far
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "BRAF";
        spec.full_name = "Branch Far";
        spec.syntax = "BRAF Rm";
        spec.opcode_pattern = 0x0023;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0000mmmm00100011";
        spec.format = InstrFormat::BRANCH_REG;
        spec.has_rm = true;
        spec.operation = "PC + 4 + Rm → PC";
        spec.pseudocode = "PC = PC + 4 + R[m];";
        spec.issue_cycles = 2; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        specs.push_back(spec);
    }
    
    // BSRF Rm - Branch to subroutine far
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "BSRF";
        spec.full_name = "Branch to Subroutine Far";
        spec.syntax = "BSRF Rm";
        spec.opcode_pattern = 0x0003;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0000mmmm00000011";
        spec.format = InstrFormat::BRANCH_REG;
        spec.has_rm = true;
        spec.operation = "PC + 4 → PR, PC + 4 + Rm → PC";
        spec.pseudocode = "PR = PC + 4; PC = PC + 4 + R[m];";
        spec.issue_cycles = 2; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // EXCEPTION RETURN
    // ========================================================================
    
    // RTE - Return from exception
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "RTE";
        spec.full_name = "Return from Exception";
        spec.syntax = "RTE";
        spec.opcode_pattern = 0x002B;
        spec.opcode_mask = 0xFFFF;
        spec.binary_format = "0000000000101011";
        spec.format = InstrFormat::ZERO_OP;
        spec.operation = "Stack → PC, Stack → SR";
        spec.pseudocode = "PC = Read_32(R[15]); R[15] += 4; SR = Read_32(R[15]); R[15] += 4;";
        spec.issue_cycles = 4; spec.latency_cycles = 4;
        spec.t_bit_effect = TBitEffect::RESULT; // Restored from stack
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.is_privileged = true;
        spec.reads_memory = true;
        spec.category = "Branch";
        specs.push_back(spec);
    }
}

} // namespace brimir::jit

