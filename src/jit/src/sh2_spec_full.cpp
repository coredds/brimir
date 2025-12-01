/**
 * @file sh2_spec_full.cpp
 * @brief Complete SH-2 Instruction Set - All 133 Instructions
 * 
 * Manual high-quality implementation
 * Reference: Saturn Open SDK https://saturnopensdk.github.io/sh2.html
 */

#include "../include/sh2_spec.hpp"
#include <algorithm>

namespace brimir::jit {

std::vector<SH2InstructionSpec> SH2SpecDatabase::BuildDatabase() {
    std::vector<SH2InstructionSpec> specs;
    
    // ========================================================================
    // ARITHMETIC INSTRUCTIONS - COMPARISON FAMILY (8 instructions)
    // Critical for control flow - High Priority
    // ========================================================================
    
    // CMP/EQ Rm, Rn - Compare equal
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "CMP/EQ";
        spec.full_name = "Compare Equal";
        spec.syntax = "CMP/EQ Rm, Rn";
        spec.opcode_pattern = 0x3000;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm0000";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "If Rn = Rm: 1 → T, else: 0 → T";
        spec.pseudocode = "T = (R[n] == R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
        spec.tags = {"compare", "conditional"};
        specs.push_back(spec);
    }
    
    // CMP/EQ #imm, R0 - Compare equal immediate
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "CMP/EQ";
        spec.full_name = "Compare Equal Immediate";
        spec.syntax = "CMP/EQ #imm, R0";
        spec.opcode_pattern = 0x8800;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "10001000iiiiiiii";
        spec.format = InstrFormat::REG_IMM;
        spec.has_imm = true;
        spec.imm_bits = 8; spec.imm_signed = true;
        spec.operation = "If R0 = sign_extend(imm): 1 → T, else: 0 → T";
        spec.pseudocode = "T = (R[0] == (signed char)imm);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
        spec.tags = {"compare", "immediate"};
        specs.push_back(spec);
    }
    
    // CMP/HS Rm, Rn - Compare unsigned higher or same
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "CMP/HS";
        spec.full_name = "Compare Unsigned Higher or Same";
        spec.syntax = "CMP/HS Rm, Rn";
        spec.opcode_pattern = 0x3002;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm0010";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "If Rn ≥ Rm (unsigned): 1 → T, else: 0 → T";
        spec.pseudocode = "T = ((unsigned)R[n] >= (unsigned)R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
        spec.tags = {"compare", "unsigned"};
        specs.push_back(spec);
    }
    
    // CMP/GE Rm, Rn - Compare signed greater or equal
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "CMP/GE";
        spec.full_name = "Compare Signed Greater or Equal";
        spec.syntax = "CMP/GE Rm, Rn";
        spec.opcode_pattern = 0x3003;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm0011";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "If Rn ≥ Rm (signed): 1 → T, else: 0 → T";
        spec.pseudocode = "T = ((signed)R[n] >= (signed)R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
        spec.tags = {"compare", "signed"};
        specs.push_back(spec);
    }
    
    // CMP/HI Rm, Rn - Compare unsigned higher
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "CMP/HI";
        spec.full_name = "Compare Unsigned Higher";
        spec.syntax = "CMP/HI Rm, Rn";
        spec.opcode_pattern = 0x3006;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm0110";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "If Rn > Rm (unsigned): 1 → T, else: 0 → T";
        spec.pseudocode = "T = ((unsigned)R[n] > (unsigned)R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
        spec.tags = {"compare", "unsigned"};
        specs.push_back(spec);
    }
    
    // CMP/GT Rm, Rn - Compare signed greater
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "CMP/GT";
        spec.full_name = "Compare Signed Greater";
        spec.syntax = "CMP/GT Rm, Rn";
        spec.opcode_pattern = 0x3007;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm0111";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "If Rn > Rm (signed): 1 → T, else: 0 → T";
        spec.pseudocode = "T = ((signed)R[n] > (signed)R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
        spec.tags = {"compare", "signed"};
        specs.push_back(spec);
    }
    
    // CMP/PZ Rn - Compare positive or zero
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "CMP/PZ";
        spec.full_name = "Compare Positive or Zero";
        spec.syntax = "CMP/PZ Rn";
        spec.opcode_pattern = 0x4011;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00010001";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "If Rn ≥ 0 (signed): 1 → T, else: 0 → T";
        spec.pseudocode = "T = ((signed)R[n] >= 0);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
        spec.tags = {"compare", "signed"};
        specs.push_back(spec);
    }
    
    // CMP/PL Rn - Compare positive
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "CMP/PL";
        spec.full_name = "Compare Positive";
        spec.syntax = "CMP/PL Rn";
        spec.opcode_pattern = 0x4015;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00010101";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "If Rn > 0 (signed): 1 → T, else: 0 → T";
        spec.pseudocode = "T = ((signed)R[n] > 0);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
        spec.tags = {"compare", "signed"};
        specs.push_back(spec);
    }
    
    // CMP/STR Rm, Rn - Compare string (any byte equal)
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "CMP/STR";
        spec.full_name = "Compare String";
        spec.syntax = "CMP/STR Rm, Rn";
        spec.opcode_pattern = 0x200C;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm1100";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "If any byte of Rn equals corresponding byte of Rm: 1 → T";
        spec.pseudocode = "unsigned long temp = R[n] ^ R[m]; T = ((temp & 0xFF000000) == 0) || ((temp & 0x00FF0000) == 0) || ((temp & 0x0000FF00) == 0) || ((temp & 0x000000FF) == 0);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
        spec.tags = {"compare", "string"};
        specs.push_back(spec);
    }
    
    // ========================================================================
    // ARITHMETIC - NEGATE/EXTEND FAMILY (6 instructions)
    // ========================================================================
    
    // NEG Rm, Rn - Negate
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "NEG";
        spec.full_name = "Negate";
        spec.syntax = "NEG Rm, Rn";
        spec.opcode_pattern = 0x600B;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm1011";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "0 - Rm → Rn";
        spec.pseudocode = "R[n] = 0 - R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        spec.tags = {"negate"};
        specs.push_back(spec);
    }
    
    // NEGC Rm, Rn - Negate with carry
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "NEGC";
        spec.full_name = "Negate with Carry";
        spec.syntax = "NEGC Rm, Rn";
        spec.opcode_pattern = 0x600A;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm1010";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "0 - Rm - T → Rn, borrow → T";
        spec.pseudocode = "unsigned long temp = 0 - R[m]; R[n] = temp - T; T = (0 < temp || temp < R[n]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::CARRY;
        spec.category = "Arithmetic";
        spec.tags = {"negate", "carry"};
        specs.push_back(spec);
    }
    
    // EXTS.B Rm, Rn - Extend sign byte to long
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "EXTS.B";
        spec.full_name = "Extend as Signed Byte";
        spec.syntax = "EXTS.B Rm, Rn";
        spec.opcode_pattern = 0x600E;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm1110";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rm[7:0] → sign extension → Rn";
        spec.pseudocode = "R[n] = (signed char)(R[m] & 0xFF);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        spec.tags = {"extend", "sign"};
        specs.push_back(spec);
    }
    
    // EXTS.W Rm, Rn - Extend sign word to long
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "EXTS.W";
        spec.full_name = "Extend as Signed Word";
        spec.syntax = "EXTS.W Rm, Rn";
        spec.opcode_pattern = 0x600F;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm1111";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rm[15:0] → sign extension → Rn";
        spec.pseudocode = "R[n] = (signed short)(R[m] & 0xFFFF);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        spec.tags = {"extend", "sign"};
        specs.push_back(spec);
    }
    
    // EXTU.B Rm, Rn - Extend unsigned byte to long
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "EXTU.B";
        spec.full_name = "Extend as Unsigned Byte";
        spec.syntax = "EXTU.B Rm, Rn";
        spec.opcode_pattern = 0x600C;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm1100";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rm[7:0] → zero extension → Rn";
        spec.pseudocode = "R[n] = (unsigned char)(R[m] & 0xFF);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        spec.tags = {"extend", "unsigned"};
        specs.push_back(spec);
    }
    
    // EXTU.W Rm, Rn - Extend unsigned word to long
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "EXTU.W";
        spec.full_name = "Extend as Unsigned Word";
        spec.syntax = "EXTU.W Rm, Rn";
        spec.opcode_pattern = 0x600D;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm1101";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rm[15:0] → zero extension → Rn";
        spec.pseudocode = "R[n] = (unsigned short)(R[m] & 0xFFFF);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        spec.tags = {"extend", "unsigned"};
        specs.push_back(spec);
    }
    
    // ========================================================================
    // ARITHMETIC - MULTIPLY/DIVIDE FAMILY (10 instructions)
    // Critical for game math
    // ========================================================================
    
    // MUL.L Rm, Rn - Multiply long
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MUL.L";
        spec.full_name = "Multiply Long";
        spec.syntax = "MUL.L Rm, Rn";
        spec.opcode_pattern = 0x0007;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0000nnnnmmmm0111";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rn × Rm → MACL (32×32=32)";
        spec.pseudocode = "MACL = R[n] * R[m];";
        spec.issue_cycles = 2; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        spec.tags = {"multiply", "long"};
        specs.push_back(spec);
    }
    
    // MULS.W Rm, Rn - Multiply signed word
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MULS.W";
        spec.full_name = "Multiply Signed Word";
        spec.syntax = "MULS.W Rm, Rn";
        spec.opcode_pattern = 0x200F;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm1111";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "(signed)Rn[15:0] × (signed)Rm[15:0] → MACL";
        spec.pseudocode = "MACL = (signed short)R[n] * (signed short)R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        spec.tags = {"multiply", "signed", "word"};
        specs.push_back(spec);
    }
    
    // MULU.W Rm, Rn - Multiply unsigned word
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MULU.W";
        spec.full_name = "Multiply Unsigned Word";
        spec.syntax = "MULU.W Rm, Rn";
        spec.opcode_pattern = 0x200E;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm1110";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "(unsigned)Rn[15:0] × (unsigned)Rm[15:0] → MACL";
        spec.pseudocode = "MACL = (unsigned short)R[n] * (unsigned short)R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        spec.tags = {"multiply", "unsigned", "word"};
        specs.push_back(spec);
    }
    
    // DMULS.L Rm, Rn - Double multiply signed
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "DMULS.L";
        spec.full_name = "Double-precision Multiply Signed";
        spec.syntax = "DMULS.L Rm, Rn";
        spec.opcode_pattern = 0x300D;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm1101";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "(signed)Rn × (signed)Rm → MAC (32×32=64)";
        spec.pseudocode = "long long result = (signed long)R[n] * (signed long)R[m]; MACH = result >> 32; MACL = result & 0xFFFFFFFF;";
        spec.issue_cycles = 2; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        spec.tags = {"multiply", "signed", "double"};
        specs.push_back(spec);
    }
    
    // DMULU.L Rm, Rn - Double multiply unsigned
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "DMULU.L";
        spec.full_name = "Double-precision Multiply Unsigned";
        spec.syntax = "DMULU.L Rm, Rn";
        spec.opcode_pattern = 0x3005;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm0101";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "(unsigned)Rn × (unsigned)Rm → MAC (32×32=64)";
        spec.pseudocode = "unsigned long long result = (unsigned long)R[n] * (unsigned long)R[m]; MACH = result >> 32; MACL = result & 0xFFFFFFFF;";
        spec.issue_cycles = 2; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        spec.tags = {"multiply", "unsigned", "double"};
        specs.push_back(spec);
    }
    
    // DIV0S Rm, Rn - Division step 0 (signed)
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "DIV0S";
        spec.full_name = "Divide Step 0 Signed";
        spec.syntax = "DIV0S Rm, Rn";
        spec.opcode_pattern = 0x2007;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm0111";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "MSB of Rn → Q, MSB of Rm → M, M^Q → T";
        spec.pseudocode = "Q = (R[n] >> 31) & 1; M = (R[m] >> 31) & 1; T = Q ^ M;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.affects_q = true; spec.affects_m = true;
        spec.category = "Arithmetic";
        spec.tags = {"divide", "signed", "setup"};
        specs.push_back(spec);
    }
    
    // DIV0U - Division step 0 (unsigned)
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "DIV0U";
        spec.full_name = "Divide Step 0 Unsigned";
        spec.syntax = "DIV0U";
        spec.opcode_pattern = 0x0019;
        spec.opcode_mask = 0xFFFF;
        spec.binary_format = "0000000000011001";
        spec.format = InstrFormat::ZERO_OP;
        spec.operation = "0 → M, 0 → Q, 0 → T";
        spec.pseudocode = "M = 0; Q = 0; T = 0;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::CLEAR;
        spec.affects_q = true; spec.affects_m = true;
        spec.category = "Arithmetic";
        spec.tags = {"divide", "unsigned", "setup"};
        specs.push_back(spec);
    }
    
    // DIV1 Rm, Rn - Division step 1
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "DIV1";
        spec.full_name = "Divide Step 1";
        spec.syntax = "DIV1 Rm, Rn";
        spec.opcode_pattern = 0x3004;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm0100";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "1-step division (Rn ÷ Rm)";
        spec.pseudocode = "// Complex division algorithm - see Saturn Open SDK for full pseudocode";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.affects_q = true;
        spec.category = "Arithmetic";
        spec.tags = {"divide", "step"};
        specs.push_back(spec);
    }
    
    // DT Rn - Decrement and test
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "DT";
        spec.full_name = "Decrement and Test";
        spec.syntax = "DT Rn";
        spec.opcode_pattern = 0x4010;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00010000";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "Rn - 1 → Rn, if Rn is 0: 1 → T, else: 0 → T";
        spec.pseudocode = "R[n]--; T = (R[n] == 0);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Arithmetic";
        spec.tags = {"decrement", "loop"};
        specs.push_back(spec);
    }
    
    // MAC.L @Rm+, @Rn+ - Multiply and accumulate long
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MAC.L";
        spec.full_name = "Multiply and Accumulate Long";
        spec.syntax = "MAC.L @Rm+, @Rn+";
        spec.opcode_pattern = 0x000F;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0000nnnnmmmm1111";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "MAC + (signed)@Rn × (signed)@Rm → MAC";
        spec.pseudocode = "signed long RnL = Read_32(R[n]); signed long RmL = Read_32(R[m]); R[n] += 4; R[m] += 4; MAC += (signed long long)RnL * (signed long long)RmL;";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.affects_s = true;
        spec.reads_memory = true;
        spec.category = "Arithmetic";
        spec.tags = {"multiply", "accumulate", "long"};
        specs.push_back(spec);
    }
    
    // ========================================================================
    // SHIFT & ROTATE INSTRUCTIONS (14 instructions)
    // Very common in bit manipulation
    // ========================================================================
    
    // SHLL Rn - Shift left logical
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLL";
        spec.full_name = "Shift Left Logical";
        spec.syntax = "SHLL Rn";
        spec.opcode_pattern = 0x4000;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00000000";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "T ← Rn[31], Rn << 1 → Rn";
        spec.pseudocode = "T = (R[n] >> 31) & 1; R[n] <<= 1;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        spec.tags = {"shift", "left"};
        specs.push_back(spec);
    }
    
    // SHLR Rn - Shift right logical
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLR";
        spec.full_name = "Shift Right Logical";
        spec.syntax = "SHLR Rn";
        spec.opcode_pattern = 0x4001;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00000001";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "T ← Rn[0], Rn >> 1 → Rn";
        spec.pseudocode = "T = R[n] & 1; R[n] >>= 1;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        spec.tags = {"shift", "right", "logical"};
        specs.push_back(spec);
    }
    
    // SHAL Rn - Shift left arithmetic
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHAL";
        spec.full_name = "Shift Left Arithmetic";
        spec.syntax = "SHAL Rn";
        spec.opcode_pattern = 0x4020;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00100000";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "T ← Rn[31], Rn << 1 → Rn";
        spec.pseudocode = "T = (R[n] >> 31) & 1; R[n] <<= 1;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        spec.tags = {"shift", "left", "arithmetic"};
        specs.push_back(spec);
    }
    
    // SHAR Rn - Shift right arithmetic
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHAR";
        spec.full_name = "Shift Right Arithmetic";
        spec.syntax = "SHAR Rn";
        spec.opcode_pattern = 0x4021;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00100001";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "T ← Rn[0], Rn >> 1 (arithmetic) → Rn";
        spec.pseudocode = "T = R[n] & 1; R[n] = (signed)R[n] >> 1;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        spec.tags = {"shift", "right", "arithmetic"};
        specs.push_back(spec);
    }
    
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
        spec.operation = "T ← Rn[31], Rn << 1 → Rn, Rn[0] ← T";
        spec.pseudocode = "T = (R[n] >> 31) & 1; R[n] = (R[n] << 1) | T;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        spec.tags = {"rotate", "left"};
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
        spec.operation = "T ← Rn[0], Rn >> 1 → Rn, Rn[31] ← T";
        spec.pseudocode = "T = R[n] & 1; R[n] = (R[n] >> 1) | (T << 31);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        spec.tags = {"rotate", "right"};
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
        spec.operation = "T ← Rn[31], Rn << 1 → Rn, old T → Rn[0]";
        spec.pseudocode = "unsigned long temp = T; T = (R[n] >> 31) & 1; R[n] = (R[n] << 1) | temp;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        spec.tags = {"rotate", "carry", "left"};
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
        spec.operation = "T ← Rn[0], Rn >> 1 → Rn, old T → Rn[31]";
        spec.pseudocode = "unsigned long temp = T; T = R[n] & 1; R[n] = (R[n] >> 1) | (temp << 31);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Shift";
        spec.tags = {"rotate", "carry", "right"};
        specs.push_back(spec);
    }
    
    // SHLL2 Rn - Shift left logical 2 bits
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLL2";
        spec.full_name = "Shift Left Logical 2";
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
        spec.tags = {"shift", "left", "fast"};
        specs.push_back(spec);
    }
    
    // SHLL8 Rn - Shift left logical 8 bits
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLL8";
        spec.full_name = "Shift Left Logical 8";
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
        spec.tags = {"shift", "left", "fast"};
        specs.push_back(spec);
    }
    
    // SHLL16 Rn - Shift left logical 16 bits
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLL16";
        spec.full_name = "Shift Left Logical 16";
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
        spec.tags = {"shift", "left", "fast"};
        specs.push_back(spec);
    }
    
    // SHLR2 Rn - Shift right logical 2 bits
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLR2";
        spec.full_name = "Shift Right Logical 2";
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
        spec.tags = {"shift", "right", "fast"};
        specs.push_back(spec);
    }
    
    // SHLR8 Rn - Shift right logical 8 bits
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLR8";
        spec.full_name = "Shift Right Logical 8";
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
        spec.tags = {"shift", "right", "fast"};
        specs.push_back(spec);
    }
    
    // SHLR16 Rn - Shift right logical 16 bits
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SHLR16";
        spec.full_name = "Shift Right Logical 16";
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
        spec.tags = {"shift", "right", "fast"};
        specs.push_back(spec);
    }
    
    // ========================================================================
    // ARITHMETIC - Remaining (SUBV, MAC.W)
    // ========================================================================
    
    // SUBV Rm, Rn - Subtract with overflow check
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SUBV";
        spec.full_name = "Subtract with V flag";
        spec.syntax = "SUBV Rm, Rn";
        spec.opcode_pattern = 0x300B;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm1011";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rn - Rm → Rn, underflow → T";
        spec.pseudocode = "long dest = (long)R[n]; long src = (long)R[m]; long ans = dest - src; R[n] = ans; T = ((dest >= 0 && src < 0 && ans < 0) || (dest < 0 && src >= 0 && ans >= 0));";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::OVERFLOW;
        spec.category = "Arithmetic";
        spec.tags = {"subtract", "overflow"};
        specs.push_back(spec);
    }
    
    // MAC.W @Rm+, @Rn+ - Multiply and accumulate word
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MAC.W";
        spec.full_name = "Multiply and Accumulate Word";
        spec.syntax = "MAC.W @Rm+, @Rn+";
        spec.opcode_pattern = 0x400F;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0100nnnnmmmm1111";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "MAC + (signed)@Rn × (signed)@Rm → MAC";
        spec.pseudocode = "signed short RnW = Read_16(R[n]); signed short RmW = Read_16(R[m]); R[n] += 2; R[m] += 2; MAC += (signed long long)RnW * (signed long long)RmW;";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.affects_s = true;
        spec.reads_memory = true;
        spec.category = "Arithmetic";
        spec.tags = {"multiply", "accumulate", "word"};
        specs.push_back(spec);
    }
    
    // ========================================================================
    // BRANCH INSTRUCTIONS - Remaining (6 instructions)
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
        spec.operation = "If T = 1: PC + 4 + disp×2 → PC (with delay slot)";
        spec.pseudocode = "if (T) { delay_slot(); PC = PC + 4 + (sign_extend(disp) << 1); }";
        spec.issue_cycles = 2; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        spec.tags = {"branch", "conditional", "delay"};
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
        spec.operation = "If T = 0: PC + 4 + disp×2 → PC (with delay slot)";
        spec.pseudocode = "if (!T) { delay_slot(); PC = PC + 4 + (sign_extend(disp) << 1); }";
        spec.issue_cycles = 2; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        spec.tags = {"branch", "conditional", "delay"};
        specs.push_back(spec);
    }
    
    // BRAF Rm - Branch far always
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "BRAF";
        spec.full_name = "Branch Far Always";
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
        spec.tags = {"branch", "far"};
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
        spec.tags = {"branch", "subroutine", "far"};
        specs.push_back(spec);
    }
    
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
        spec.operation = "Stack → PC/SR";
        spec.pseudocode = "PC = Read_32(R[15]); R[15] += 4; SR = Read_32(R[15]); R[15] += 4;";
        spec.issue_cycles = 4; spec.latency_cycles = 4;
        spec.t_bit_effect = TBitEffect::RESULT; // Restored from stack
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.is_privileged = false; // Actually not privileged on SH-2
        spec.reads_memory = true;
        spec.category = "Branch";
        spec.tags = {"return", "exception"};
        specs.push_back(spec);
    }
    
    // ========================================================================
    // SYSTEM CONTROL - LDC/STC FAMILY (12 instructions)
    // ========================================================================
    
    // LDC Rm, SR - Load control register SR
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDC";
        spec.full_name = "Load Control Register";
        spec.syntax = "LDC Rm, SR";
        spec.opcode_pattern = 0x400E;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00001110";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rm = true;
        spec.operation = "Rm → SR";
        spec.pseudocode = "SR = R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT; // SR loaded, so T changes
        spec.category = "System";
        spec.tags = {"load", "control"};
        specs.push_back(spec);
    }
    
    // LDC Rm, GBR - Load control register GBR
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDC";
        spec.full_name = "Load Control Register";
        spec.syntax = "LDC Rm, GBR";
        spec.opcode_pattern = 0x401E;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00011110";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rm = true;
        spec.operation = "Rm → GBR";
        spec.pseudocode = "GBR = R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        spec.tags = {"load", "control"};
        specs.push_back(spec);
    }
    
    // LDC Rm, VBR - Load control register VBR
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDC";
        spec.full_name = "Load Control Register";
        spec.syntax = "LDC Rm, VBR";
        spec.opcode_pattern = 0x402E;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00101110";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rm = true;
        spec.operation = "Rm → VBR";
        spec.pseudocode = "VBR = R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        spec.tags = {"load", "control"};
        specs.push_back(spec);
    }
    
    // STC SR, Rn - Store control register SR
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STC";
        spec.full_name = "Store Control Register";
        spec.syntax = "STC SR, Rn";
        spec.opcode_pattern = 0x0002;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0000nnnn00000010";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "SR → Rn";
        spec.pseudocode = "R[n] = SR;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        spec.tags = {"store", "control"};
        specs.push_back(spec);
    }
    
    // STC GBR, Rn - Store control register GBR
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STC";
        spec.full_name = "Store Control Register";
        spec.syntax = "STC GBR, Rn";
        spec.opcode_pattern = 0x0012;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0000nnnn00010010";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "GBR → Rn";
        spec.pseudocode = "R[n] = GBR;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        spec.tags = {"store", "control"};
        specs.push_back(spec);
    }
    
    // STC VBR, Rn - Store control register VBR
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STC";
        spec.full_name = "Store Control Register";
        spec.syntax = "STC VBR, Rn";
        spec.opcode_pattern = 0x0022;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0000nnnn00100010";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "VBR → Rn";
        spec.pseudocode = "R[n] = VBR;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        spec.tags = {"store", "control"};
        specs.push_back(spec);
    }
    
    // LDS Rm, MACH - Load system register MACH
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDS";
        spec.full_name = "Load System Register";
        spec.syntax = "LDS Rm, MACH";
        spec.opcode_pattern = 0x400A;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00001010";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rm = true;
        spec.operation = "Rm → MACH";
        spec.pseudocode = "MACH = R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        spec.tags = {"load", "system"};
        specs.push_back(spec);
    }
    
    // LDS Rm, MACL - Load system register MACL
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDS";
        spec.full_name = "Load System Register";
        spec.syntax = "LDS Rm, MACL";
        spec.opcode_pattern = 0x401A;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00011010";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rm = true;
        spec.operation = "Rm → MACL";
        spec.pseudocode = "MACL = R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        spec.tags = {"load", "system"};
        specs.push_back(spec);
    }
    
    // LDS Rm, PR - Load system register PR
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDS";
        spec.full_name = "Load System Register";
        spec.syntax = "LDS Rm, PR";
        spec.opcode_pattern = 0x402A;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00101010";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rm = true;
        spec.operation = "Rm → PR";
        spec.pseudocode = "PR = R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        spec.tags = {"load", "system"};
        specs.push_back(spec);
    }
    
    // STS MACH, Rn - Store system register MACH
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STS";
        spec.full_name = "Store System Register";
        spec.syntax = "STS MACH, Rn";
        spec.opcode_pattern = 0x000A;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0000nnnn00001010";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "MACH → Rn";
        spec.pseudocode = "R[n] = MACH;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        spec.tags = {"store", "system"};
        specs.push_back(spec);
    }
    
    // STS MACL, Rn - Store system register MACL
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STS";
        spec.full_name = "Store System Register";
        spec.syntax = "STS MACL, Rn";
        spec.opcode_pattern = 0x001A;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0000nnnn00011010";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "MACL → Rn";
        spec.pseudocode = "R[n] = MACL;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        spec.tags = {"store", "system"};
        specs.push_back(spec);
    }
    
    // STS PR, Rn - Store system register PR
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STS";
        spec.full_name = "Store System Register";
        spec.syntax = "STS PR, Rn";
        spec.opcode_pattern = 0x002A;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0000nnnn00101010";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rn = true;
        spec.operation = "PR → Rn";
        spec.pseudocode = "R[n] = PR;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        spec.tags = {"store", "system"};
        specs.push_back(spec);
    }
    
    // SLEEP - Enter sleep mode
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SLEEP";
        spec.full_name = "Sleep";
        spec.syntax = "SLEEP";
        spec.opcode_pattern = 0x001B;
        spec.opcode_mask = 0xFFFF;
        spec.binary_format = "0000000000011011";
        spec.format = InstrFormat::ZERO_OP;
        spec.operation = "Enter power-down mode";
        spec.pseudocode = "// Wait for interrupt";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        spec.tags = {"power", "sleep"};
        specs.push_back(spec);
    }
    
    // TRAPA #imm - Trap always
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "TRAPA";
        spec.full_name = "Trap Always";
        spec.syntax = "TRAPA #imm";
        spec.opcode_pattern = 0xC300;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11000011iiiiiiii";
        spec.format = InstrFormat::REG_IMM;
        spec.has_imm = true;
        spec.imm_bits = 8; spec.imm_signed = false;
        spec.operation = "PC+2 → Stack, SR → Stack, @(VBR + imm×4) → PC";
        spec.pseudocode = "R[15] -= 4; Write_32(R[15], SR); R[15] -= 4; Write_32(R[15], PC + 2); PC = Read_32(VBR + (imm << 2));";
        spec.issue_cycles = 8; spec.latency_cycles = 8;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.reads_memory = true;
        spec.category = "System";
        spec.tags = {"trap", "exception"};
        specs.push_back(spec);
    }
    
    // CLRS - Clear S bit
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "CLRS";
        spec.full_name = "Clear S bit";
        spec.syntax = "CLRS";
        spec.opcode_pattern = 0x0048;
        spec.opcode_mask = 0xFFFF;
        spec.binary_format = "0000000001001000";
        spec.format = InstrFormat::ZERO_OP;
        spec.operation = "0 → S";
        spec.pseudocode = "S = 0;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.affects_s = true;
        spec.category = "System";
        spec.tags = {"flag", "clear"};
        specs.push_back(spec);
    }
    
    // SETS - Set S bit
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SETS";
        spec.full_name = "Set S bit";
        spec.syntax = "SETS";
        spec.opcode_pattern = 0x0058;
        spec.opcode_mask = 0xFFFF;
        spec.binary_format = "0000000001011000";
        spec.format = InstrFormat::ZERO_OP;
        spec.operation = "1 → S";
        spec.pseudocode = "S = 1;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.affects_s = true;
        spec.category = "System";
        spec.tags = {"flag", "set"};
        specs.push_back(spec);
    }
    
    // ========================================================================
    // DATA TRANSFER - Post-increment/Pre-decrement variants (10 instructions)
    // ========================================================================
    
    // MOV.B @Rm+, Rn - Load byte with post-increment
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte Post-increment";
        spec.syntax = "MOV.B @Rm+, Rn";
        spec.opcode_pattern = 0x6004;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm0100";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "@Rm → sign extension → Rn, Rm + 1 → Rm";
        spec.pseudocode = "R[n] = (signed char)Read_8(R[m]); R[m]++;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "post-increment"};
        specs.push_back(spec);
    }
    
    // MOV.W @Rm+, Rn - Load word with post-increment
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word Post-increment";
        spec.syntax = "MOV.W @Rm+, Rn";
        spec.opcode_pattern = 0x6005;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm0101";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "@Rm → sign extension → Rn, Rm + 2 → Rm";
        spec.pseudocode = "R[n] = (signed short)Read_16(R[m]); R[m] += 2;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "post-increment"};
        specs.push_back(spec);
    }
    
    // MOV.L @Rm+, Rn - Load long with post-increment
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long Post-increment";
        spec.syntax = "MOV.L @Rm+, Rn";
        spec.opcode_pattern = 0x6006;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm0110";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "@Rm → Rn, Rm + 4 → Rm";
        spec.pseudocode = "R[n] = Read_32(R[m]); R[m] += 4;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "post-increment"};
        specs.push_back(spec);
    }
    
    // MOV.B Rm, @-Rn - Store byte with pre-decrement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte Pre-decrement";
        spec.syntax = "MOV.B Rm, @-Rn";
        spec.opcode_pattern = 0x2004;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm0100";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rn - 1 → Rn, Rm → @Rn";
        spec.pseudocode = "R[n]--; Write_8(R[n], R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "pre-decrement"};
        specs.push_back(spec);
    }
    
    // MOV.W Rm, @-Rn - Store word with pre-decrement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word Pre-decrement";
        spec.syntax = "MOV.W Rm, @-Rn";
        spec.opcode_pattern = 0x2005;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm0101";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rn - 2 → Rn, Rm → @Rn";
        spec.pseudocode = "R[n] -= 2; Write_16(R[n], R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "pre-decrement"};
        specs.push_back(spec);
    }
    
    // MOV.L Rm, @-Rn - Store long with pre-decrement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long Pre-decrement";
        spec.syntax = "MOV.L Rm, @-Rn";
        spec.opcode_pattern = 0x2006;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm0110";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rn - 4 → Rn, Rm → @Rn";
        spec.pseudocode = "R[n] -= 4; Write_32(R[n], R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "pre-decrement"};
        specs.push_back(spec);
    }
    
    // MOV.B @(R0,Rm), Rn - Load byte indexed
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte Indexed";
        spec.syntax = "MOV.B @(R0,Rm), Rn";
        spec.opcode_pattern = 0x000C;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0000nnnnmmmm1100";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "@(R0 + Rm) → sign extension → Rn";
        spec.pseudocode = "R[n] = (signed char)Read_8(R[0] + R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "indexed"};
        specs.push_back(spec);
    }
    
    // MOV.W @(R0,Rm), Rn - Load word indexed
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word Indexed";
        spec.syntax = "MOV.W @(R0,Rm), Rn";
        spec.opcode_pattern = 0x000D;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0000nnnnmmmm1101";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "@(R0 + Rm) → sign extension → Rn";
        spec.pseudocode = "R[n] = (signed short)Read_16(R[0] + R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "indexed"};
        specs.push_back(spec);
    }
    
    // MOV.L @(R0,Rm), Rn - Load long indexed
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long Indexed";
        spec.syntax = "MOV.L @(R0,Rm), Rn";
        spec.opcode_pattern = 0x000E;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0000nnnnmmmm1110";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "@(R0 + Rm) → Rn";
        spec.pseudocode = "R[n] = Read_32(R[0] + R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "indexed"};
        specs.push_back(spec);
    }
    
    // MOV.B Rm, @(R0,Rn) - Store byte indexed
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte Indexed Store";
        spec.syntax = "MOV.B Rm, @(R0,Rn)";
        spec.opcode_pattern = 0x0004;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0000nnnnmmmm0100";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rm → @(R0 + Rn)";
        spec.pseudocode = "Write_8(R[0] + R[n], R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "indexed"};
        specs.push_back(spec);
    }
    
    // ========================================================================
    // DATA TRANSFER - Displacement variants (15 instructions)
    // ========================================================================
    
    // MOV.B R0, @(disp,Rn) - Store byte with displacement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte Displacement";
        spec.syntax = "MOV.B R0, @(disp,Rn)";
        spec.opcode_pattern = 0x8000;
        spec.opcode_mask = 0xF000;
        spec.binary_format = "10000000nnnndddd";
        spec.format = InstrFormat::REG_DISP;
        spec.has_rn = true; spec.has_disp = true;
        spec.operation = "R0 → @(disp + Rn)";
        spec.pseudocode = "Write_8(R[n] + disp, R[0]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "displacement"};
        specs.push_back(spec);
    }
    
    // MOV.W R0, @(disp,Rn) - Store word with displacement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word Displacement";
        spec.syntax = "MOV.W R0, @(disp,Rn)";
        spec.opcode_pattern = 0x8100;
        spec.opcode_mask = 0xF000;
        spec.binary_format = "10000001nnnndddd";
        spec.format = InstrFormat::REG_DISP;
        spec.has_rn = true; spec.has_disp = true;
        spec.operation = "R0 → @(disp×2 + Rn)";
        spec.pseudocode = "Write_16(R[n] + (disp << 1), R[0]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "displacement"};
        specs.push_back(spec);
    }
    
    // MOV.L Rm, @(disp,Rn) - Store long with displacement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long Displacement";
        spec.syntax = "MOV.L Rm, @(disp,Rn)";
        spec.opcode_pattern = 0x1000;
        spec.opcode_mask = 0xF000;
        spec.binary_format = "0001nnnnmmmmdddd";
        spec.format = InstrFormat::REG_DISP;
        spec.has_rn = true; spec.has_rm = true; spec.has_disp = true;
        spec.operation = "Rm → @(disp×4 + Rn)";
        spec.pseudocode = "Write_32(R[n] + (disp << 2), R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "displacement"};
        specs.push_back(spec);
    }
    
    // MOV.B @(disp,Rm), R0 - Load byte with displacement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte Displacement Load";
        spec.syntax = "MOV.B @(disp,Rm), R0";
        spec.opcode_pattern = 0x8400;
        spec.opcode_mask = 0xF000;
        spec.binary_format = "10000100mmmmdddd";
        spec.format = InstrFormat::REG_DISP;
        spec.has_rm = true; spec.has_disp = true;
        spec.operation = "@(disp + Rm) → sign extension → R0";
        spec.pseudocode = "R[0] = (signed char)Read_8(R[m] + disp);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "displacement"};
        specs.push_back(spec);
    }
    
    // MOV.W @(disp,Rm), R0 - Load word with displacement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word Displacement Load";
        spec.syntax = "MOV.W @(disp,Rm), R0";
        spec.opcode_pattern = 0x8500;
        spec.opcode_mask = 0xF000;
        spec.binary_format = "10000101mmmmdddd";
        spec.format = InstrFormat::REG_DISP;
        spec.has_rm = true; spec.has_disp = true;
        spec.operation = "@(disp×2 + Rm) → sign extension → R0";
        spec.pseudocode = "R[0] = (signed short)Read_16(R[m] + (disp << 1));";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "displacement"};
        specs.push_back(spec);
    }
    
    // MOV.L @(disp,Rm), Rn - Load long with displacement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long Displacement Load";
        spec.syntax = "MOV.L @(disp,Rm), Rn";
        spec.opcode_pattern = 0x5000;
        spec.opcode_mask = 0xF000;
        spec.binary_format = "0101nnnnmmmmdddd";
        spec.format = InstrFormat::REG_DISP;
        spec.has_rn = true; spec.has_rm = true; spec.has_disp = true;
        spec.operation = "@(disp×4 + Rm) → Rn";
        spec.pseudocode = "R[n] = Read_32(R[m] + (disp << 2));";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "displacement"};
        specs.push_back(spec);
    }
    
    // MOV.B R0, @(disp,GBR) - Store byte GBR-relative
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte GBR-relative";
        spec.syntax = "MOV.B R0, @(disp,GBR)";
        spec.opcode_pattern = 0xC000;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11000000dddddddd";
        spec.format = InstrFormat::REG_DISP;
        spec.has_disp = true;
        spec.operation = "R0 → @(disp + GBR)";
        spec.pseudocode = "Write_8(GBR + disp, R[0]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "gbr"};
        specs.push_back(spec);
    }
    
    // MOV.W R0, @(disp,GBR) - Store word GBR-relative
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word GBR-relative";
        spec.syntax = "MOV.W R0, @(disp,GBR)";
        spec.opcode_pattern = 0xC100;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11000001dddddddd";
        spec.format = InstrFormat::REG_DISP;
        spec.has_disp = true;
        spec.operation = "R0 → @(disp×2 + GBR)";
        spec.pseudocode = "Write_16(GBR + (disp << 1), R[0]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "gbr"};
        specs.push_back(spec);
    }
    
    // MOV.L R0, @(disp,GBR) - Store long GBR-relative
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long GBR-relative";
        spec.syntax = "MOV.L R0, @(disp,GBR)";
        spec.opcode_pattern = 0xC200;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11000010dddddddd";
        spec.format = InstrFormat::REG_DISP;
        spec.has_disp = true;
        spec.operation = "R0 → @(disp×4 + GBR)";
        spec.pseudocode = "Write_32(GBR + (disp << 2), R[0]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "gbr"};
        specs.push_back(spec);
    }
    
    // MOV.B @(disp,GBR), R0 - Load byte GBR-relative
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte GBR-relative Load";
        spec.syntax = "MOV.B @(disp,GBR), R0";
        spec.opcode_pattern = 0xC400;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11000100dddddddd";
        spec.format = InstrFormat::REG_DISP;
        spec.has_disp = true;
        spec.operation = "@(disp + GBR) → sign extension → R0";
        spec.pseudocode = "R[0] = (signed char)Read_8(GBR + disp);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "gbr"};
        specs.push_back(spec);
    }
    
    // MOV.W @(disp,GBR), R0 - Load word GBR-relative
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word GBR-relative Load";
        spec.syntax = "MOV.W @(disp,GBR), R0";
        spec.opcode_pattern = 0xC500;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11000101dddddddd";
        spec.format = InstrFormat::REG_DISP;
        spec.has_disp = true;
        spec.operation = "@(disp×2 + GBR) → sign extension → R0";
        spec.pseudocode = "R[0] = (signed short)Read_16(GBR + (disp << 1));";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "gbr"};
        specs.push_back(spec);
    }
    
    // MOV.L @(disp,GBR), R0 - Load long GBR-relative
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long GBR-relative Load";
        spec.syntax = "MOV.L @(disp,GBR), R0";
        spec.opcode_pattern = 0xC600;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11000110dddddddd";
        spec.format = InstrFormat::REG_DISP;
        spec.has_disp = true;
        spec.operation = "@(disp×4 + GBR) → R0";
        spec.pseudocode = "R[0] = Read_32(GBR + (disp << 2));";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "gbr"};
        specs.push_back(spec);
    }
    
    // ========================================================================
    // LOGIC - Immediate and Memory variants (6 instructions)
    // ========================================================================
    
    // AND #imm, R0 - AND immediate
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
        spec.imm_bits = 8; spec.imm_signed = false;
        spec.operation = "R0 & imm → R0";
        spec.pseudocode = "R[0] &= imm;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Logic";
        spec.tags = {"logic", "immediate"};
        specs.push_back(spec);
    }
    
    // AND.B #imm, @(R0,GBR) - AND byte memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "AND.B";
        spec.full_name = "AND Byte Memory";
        spec.syntax = "AND.B #imm, @(R0,GBR)";
        spec.opcode_pattern = 0xCD00;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11001101iiiiiiii";
        spec.format = InstrFormat::REG_IMM;
        spec.has_imm = true;
        spec.imm_bits = 8; spec.imm_signed = false;
        spec.operation = "@(R0+GBR) & imm → @(R0+GBR)";
        spec.pseudocode = "unsigned char temp = Read_8(GBR + R[0]); Write_8(GBR + R[0], temp & imm);";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.writes_memory = true;
        spec.category = "Logic";
        spec.tags = {"logic", "memory"};
        specs.push_back(spec);
    }
    
    // OR #imm, R0 - OR immediate
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
        spec.imm_bits = 8; spec.imm_signed = false;
        spec.operation = "R0 | imm → R0";
        spec.pseudocode = "R[0] |= imm;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Logic";
        spec.tags = {"logic", "immediate"};
        specs.push_back(spec);
    }
    
    // OR.B #imm, @(R0,GBR) - OR byte memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "OR.B";
        spec.full_name = "OR Byte Memory";
        spec.syntax = "OR.B #imm, @(R0,GBR)";
        spec.opcode_pattern = 0xCF00;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11001111iiiiiiii";
        spec.format = InstrFormat::REG_IMM;
        spec.has_imm = true;
        spec.imm_bits = 8; spec.imm_signed = false;
        spec.operation = "@(R0+GBR) | imm → @(R0+GBR)";
        spec.pseudocode = "unsigned char temp = Read_8(GBR + R[0]); Write_8(GBR + R[0], temp | imm);";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.writes_memory = true;
        spec.category = "Logic";
        spec.tags = {"logic", "memory"};
        specs.push_back(spec);
    }
    
    // XOR #imm, R0 - XOR immediate
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
        spec.imm_bits = 8; spec.imm_signed = false;
        spec.operation = "R0 ^ imm → R0";
        spec.pseudocode = "R[0] ^= imm;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Logic";
        spec.tags = {"logic", "immediate"};
        specs.push_back(spec);
    }
    
    // XOR.B #imm, @(R0,GBR) - XOR byte memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "XOR.B";
        spec.full_name = "XOR Byte Memory";
        spec.syntax = "XOR.B #imm, @(R0,GBR)";
        spec.opcode_pattern = 0xCE00;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11001110iiiiiiii";
        spec.format = InstrFormat::REG_IMM;
        spec.has_imm = true;
        spec.imm_bits = 8; spec.imm_signed = false;
        spec.operation = "@(R0+GBR) ^ imm → @(R0+GBR)";
        spec.pseudocode = "unsigned char temp = Read_8(GBR + R[0]); Write_8(GBR + R[0], temp ^ imm);";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.writes_memory = true;
        spec.category = "Logic";
        spec.tags = {"logic", "memory"};
        specs.push_back(spec);
    }
    
    // TST #imm, R0 - Test immediate
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
        spec.imm_bits = 8; spec.imm_signed = false;
        spec.operation = "R0 & imm, if result is 0: 1 → T";
        spec.pseudocode = "T = ((R[0] & imm) == 0);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Logic";
        spec.tags = {"test", "immediate"};
        specs.push_back(spec);
    }
    
    // TST.B #imm, @(R0,GBR) - Test byte memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "TST.B";
        spec.full_name = "Test Byte Memory";
        spec.syntax = "TST.B #imm, @(R0,GBR)";
        spec.opcode_pattern = 0xCC00;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11001100iiiiiiii";
        spec.format = InstrFormat::REG_IMM;
        spec.has_imm = true;
        spec.imm_bits = 8; spec.imm_signed = false;
        spec.operation = "@(R0+GBR) & imm, if result is 0: 1 → T";
        spec.pseudocode = "T = ((Read_8(GBR + R[0]) & imm) == 0);";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.reads_memory = true;
        spec.category = "Logic";
        spec.tags = {"test", "memory"};
        specs.push_back(spec);
    }
    
    // ========================================================================
    // SYSTEM - Memory-to-memory LDC/STC/LDS/STS (12 instructions)
    // ========================================================================
    
    // LDC.L @Rm+, SR - Load control register from memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDC.L";
        spec.full_name = "Load Control Register from Memory";
        spec.syntax = "LDC.L @Rm+, SR";
        spec.opcode_pattern = 0x4007;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00000111";
        spec.format = InstrFormat::MEMORY;
        spec.has_rm = true;
        spec.operation = "@Rm → SR, Rm + 4 → Rm";
        spec.pseudocode = "SR = Read_32(R[m]); R[m] += 4;";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.reads_memory = true;
        spec.category = "System";
        spec.tags = {"load", "control", "memory"};
        specs.push_back(spec);
    }
    
    // LDC.L @Rm+, GBR - Load GBR from memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDC.L";
        spec.full_name = "Load GBR from Memory";
        spec.syntax = "LDC.L @Rm+, GBR";
        spec.opcode_pattern = 0x4017;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00010111";
        spec.format = InstrFormat::MEMORY;
        spec.has_rm = true;
        spec.operation = "@Rm → GBR, Rm + 4 → Rm";
        spec.pseudocode = "GBR = Read_32(R[m]); R[m] += 4;";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "System";
        spec.tags = {"load", "control", "memory"};
        specs.push_back(spec);
    }
    
    // LDC.L @Rm+, VBR - Load VBR from memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDC.L";
        spec.full_name = "Load VBR from Memory";
        spec.syntax = "LDC.L @Rm+, VBR";
        spec.opcode_pattern = 0x4027;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00100111";
        spec.format = InstrFormat::MEMORY;
        spec.has_rm = true;
        spec.operation = "@Rm → VBR, Rm + 4 → Rm";
        spec.pseudocode = "VBR = Read_32(R[m]); R[m] += 4;";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "System";
        spec.tags = {"load", "control", "memory"};
        specs.push_back(spec);
    }
    
    // STC.L SR, @-Rn - Store SR to memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STC.L";
        spec.full_name = "Store SR to Memory";
        spec.syntax = "STC.L SR, @-Rn";
        spec.opcode_pattern = 0x4003;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00000011";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true;
        spec.operation = "Rn - 4 → Rn, SR → @Rn";
        spec.pseudocode = "R[n] -= 4; Write_32(R[n], SR);";
        spec.issue_cycles = 2; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "System";
        spec.tags = {"store", "control", "memory"};
        specs.push_back(spec);
    }
    
    // STC.L GBR, @-Rn - Store GBR to memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STC.L";
        spec.full_name = "Store GBR to Memory";
        spec.syntax = "STC.L GBR, @-Rn";
        spec.opcode_pattern = 0x4013;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00010011";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true;
        spec.operation = "Rn - 4 → Rn, GBR → @Rn";
        spec.pseudocode = "R[n] -= 4; Write_32(R[n], GBR);";
        spec.issue_cycles = 2; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "System";
        spec.tags = {"store", "control", "memory"};
        specs.push_back(spec);
    }
    
    // STC.L VBR, @-Rn - Store VBR to memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STC.L";
        spec.full_name = "Store VBR to Memory";
        spec.syntax = "STC.L VBR, @-Rn";
        spec.opcode_pattern = 0x4023;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00100011";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true;
        spec.operation = "Rn - 4 → Rn, VBR → @Rn";
        spec.pseudocode = "R[n] -= 4; Write_32(R[n], VBR);";
        spec.issue_cycles = 2; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "System";
        spec.tags = {"store", "control", "memory"};
        specs.push_back(spec);
    }
    
    // LDS.L @Rm+, MACH - Load MACH from memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDS.L";
        spec.full_name = "Load MACH from Memory";
        spec.syntax = "LDS.L @Rm+, MACH";
        spec.opcode_pattern = 0x4006;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00000110";
        spec.format = InstrFormat::MEMORY;
        spec.has_rm = true;
        spec.operation = "@Rm → MACH, Rm + 4 → Rm";
        spec.pseudocode = "MACH = Read_32(R[m]); R[m] += 4;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "System";
        spec.tags = {"load", "system", "memory"};
        specs.push_back(spec);
    }
    
    // LDS.L @Rm+, MACL - Load MACL from memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDS.L";
        spec.full_name = "Load MACL from Memory";
        spec.syntax = "LDS.L @Rm+, MACL";
        spec.opcode_pattern = 0x4016;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00010110";
        spec.format = InstrFormat::MEMORY;
        spec.has_rm = true;
        spec.operation = "@Rm → MACL, Rm + 4 → Rm";
        spec.pseudocode = "MACL = Read_32(R[m]); R[m] += 4;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "System";
        spec.tags = {"load", "system", "memory"};
        specs.push_back(spec);
    }
    
    // LDS.L @Rm+, PR - Load PR from memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDS.L";
        spec.full_name = "Load PR from Memory";
        spec.syntax = "LDS.L @Rm+, PR";
        spec.opcode_pattern = 0x4026;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00100110";
        spec.format = InstrFormat::MEMORY;
        spec.has_rm = true;
        spec.operation = "@Rm → PR, Rm + 4 → Rm";
        spec.pseudocode = "PR = Read_32(R[m]); R[m] += 4;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "System";
        spec.tags = {"load", "system", "memory"};
        specs.push_back(spec);
    }
    
    // STS.L MACH, @-Rn - Store MACH to memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STS.L";
        spec.full_name = "Store MACH to Memory";
        spec.syntax = "STS.L MACH, @-Rn";
        spec.opcode_pattern = 0x4002;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00000010";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true;
        spec.operation = "Rn - 4 → Rn, MACH → @Rn";
        spec.pseudocode = "R[n] -= 4; Write_32(R[n], MACH);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "System";
        spec.tags = {"store", "system", "memory"};
        specs.push_back(spec);
    }
    
    // STS.L MACL, @-Rn - Store MACL to memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STS.L";
        spec.full_name = "Store MACL to Memory";
        spec.syntax = "STS.L MACL, @-Rn";
        spec.opcode_pattern = 0x4012;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00010010";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true;
        spec.operation = "Rn - 4 → Rn, MACL → @Rn";
        spec.pseudocode = "R[n] -= 4; Write_32(R[n], MACL);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "System";
        spec.tags = {"store", "system", "memory"};
        specs.push_back(spec);
    }
    
    // STS.L PR, @-Rn - Store PR to memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STS.L";
        spec.full_name = "Store PR to Memory";
        spec.syntax = "STS.L PR, @-Rn";
        spec.opcode_pattern = 0x4022;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100nnnn00100010";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true;
        spec.operation = "Rn - 4 → Rn, PR → @Rn";
        spec.pseudocode = "R[n] -= 4; Write_32(R[n], PR);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "System";
        spec.tags = {"store", "system", "memory"};
        specs.push_back(spec);
    }
    
    // ========================================================================
    // MISCELLANEOUS (3 final instructions)
    // ========================================================================
    
    // PREF @Rn - Prefetch
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "PREF";
        spec.full_name = "Prefetch";
        spec.syntax = "PREF @Rn";
        spec.opcode_pattern = 0x0083;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0000nnnn10000011";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true;
        spec.operation = "Prefetch operand cache block";
        spec.pseudocode = "// Cache prefetch";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        spec.tags = {"cache", "prefetch"};
        specs.push_back(spec);
    }
    
    // MOV.W Rm, @(R0,Rn) - Store word indexed
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word Indexed Store";
        spec.syntax = "MOV.W Rm, @(R0,Rn)";
        spec.opcode_pattern = 0x0005;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0000nnnnmmmm0101";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rm → @(R0 + Rn)";
        spec.pseudocode = "Write_16(R[0] + R[n], R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "indexed"};
        specs.push_back(spec);
    }
    
    // MOV.L Rm, @(R0,Rn) - Store long indexed
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long Indexed Store";
        spec.syntax = "MOV.L Rm, @(R0,Rn)";
        spec.opcode_pattern = 0x0006;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0000nnnnmmmm0110";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "Rm → @(R0 + Rn)";
        spec.pseudocode = "Write_32(R[0] + R[n], R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"move", "indexed"};
        specs.push_back(spec);
    }
    
    // ✅ COMPLETE: All 133 SH-2 instructions specified!
    
    return specs;
}

// Implementation remains same as before...

const std::vector<SH2InstructionSpec>& SH2SpecDatabase::GetAllInstructions() {
    static std::vector<SH2InstructionSpec> specs = BuildDatabase();
    return specs;
}

std::optional<SH2InstructionSpec> SH2SpecDatabase::GetByMnemonic(const std::string& mnemonic) {
    const auto& specs = GetAllInstructions();
    auto it = std::find_if(specs.begin(), specs.end(),
        [&mnemonic](const SH2InstructionSpec& spec) {
            return spec.mnemonic == mnemonic;
        });
    if (it != specs.end()) return *it;
    return std::nullopt;
}

std::optional<SH2InstructionSpec> SH2SpecDatabase::Decode(uint16 instr) {
    const auto& specs = GetAllInstructions();
    for (const auto& spec : specs) {
        if (spec.Matches(instr)) return spec;
    }
    return std::nullopt;
}

std::vector<SH2InstructionSpec> SH2SpecDatabase::GetByCategory(const std::string& category) {
    const auto& specs = GetAllInstructions();
    std::vector<SH2InstructionSpec> result;
    std::copy_if(specs.begin(), specs.end(), std::back_inserter(result),
        [&category](const SH2InstructionSpec& spec) {
            return spec.category == category;
        });
    return result;
}

SH2SpecDatabase::Stats SH2SpecDatabase::GetStats() {
    const auto& specs = GetAllInstructions();
    Stats stats;
    stats.total_instructions = specs.size();
    for (const auto& spec : specs) {
        if (spec.implemented) stats.implemented++;
        if (spec.tested) stats.tested++;
    }
    return stats;
}

} // namespace brimir::jit

