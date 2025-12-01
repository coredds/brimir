#pragma once

/**
 * @file jit_sh2_accessor.hpp
 * @brief Helper to access SH-2 internal state for JIT validation
 * 
 * This provides direct access to SH-2 registers without going through
 * the full emulation loop. Used only for JIT testing/validation.
 */

#include <brimir/hw/sh2/sh2.hpp>
#include "jit_validator.hpp"

namespace brimir::jit {

/**
 * @brief Accessor for SH-2 internal state
 * 
 * This class provides controlled access to SH-2 internal registers
 * for the purpose of JIT validation. It's a friend of SH2 class.
 */
class SH2StateAccessor {
public:
    /**
     * @brief Extract complete state from SH-2 instance
     */
    static SH2State GetState(const brimir::sh2::SH2& sh2) {
        SH2State state;
        
        // Access registers via public interface
        // Note: SH2 class exposes registers through nested structs
        // We need to access m_sh2 member which contains the actual registers
        
        // For now, create a simplified accessor
        // TODO: This requires either:
        //  1. Friend access to SH2 class
        //  2. Public getter methods
        //  3. Reflection-based access
        
        // Placeholder - will be implemented properly
        state.PC = 0;
        state.cycles = 0;
        
        return state;
    }
    
    /**
     * @brief Set SH-2 state from SH2State
     */
    static void SetState(brimir::sh2::SH2& sh2, const SH2State& state) {
        // Placeholder - requires proper access to SH2 internals
    }
};

} // namespace brimir::jit

