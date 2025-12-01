#pragma once

/**
 * @file brimir_sh2_wrapper.hpp
 * @brief Isolated SH-2 wrapper for JIT testing
 * 
 * Provides a minimal, controlled environment for executing SH-2 code
 * via Ymir's interpreter. Used by the JIT test framework to validate
 * compiled code against the reference implementation.
 * 
 * This wrapper:
 * - Creates an isolated SH-2 instance with minimal dependencies
 * - Provides simple memory management (RAM only, no hardware)
 * - Captures full CPU state for comparison
 * - Executes single instructions or blocks
 */

#include <brimir/core/types.hpp>
#include <cstdint>
#include <array>
#include <memory>
#include <vector>

// Forward declarations to avoid pulling in all of Brimir
namespace brimir {
    namespace core { class Scheduler; }
    namespace sys { 
        // SH2Bus is actually a type alias for Bus<27, 16>, so we can't forward declare it
        // We'll need to include the header in the implementation file
    }
    namespace sh2 { class SH2; }
}

namespace jit {

/**
 * @brief SH-2 CPU state snapshot
 * 
 * Captures all architecturally visible state of the SH-2 CPU.
 * Used for comparing interpreter vs JIT execution results.
 */
struct SH2StateSnapshot {
    // General-purpose registers
    std::array<uint32_t, 16> R;
    
    // Control registers
    uint32_t PC;
    uint32_t PR;
    uint32_t GBR;
    uint32_t VBR;
    
    // MAC registers
    uint32_t MACH;
    uint32_t MACL;
    
    // Status register fields
    uint32_t SR;        // Full SR value
    bool T;             // T-bit (test flag)
    bool S;             // S-bit (saturation)
    uint8_t ILevel;     // Interrupt level (4 bits)
    bool Q;             // Q-bit (quotient)
    bool M;             // M-bit (modulus)
    
    // Execution state
    bool inDelaySlot;
    uint32_t delaySlotTarget;
    
    // Cycle count (CRITICAL for accuracy!)
    uint64_t cycles;
    
    /**
     * @brief Compare two state snapshots
     * @return true if states are identical
     */
    bool operator==(const SH2StateSnapshot& other) const;
    
    /**
     * @brief Get human-readable diff between states
     * @return String describing differences, empty if identical
     */
    std::string Diff(const SH2StateSnapshot& other) const;
};

/**
 * @brief Minimal isolated SH-2 environment for testing
 * 
 * Wraps Ymir's SH-2 interpreter in a controlled environment:
 * - 16 MB RAM (0x00000000 - 0x00FFFFFF)
 * - No peripherals (minimal bus configuration)
 * - No interrupts (for deterministic testing)
 * - Cycle-accurate execution
 */
class YmirSH2Wrapper {
public:
    /**
     * @brief Create an isolated SH-2 test instance
     * @param ramSize Size of RAM in bytes (default 16 MB)
     */
    YmirSH2Wrapper(size_t ramSize = 16 * 1024 * 1024);
    
    ~YmirSH2Wrapper();
    
    // Disable copy/move (contains unique hardware instances)
    YmirSH2Wrapper(const YmirSH2Wrapper&) = delete;
    YmirSH2Wrapper& operator=(const YmirSH2Wrapper&) = delete;
    
    /**
     * @brief Reset the CPU to initial state
     * @param pc Starting program counter (default 0x00000000)
     */
    void Reset(uint32_t pc = 0x00000000);
    
    /**
     * @brief Write memory (for loading test code/data)
     * @param address Physical address
     * @param data Data to write
     * @param size Number of bytes
     */
    void WriteMemory(uint32_t address, const void* data, size_t size);
    
    /**
     * @brief Read memory (for verification)
     * @param address Physical address
     * @param data Output buffer
     * @param size Number of bytes
     */
    void ReadMemory(uint32_t address, void* data, size_t size);
    
    /**
     * @brief Write a single word of code
     * @param address Address to write
     * @param instruction 16-bit SH-2 instruction
     */
    void WriteInstruction(uint32_t address, uint16_t instruction);
    
    /**
     * @brief Set CPU registers (for test setup)
     */
    void SetRegister(uint8_t reg, uint32_t value);
    void SetPC(uint32_t value);
    void SetPR(uint32_t value);
    void SetGBR(uint32_t value);
    void SetVBR(uint32_t value);
    void SetMACH(uint32_t value);
    void SetMACL(uint32_t value);
    void SetSR(uint32_t value);
    void SetTBit(bool value);
    
    /**
     * @brief Execute a single instruction
     * @return Number of cycles executed
     */
    uint64_t ExecuteInstruction();
    
    /**
     * @brief Execute N instructions
     * @param count Number of instructions to execute
     * @return Total cycles executed
     */
    uint64_t ExecuteInstructions(size_t count);
    
    /**
     * @brief Execute for N cycles
     * @param cycles Number of cycles to execute
     * @return Actual cycles executed (may be slightly more due to instruction boundaries)
     */
    uint64_t ExecuteCycles(uint64_t cycles);
    
    /**
     * @brief Capture current CPU state
     * @return Snapshot of all CPU state
     */
    SH2StateSnapshot CaptureState() const;
    
    /**
     * @brief Restore CPU state from snapshot
     * @param state State to restore
     */
    void RestoreState(const SH2StateSnapshot& state);
    
    /**
     * @brief Get current cycle count
     */
    uint64_t GetCycles() const { return m_cycleCount; }
    
    /**
     * @brief Reset cycle counter
     */
    void ResetCycles() { m_cycleCount = 0; }
    
private:
    // Implementation details (opaque pointers to avoid Ymir dependencies in header)
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    
    // RAM storage
    std::vector<uint8_t> m_ram;
    
    // Cycle tracking
    uint64_t m_cycleCount;
};

} // namespace jit

