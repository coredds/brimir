/**
 * @file sh2_spec_datatransfer_ext.cpp
 * @brief Extended Data Transfer Instructions for SH-2
 * 
 * All remaining addressing mode variants from Saturn Open SDK
 * Includes: post-increment, pre-decrement, indexed, GBR-relative
 * Reference: https://saturnopensdk.github.io/sh2.html
 */

#include "../include/sh2_spec.hpp"

namespace brimir::jit {

void AddExtendedDataTransferInstructions(std::vector<SH2InstructionSpec>& specs) {
    
    // ========================================================================
    // POST-INCREMENT ADDRESSING
    // ========================================================================
    
    // MOV.B @Rm+, Rn - Load byte with post-increment
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte with Post-Increment";
        spec.syntax = "MOV.B @Rm+, Rn";
        spec.opcode_pattern = 0x6004;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm0100";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "@Rm → sign extension → Rn, Rm + 1 → Rm";
        spec.pseudocode = "R[n] = (signed char)Read_8(R[m]); if (n != m) R[m]++;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // MOV.W @Rm+, Rn - Load word with post-increment
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word with Post-Increment";
        spec.syntax = "MOV.W @Rm+, Rn";
        spec.opcode_pattern = 0x6005;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm0101";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "@Rm → sign extension → Rn, Rm + 2 → Rm";
        spec.pseudocode = "R[n] = (signed short)Read_16(R[m]); if (n != m) R[m] += 2;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // MOV.L @Rm+, Rn - Load long with post-increment
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long with Post-Increment";
        spec.syntax = "MOV.L @Rm+, Rn";
        spec.opcode_pattern = 0x6006;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm0110";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true;
        spec.operation = "@Rm → Rn, Rm + 4 → Rm";
        spec.pseudocode = "R[n] = Read_32(R[m]); if (n != m) R[m] += 4;";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // PRE-DECREMENT ADDRESSING
    // ========================================================================
    
    // MOV.B Rm, @-Rn - Store byte with pre-decrement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte with Pre-Decrement";
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
        specs.push_back(spec);
    }
    
    // MOV.W Rm, @-Rn - Store word with pre-decrement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word with Pre-Decrement";
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
        specs.push_back(spec);
    }
    
    // MOV.L Rm, @-Rn - Store long with pre-decrement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long with Pre-Decrement";
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
        specs.push_back(spec);
    }
    
    // ========================================================================
    // R0-INDEXED ADDRESSING
    // ========================================================================
    
    // MOV.B @(R0,Rm), Rn - Load byte indexed with R0
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte Indexed with R0";
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
        specs.push_back(spec);
    }
    
    // MOV.W @(R0,Rm), Rn - Load word indexed with R0
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word Indexed with R0";
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
        specs.push_back(spec);
    }
    
    // MOV.L @(R0,Rm), Rn - Load long indexed with R0
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long Indexed with R0";
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
        specs.push_back(spec);
    }
    
    // MOV.B Rm, @(R0,Rn) - Store byte indexed with R0
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte Store Indexed with R0";
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
        specs.push_back(spec);
    }
    
    // MOV.W Rm, @(R0,Rn) - Store word indexed with R0
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word Store Indexed with R0";
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
        specs.push_back(spec);
    }
    
    // MOV.L Rm, @(R0,Rn) - Store long indexed with R0
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long Store Indexed with R0";
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
        specs.push_back(spec);
    }
    
    // ========================================================================
    // DISPLACEMENT ADDRESSING (with Rn)
    // ========================================================================
    
    // MOV.B R0, @(disp,Rn) - Store R0 with displacement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte R0 with Displacement";
        spec.syntax = "MOV.B R0, @(disp,Rn)";
        spec.opcode_pattern = 0x8000;
        spec.opcode_mask = 0xF000;
        spec.binary_format = "10000000nnnndddd";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_disp = true;
        spec.operation = "R0 → @(disp + Rn)";
        spec.pseudocode = "Write_8(R[n] + disp, R[0]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // MOV.W R0, @(disp,Rn) - Store R0 word with displacement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word R0 with Displacement";
        spec.syntax = "MOV.W R0, @(disp,Rn)";
        spec.opcode_pattern = 0x8100;
        spec.opcode_mask = 0xF000;
        spec.binary_format = "10000001nnnndddd";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_disp = true;
        spec.operation = "R0 → @(disp×2 + Rn)";
        spec.pseudocode = "Write_16(R[n] + (disp << 1), R[0]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // MOV.L Rm, @(disp,Rn) - Store long with displacement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long with Displacement";
        spec.syntax = "MOV.L Rm, @(disp,Rn)";
        spec.opcode_pattern = 0x1000;
        spec.opcode_mask = 0xF000;
        spec.binary_format = "0001nnnnmmmmdddd";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true; spec.has_disp = true;
        spec.operation = "Rm → @(disp×4 + Rn)";
        spec.pseudocode = "Write_32(R[n] + (disp << 2), R[m]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // MOV.B @(disp,Rm), R0 - Load R0 byte with displacement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte to R0 with Displacement";
        spec.syntax = "MOV.B @(disp,Rm), R0";
        spec.opcode_pattern = 0x8400;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "10000100mmmmdddd";
        spec.format = InstrFormat::MEMORY;
        spec.has_rm = true; spec.has_disp = true;
        spec.operation = "@(disp + Rm) → sign extension → R0";
        spec.pseudocode = "R[0] = (signed char)Read_8(R[m] + disp);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // MOV.W @(disp,Rm), R0 - Load R0 word with displacement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word to R0 with Displacement";
        spec.syntax = "MOV.W @(disp,Rm), R0";
        spec.opcode_pattern = 0x8500;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "10000101mmmmdddd";
        spec.format = InstrFormat::MEMORY;
        spec.has_rm = true; spec.has_disp = true;
        spec.operation = "@(disp×2 + Rm) → sign extension → R0";
        spec.pseudocode = "R[0] = (signed short)Read_16(R[m] + (disp << 1));";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // MOV.L @(disp,Rm), Rn - Load long with displacement
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long with Displacement Load";
        spec.syntax = "MOV.L @(disp,Rm), Rn";
        spec.opcode_pattern = 0x5000;
        spec.opcode_mask = 0xF000;
        spec.binary_format = "0101nnnnmmmmdddd";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true; spec.has_rm = true; spec.has_disp = true;
        spec.operation = "@(disp×4 + Rm) → Rn";
        spec.pseudocode = "R[n] = Read_32(R[m] + (disp << 2));";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // ========================================================================
    // GBR-RELATIVE ADDRESSING
    // ========================================================================
    
    // MOV.B R0, @(disp,GBR) - Store R0 byte GBR-relative
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte R0 GBR-relative";
        spec.syntax = "MOV.B R0, @(disp,GBR)";
        spec.opcode_pattern = 0xC000;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11000000dddddddd";
        spec.format = InstrFormat::MEMORY;
        spec.has_disp = true;
        spec.operation = "R0 → @(disp + GBR)";
        spec.pseudocode = "Write_8(GBR + disp, R[0]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // MOV.W R0, @(disp,GBR) - Store R0 word GBR-relative
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word R0 GBR-relative";
        spec.syntax = "MOV.W R0, @(disp,GBR)";
        spec.opcode_pattern = 0xC100;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11000001dddddddd";
        spec.format = InstrFormat::MEMORY;
        spec.has_disp = true;
        spec.operation = "R0 → @(disp×2 + GBR)";
        spec.pseudocode = "Write_16(GBR + (disp << 1), R[0]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // MOV.L R0, @(disp,GBR) - Store R0 long GBR-relative
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long R0 GBR-relative";
        spec.syntax = "MOV.L R0, @(disp,GBR)";
        spec.opcode_pattern = 0xC200;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11000010dddddddd";
        spec.format = InstrFormat::MEMORY;
        spec.has_disp = true;
        spec.operation = "R0 → @(disp×4 + GBR)";
        spec.pseudocode = "Write_32(GBR + (disp << 2), R[0]);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // MOV.B @(disp,GBR), R0 - Load R0 byte GBR-relative
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.B";
        spec.full_name = "Move Byte to R0 GBR-relative";
        spec.syntax = "MOV.B @(disp,GBR), R0";
        spec.opcode_pattern = 0xC400;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11000100dddddddd";
        spec.format = InstrFormat::MEMORY;
        spec.has_disp = true;
        spec.operation = "@(disp + GBR) → sign extension → R0";
        spec.pseudocode = "R[0] = (signed char)Read_8(GBR + disp);";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // MOV.W @(disp,GBR), R0 - Load R0 word GBR-relative
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.W";
        spec.full_name = "Move Word to R0 GBR-relative";
        spec.syntax = "MOV.W @(disp,GBR), R0";
        spec.opcode_pattern = 0xC500;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11000101dddddddd";
        spec.format = InstrFormat::MEMORY;
        spec.has_disp = true;
        spec.operation = "@(disp×2 + GBR) → sign extension → R0";
        spec.pseudocode = "R[0] = (signed short)Read_16(GBR + (disp << 1));";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
    
    // MOV.L @(disp,GBR), R0 - Load R0 long GBR-relative
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long to R0 GBR-relative";
        spec.syntax = "MOV.L @(disp,GBR), R0";
        spec.opcode_pattern = 0xC600;
        spec.opcode_mask = 0xFF00;
        spec.binary_format = "11000110dddddddd";
        spec.format = InstrFormat::MEMORY;
        spec.has_disp = true;
        spec.operation = "@(disp×4 + GBR) → R0";
        spec.pseudocode = "R[0] = Read_32(GBR + (disp << 2));";
        spec.issue_cycles = 1; spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        specs.push_back(spec);
    }
}

} // namespace brimir::jit

