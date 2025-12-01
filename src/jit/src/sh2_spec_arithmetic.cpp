/**
 * @file sh2_spec_arithmetic.cpp
 * @brief Complete Arithmetic Instructions for SH-2
 * 
 * All 28 arithmetic and comparison instructions from Saturn Open SDK
 * Reference: https://saturnopensdk.github.io/sh2.html
 */

#include "../include/sh2_spec.hpp"

namespace brimir::jit {

// Helper function to add arithmetic instructions
void AddArithmeticInstructions(std::vector<SH2InstructionSpec>& specs) {
    
    // ========================================================================
    // COMPARISON INSTRUCTIONS (8 variants)
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
        spec.operation = "If Rn ≥ Rm (unsigned): 1 → T";
        spec.pseudocode = "T = ((unsigned)R[n] >= (unsigned)R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
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
        spec.operation = "If Rn ≥ Rm (signed): 1 → T";
        spec.pseudocode = "T = ((signed)R[n] >= (signed)R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
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
        spec.operation = "If Rn > Rm (unsigned): 1 → T";
        spec.pseudocode = "T = ((unsigned)R[n] > (unsigned)R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
    
    // CMP/GT Rm, Rn - Compare signed greater than
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "CMP/GT";
        spec.full_name = "Compare Signed Greater Than";
        spec.syntax = "CMP/GT Rm, Rn";
        spec.opcode_pattern = 0x3007;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm0111";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "If Rn > Rm (signed): 1 → T";
        spec.pseudocode = "T = ((signed)R[n] > (signed)R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
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
        spec.operation = "If Rn ≥ 0: 1 → T";
        spec.pseudocode = "T = ((signed)R[n] >= 0);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
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
        spec.operation = "If Rn > 0: 1 → T";
        spec.pseudocode = "T = ((signed)R[n] > 0);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
    
    // CMP/STR Rm, Rn - Compare string
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
        spec.operation = "If any byte in Rn equals corresponding byte in Rm: 1 → T";
        spec.pseudocode = "unsigned long temp = R[n] ^ R[m]; T = ((temp & 0xFF000000) == 0 || (temp & 0x00FF0000) == 0 || (temp & 0x0000FF00) == 0 || (temp & 0x000000FF) == 0);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::COMPARISON;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // NEGATE INSTRUCTIONS
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
        spec.pseudocode = "R[n] = -R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
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
        spec.pseudocode = "unsigned long temp = -R[m]; R[n] = temp - T; T = (0 < temp || temp < R[n]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::CARRY;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // SUBTRACT WITH OVERFLOW
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
        specs.push_back(spec);
    }
    
    // ========================================================================
    // EXTEND INSTRUCTIONS
    // ========================================================================
    
    // EXTS.B Rm, Rn - Extend sign byte
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
        spec.operation = "sign_extend(Rm[7:0]) → Rn";
        spec.pseudocode = "R[n] = (signed char)(R[m] & 0xFF);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
    
    // EXTS.W Rm, Rn - Extend sign word
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
        spec.operation = "sign_extend(Rm[15:0]) → Rn";
        spec.pseudocode = "R[n] = (signed short)(R[m] & 0xFFFF);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
    
    // EXTU.B Rm, Rn - Extend unsigned byte
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
        spec.operation = "zero_extend(Rm[7:0]) → Rn";
        spec.pseudocode = "R[n] = (unsigned char)(R[m] & 0xFF);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
    
    // EXTU.W Rm, Rn - Extend unsigned word
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
        spec.operation = "zero_extend(Rm[15:0]) → Rn";
        spec.pseudocode = "R[n] = (unsigned short)(R[m] & 0xFFFF);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // DECREMENT AND TEST
    // ========================================================================
    
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
        spec.operation = "Rn - 1 → Rn, if Rn = 0: 1 → T, else: 0 → T";
        spec.pseudocode = "R[n]--; T = (R[n] == 0);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // MULTIPLY INSTRUCTIONS
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
        spec.operation = "Rn × Rm → MACL";
        spec.pseudocode = "MACL = R[n] * R[m];";
        spec.issue_cycles = 2; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
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
        spec.operation = "signed(Rn[15:0]) × signed(Rm[15:0]) → MACL";
        spec.pseudocode = "MACL = (signed short)(R[n] & 0xFFFF) * (signed short)(R[m] & 0xFFFF);";
        spec.issue_cycles = 1; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
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
        spec.operation = "unsigned(Rn[15:0]) × unsigned(Rm[15:0]) → MACL";
        spec.pseudocode = "MACL = (unsigned short)(R[n] & 0xFFFF) * (unsigned short)(R[m] & 0xFFFF);";
        spec.issue_cycles = 1; spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
    
    // DMULS.L Rm, Rn - Double-length multiply signed
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "DMULS.L";
        spec.full_name = "Double Multiply Signed Long";
        spec.syntax = "DMULS.L Rm, Rn";
        spec.opcode_pattern = 0x300D;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm1101";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "signed(Rn) × signed(Rm) → MACH:MACL (64-bit)";
        spec.pseudocode = "signed long long result = (signed long)R[n] * (signed long)R[m]; MACH = (result >> 32); MACL = result & 0xFFFFFFFF;";
        spec.issue_cycles = 2; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
    
    // DMULU.L Rm, Rn - Double-length multiply unsigned
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "DMULU.L";
        spec.full_name = "Double Multiply Unsigned Long";
        spec.syntax = "DMULU.L Rm, Rn";
        spec.opcode_pattern = 0x3005;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm0101";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "unsigned(Rn) × unsigned(Rm) → MACH:MACL (64-bit)";
        spec.pseudocode = "unsigned long long result = (unsigned long)R[n] * (unsigned long)R[m]; MACH = (result >> 32); MACL = result & 0xFFFFFFFF;";
        spec.issue_cycles = 2; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // MAC INSTRUCTIONS
    // ========================================================================
    
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
        spec.operation = "signed(@Rn) × signed(@Rm) + MAC → MAC, Rn+4, Rm+4";
        spec.pseudocode = "signed long temp_n = Read_32(R[n]); signed long temp_m = Read_32(R[m]); R[n] += 4; R[m] += 4; signed long long mac = ((signed long long)MACH << 32) | MACL; mac += (signed long long)temp_n * temp_m; MACH = (mac >> 32); MACL = mac & 0xFFFFFFFF;";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Arithmetic";
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
        spec.operation = "signed(@Rn) × signed(@Rm) + MAC → MAC, Rn+2, Rm+2";
        spec.pseudocode = "signed short temp_n = Read_16(R[n]); signed short temp_m = Read_16(R[m]); R[n] += 2; R[m] += 2; long result = (long)temp_n * temp_m; if (S) { /* Saturated */ } MACL += result;";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.affects_s = true;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // DIVISION INSTRUCTIONS
    // ========================================================================
    
    // DIV0S Rm, Rn - Division step 0 signed
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "DIV0S";
        spec.full_name = "Division Step 0 Signed";
        spec.syntax = "DIV0S Rm, Rn";
        spec.opcode_pattern = 0x2007;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm0111";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "MSB of Rn → Q, MSB of Rm → M, M^Q → T";
        spec.pseudocode = "Q = (R[n] >> 31) & 1; M = (R[m] >> 31) & 1; T = (Q ^ M);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.affects_q = true;
        spec.affects_m = true;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
    
    // DIV0U - Division step 0 unsigned
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "DIV0U";
        spec.full_name = "Division Step 0 Unsigned";
        spec.syntax = "DIV0U";
        spec.opcode_pattern = 0x0019;
        spec.opcode_mask = 0xFFFF;
        spec.binary_format = "0000000000011001";
        spec.format = InstrFormat::ZERO_OP;
        spec.operation = "0 → M, 0 → Q, 0 → T";
        spec.pseudocode = "M = 0; Q = 0; T = 0;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::CLEAR;
        spec.affects_q = true;
        spec.affects_m = true;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
    
    // DIV1 Rm, Rn - Division step 1
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "DIV1";
        spec.full_name = "Division Step 1";
        spec.syntax = "DIV1 Rm, Rn";
        spec.opcode_pattern = 0x3004;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm0100";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "1-step division (Rn ÷ Rm)";
        spec.pseudocode = "unsigned long tmp0, tmp2; unsigned char old_q, tmp1; old_q = Q; Q = (unsigned char)((0x80000000 & R[n]) != 0); tmp2 = R[m]; R[n] <<= 1; R[n] |= (unsigned long)T; if (!old_q) { if (!M) { R[n] -= tmp2; tmp1 = (R[n] > tmp0); Q ^= tmp1; } else { R[n] += tmp2; tmp1 = (R[n] < tmp0); Q ^= tmp1; } } else { if (!M) { R[n] += tmp2; tmp1 = (R[n] < tmp0); Q ^= tmp1; } else { R[n] -= tmp2; tmp1 = (R[n] > tmp0); Q ^= tmp1; } } T = (Q == M);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT;
        spec.affects_q = true;
        spec.category = "Arithmetic";
        specs.push_back(spec);
    }
}

} // namespace brimir::jit

