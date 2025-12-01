#pragma once

/**
 * @file jit_validator.hpp
 * @brief JIT Validation Framework - Dual Execution Testing
 * 
 * Validates JIT compilation against interpreter (reference implementation).
 * Uses Saturn Open SDK as the source of truth for SH-2 behavior.
 * 
 * CRITICAL: Every JIT-compiled instruction must produce identical results
 * to the interpreter across ALL architectural state.
 */

#include <brimir/core/types.hpp>
#include <string>
#include <vector>
#include <memory>

namespace brimir::jit {

using namespace brimir;

/// @brief Complete SH-2 architectural state for comparison
struct SH2State {
    // General purpose registers
    uint32 R[16];
    
    // Control registers
    uint32 PC;
    uint32 PR;      // Procedure register
    uint32 GBR;     // Global base register
    uint32 VBR;     // Vector base register
    uint32 MACH;    // Multiply-accumulate high
    uint32 MACL;    // Multiply-accumulate low
    
    // Status register
    uint32 SR;
    bool T;         // T-bit (test/carry)
    bool S;         // S-bit (saturation for MAC)
    uint8 IMASK;    // Interrupt mask (4 bits)
    bool Q;         // Q-bit (for DIV)
    bool M;         // M-bit (for DIV)
    
    // Execution state
    uint64 cycles;  // Cycle counter
    bool in_delay_slot;
    uint32 delay_slot_pc;
    
    /// @brief Compare two states for equality
    bool operator==(const SH2State& other) const;
    
    /// @brief Get human-readable diff
    std::string Diff(const SH2State& other) const;
};

/// @brief Single instruction test case
struct InstructionTest {
    uint16 instruction;         ///< Encoded SH-2 instruction
    SH2State initial_state;     ///< Initial CPU state
    SH2State expected_state;    ///< Expected final state
    uint32 expected_cycles;     ///< Expected cycle count
    std::string mnemonic;       ///< Instruction name (e.g., "ADD R1, R2")
    std::string description;    ///< Test description
    uint32 sh2_address;         ///< Address where instruction is placed
    
    InstructionTest() : instruction(0), expected_cycles(0), sh2_address(0) {}
};

/// @brief Validation result for a single test
struct ValidationResult {
    bool passed = true;         ///< Overall pass/fail
    bool registers_match = true;
    bool flags_match = true;
    bool pc_match = true;
    bool cycles_match = true;
    bool memory_match = true;
    
    SH2State interpreter_state; ///< Final state from interpreter
    SH2State jit_state;          ///< Final state from JIT
    
    std::string error_message;   ///< Detailed error if failed
    
    /// @brief Check if validation passed
    bool Passed() const { return passed; }
    
    /// @brief Get detailed failure report
    std::string GetReport() const;
};

/// @brief Test suite for an instruction category
struct InstructionTestSuite {
    std::string category;               ///< e.g., "Arithmetic", "Branch"
    std::vector<InstructionTest> tests; ///< All tests for this category
    
    InstructionTestSuite(const std::string& cat) : category(cat) {}
    
    /// @brief Add a test to the suite
    void AddTest(const InstructionTest& test) {
        tests.push_back(test);
    }
    
    /// @brief Get test count
    size_t Count() const { return tests.size(); }
};

/// @brief JIT Validator - Executes tests and compares results
class JITValidator {
public:
    /**
     * @brief Construct a validator
     * @param interpreter Reference to interpreter (oracle)
     * @param jit Reference to JIT compiler (under test)
     */
    JITValidator(/* interpreter ref */, /* jit ref */);
    
    /**
     * @brief Validate a single instruction test
     * @param test Test case to execute
     * @return Validation result with detailed comparison
     */
    ValidationResult ValidateInstruction(const InstructionTest& test);
    
    /**
     * @brief Validate entire test suite
     * @param suite Suite of tests to run
     * @return Summary of results
     */
    struct SuiteResults {
        size_t total_tests = 0;
        size_t passed = 0;
        size_t failed = 0;
        std::vector<ValidationResult> failures;
        
        double PassRate() const {
            return total_tests > 0 ? (100.0 * passed / total_tests) : 0.0;
        }
    };
    
    SuiteResults ValidateSuite(const InstructionTestSuite& suite);
    
    /**
     * @brief Run all test suites
     * @param suites All suites to validate
     * @return Overall results
     */
    struct OverallResults {
        size_t total_suites = 0;
        size_t total_tests = 0;
        size_t passed = 0;
        size_t failed = 0;
        std::vector<SuiteResults> suite_results;
        
        double PassRate() const {
            return total_tests > 0 ? (100.0 * passed / total_tests) : 0.0;
        }
        
        bool AllPassed() const { return failed == 0; }
    };
    
    OverallResults RunAllTests(const std::vector<InstructionTestSuite>& suites);
    
    /**
     * @brief Generate detailed report
     * @param results Overall results
     * @return Human-readable report
     */
    static std::string GenerateReport(const OverallResults& results);
    
private:
    // TODO: Add interpreter and JIT references
    
    /**
     * @brief Execute instruction in interpreter (reference)
     */
    SH2State ExecuteInterpreter(const InstructionTest& test);
    
    /**
     * @brief Execute instruction in JIT (under test)
     */
    SH2State ExecuteJIT(const InstructionTest& test);
    
    /**
     * @brief Compare two states in detail
     */
    ValidationResult CompareStates(const SH2State& interpreter, 
                                   const SH2State& jit,
                                   const InstructionTest& test);
};

/// @brief Test generator from Saturn Open SDK specification
class TestGenerator {
public:
    /**
     * @brief Generate tests for a specific instruction
     * @param mnemonic Instruction mnemonic (e.g., "ADD", "MOV")
     * @param opcode_pattern Binary pattern (e.g., 0x300C for ADD)
     * @param opcode_mask Mask for pattern (e.g., 0xF00F)
     * @return Test suite with comprehensive coverage
     */
    static InstructionTestSuite GenerateInstructionTests(
        const std::string& mnemonic,
        uint16 opcode_pattern,
        uint16 opcode_mask);
    
    /**
     * @brief Generate all tests from Saturn Open SDK
     * @return All 133 instructions × ~10 tests each = 1300+ tests
     */
    static std::vector<InstructionTestSuite> GenerateAllTests();
    
private:
    /// @brief Generate normal case tests
    static void AddNormalTests(InstructionTestSuite& suite, 
                               const std::string& mnemonic,
                               uint16 pattern);
    
    /// @brief Generate edge case tests (0, -1, max)
    static void AddEdgeTests(InstructionTestSuite& suite,
                            const std::string& mnemonic,
                            uint16 pattern);
    
    /// @brief Generate flag-specific tests
    static void AddFlagTests(InstructionTestSuite& suite,
                            const std::string& mnemonic,
                            uint16 pattern);
};

} // namespace brimir::jit

