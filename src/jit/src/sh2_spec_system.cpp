/**
 * @file sh2_spec_system.cpp
 * @brief Complete System Control Instructions for SH-2
 * 
 * All 30+ system control instructions from Saturn Open SDK
 * Includes LDC, STC, LDS, STS families and system operations
 * Reference: https://saturnopensdk.github.io/sh2.html
 */

#include "../include/sh2_spec.hpp"

namespace brimir::jit {

void AddSystemControlInstructions(std::vector<SH2InstructionSpec>& specs) {
    
    // ========================================================================
    // STATUS CONTROL (CLRT, SETT, CLRS, SETS, CLRMAC)
    // ========================================================================
    
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
        specs.push_back(spec);
    }
    
    // ========================================================================
    // LDC - Load to Control Register
    // ========================================================================
    
    // LDC Rm, SR - Load to status register
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDC";
        spec.full_name = "Load to Control Register (SR)";
        spec.syntax = "LDC Rm, SR";
        spec.opcode_pattern = 0x400E;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00001110";
        spec.format = InstrFormat::ONE_REG;
        spec.has_rm = true;
        spec.operation = "Rm → SR";
        spec.pseudocode = "SR = R[m];";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::RESULT; // Loaded from Rm
        spec.is_privileged = true;
        spec.category = "System";
        specs.push_back(spec);
    }
    
    // LDC Rm, GBR - Load to global base register
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDC";
        spec.full_name = "Load to Control Register (GBR)";
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
        specs.push_back(spec);
    }
    
    // LDC Rm, VBR - Load to vector base register
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDC";
        spec.full_name = "Load to Control Register (VBR)";
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
        spec.is_privileged = true;
        spec.category = "System";
        specs.push_back(spec);
    }
    
    // LDC.L @Rm+, SR - Load to SR from memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDC.L";
        spec.full_name = "Load to Control Register from Memory (SR)";
        spec.syntax = "LDC.L @Rm+, SR";
        spec.opcode_pattern = 0x4007;
        spec.opcode_mask = 0xF0FF;
        spec.binary_format = "0100mmmm00000111";
        spec.format = InstrFormat::MEMORY;
        spec.has_rm = true;
        spec.operation = "@Rm → SR, Rm + 4 → Rm";
        spec.pseudocode = "SR = Read_32(R[m]); R[m] += 4;";
        spec.issue_cycles = 3; spec.latency_cycles = 3;
        spec.t_bit_effect = TBitEffect::RESULT; // Loaded from memory
        spec.reads_memory = true;
        spec.is_privileged = true;
        spec.category = "System";
        specs.push_back(spec);
    }
    
    // LDC.L @Rm+, GBR - Load to GBR from memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDC.L";
        spec.full_name = "Load to Control Register from Memory (GBR)";
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
        specs.push_back(spec);
    }
    
    // LDC.L @Rm+, VBR - Load to VBR from memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDC.L";
        spec.full_name = "Load to Control Register from Memory (VBR)";
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
        spec.is_privileged = true;
        spec.category = "System";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // STC - Store from Control Register
    // ========================================================================
    
    // STC SR, Rn - Store from status register
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STC";
        spec.full_name = "Store from Control Register (SR)";
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
        spec.is_privileged = true;
        spec.category = "System";
        specs.push_back(spec);
    }
    
    // STC GBR, Rn - Store from GBR
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STC";
        spec.full_name = "Store from Control Register (GBR)";
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
        specs.push_back(spec);
    }
    
    // STC VBR, Rn - Store from VBR
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STC";
        spec.full_name = "Store from Control Register (VBR)";
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
        spec.is_privileged = true;
        spec.category = "System";
        specs.push_back(spec);
    }
    
    // STC.L SR, @-Rn - Store SR to memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STC.L";
        spec.full_name = "Store from Control Register to Memory (SR)";
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
        spec.is_privileged = true;
        spec.category = "System";
        specs.push_back(spec);
    }
    
    // STC.L GBR, @-Rn
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STC.L";
        spec.full_name = "Store from Control Register to Memory (GBR)";
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
        specs.push_back(spec);
    }
    
    // STC.L VBR, @-Rn
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STC.L";
        spec.full_name = "Store from Control Register to Memory (VBR)";
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
        spec.is_privileged = true;
        spec.category = "System";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // LDS - Load to System Register
    // ========================================================================
    
    // LDS Rm, MACH - Load to MACH
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDS";
        spec.full_name = "Load to System Register (MACH)";
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
        specs.push_back(spec);
    }
    
    // LDS Rm, MACL - Load to MACL
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDS";
        spec.full_name = "Load to System Register (MACL)";
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
        specs.push_back(spec);
    }
    
    // LDS Rm, PR - Load to procedure register
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDS";
        spec.full_name = "Load to System Register (PR)";
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
        specs.push_back(spec);
    }
    
    // LDS.L @Rm+, MACH - Load MACH from memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDS.L";
        spec.full_name = "Load to System Register from Memory (MACH)";
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
        specs.push_back(spec);
    }
    
    // LDS.L @Rm+, MACL - Load MACL from memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDS.L";
        spec.full_name = "Load to System Register from Memory (MACL)";
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
        specs.push_back(spec);
    }
    
    // LDS.L @Rm+, PR - Load PR from memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "LDS.L";
        spec.full_name = "Load to System Register from Memory (PR)";
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
        specs.push_back(spec);
    }
    
    // ========================================================================
    // STS - Store from System Register
    // ========================================================================
    
    // STS MACH, Rn - Store from MACH
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STS";
        spec.full_name = "Store from System Register (MACH)";
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
        specs.push_back(spec);
    }
    
    // STS MACL, Rn - Store from MACL
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STS";
        spec.full_name = "Store from System Register (MACL)";
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
        specs.push_back(spec);
    }
    
    // STS PR, Rn - Store from PR
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STS";
        spec.full_name = "Store from System Register (PR)";
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
        specs.push_back(spec);
    }
    
    // STS.L MACH, @-Rn - Store MACH to memory
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STS.L";
        spec.full_name = "Store from System Register to Memory (MACH)";
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
        specs.push_back(spec);
    }
    
    // STS.L MACL, @-Rn
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STS.L";
        spec.full_name = "Store from System Register to Memory (MACL)";
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
        specs.push_back(spec);
    }
    
    // STS.L PR, @-Rn
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "STS.L";
        spec.full_name = "Store from System Register to Memory (PR)";
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
        specs.push_back(spec);
    }
    
    // ========================================================================
    // SYSTEM OPERATIONS
    // ========================================================================
    
    // SLEEP - Sleep
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
        spec.pseudocode = "/* Enter sleep mode until interrupt */";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_privileged = true;
        spec.category = "System";
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
        spec.imm_bits = 8;
        spec.imm_signed = false;
        spec.operation = "PC + 2 → Stack, SR → Stack, (VBR + imm×4) → PC";
        spec.pseudocode = "R[15] -= 4; Write_32(R[15], SR); R[15] -= 4; Write_32(R[15], PC + 2); PC = Read_32(VBR + (imm << 2));";
        spec.issue_cycles = 8; spec.latency_cycles = 8;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.writes_memory = true;
        spec.reads_memory = true;
        spec.category = "System";
        specs.push_back(spec);
    }
}

} // namespace brimir::jit

