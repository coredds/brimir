#pragma once

/**
 * @file jit_block_analyzer.hpp
 * @brief Analyzes SH-2 code and builds IR basic blocks
 * 
 * Identifies basic blocks, translates SH-2 instructions to IR,
 * and performs initial analysis for optimization.
 */

#include "jit_ir.hpp"
#include <brimir/core/types.hpp>
#include <brimir/sys/bus.hpp>
#include <functional>
#include <memory>

namespace brimir::jit {

using namespace brimir;

/// @brief Function to read SH-2 memory
using MemoryReadFn = std::function<uint16(uint32 address)>;

/// @brief SH-2 to IR translator
class BlockAnalyzer {
public:
    /**
     * @brief Construct a block analyzer
     * @param read_fn Function to read SH-2 memory
     */
    explicit BlockAnalyzer(MemoryReadFn read_fn);
    
    /**
     * @brief Analyze and build an IR block starting at PC
     * @param start_pc Starting program counter
     * @param max_instructions Maximum instructions per block (default 100)
     * @return IR block ready for compilation
     */
    std::unique_ptr<IRBlock> Analyze(uint32 start_pc, size_t max_instructions = 100);
    
    /**
     * @brief Check if address is a valid block start
     * @param addr Address to check
     * @return true if this is a valid block entry point
     */
    bool IsBlockStart(uint32 addr) const;
    
    /**
     * @brief Perform live register analysis on a block
     * @param block IR block to analyze
     * @return Live register information
     */
    LiveRanges AnalyzeLiveness(const IRBlock& block) const;
    
private:
    MemoryReadFn read_memory_;
    
    /**
     * @brief Fetch and decode a single SH-2 instruction
     * @param addr Address to fetch from
     * @param instr [out] Decoded instruction
     * @return true if decode successful
     */
    bool FetchAndDecode(uint32 addr, uint16& raw_instr);
    
    /**
     * @brief Translate SH-2 instruction to IR
     * @param addr SH-2 address
     * @param instr Raw 16-bit instruction
     * @param block [out] Block to add IR instructions to
     * @return true if translation successful
     */
    bool TranslateToIR(uint32 addr, uint16 instr, IRBlock& block);
    
    /**
     * @brief Check if instruction is a block terminator
     * @param instr Raw instruction
     * @return true if this ends a basic block
     */
    bool IsBlockTerminator(uint16 instr) const;
    
    /**
     * @brief Extract register field 'n' from instruction
     */
    static inline uint8 GetRn(uint16 instr) {
        return (instr >> 8) & 0xF;
    }
    
    /**
     * @brief Extract register field 'm' from instruction
     */
    static inline uint8 GetRm(uint16 instr) {
        return (instr >> 4) & 0xF;
    }
    
    /**
     * @brief Extract 8-bit immediate (sign-extended)
     */
    static inline sint32 GetImm8(uint16 instr) {
        return static_cast<sint8>(instr & 0xFF);
    }
    
    /**
     * @brief Extract 8-bit immediate (unsigned)
     */
    static inline uint8 GetImm8U(uint16 instr) {
        return instr & 0xFF;
    }
    
    /**
     * @brief Extract 12-bit displacement (sign-extended)
     */
    static inline sint32 GetDisp12(uint16 instr) {
        sint32 disp = instr & 0xFFF;
        if (disp & 0x800) disp |= 0xFFFFF000; // Sign extend
        return disp;
    }
};

/// @brief Block cache for compiled blocks
class BlockCache {
public:
    /**
     * @brief Lookup a compiled block
     * @param addr SH-2 address
     * @return Cached IR block or nullptr if not found
     */
    IRBlock* Lookup(uint32 addr);
    
    /**
     * @brief Insert a block into cache
     * @param block Block to cache (takes ownership)
     */
    void Insert(std::unique_ptr<IRBlock> block);
    
    /**
     * @brief Invalidate blocks in address range
     * @param start Start address (inclusive)
     * @param end End address (exclusive)
     */
    void Invalidate(uint32 start, uint32 end);
    
    /**
     * @brief Clear entire cache
     */
    void Clear();
    
    /**
     * @brief Get cache statistics
     */
    struct Stats {
        size_t block_count = 0;
        size_t total_instructions = 0;
        size_t hits = 0;
        size_t misses = 0;
    };
    
    Stats GetStats() const;
    
private:
    // Simple hash map for now (can optimize later with better structure)
    std::unordered_map<uint32, std::unique_ptr<IRBlock>> blocks_;
    Stats stats_;
};

} // namespace brimir::jit

