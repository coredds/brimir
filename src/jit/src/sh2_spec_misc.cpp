/**
 * @file sh2_spec_misc.cpp
 * @brief Miscellaneous and Cache Control Instructions for SH-2
 * 
 * Final remaining instructions to complete 133 total
 * Includes: logic immediate variants, cache control, prefetch
 * Reference: https://saturnopensdk.github.io/sh2.html
 */

#include "../include/sh2_spec.hpp"

namespace brimir::jit {

void AddMiscellaneousInstructions(std::vector<SH2InstructionSpec>& specs) {
    
    // ========================================================================
    // LOGIC IMMEDIATE VARIANTS
    // ========================================================================
    
    // AND #imm, R0 - AND immediate with R0
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "AND";
        spec.full_name = "AND Immediate";
        spec.syntax = "AND #imm, R0";
        spec.opcode_pattern = 0xC900;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11001001iiiiiiii";
        spec.format = InstrFormat::REG_IMM;
        spec.has_imm = true;
        spec.imm_bits = 8;
        spec.imm_signed = false;
        spec.operation = "R0 & imm → R0";
        spec.pseudocode = "R[0] &= (unsigned char)imm;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Logic";
        specs.push_back(spec);
    }
    
    // OR #imm, R0 - OR immediate with R0
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "OR";
        spec.full_name = "OR Immediate";
        spec.syntax = "OR #imm, R0";
        spec.opcode_pattern = 0xCB00;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11001011iiiiiiii";
        spec.format = InstrFormat::REG_IMM;
        spec.has_imm = true;
        spec.imm_bits = 8;
        spec.imm_signed = false;
        spec.operation = "R0 | imm → R0";
        spec.pseudocode = "R[0] |= (unsigned char)imm;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Logic";
        specs.push_back(spec);
    }
    
    // XOR #imm, R0 - XOR immediate with R0
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "XOR";
        spec.full_name = "XOR Immediate";
        spec.syntax = "XOR #imm, R0";
        spec.opcode_pattern = 0xCA00;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11001010iiiiiiii";
        spec.format = InstrFormat::REG_IMM;
        spec.has_imm = true;
        spec.imm_bits = 8;
        spec.imm_signed = false;
        spec.operation = "R0 ^ imm → R0";
        spec.pseudocode = "R[0] ^= (unsigned char)imm;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Logic";
        specs.push_back(spec);
    }
    
    // TST #imm, R0 - Test immediate with R0
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "TST";
        spec.full_name = "Test Immediate";
        spec.syntax = "TST #imm, R0";
        spec.opcode_pattern = 0xC800;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11001000iiiiiiii";
        spec.format = InstrFormat::REG_IMM;
        spec.has_imm = true;
        spec.imm_bits = 8;
        spec.imm_signed = false;
        spec.operation = "R0 & imm, if result is 0 then 1 → T";
        spec.pseudocode = "T = ((R[0] & (unsigned char)imm) == 0);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Logic";
        specs.push_back(spec);
    }
    
    // AND.B #imm, @(R0,GBR) - AND byte with GBR-relative memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "AND.B";
        spec.full_name = "AND Byte with GBR Memory";
        spec.syntax = "AND.B #imm, @(R0,GBR)";
        spec.opcode_pattern = 0xCD00;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11001101iiiiiiii";
        spec.format = InstrFormat::MEMORY;
        spec.has_imm = true;
        spec.imm_bits = 8;
        spec.imm_signed = false;
        spec.operation = "@(R0 + GBR) & imm → @(R0 + GBR)";
        spec.pseudocode = "unsigned char temp = Read_8(R[0] + GBR); Write_8(R[0] + GBR, temp & imm);";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.writes_memory = true;
        spec.category = "Logic";
        specs.push_back(spec);
    }
    
    // OR.B #imm, @(R0,GBR) - OR byte with GBR-relative memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "OR.B";
        spec.full_name = "OR Byte with GBR Memory";
        spec.syntax = "OR.B #imm, @(R0,GBR)";
        spec.opcode_pattern = 0xCF00;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11001111iiiiiiii";
        spec.format = InstrFormat::MEMORY;
        spec.has_imm = true;
        spec.imm_bits = 8;
        spec.imm_signed = false;
        spec.operation = "@(R0 + GBR) | imm → @(R0 + GBR)";
        spec.pseudocode = "unsigned char temp = Read_8(R[0] + GBR); Write_8(R[0] + GBR, temp | imm);";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.writes_memory = true;
        spec.category = "Logic";
        specs.push_back(spec);
    }
    
    // XOR.B #imm, @(R0,GBR) - XOR byte with GBR-relative memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "XOR.B";
        spec.full_name = "XOR Byte with GBR Memory";
        spec.syntax = "XOR.B #imm, @(R0,GBR)";
        spec.opcode_pattern = 0xCE00;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11001110iiiiiiii";
        spec.format = InstrFormat::MEMORY;
        spec.has_imm = true;
        spec.imm_bits = 8;
        spec.imm_signed = false;
        spec.operation = "@(R0 + GBR) ^ imm → @(R0 + GBR)";
        spec.pseudocode = "unsigned char temp = Read_8(R[0] + GBR); Write_8(R[0] + GBR, temp ^ imm);";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.writes_memory = true;
        spec.category = "Logic";
        specs.push_back(spec);
    }
    
    // TST.B #imm, @(R0,GBR) - Test byte with GBR-relative memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "TST.B";
        spec.full_name = "Test Byte with GBR Memory";
        spec.syntax = "TST.B #imm, @(R0,GBR)";
        spec.opcode_pattern = 0xCC00;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11001100iiiiiiii";
        spec.format = InstrFormat::MEMORY;
        spec.has_imm = true;
        spec.imm_bits = 8;
        spec.imm_signed = false;
        spec.operation = "@(R0 + GBR) & imm, if result is 0 then 1 → T";
        spec.pseudocode = "T = ((Read_8(R[0] + GBR) & (unsigned char)imm) == 0);";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.reads_memory = true;
        spec.category = "Logic";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // CACHE CONTROL INSTRUCTIONS
    // ========================================================================
    
    // PREF @Rn - Prefetch data to cache
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "PREF";
        spec.full_name = "Prefetch Data";
        spec.syntax = "PREF @Rn";
        spec.opcode_pattern = 0x0083;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0000nnnn10000011";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "Prefetch cache line at @Rn";
        spec.pseudocode = "/* Prefetch data at R[n] into cache */";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_cache_control = true;
        spec.category = "Cache";
        specs.push_back(spec);
    }
    
    // ICBI @Rn - Invalidate instruction cache block
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "ICBI";
        spec.full_name = "Instruction Cache Block Invalidate";
        spec.syntax = "ICBI @Rn";
        spec.opcode_pattern = 0x00E3;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0000nnnn11100011";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "Invalidate instruction cache block at @Rn";
        spec.pseudocode = "/* Invalidate I-cache line containing R[n] */";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_cache_control = true;
        spec.category = "Cache";
        specs.push_back(spec);
    }
    
    // OCBI @Rn - Operand cache block invalidate
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "OCBI";
        spec.full_name = "Operand Cache Block Invalidate";
        spec.syntax = "OCBI @Rn";
        spec.opcode_pattern = 0x0093;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0000nnnn10010011";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "Invalidate operand cache block at @Rn";
        spec.pseudocode = "/* Invalidate D-cache line containing R[n] */";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_cache_control = true;
        spec.category = "Cache";
        specs.push_back(spec);
    }
    
    // OCBP @Rn - Operand cache block purge
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "OCBP";
        spec.full_name = "Operand Cache Block Purge";
        spec.syntax = "OCBP @Rn";
        spec.opcode_pattern = 0x00A3;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0000nnnn10100011";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "Purge (write-back + invalidate) operand cache block at @Rn";
        spec.pseudocode = "/* If dirty, write-back D-cache line at R[n], then invalidate */";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_cache_control = true;
        spec.writes_memory = true; // Conditionally
        spec.category = "Cache";
        specs.push_back(spec);
    }
    
    // OCBWB @Rn - Operand cache block write-back
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "OCBWB";
        spec.full_name = "Operand Cache Block Write-Back";
        spec.syntax = "OCBWB @Rn";
        spec.opcode_pattern = 0x00B3;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0000nnnn10110011";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "Write-back operand cache block at @Rn";
        spec.pseudocode = "/* If dirty, write-back D-cache line at R[n] */";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_cache_control = true;
        spec.writes_memory = true; // Conditionally
        spec.category = "Cache";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // SPECIAL LOAD IMMEDIATE (for completeness)
    // ========================================================================
    
    // These might already be covered, but adding for absolute completeness
    
    // BT disp - Branch if true (no delay slot) - covered in base branch
    // BF disp - Branch if false (no delay slot) - covered in base branch
    
    // JSR @Rm, JMP @Rm - covered in base branch
    // RTS, RTE - covered
    
    // MOVT Rn - covered in data transfer
}

} // namespace brimir::jit

