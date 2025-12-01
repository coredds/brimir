#include "../include/jit_validator.hpp"
#include <sstream>
#include <iomanip>
#include <cmath>

namespace brimir::jit {

// ============================================================================
// SH2State implementation
// ============================================================================

bool SH2State::operator==(const SH2State& other) const {
    // Compare all registers
    for (int i = 0; i < 16; i++) {
        if (R[i] != other.R[i]) return false;
    }
    
    // Compare control registers
    if (PC != other.PC) return false;
    if (PR != other.PR) return false;
    if (GBR != other.GBR) return false;
    if (VBR != other.VBR) return false;
    if (MACH != other.MACH) return false;
    if (MACL != other.MACL) return false;
    
    // Compare status register and flags
    if (SR != other.SR) return false;
    if (T != other.T) return false;
    if (S != other.S) return false;
    if (IMASK != other.IMASK) return false;
    if (Q != other.Q) return false;
    if (M != other.M) return false;
    
    // Compare execution state
    if (cycles != other.cycles) return false;
    if (in_delay_slot != other.in_delay_slot) return false;
    if (delay_slot_pc != other.delay_slot_pc) return false;
    
    return true;
}

std::string SH2State::Diff(const SH2State& other) const {
    std::ostringstream oss;
    bool has_diff = false;
    
    // Check registers
    for (int i = 0; i < 16; i++) {
        if (R[i] != other.R[i]) {
            oss << "  R" << i << ": 0x" << std::hex << std::setw(8) << std::setfill('0') 
                << R[i] << " vs 0x" << other.R[i] << std::dec << "\n";
            has_diff = true;
        }
    }
    
    // Check control registers
    if (PC != other.PC) {
        oss << "  PC: 0x" << std::hex << std::setw(8) << std::setfill('0')
            << PC << " vs 0x" << other.PC << std::dec << "\n";
        has_diff = true;
    }
    if (PR != other.PR) {
        oss << "  PR: 0x" << std::hex << std::setw(8) << std::setfill('0')
            << PR << " vs 0x" << other.PR << std::dec << "\n";
        has_diff = true;
    }
    if (GBR != other.GBR) {
        oss << "  GBR: 0x" << std::hex << std::setw(8) << std::setfill('0')
            << GBR << " vs 0x" << other.GBR << std::dec << "\n";
        has_diff = true;
    }
    
    // Check flags
    if (T != other.T) {
        oss << "  T-bit: " << T << " vs " << other.T << "\n";
        has_diff = true;
    }
    if (S != other.S) {
        oss << "  S-bit: " << S << " vs " << other.S << "\n";
        has_diff = true;
    }
    if (Q != other.Q) {
        oss << "  Q-bit: " << Q << " vs " << other.Q << "\n";
        has_diff = true;
    }
    if (M != other.M) {
        oss << "  M-bit: " << M << " vs " << other.M << "\n";
        has_diff = true;
    }
    
    // Check cycles (CRITICAL!)
    if (cycles != other.cycles) {
        oss << "  Cycles: " << cycles << " vs " << other.cycles << "\n";
        has_diff = true;
    }
    
    return has_diff ? oss.str() : "";
}

// ============================================================================
// ValidationResult implementation
// ============================================================================

std::string ValidationResult::GetReport() const {
    if (passed) {
        return "PASS";
    }
    
    std::ostringstream oss;
    oss << "FAIL:\n";
    
    if (!registers_match) {
        oss << "  Register mismatch:\n";
        oss << interpreter_state.Diff(jit_state);
    }
    
    if (!flags_match) {
        oss << "  Flag mismatch\n";
    }
    
    if (!pc_match) {
        oss << "  PC mismatch: Interpreter=0x" << std::hex << interpreter_state.PC
            << " JIT=0x" << jit_state.PC << std::dec << "\n";
    }
    
    if (!cycles_match) {
        oss << "  Cycle mismatch: Interpreter=" << interpreter_state.cycles
            << " JIT=" << jit_state.cycles << "\n";
    }
    
    if (!error_message.empty()) {
        oss << "  Error: " << error_message << "\n";
    }
    
    return oss.str();
}

// ============================================================================
// JITValidator implementation
// ============================================================================

JITValidator::JITValidator(/* interpreter ref */, /* jit ref */) {
    // TODO: Store references to interpreter and JIT compiler
}

ValidationResult JITValidator::ValidateInstruction(const InstructionTest& test) {
    ValidationResult result;
    
    // TODO: Execute in interpreter
    // result.interpreter_state = ExecuteInterpreter(test);
    
    // TODO: Execute in JIT
    // result.jit_state = ExecuteJIT(test);
    
    // TODO: Compare states
    // result = CompareStates(result.interpreter_state, result.jit_state, test);
    
    // Placeholder for now
    result.passed = true;
    result.error_message = "Validator not yet connected to interpreter/JIT";
    
    return result;
}

JITValidator::SuiteResults JITValidator::ValidateSuite(const InstructionTestSuite& suite) {
    SuiteResults results;
    results.total_tests = suite.Count();
    
    for (const auto& test : suite.tests) {
        auto validation = ValidateInstruction(test);
        
        if (validation.Passed()) {
            results.passed++;
        } else {
            results.failed++;
            results.failures.push_back(validation);
        }
    }
    
    return results;
}

JITValidator::OverallResults JITValidator::RunAllTests(
    const std::vector<InstructionTestSuite>& suites) {
    
    OverallResults overall;
    overall.total_suites = suites.size();
    
    for (const auto& suite : suites) {
        auto suite_result = ValidateSuite(suite);
        overall.total_tests += suite_result.total_tests;
        overall.passed += suite_result.passed;
        overall.failed += suite_result.failed;
        overall.suite_results.push_back(suite_result);
    }
    
    return overall;
}

std::string JITValidator::GenerateReport(const OverallResults& results) {
    std::ostringstream oss;
    
    oss << "\n";
    oss << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    oss << "JIT VALIDATION REPORT\n";
    oss << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
    
    oss << "Overall Results:\n";
    oss << "  Total Suites: " << results.total_suites << "\n";
    oss << "  Total Tests:  " << results.total_tests << "\n";
    oss << "  Passed:       " << results.passed << " (" 
        << std::fixed << std::setprecision(1) << results.PassRate() << "%)\n";
    oss << "  Failed:       " << results.failed << "\n";
    oss << "\n";
    
    if (results.AllPassed()) {
        oss << "✅ ALL TESTS PASSED!\n";
    } else {
        oss << "❌ " << results.failed << " test(s) failed\n\n";
        oss << "Failed Tests:\n";
        
        size_t suite_idx = 0;
        for (const auto& suite_result : results.suite_results) {
            if (suite_result.failed > 0) {
                oss << "\nSuite " << (suite_idx + 1) << ": "
                    << suite_result.failed << " failure(s)\n";
                
                for (const auto& failure : suite_result.failures) {
                    oss << failure.GetReport();
                }
            }
            suite_idx++;
        }
    }
    
    oss << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    
    return oss.str();
}

SH2State JITValidator::ExecuteInterpreter(const InstructionTest& test) {
    // TODO: Execute instruction in real interpreter
    // This will be connected to the actual SH-2 interpreter via brimir_sh2_wrapper
    return test.expected_state;
}

SH2State JITValidator::ExecuteJIT(const InstructionTest& test) {
    // TODO: Execute instruction via JIT compiler
    // This will be connected once x86-64 code generator is implemented
    return test.expected_state;
}

ValidationResult JITValidator::CompareStates(const SH2State& interpreter,
                                             const SH2State& jit,
                                             const InstructionTest& test) {
    ValidationResult result;
    result.interpreter_state = interpreter;
    result.jit_state = jit;
    
    // Compare registers
    result.registers_match = true;
    for (int i = 0; i < 16; i++) {
        if (interpreter.R[i] != jit.R[i]) {
            result.registers_match = false;
            break;
        }
    }
    
    // Compare flags
    result.flags_match = (interpreter.T == jit.T &&
                          interpreter.S == jit.S &&
                          interpreter.Q == jit.Q &&
                          interpreter.M == jit.M);
    
    // Compare PC
    result.pc_match = (interpreter.PC == jit.PC);
    
    // Compare cycles (CRITICAL for accuracy!)
    result.cycles_match = (interpreter.cycles == jit.cycles);
    
    // Overall pass/fail
    result.passed = (result.registers_match && 
                     result.flags_match && 
                     result.pc_match && 
                     result.cycles_match);
    
    if (!result.passed) {
        result.error_message = "State mismatch detected";
    }
    
    return result;
}

} // namespace brimir::jit

