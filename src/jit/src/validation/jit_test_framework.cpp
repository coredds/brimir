/**
 * @file jit_test_framework.cpp
 * @brief Implementation of JIT test framework
 */

#include "../../include/jit_test_framework.hpp"
#include <sstream>
#include <iomanip>
#include <cstring>

namespace brimir::jit::test {

// -----------------------------------------------------------------------------
// SH2State Implementation
// -----------------------------------------------------------------------------

bool SH2State::operator==(const SH2State& other) const {
    // Compare all registers
    if (std::memcmp(R, other.R, sizeof(R)) != 0) return false;
    
    // Compare control registers
    if (PC != other.PC) return false;
    if (PR != other.PR) return false;
    if (GBR != other.GBR) return false;
    if (VBR != other.VBR) return false;
    
    // Compare MAC
    if (MACH != other.MACH) return false;
    if (MACL != other.MACL) return false;
    
    // Compare status register (including T-bit)
    if (SR != other.SR) return false;
    if (T != other.T) return false;
    
    // Compare execution state
    if (cycles != other.cycles) return false;
    if (delay_slot != other.delay_slot) return false;
    if (delay_slot && delay_target != other.delay_target) return false;
    
    return true;
}

std::string SH2State::GetDiff(const SH2State& other) const {
    std::ostringstream oss;
    bool has_diff = false;
    
    // Check general registers
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
    if (VBR != other.VBR) {
        oss << "  VBR: 0x" << std::hex << std::setw(8) << std::setfill('0') 
            << VBR << " vs 0x" << other.VBR << std::dec << "\n";
        has_diff = true;
    }
    
    // Check MAC registers
    if (MACH != other.MACH) {
        oss << "  MACH: 0x" << std::hex << std::setw(8) << std::setfill('0') 
            << MACH << " vs 0x" << other.MACH << std::dec << "\n";
        has_diff = true;
    }
    if (MACL != other.MACL) {
        oss << "  MACL: 0x" << std::hex << std::setw(8) << std::setfill('0') 
            << MACL << " vs 0x" << other.MACL << std::dec << "\n";
        has_diff = true;
    }
    
    // Check SR and T-bit
    if (SR != other.SR) {
        oss << "  SR: 0x" << std::hex << std::setw(8) << std::setfill('0') 
            << SR << " vs 0x" << other.SR << std::dec << "\n";
        has_diff = true;
    }
    if (T != other.T) {
        oss << "  T-bit: " << (T ? "1" : "0") << " vs " << (other.T ? "1" : "0") << "\n";
        has_diff = true;
    }
    
    // Check cycles (CRITICAL!)
    if (cycles != other.cycles) {
        oss << "  Cycles: " << cycles << " vs " << other.cycles 
            << " (diff: " << (int64_t)cycles - (int64_t)other.cycles << ")\n";
        has_diff = true;
    }
    
    // Check delay slot state
    if (delay_slot != other.delay_slot) {
        oss << "  Delay slot: " << (delay_slot ? "yes" : "no") 
            << " vs " << (other.delay_slot ? "yes" : "no") << "\n";
        has_diff = true;
    }
    if (delay_slot && delay_target != other.delay_target) {
        oss << "  Delay target: 0x" << std::hex << std::setw(8) << std::setfill('0') 
            << delay_target << " vs 0x" << other.delay_target << std::dec << "\n";
        has_diff = true;
    }
    
    return has_diff ? oss.str() : "";
}

std::string SH2State::ToString() const {
    std::ostringstream oss;
    oss << "SH-2 State:\n";
    oss << std::hex << std::setfill('0');
    
    // General registers (4 per line)
    for (int i = 0; i < 16; i += 4) {
        oss << "  R" << std::dec << std::setw(2) << i << "-R" << (i+3) << ": ";
        for (int j = 0; j < 4; j++) {
            oss << "0x" << std::hex << std::setw(8) << R[i+j];
            if (j < 3) oss << " ";
        }
        oss << "\n";
    }
    
    // Control registers
    oss << "  PC:  0x" << std::hex << std::setw(8) << PC << "\n";
    oss << "  PR:  0x" << std::hex << std::setw(8) << PR << "\n";
    oss << "  GBR: 0x" << std::hex << std::setw(8) << GBR << "\n";
    oss << "  VBR: 0x" << std::hex << std::setw(8) << VBR << "\n";
    
    // MAC
    oss << "  MACH: 0x" << std::hex << std::setw(8) << MACH << "\n";
    oss << "  MACL: 0x" << std::hex << std::setw(8) << MACL << "\n";
    
    // Status
    oss << "  SR: 0x" << std::hex << std::setw(8) << SR 
        << " (T=" << (T ? "1" : "0") << ")\n";
    
    // Execution state
    oss << "  Cycles: " << std::dec << cycles << "\n";
    if (delay_slot) {
        oss << "  In delay slot, target: 0x" << std::hex << std::setw(8) << delay_target << "\n";
    }
    
    return oss.str();
}

// -----------------------------------------------------------------------------
// TestResult Implementation
// -----------------------------------------------------------------------------

std::string TestResult::GetReport() const {
    std::ostringstream oss;
    
    oss << "================================================================================\n";
    oss << "Test: " << test_name << "\n";
    oss << "Result: " << (passed ? "PASS ✓" : "FAIL ✗") << "\n";
    oss << "================================================================================\n";
    
    if (!passed) {
        oss << "\nFailure Reason:\n" << failure_reason << "\n";
        
        oss << "\n--- State Differences ---\n";
        std::string diff = interpreter_final.GetDiff(jit_final);
        if (!diff.empty()) {
            oss << diff;
        } else {
            oss << "(States are identical - failure in custom validator?)\n";
        }
        
        oss << "\n--- Interpreter Final State ---\n";
        oss << interpreter_final.ToString();
        
        oss << "\n--- JIT Final State ---\n";
        oss << jit_final.ToString();
    }
    
    oss << "\nCycles:\n";
    oss << "  Interpreter: " << interpreter_cycles << "\n";
    oss << "  JIT:         " << jit_cycles << "\n";
    if (interpreter_cycles != jit_cycles) {
        oss << "  Difference:  " << (int64_t)jit_cycles - (int64_t)interpreter_cycles << "\n";
    }
    
    return oss.str();
}

// -----------------------------------------------------------------------------
// DualExecutionHarness Implementation
// -----------------------------------------------------------------------------

DualExecutionHarness::DualExecutionHarness() {
    // TODO: Initialize interpreter and JIT instances
}

DualExecutionHarness::~DualExecutionHarness() {
    // TODO: Cleanup
}

TestResult DualExecutionHarness::RunTest(const TestCase& test) {
    TestResult result;
    result.test_name = test.name;
    result.passed = false;  // Assume failure until proven otherwise
    
    if (m_verbose) {
        std::cout << "Running test: " << test.name << "\n";
        std::cout << "Description: " << test.description << "\n";
    }
    
    try {
        // Execute on interpreter
        auto [interp_state, interp_cycles] = ExecuteOnInterpreter(
            test.initial_state,
            test.code,
            test.memory_setup
        );
        
        result.interpreter_final = interp_state;
        result.interpreter_cycles = interp_cycles;
        
        // Execute on JIT (or interpreter again if JIT disabled)
        auto [jit_state, jit_cycles] = m_jit_enabled
            ? ExecuteOnJIT(test.initial_state, test.code, test.memory_setup)
            : ExecuteOnInterpreter(test.initial_state, test.code, test.memory_setup);
        
        result.jit_final = jit_state;
        result.jit_cycles = jit_cycles;
        
        // Compare results
        std::string diff = CompareResults(
            {interp_state, interp_cycles},
            {jit_state, jit_cycles}
        );
        
        if (!diff.empty()) {
            result.failure_reason = "State mismatch:\n" + diff;
            return result;
        }
        
        // Run custom validator if provided
        if (test.custom_validator) {
            std::string validation_error = (*test.custom_validator)(interp_state, jit_state);
            if (!validation_error.empty()) {
                result.failure_reason = "Custom validation failed: " + validation_error;
                return result;
            }
        }
        
        // Check against expected state if provided
        if (test.expected_final) {
            std::string expected_diff = test.expected_final->GetDiff(interp_state);
            if (!expected_diff.empty()) {
                result.failure_reason = "Interpreter doesn't match expected state:\n" + expected_diff;
                return result;
            }
        }
        
        // Check against expected cycles if provided
        if (test.expected_cycles && interp_cycles != *test.expected_cycles) {
            result.failure_reason = "Cycle count mismatch: expected " + 
                std::to_string(*test.expected_cycles) + ", got " + std::to_string(interp_cycles);
            return result;
        }
        
        // All checks passed!
        result.passed = true;
        
    } catch (const std::exception& e) {
        result.failure_reason = std::string("Exception: ") + e.what();
    }
    
    return result;
}

std::vector<TestResult> DualExecutionHarness::RunTests(const std::vector<TestCase>& tests) {
    std::vector<TestResult> results;
    results.reserve(tests.size());
    
    for (const auto& test : tests) {
        results.push_back(RunTest(test));
    }
    
    return results;
}

std::string DualExecutionHarness::RunTestsWithReport(const std::vector<TestCase>& tests) {
    auto results = RunTests(tests);
    
    std::ostringstream oss;
    oss << "================================================================================\n";
    oss << "JIT Test Suite Results\n";
    oss << "================================================================================\n\n";
    
    size_t passed = 0;
    size_t failed = 0;
    
    for (const auto& result : results) {
        if (result.passed) {
            passed++;
        } else {
            failed++;
        }
    }
    
    oss << "Total:  " << tests.size() << " tests\n";
    oss << "Passed: " << passed << " (" << (100.0 * passed / tests.size()) << "%)\n";
    oss << "Failed: " << failed << " (" << (100.0 * failed / tests.size()) << "%)\n";
    oss << "\n";
    
    // List failed tests
    if (failed > 0) {
        oss << "Failed Tests:\n";
        oss << "--------------------------------------------------------------------------------\n";
        for (const auto& result : results) {
            if (!result.passed) {
                oss << result.GetReport() << "\n";
            }
        }
    }
    
    return oss.str();
}

std::pair<SH2State, uint64_t> DualExecutionHarness::ExecuteOnInterpreter(
    const SH2State& initial,
    const std::vector<uint16_t>& code,
    const std::vector<TestCase::MemoryRegion>& memory
) {
    // TODO: Implement interpreter execution
    // 1. Create isolated SH-2 instance
    // 2. Load initial state
    // 3. Setup memory regions
    // 4. Execute code
    // 5. Capture final state
    // 6. Return state + cycle count
    
    // Placeholder:
    SH2State final_state = initial;
    uint64_t cycles = 0;
    
    // This will be implemented when we integrate with Ymir's SH-2
    throw std::runtime_error("ExecuteOnInterpreter: Not yet implemented (Phase 2)");
    
    return {final_state, cycles};
}

std::pair<SH2State, uint64_t> DualExecutionHarness::ExecuteOnJIT(
    const SH2State& initial,
    const std::vector<uint16_t>& code,
    const std::vector<TestCase::MemoryRegion>& memory
) {
    // TODO: Implement JIT execution
    // 1. Create isolated SH-2 instance with JIT enabled
    // 2. Load initial state
    // 3. Setup memory regions
    // 4. Execute code (will compile if needed)
    // 5. Capture final state
    // 6. Return state + cycle count
    
    // Placeholder:
    SH2State final_state = initial;
    uint64_t cycles = 0;
    
    // This will be implemented in Phase 3 (x86-64 backend)
    throw std::runtime_error("ExecuteOnJIT: Not yet implemented (Phase 3)");
    
    return {final_state, cycles};
}

std::string DualExecutionHarness::CompareResults(
    const std::pair<SH2State, uint64_t>& interp,
    const std::pair<SH2State, uint64_t>& jit
) {
    const auto& [interp_state, interp_cycles] = interp;
    const auto& [jit_state, jit_cycles] = jit;
    
    std::ostringstream oss;
    
    // Compare states
    std::string state_diff = interp_state.GetDiff(jit_state);
    if (!state_diff.empty()) {
        oss << "CPU State Differences:\n" << state_diff;
    }
    
    // Compare cycles (CRITICAL for accuracy!)
    if (interp_cycles != jit_cycles) {
        oss << "Cycle Count Mismatch:\n";
        oss << "  Interpreter: " << interp_cycles << "\n";
        oss << "  JIT:         " << jit_cycles << "\n";
        oss << "  Difference:  " << (int64_t)jit_cycles - (int64_t)interp_cycles << "\n";
    }
    
    return oss.str();
}

// -----------------------------------------------------------------------------
// Utility Functions
// -----------------------------------------------------------------------------

SH2State CreateRandomState(uint32_t seed) {
    SH2State state = {};
    
    // Simple LCG random number generator
    auto lcg = [](uint32_t& s) -> uint32_t {
        s = s * 1664525u + 1013904223u;
        return s;
    };
    
    // Randomize general registers
    for (int i = 0; i < 16; i++) {
        state.R[i] = lcg(seed);
    }
    
    // Randomize control registers
    state.PC = lcg(seed) & 0x0FFFFFFC;  // Align to 4 bytes, keep in valid range
    state.PR = lcg(seed);
    state.GBR = lcg(seed);
    state.VBR = lcg(seed);
    
    // Randomize MAC
    state.MACH = lcg(seed);
    state.MACL = lcg(seed);
    
    // Randomize SR (but keep valid bits)
    state.SR = lcg(seed) & 0x000003F3;  // Valid SR bits
    state.T = (lcg(seed) & 1) != 0;
    
    // Clear execution state
    state.cycles = 0;
    state.delay_slot = false;
    state.delay_target = 0;
    
    return state;
}

SH2State CreateStateWithRegisters(uint32_t pc, const std::array<uint32_t, 16>& registers) {
    SH2State state = {};
    
    std::copy(registers.begin(), registers.end(), state.R);
    state.PC = pc;
    
    // Zero other fields
    state.PR = 0;
    state.GBR = 0;
    state.VBR = 0;
    state.MACH = 0;
    state.MACL = 0;
    state.SR = 0;
    state.T = false;
    state.cycles = 0;
    state.delay_slot = false;
    state.delay_target = 0;
    
    return state;
}

std::string DisassembleInstruction(uint16_t opcode) {
    // TODO: Implement SH-2 disassembly
    // For now, just return hex
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(4) << std::setfill('0') << opcode;
    return oss.str();
}

std::optional<uint16_t> EncodeInstruction(
    const std::string& mnemonic,
    const std::vector<std::string>& operands
) {
    // TODO: Implement SH-2 assembly encoding
    // This will be a lookup table + encoding logic
    return std::nullopt;
}

} // namespace brimir::jit::test

