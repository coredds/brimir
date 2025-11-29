/**
 * @file fuzz_test_generator.cpp
 * @brief Generate randomized test cases for fuzzing (Level 5 tests)
 * 
 * Generates millions of random instruction sequences to discover edge cases.
 */

#include "../../include/jit_test_framework.hpp"
#include <random>
#include <set>

namespace brimir::jit::test {

// All valid SH-2 opcodes (subset for safety - avoid branches initially)
const uint16_t SAFE_OPCODES[] = {
    0x0009, // NOP
    0x0008, // CLRT
    0x0018, // SETT
    0x0028, // CLRMAC
    0x0019, // DIV0U
};

// Generate a random but valid SH-2 instruction
uint16_t GenerateRandomInstruction(std::mt19937& rng, bool allow_branches = false) {
    // Distribution of instruction types
    uint32_t type = rng() % 100;
    uint8_t rn = (rng() % 16);
    uint8_t rm = (rng() % 16);
    uint8_t imm = rng() & 0xFF;
    int8_t disp = rng() & 0xFF;
    
    if (type < 10) {
        // 10%: NOP and simple instructions
        return SAFE_OPCODES[rng() % (sizeof(SAFE_OPCODES) / sizeof(SAFE_OPCODES[0]))];
    }
    else if (type < 30) {
        // 20%: MOV Rm, Rn
        return 0x6003 | (rn << 8) | (rm << 4);
    }
    else if (type < 45) {
        // 15%: MOV #imm, Rn
        return 0xE000 | (rn << 8) | imm;
    }
    else if (type < 55) {
        // 10%: ADD Rm, Rn
        return 0x300C | (rn << 8) | (rm << 4);
    }
    else if (type < 65) {
        // 10%: ADD #imm, Rn
        return 0x7000 | (rn << 8) | imm;
    }
    else if (type < 70) {
        // 5%: SUB Rm, Rn
        return 0x3008 | (rn << 8) | (rm << 4);
    }
    else if (type < 75) {
        // 5%: AND Rm, Rn
        return 0x2009 | (rn << 8) | (rm << 4);
    }
    else if (type < 80) {
        // 5%: OR Rm, Rn
        return 0x200B | (rn << 8) | (rm << 4);
    }
    else if (type < 85) {
        // 5%: XOR Rm, Rn
        return 0x200A | (rn << 8) | (rm << 4);
    }
    else if (type < 90) {
        // 5%: Shifts
        uint32_t shift_type = rng() % 4;
        switch (shift_type) {
            case 0: return 0x4000 | (rn << 8); // SHLL
            case 1: return 0x4001 | (rn << 8); // SHLR
            case 2: return 0x4020 | (rn << 8); // SHAL
            case 3: return 0x4021 | (rn << 8); // SHAR
        }
    }
    else if (type < 95) {
        // 5%: Comparisons
        uint32_t cmp_type = rng() % 3;
        switch (cmp_type) {
            case 0: return 0x3000 | (rn << 8) | (rm << 4); // CMP/EQ
            case 1: return 0x3007 | (rn << 8) | (rm << 4); // CMP/GT
            case 2: return 0x3006 | (rn << 8) | (rm << 4); // CMP/HI
        }
    }
    else if (allow_branches && type < 98) {
        // 3%: Conditional branches (if allowed)
        if (rng() % 2) {
            return 0x8900 | (disp & 0xFF); // BT
        } else {
            return 0x8B00 | (disp & 0xFF); // BF
        }
    }
    
    // Default: NOP
    return 0x0009;
}

std::vector<TestCase> FuzzTestGenerator::GenerateRandomSequences(
    size_t sequence_length,
    size_t count,
    uint32_t seed
) {
    std::vector<TestCase> tests;
    std::mt19937 rng(seed == 0 ? std::random_device{}() : seed);
    
    for (size_t i = 0; i < count; i++) {
        TestCase test;
        test.name = "fuzz_" + std::to_string(sequence_length) + "_" + std::to_string(i);
        test.description = "Fuzzed sequence of " + std::to_string(sequence_length) + " instructions";
        
        // Random initial state
        test.initial_state = CreateRandomState(rng());
        test.initial_state.PC = 0x06004000;
        
        // Generate random sequence
        for (size_t j = 0; j < sequence_length; j++) {
            test.code.push_back(GenerateRandomInstruction(rng, false));  // No branches for now
        }
        
        tests.push_back(test);
    }
    
    return tests;
}

/**
 * @brief Generate edge case tests (boundary conditions)
 */
std::vector<TestCase> GenerateEdgeCaseTests() {
    std::vector<TestCase> tests;
    
    // Test 1: All registers at max value
    {
        TestCase test;
        test.name = "edge_all_max_values";
        test.description = "All registers at 0xFFFFFFFF";
        test.initial_state = {};
        for (int i = 0; i < 16; i++) {
            test.initial_state.R[i] = 0xFFFFFFFF;
        }
        test.initial_state.PC = 0x06004000;
        test.code = {
            0x300C, // ADD R0, R0 (overflow)
            0x3008, // SUB R0, R0 (zero)
            0x2009  // AND R0, R0
        };
        tests.push_back(test);
    }
    
    // Test 2: All registers at zero
    {
        TestCase test;
        test.name = "edge_all_zero";
        test.description = "All registers at 0";
        test.initial_state = {};
        for (int i = 0; i < 16; i++) {
            test.initial_state.R[i] = 0;
        }
        test.initial_state.PC = 0x06004000;
        test.code = {
            0x300C, // ADD R0, R0
            0x3008, // SUB R0, R0
            0x200A  // XOR R0, R0
        };
        tests.push_back(test);
    }
    
    // Test 3: Alternating bit patterns
    {
        TestCase test;
        test.name = "edge_alternating_bits";
        test.description = "0xAAAAAAAA and 0x55555555 patterns";
        test.initial_state = {};
        for (int i = 0; i < 8; i++) {
            test.initial_state.R[i] = 0xAAAAAAAA;
            test.initial_state.R[i + 8] = 0x55555555;
        }
        test.initial_state.PC = 0x06004000;
        test.code = {
            0x2009, // AND R0, R0
            0x200B, // OR R0, R0
            0x200A  // XOR R0, R0
        };
        tests.push_back(test);
    }
    
    // Test 4: Power of 2 values
    {
        TestCase test;
        test.name = "edge_powers_of_two";
        test.description = "Registers with powers of 2";
        test.initial_state = {};
        for (int i = 0; i < 16; i++) {
            test.initial_state.R[i] = 1 << i;
        }
        test.initial_state.PC = 0x06004000;
        test.code = {
            0x4000, // SHLL R0
            0x4001, // SHLR R0
            0x4020  // SHAL R0
        };
        tests.push_back(test);
    }
    
    // Test 5: Sign bit patterns
    {
        TestCase test;
        test.name = "edge_sign_bits";
        test.description = "Test sign extension and signed operations";
        test.initial_state = {};
        test.initial_state.R[0] = 0x80000000;  // Most negative
        test.initial_state.R[1] = 0x7FFFFFFF;  // Most positive
        test.initial_state.R[2] = 0x00000080;  // Sign bit in byte
        test.initial_state.R[3] = 0x00008000;  // Sign bit in word
        test.initial_state.PC = 0x06004000;
        test.code = {
            0x600E, // EXTS.B R0, R0
            0x600F, // EXTS.W R1, R1
            0x600C, // EXTU.B R2, R2
            0x600D  // EXTU.W R3, R3
        };
        tests.push_back(test);
    }
    
    return tests;
}

/**
 * @brief Generate stress tests (long sequences, deep nesting)
 */
std::vector<TestCase> GenerateStressTests() {
    std::vector<TestCase> tests;
    
    // Test 1: Very long instruction sequence
    {
        TestCase test;
        test.name = "stress_long_sequence";
        test.description = "100 instruction sequence";
        test.initial_state = CreateRandomState(99999);
        test.initial_state.PC = 0x06004000;
        
        for (int i = 0; i < 100; i++) {
            test.code.push_back(0xE000 | ((i % 16) << 8) | (i & 0xFF));  // MOV #i, Rn
        }
        
        tests.push_back(test);
    }
    
    // Test 2: Rapid register changes
    {
        TestCase test;
        test.name = "stress_register_churn";
        test.description = "Rapid modification of all registers";
        test.initial_state = CreateRandomState(88888);
        test.initial_state.PC = 0x06004000;
        
        for (int i = 0; i < 16; i++) {
            test.code.push_back(0xE000 | (i << 8) | i);  // MOV #i, Ri
            test.code.push_back(0x300C | (i << 8) | (((i+1) % 16) << 4));  // ADD R(i+1), Ri
        }
        
        tests.push_back(test);
    }
    
    // Test 3: T-bit flipping
    {
        TestCase test;
        test.name = "stress_t_bit_flipping";
        test.description = "Rapid T-bit changes";
        test.initial_state = CreateRandomState(77777);
        test.initial_state.PC = 0x06004000;
        
        for (int i = 0; i < 50; i++) {
            test.code.push_back(0x0018);  // SETT
            test.code.push_back(0x0008);  // CLRT
        }
        
        tests.push_back(test);
    }
    
    return tests;
}

} // namespace brimir::jit::test

