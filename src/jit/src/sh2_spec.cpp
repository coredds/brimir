#include "../include/sh2_spec.hpp"
#include <algorithm>

namespace brimir::jit {

// Helper macro for defining instructions
#define INSTR(mnem, full, syntax, pattern, mask, format_str) \
    { mnem, full, syntax, pattern, mask, format_str }

std::vector<SH2InstructionSpec> SH2SpecDatabase::BuildDatabase() {
    std::vector<SH2InstructionSpec> specs;
    
    // ========================================================================
    // Data Transfer Instructions (20 instructions)
    // From Saturn Open SDK: https://saturnopensdk.github.io/
    // ========================================================================
    
    // MOV Rm, Rn - Move register to register
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV";
        spec.full_name = "Move";
        spec.syntax = "MOV Rm, Rn";
        spec.opcode_pattern = 0x6003;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm0011";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true;
        spec.has_rm = true;
        spec.operation = "Rm → Rn";
        spec.pseudocode = "R[n] = R[m];";
        spec.issue_cycles = 1;
        spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Data Transfer";
        spec.tags = {"move", "register"};
        specs.push_back(spec);
    }
    
    // MOV #imm, Rn - Move immediate to register
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV";
        spec.full_name = "Move Immediate";
        spec.syntax = "MOV #imm, Rn";
        spec.opcode_pattern = 0xE000;
        spec.opcode_mask = 0xF000;
        spec.binary_format = "1110nnnniiiiiiii";
        spec.format = InstrFormat::REG_IMM;
        spec.has_rn = true;
        spec.has_imm = true;
        spec.imm_bits = 8;
        spec.imm_signed = true;
        spec.operation = "#imm → sign extension → Rn";
        spec.pseudocode = "R[n] = (signed char)imm;";
        spec.issue_cycles = 1;
        spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Data Transfer";
        spec.tags = {"move", "immediate"};
        specs.push_back(spec);
    }
    
    // MOV.L @Rm, Rn - Load long word
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long";
        spec.syntax = "MOV.L @Rm, Rn";
        spec.opcode_pattern = 0x6002;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm0010";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true;
        spec.has_rm = true;
        spec.operation = "(Rm) → Rn";
        spec.pseudocode = "R[n] = Read_32(R[m]);";
        spec.issue_cycles = 1;
        spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.reads_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"load", "memory"};
        specs.push_back(spec);
    }
    
    // MOV.L Rm, @Rn - Store long word
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "MOV.L";
        spec.full_name = "Move Long";
        spec.syntax = "MOV.L Rm, @Rn";
        spec.opcode_pattern = 0x2002;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm0010";
        spec.format = InstrFormat::MEMORY;
        spec.has_rn = true;
        spec.has_rm = true;
        spec.operation = "Rm → (Rn)";
        spec.pseudocode = "Write_32(R[n], R[m]);";
        spec.issue_cycles = 1;
        spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.writes_memory = true;
        spec.category = "Data Transfer";
        spec.tags = {"store", "memory"};
        specs.push_back(spec);
    }
    
    // ========================================================================
    // Arithmetic Instructions (15 instructions)
    // ========================================================================
    
    // ADD Rm, Rn
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "ADD";
        spec.full_name = "Add";
        spec.syntax = "ADD Rm, Rn";
        spec.opcode_pattern = 0x300C;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm1100";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true;
        spec.has_rm = true;
        spec.operation = "Rn + Rm → Rn";
        spec.pseudocode = "R[n] += R[m];";
        spec.issue_cycles = 1;
        spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        spec.tags = {"add", "arithmetic"};
        specs.push_back(spec);
    }
    
    // ADD #imm, Rn
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "ADD";
        spec.full_name = "Add Immediate";
        spec.syntax = "ADD #imm, Rn";
        spec.opcode_pattern = 0x7000;
        spec.opcode_mask = 0xF000;
        spec.binary_format = "0111nnnniiiiiiii";
        spec.format = InstrFormat::REG_IMM;
        spec.has_rn = true;
        spec.has_imm = true;
        spec.imm_bits = 8;
        spec.imm_signed = true;
        spec.operation = "Rn + #imm → Rn";
        spec.pseudocode = "R[n] += (signed char)imm;";
        spec.issue_cycles = 1;
        spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        spec.tags = {"add", "immediate"};
        specs.push_back(spec);
    }
    
    // ADDC Rm, Rn - Add with carry
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "ADDC";
        spec.full_name = "Add with Carry";
        spec.syntax = "ADDC Rm, Rn";
        spec.opcode_pattern = 0x300E;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm1110";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true;
        spec.has_rm = true;
        spec.operation = "Rn + Rm + T → Rn, carry → T";
        spec.pseudocode = "unsigned long tmp1 = R[n] + R[m];\n"
                          "unsigned long tmp0 = R[n];\n"
                          "R[n] = tmp1 + T;\n"
                          "if (tmp0 > tmp1) T = 1;\n"
                          "else if (tmp1 > R[n]) T = 1;\n"
                          "else T = 0;";
        spec.issue_cycles = 1;
        spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::CARRY;
        spec.category = "Arithmetic";
        spec.tags = {"add", "carry", "arithmetic"};
        specs.push_back(spec);
    }
    
    // SUB Rm, Rn
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SUB";
        spec.full_name = "Subtract";
        spec.syntax = "SUB Rm, Rn";
        spec.opcode_pattern = 0x3008;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm1000";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true;
        spec.has_rm = true;
        spec.operation = "Rn - Rm → Rn";
        spec.pseudocode = "R[n] -= R[m];";
        spec.issue_cycles = 1;
        spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Arithmetic";
        spec.tags = {"sub", "arithmetic"};
        specs.push_back(spec);
    }
    
    // SUBC Rm, Rn - Subtract with carry
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "SUBC";
        spec.full_name = "Subtract with Carry";
        spec.syntax = "SUBC Rm, Rn";
        spec.opcode_pattern = 0x300A;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0011nnnnmmmm1010";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true;
        spec.has_rm = true;
        spec.operation = "Rn - Rm - T → Rn, borrow → T";
        spec.pseudocode = "unsigned long tmp1 = R[n] - R[m];\n"
                          "unsigned long tmp0 = R[n];\n"
                          "R[n] = tmp1 - T;\n"
                          "if (tmp0 < tmp1) T = 1;\n"
                          "else if (tmp1 < R[n]) T = 1;\n"
                          "else T = 0;";
        spec.issue_cycles = 1;
        spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::CARRY;
        spec.category = "Arithmetic";
        spec.tags = {"sub", "carry", "arithmetic"};
        specs.push_back(spec);
    }
    
    // ========================================================================
    // Logic Instructions (8 instructions)
    // ========================================================================
    
    // AND Rm, Rn
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "AND";
        spec.full_name = "AND logical";
        spec.syntax = "AND Rm, Rn";
        spec.opcode_pattern = 0x2009;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm1001";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true;
        spec.has_rm = true;
        spec.operation = "Rn & Rm → Rn";
        spec.pseudocode = "R[n] &= R[m];";
        spec.issue_cycles = 1;
        spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Logic";
        spec.tags = {"and", "logic"};
        specs.push_back(spec);
    }
    
    // OR Rm, Rn
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "OR";
        spec.full_name = "OR logical";
        spec.syntax = "OR Rm, Rn";
        spec.opcode_pattern = 0x200B;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm1011";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true;
        spec.has_rm = true;
        spec.operation = "Rn | Rm → Rn";
        spec.pseudocode = "R[n] |= R[m];";
        spec.issue_cycles = 1;
        spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Logic";
        spec.tags = {"or", "logic"};
        specs.push_back(spec);
    }
    
    // XOR Rm, Rn
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "XOR";
        spec.full_name = "XOR logical";
        spec.syntax = "XOR Rm, Rn";
        spec.opcode_pattern = 0x200A;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0010nnnnmmmm1010";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true;
        spec.has_rm = true;
        spec.operation = "Rn ^ Rm → Rn";
        spec.pseudocode = "R[n] ^= R[m];";
        spec.issue_cycles = 1;
        spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Logic";
        spec.tags = {"xor", "logic"};
        specs.push_back(spec);
    }
    
    // NOT Rm, Rn
    {
        SH2InstructionSpec spec;
        spec.mnemonic = "NOT";
        spec.full_name = "NOT logical";
        spec.syntax = "NOT Rm, Rn";
        spec.opcode_pattern = 0x6007;
        spec.opcode_mask = 0xF00F;
        spec.binary_format = "0110nnnnmmmm0111";
        spec.format = InstrFormat::TWO_REG;
        spec.has_rn = true;
        spec.has_rm = true;
        spec.operation = "~Rm → Rn";
        spec.pseudocode = "R[n] = ~R[m];";
        spec.issue_cycles = 1;
        spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "Logic";
        spec.tags = {"not", "logic"};
        specs.push_back(spec);
    }
    
    // ========================================================================
    // Branch Instructions (11 instructions)
    // ========================================================================
    
    // BRA disp - Branch always
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
        spec.has_imm = true;
        spec.imm_bits = 12;
        spec.imm_signed = true;
        spec.operation = "PC + 4 + disp × 2 → PC";
        spec.pseudocode = "int disp = (instr & 0xFFF);\n"
                          "if (disp & 0x800) disp |= 0xFFFFF000;\n"
                          "PC = PC + 4 + (disp << 1);";
        spec.issue_cycles = 2;
        spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        spec.tags = {"branch", "unconditional"};
        specs.push_back(spec);
    }
    
    // BT disp - Branch if true
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
        spec.has_imm = true;
        spec.imm_bits = 8;
        spec.imm_signed = true;
        spec.operation = "If T = 1: PC + 4 + disp × 2 → PC";
        spec.pseudocode = "if (T) {\n"
                          "  int disp = (signed char)(instr & 0xFF);\n"
                          "  PC = PC + 4 + (disp << 1);\n"
                          "}";
        spec.issue_cycles = 3;  // Taken
        spec.latency_cycles = 1; // Not taken
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        spec.tags = {"branch", "conditional"};
        specs.push_back(spec);
    }
    
    // BF disp - Branch if false
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
        spec.has_imm = true;
        spec.imm_bits = 8;
        spec.imm_signed = true;
        spec.operation = "If T = 0: PC + 4 + disp × 2 → PC";
        spec.pseudocode = "if (!T) {\n"
                          "  int disp = (signed char)(instr & 0xFF);\n"
                          "  PC = PC + 4 + (disp << 1);\n"
                          "}";
        spec.issue_cycles = 3;  // Taken
        spec.latency_cycles = 1; // Not taken
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        spec.tags = {"branch", "conditional"};
        specs.push_back(spec);
    }
    
    // JMP @Rm - Jump to register
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
        spec.issue_cycles = 2;
        spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        spec.tags = {"jump", "indirect"};
        specs.push_back(spec);
    }
    
    // RTS - Return from subroutine
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
        spec.issue_cycles = 2;
        spec.latency_cycles = 2;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.is_branch = true;
        spec.has_delay_slot = true;
        spec.category = "Branch";
        spec.tags = {"return", "subroutine"};
        specs.push_back(spec);
    }
    
    // ========================================================================
    // System Instructions
    // ========================================================================
    
    // NOP - No operation
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
        spec.issue_cycles = 1;
        spec.latency_cycles = 1;
        spec.t_bit_effect = TBitEffect::UNCHANGED;
        spec.category = "System";
        spec.tags = {"nop", "system"};
        specs.push_back(spec);
    }
    
    // TODO: Add remaining 115+ instructions from Saturn Open SDK
    // This is a starter set of 18 most common instructions
    
    return specs;
}

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
    
    if (it != specs.end()) {
        return *it;
    }
    return std::nullopt;
}

std::optional<SH2InstructionSpec> SH2SpecDatabase::Decode(uint16 instr) {
    const auto& specs = GetAllInstructions();
    for (const auto& spec : specs) {
        if (spec.Matches(instr)) {
            return spec;
        }
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

