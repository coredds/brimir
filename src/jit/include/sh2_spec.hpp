#pragma once

/**
 * @file sh2_spec.hpp
 * @brief SH-2 Instruction Specification Database
 * 
 * Contains complete SH-2 instruction set specification from Saturn Open SDK.
 * This is the SOURCE OF TRUTH for JIT implementation and validation.
 * 
 * Reference: https://saturnopensdk.github.io/
 */

#include <brimir/core/types.hpp>
#include <string>
#include <vector>
#include <optional>

namespace brimir::jit {

using namespace brimir;

/// @brief Instruction format type
enum class InstrFormat {
    ZERO_OP,        ///< No operands (NOP, RTS)
    ONE_REG,        ///< Single register (DT Rn)
    TWO_REG,        ///< Two registers (MOV Rm, Rn)
    REG_IMM,        ///< Register + immediate (ADD #imm, Rn)
    REG_DISP,       ///< Register + displacement (MOV.L @(disp, Rm), Rn)
    BRANCH,         ///< Branch with displacement
    BRANCH_REG,     ///< Branch to register (JMP @Rm)
    MEMORY,         ///< Memory operation
    SYSTEM          ///< System operation
};

/// @brief T-bit behavior
enum class TBitEffect {
    UNCHANGED,      ///< T-bit not affected
    RESULT,         ///< T = result of operation
    CARRY,          ///< T = carry flag
    OVERFLOW,       ///< T = overflow flag
    COMPARISON,     ///< T = comparison result
    SET,            ///< T = 1
    CLEAR           ///< T = 0
};

/// @brief Complete instruction specification from Saturn Open SDK
struct SH2InstructionSpec {
    // Identification
    std::string mnemonic;       ///< "ADD", "MOV", etc.
    std::string full_name;      ///< "Add", "Move", etc.
    std::string syntax;         ///< "ADD Rm, Rn"
    
    // Encoding (from Saturn Open SDK binary format)
    uint16 opcode_pattern;      ///< e.g., 0x300C for ADD
    uint16 opcode_mask;         ///< e.g., 0xF00F for ADD
    std::string binary_format;  ///< e.g., "0011nnnnmmmm1100"
    
    // Operands
    InstrFormat format;
    bool has_rn = false;        ///< Has Rn field (dest register)
    bool has_rm = false;        ///< Has Rm field (source register)
    bool has_imm = false;       ///< Has immediate field
    bool has_disp = false;      ///< Has displacement field
    uint8 imm_bits = 0;         ///< Size of immediate (4, 8, 12 bits)
    bool imm_signed = false;    ///< Is immediate sign-extended?
    
    // Operation (from Saturn Open SDK pseudocode)
    std::string operation;      ///< "Rn + Rm → Rn"
    std::string pseudocode;     ///< Full C pseudocode
    
    // Timing (from Saturn Open SDK)
    uint8 issue_cycles;         ///< Issue cycles
    uint8 latency_cycles;       ///< Latency cycles
    
    // Flags
    TBitEffect t_bit_effect;    ///< How T-bit is affected
    bool affects_q = false;     ///< Affects Q-bit (DIV)
    bool affects_m = false;     ///< Affects M-bit (DIV)
    bool affects_s = false;     ///< Affects S-bit (MAC)
    
    // Side Effects
    bool reads_memory = false;
    bool writes_memory = false;
    bool is_branch = false;
    bool is_privileged = false;
    bool has_delay_slot = false;
    
    // Categories
    std::string category;       ///< "Arithmetic", "Branch", etc.
    std::vector<std::string> tags; ///< Additional tags
    
    // Validation
    bool implemented = false;   ///< Is this implemented in JIT?
    bool tested = false;        ///< Does it have passing tests?
    
    /**
     * @brief Check if an instruction matches this spec
     * @param instr 16-bit instruction
     * @return true if instruction matches pattern
     */
    bool Matches(uint16 instr) const {
        return (instr & opcode_mask) == opcode_pattern;
    }
    
    /**
     * @brief Extract Rn field from instruction
     */
    uint8 ExtractRn(uint16 instr) const {
        if (!has_rn) return 0;
        return (instr >> 8) & 0xF;
    }
    
    /**
     * @brief Extract Rm field from instruction
     */
    uint8 ExtractRm(uint16 instr) const {
        if (!has_rm) return 0;
        return (instr >> 4) & 0xF;
    }
    
    /**
     * @brief Extract immediate field from instruction
     */
    sint32 ExtractImm(uint16 instr) const {
        if (!has_imm) return 0;
        
        sint32 imm = 0;
        switch (imm_bits) {
            case 4:
                imm = instr & 0xF;
                if (imm_signed && (imm & 0x8)) imm |= 0xFFFFFFF0;
                break;
            case 8:
                imm = instr & 0xFF;
                if (imm_signed && (imm & 0x80)) imm |= 0xFFFFFF00;
                break;
            case 12:
                imm = instr & 0xFFF;
                if (imm_signed && (imm & 0x800)) imm |= 0xFFFFF000;
                break;
        }
        return imm;
    }
};

/// @brief SH-2 Instruction Set Database
class SH2SpecDatabase {
public:
    /**
     * @brief Get the complete SH-2 instruction set
     * @return All 133 instructions from Saturn Open SDK
     */
    static const std::vector<SH2InstructionSpec>& GetAllInstructions();
    
    /**
     * @brief Lookup instruction spec by mnemonic
     * @param mnemonic Instruction name (e.g., "ADD")
     * @return Spec if found, nullopt otherwise
     */
    static std::optional<SH2InstructionSpec> GetByMnemonic(const std::string& mnemonic);
    
    /**
     * @brief Decode instruction and find matching spec
     * @param instr 16-bit instruction
     * @return Spec if found, nullopt otherwise
     */
    static std::optional<SH2InstructionSpec> Decode(uint16 instr);
    
    /**
     * @brief Get instructions by category
     * @param category Category name
     * @return All instructions in category
     */
    static std::vector<SH2InstructionSpec> GetByCategory(const std::string& category);
    
    /**
     * @brief Get statistics
     */
    struct Stats {
        size_t total_instructions = 0;
        size_t implemented = 0;
        size_t tested = 0;
        
        double ImplementationProgress() const {
            return total_instructions > 0 ? (100.0 * implemented / total_instructions) : 0.0;
        }
        
        double TestProgress() const {
            return total_instructions > 0 ? (100.0 * tested / total_instructions) : 0.0;
        }
    };
    
    static Stats GetStats();
    
private:
    static std::vector<SH2InstructionSpec> BuildDatabase();
};

} // namespace brimir::jit

