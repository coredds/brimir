#pragma once

/**
 * @file jit_compiler.hpp
 * @brief SH-2 Just-In-Time Compiler Interface
 * 
 * This module implements a dynamic recompiler for the Hitachi SH-2 CPU.
 * It translates SH-2 instructions to native code (x86-64, ARM64) at runtime
 * for improved performance on target platforms.
 * 
 * CRITICAL: The JIT supplements, not replaces, the interpreter.
 * The interpreter is always available as a fallback and remains the
 * reference implementation.
 * 
 * @note This is Phase 0 - Planning stage. Implementation begins after
 *       test infrastructure is complete.
 * 
 * @see docs/SH2_JIT_EVALUATION.md for design rationale
 * @see docs/SH2_JIT_ROADMAP.md for implementation plan
 */

#include <cstdint>
#include <memory>

namespace brimir::jit {

// Forward declarations
class JITCache;
class JITBackend;
struct IRBlock;

/**
 * @brief JIT Compiler for SH-2 CPU
 * 
 * Translates SH-2 machine code to native code via intermediate representation (IR).
 * 
 * Architecture:
 * 1. Fetch SH-2 instructions from PC until branch/block end
 * 2. Translate to platform-independent IR
 * 3. Pass IR to backend for native code generation
 * 4. Cache compiled block for reuse
 * 5. Execute native code
 * 
 * Usage:
 *   auto block = compiler.Compile(pc, sh2_state);
 *   uint64 cycles = block->Execute(sh2_state);
 */
class JITCompiler {
public:
    /**
     * @brief Construct a JIT compiler
     * @param backend Code generation backend (x86-64, ARM64, etc.)
     * @param cache Compiled block cache
     */
    JITCompiler(std::unique_ptr<JITBackend> backend, std::shared_ptr<JITCache> cache);
    
    ~JITCompiler();
    
    /**
     * @brief Compile a basic block starting at the given PC
     * @param pc Program counter (SH-2 address)
     * @return Compiled block ready for execution, or nullptr on failure
     * 
     * @note Falls back to interpreter if compilation fails
     * @note Cached blocks are returned from cache, not recompiled
     */
    // TODO: Phase 2 - Implement compilation
    // struct CompiledBlock* Compile(uint32_t pc);
    
    /**
     * @brief Invalidate compiled blocks in a given address range
     * @param start Start address (inclusive)
     * @param end End address (exclusive)
     * 
     * @note Called when self-modifying code is detected or cache is purged
     */
    // TODO: Phase 2 - Implement invalidation
    // void Invalidate(uint32_t start, uint32_t end);
    
    /**
     * @brief Get compilation statistics
     */
    // TODO: Phase 3 - Add statistics tracking
    // struct Stats {
    //     uint64_t blocks_compiled;
    //     uint64_t cache_hits;
    //     uint64_t cache_misses;
    //     uint64_t invalidations;
    // };
    // Stats GetStats() const;

private:
    // TODO: Phase 2 - Add member variables
    // std::unique_ptr<JITBackend> m_backend;
    // std::shared_ptr<JITCache> m_cache;
    
    // TODO: Phase 2 - Implement IR translation
    // IRBlock* TranslateToIR(uint32_t pc);
    
    // TODO: Phase 3/4 - Backend code generation
    // CompiledBlock* GenerateNativeCode(const IRBlock& ir);
};

/**
 * @brief Configuration options for JIT
 */
struct JITConfig {
    bool enabled = true;              ///< Master JIT enable/disable
    bool debug_mode = false;          ///< Force interpreter for debugging
    uint32_t max_block_size = 256;    ///< Max instructions per compiled block
    uint32_t cache_size_mb = 16;      ///< Code cache size in megabytes
    
    // TODO: Phase 5 - Per-game blacklist
    // std::vector<std::string> blacklisted_games;
};

} // namespace brimir::jit

