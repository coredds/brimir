#pragma once

/**
 * @file jit_test_framework.hpp
 * @brief SH-2 JIT Test Framework - Dual Execution Validation
 * 
 * This framework enables comprehensive testing of the JIT compiler by:
 * 1. Running the same code on both interpreter and JIT
 * 2. Comparing full CPU state after execution
 * 3. Detecting any divergence in behavior
 * 
 * THE GOLDEN RULE: "The interpreter is always right. If JIT disagrees, JIT is wrong."
 * 
 * @author Brimir JIT Team
 * @date 2025-11-29
 */

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace brimir::jit::test {

// -----------------------------------------------------------------------------
// SH-2 CPU State Snapshot
// -----------------------------------------------------------------------------

/**
 * @brief Complete snapshot of SH-2 CPU state for comparison
 * 
 * Captures all registers, flags, and internal state that must match
 * between interpreter and JIT execution.
 */
struct SH2State {
    // General-purpose registers
    uint32_t R[16];         ///< R0-R15 (R15 = SP)
    
    // Control registers
    uint32_t PC;            ///< Program Counter
    uint32_t PR;            ///< Procedure Register (return address)
    uint32_t GBR;           ///< Global Base Register
    uint32_t VBR;           ///< Vector Base Register
    
    // MAC registers
    uint32_t MACH;          ///< MAC high 32 bits
    uint32_t MACL;          ///< MAC low 32 bits
    
    // Status Register components
    uint32_t SR;            ///< Full Status Register
    bool T;                 ///< T-bit (separate for convenience)
    
    // Execution state
    uint64_t cycles;        ///< Cycle count executed
    bool delay_slot;        ///< Currently in delay slot?
    uint32_t delay_target;  ///< Delay slot target PC
    
    /**
     * @brief Compare two states for equality
     * @return true if states match exactly, false otherwise
     */
    bool operator==(const SH2State& other) const;
    
    /**
     * @brief Get human-readable diff between states
     * @param other State to compare against
     * @return String describing differences, empty if equal
     */
    std::string GetDiff(const SH2State& other) const;
    
    /**
     * @brief Dump state to string for debugging
     */
    std::string ToString() const;
};

// -----------------------------------------------------------------------------
// Test Result
// -----------------------------------------------------------------------------

/**
 * @brief Result of a single test execution
 */
struct TestResult {
    bool passed;                    ///< Did test pass?
    std::string test_name;          ///< Name/ID of test
    std::string failure_reason;     ///< Why did it fail? (if failed)
    
    SH2State interpreter_final;     ///< Final interpreter state
    SH2State jit_final;             ///< Final JIT state
    
    uint64_t interpreter_cycles;    ///< Cycles taken by interpreter
    uint64_t jit_cycles;            ///< Cycles taken by JIT
    
    /**
     * @brief Get detailed failure report
     */
    std::string GetReport() const;
};

// -----------------------------------------------------------------------------
// Test Case Definition
// -----------------------------------------------------------------------------

/**
 * @brief Definition of a single test case
 */
struct TestCase {
    std::string name;               ///< Test name/identifier
    std::string description;        ///< What this test validates
    
    SH2State initial_state;         ///< Starting CPU state
    std::vector<uint16_t> code;     ///< SH-2 instructions to execute
    
    // Optional memory setup
    struct MemoryRegion {
        uint32_t address;
        std::vector<uint8_t> data;
    };
    std::vector<MemoryRegion> memory_setup;  ///< Memory to initialize
    
    // Expected results (optional - can just compare interpreter vs JIT)
    std::optional<SH2State> expected_final;  ///< Expected final state
    std::optional<uint64_t> expected_cycles; ///< Expected cycle count
    
    /**
     * @brief Validation function (optional custom validation)
     * @param interp Interpreter final state
     * @param jit JIT final state
     * @return Empty string if valid, error message if invalid
     */
    using ValidatorFunc = std::function<std::string(const SH2State&, const SH2State&)>;
    std::optional<ValidatorFunc> custom_validator;
};

// -----------------------------------------------------------------------------
// Dual-Execution Test Harness
// -----------------------------------------------------------------------------

/**
 * @brief Main test harness for dual-execution validation
 * 
 * Executes test cases on both interpreter and JIT, comparing results.
 */
class DualExecutionHarness {
public:
    DualExecutionHarness();
    ~DualExecutionHarness();
    
    /**
     * @brief Run a single test case
     * @param test Test case to execute
     * @return Test result with pass/fail and diagnostics
     */
    TestResult RunTest(const TestCase& test);
    
    /**
     * @brief Run multiple test cases
     * @param tests Vector of test cases
     * @return Vector of results (same order as input)
     */
    std::vector<TestResult> RunTests(const std::vector<TestCase>& tests);
    
    /**
     * @brief Run tests and generate summary report
     * @param tests Test cases to run
     * @return Summary string with pass/fail counts and details
     */
    std::string RunTestsWithReport(const std::vector<TestCase>& tests);
    
    /**
     * @brief Enable/disable JIT for testing
     * @param enabled If false, both paths use interpreter (sanity check)
     */
    void SetJITEnabled(bool enabled) { m_jit_enabled = enabled; }
    
    /**
     * @brief Enable verbose output for debugging
     */
    void SetVerbose(bool verbose) { m_verbose = verbose; }

private:
    bool m_jit_enabled = true;
    bool m_verbose = false;
    
    /**
     * @brief Execute code on interpreter
     * @param initial Initial CPU state
     * @param code Instructions to execute
     * @param memory Memory setup
     * @return Final CPU state and cycle count
     */
    std::pair<SH2State, uint64_t> ExecuteOnInterpreter(
        const SH2State& initial,
        const std::vector<uint16_t>& code,
        const std::vector<TestCase::MemoryRegion>& memory
    );
    
    /**
     * @brief Execute code on JIT
     * @param initial Initial CPU state
     * @param code Instructions to execute
     * @param memory Memory setup
     * @return Final CPU state and cycle count
     */
    std::pair<SH2State, uint64_t> ExecuteOnJIT(
        const SH2State& initial,
        const std::vector<uint16_t>& code,
        const std::vector<TestCase::MemoryRegion>& memory
    );
    
    /**
     * @brief Compare two execution results
     * @param interp Interpreter result
     * @param jit JIT result
     * @return Empty string if equal, diff description if different
     */
    std::string CompareResults(
        const std::pair<SH2State, uint64_t>& interp,
        const std::pair<SH2State, uint64_t>& jit
    );
};

// -----------------------------------------------------------------------------
// Test Case Generators
// -----------------------------------------------------------------------------

/**
 * @brief Generate test cases for a specific instruction
 * 
 * Creates variations with different register values, flags, etc.
 */
class InstructionTestGenerator {
public:
    /**
     * @brief Generate tests for a single instruction opcode
     * @param opcode SH-2 instruction opcode (16-bit)
     * @param count Number of test variations to generate
     * @return Vector of test cases
     */
    static std::vector<TestCase> GenerateForOpcode(uint16_t opcode, size_t count = 10);
    
    /**
     * @brief Generate tests for all variations of an instruction family
     * @param mnemonic Instruction mnemonic (e.g., "ADD", "MOV")
     * @return Vector of test cases covering all addressing modes
     */
    static std::vector<TestCase> GenerateForInstruction(const std::string& mnemonic);
    
    /**
     * @brief Generate comprehensive instruction test suite
     * @return All instruction tests (500-800 tests)
     */
    static std::vector<TestCase> GenerateAllInstructionTests();
};

/**
 * @brief Generate basic block test cases (multi-instruction sequences)
 */
class BlockTestGenerator {
public:
    /**
     * @brief Generate test with multiple instructions (no branches)
     * @param length Number of instructions in block
     * @param count Number of test blocks to generate
     * @return Vector of test cases
     */
    static std::vector<TestCase> GenerateRandomBlocks(size_t length, size_t count);
    
    /**
     * @brief Generate tests for common instruction patterns
     * @return Test cases for typical sequences (load-compute-store, etc.)
     */
    static std::vector<TestCase> GenerateCommonPatterns();
};

/**
 * @brief Generate control flow test cases (branches, delay slots)
 */
class ControlFlowTestGenerator {
public:
    /**
     * @brief Generate tests for conditional branches
     * @return Test cases covering BT, BF with various conditions
     */
    static std::vector<TestCase> GenerateBranchTests();
    
    /**
     * @brief Generate tests for delay slots
     * @return Test cases for tricky delay slot scenarios
     */
    static std::vector<TestCase> GenerateDelaySlotTests();
    
    /**
     * @brief Generate tests for jumps and calls
     * @return Test cases for JMP, JSR, BRAF, BSRF, RTS
     */
    static std::vector<TestCase> GenerateJumpTests();
};

/**
 * @brief Generate randomized test cases (fuzzing)
 */
class FuzzTestGenerator {
public:
    /**
     * @brief Generate random instruction sequences
     * @param sequence_length Number of instructions per sequence
     * @param count Number of sequences to generate
     * @param seed Random seed for reproducibility
     * @return Vector of randomized test cases
     */
    static std::vector<TestCase> GenerateRandomSequences(
        size_t sequence_length,
        size_t count,
        uint32_t seed = 0
    );
};

// -----------------------------------------------------------------------------
// Test Utilities
// -----------------------------------------------------------------------------

/**
 * @brief Create initial state with random register values
 * @param seed Random seed
 * @return Randomized SH-2 state
 */
SH2State CreateRandomState(uint32_t seed);

/**
 * @brief Create initial state with specific register values
 * @param pc Initial PC
 * @param registers Initial R0-R15 values
 * @return Configured SH-2 state
 */
SH2State CreateStateWithRegisters(uint32_t pc, const std::array<uint32_t, 16>& registers);

/**
 * @brief Disassemble SH-2 instruction to string
 * @param opcode 16-bit instruction
 * @return Assembly string (e.g., "ADD R1, R0")
 */
std::string DisassembleInstruction(uint16_t opcode);

/**
 * @brief Encode SH-2 instruction from mnemonic and operands
 * @param mnemonic Instruction name (e.g., "ADD")
 * @param operands Operand strings
 * @return 16-bit encoded instruction, or nullopt if invalid
 */
std::optional<uint16_t> EncodeInstruction(const std::string& mnemonic, const std::vector<std::string>& operands);

} // namespace brimir::jit::test

