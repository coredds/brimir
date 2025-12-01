/**
 * @file sh2_spec_shift.cpp
 * @brief Complete Shift and Rotate Instructions for SH-2
 * 
 * All 14 shift and rotate instructions from Saturn Open SDK
 * Reference: https://saturnopensdk.github.io/sh2.html
 */

#include "../include/sh2_spec.hpp"

namespace brimir::jit {

void AddShiftRotateInstructions(std::vector<SH2InstructionSpec>& specs) {
    
    // ========================================================================
    // LOGICAL SHIFT INSTRUCTIONS
    // ========================================================================
    
    // SHLL Rn - Shift logical left
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLL";
        spec.full_name = "Shift Logical Left";
        spec.syntax = "SHLL Rn";
        spec.opcode_pattern = 0x4000;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00000000";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "T ← Rn ← 0";
        spec.pseudocode = "T = (R[n] >> 31) & 1; R[n] <<= 1;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        specs.push_back(spec);
    }
    
    // SHLR Rn - Shift logical right
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLR";
        spec.full_name = "Shift Logical Right";
        spec.syntax = "SHLR Rn";
        spec.opcode_pattern = 0x4001;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00000001";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "0 → Rn → T";
        spec.pseudocode = "T = R[n] & 1; R[n] >>= 1;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        specs.push_back(spec);
    }
    
    // SHLL2 Rn - Shift logical left 2
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLL2";
        spec.full_name = "Shift Logical Left 2";
        spec.syntax = "SHLL2 Rn";
        spec.opcode_pattern = 0x4008;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00001000";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "Rn << 2 → Rn";
        spec.pseudocode = "R[n] <<= 2;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Shift";
        specs.push_back(spec);
    }
    
    // SHLR2 Rn - Shift logical right 2
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLR2";
        spec.full_name = "Shift Logical Right 2";
        spec.syntax = "SHLR2 Rn";
        spec.opcode_pattern = 0x4009;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00001001";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "Rn >> 2 → Rn";
        spec.pseudocode = "R[n] >>= 2;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Shift";
        specs.push_back(spec);
    }
    
    // SHLL8 Rn - Shift logical left 8
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLL8";
        spec.full_name = "Shift Logical Left 8";
        spec.syntax = "SHLL8 Rn";
        spec.opcode_pattern = 0x4018;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00011000";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "Rn << 8 → Rn";
        spec.pseudocode = "R[n] <<= 8;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Shift";
        specs.push_back(spec);
    }
    
    // SHLR8 Rn - Shift logical right 8
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLR8";
        spec.full_name = "Shift Logical Right 8";
        spec.syntax = "SHLR8 Rn";
        spec.opcode_pattern = 0x4019;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00011001";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "Rn >> 8 → Rn";
        spec.pseudocode = "R[n] >>= 8;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Shift";
        specs.push_back(spec);
    }
    
    // SHLL16 Rn - Shift logical left 16
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLL16";
        spec.full_name = "Shift Logical Left 16";
        spec.syntax = "SHLL16 Rn";
        spec.opcode_pattern = 0x4028;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00101000";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "Rn << 16 → Rn";
        spec.pseudocode = "R[n] <<= 16;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Shift";
        specs.push_back(spec);
    }
    
    // SHLR16 Rn - Shift logical right 16
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLR16";
        spec.full_name = "Shift Logical Right 16";
        spec.syntax = "SHLR16 Rn";
        spec.opcode_pattern = 0x4029;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00101001";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "Rn >> 16 → Rn";
        spec.pseudocode = "R[n] >>= 16;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Shift";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // ARITHMETIC SHIFT INSTRUCTIONS
    // ========================================================================
    
    // SHAL Rn - Shift arithmetic left
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHAL";
        spec.full_name = "Shift Arithmetic Left";
        spec.syntax = "SHAL Rn";
        spec.opcode_pattern = 0x4020;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00100000";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "T ← Rn ← 0";
        spec.pseudocode = "T = (R[n] >> 31) & 1; R[n] <<= 1;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        specs.push_back(spec);
    }
    
    // SHAR Rn - Shift arithmetic right
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHAR";
        spec.full_name = "Shift Arithmetic Right";
        spec.syntax = "SHAR Rn";
        spec.opcode_pattern = 0x4021;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00100001";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "MSB → Rn → T (arithmetic)";
        spec.pseudocode = "T = R[n] & 1; R[n] = (signed long)R[n] >> 1;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // ROTATE INSTRUCTIONS
    // ========================================================================
    
    // ROTL Rn - Rotate left
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "ROTL";
        spec.full_name = "Rotate Left";
        spec.syntax = "ROTL Rn";
        spec.opcode_pattern = 0x4004;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00000100";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "T ← Rn ← MSB";
        spec.pseudocode = "T = (R[n] >> 31) & 1; R[n] = (R[n] << 1) | T;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        specs.push_back(spec);
    }
    
    // ROTR Rn - Rotate right
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "ROTR";
        spec.full_name = "Rotate Right";
        spec.syntax = "ROTR Rn";
        spec.opcode_pattern = 0x4005;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00000101";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "LSB → Rn → T";
        spec.pseudocode = "T = R[n] & 1; R[n] = (R[n] >> 1) | (T << 31);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        specs.push_back(spec);
    }
    
    // ROTCL Rn - Rotate with carry left
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "ROTCL";
        spec.full_name = "Rotate with Carry Left";
        spec.syntax = "ROTCL Rn";
        spec.opcode_pattern = 0x4024;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00100100";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "T ← Rn ← T";
        spec.pseudocode = "unsigned long temp = (R[n] >> 31) & 1; R[n] = (R[n] << 1) | T; T = temp;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        specs.push_back(spec);
    }
    
    // ROTCR Rn - Rotate with carry right
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "ROTCR";
        spec.full_name = "Rotate with Carry Right";
        spec.syntax = "ROTCR Rn";
        spec.opcode_pattern = 0x4025;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00100101";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "T → Rn → T";
        spec.pseudocode = "unsigned long temp = R[n] & 1; R[n] = (R[n] >> 1) | (T << 31); T = temp;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        specs.push_back(spec);
    }
}

} // namespace brimir::jit

