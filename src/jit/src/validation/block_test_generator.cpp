/**
 * @file block_test_generator.cpp
 * @brief Generate test cases for basic blocks (Level 2 tests)
 * 
 * Tests multi-instruction sequences without branches to validate
 * that JIT handles instruction sequences correctly.
 */

#include "../../include/jit_test_framework.hpp"
#include <random>

namespace brimir::jit::test {

// Reuse instruction encoders from instruction_test_generator.cpp
namespace sh2 {
    extern uint16_t NOP();
    extern uint16_t MOV_R(uint8_t m, uint8_t n);
    extern uint16_t MOV_I(uint8_t imm, uint8_t n);
    extern uint16_t ADD(uint8_t m, uint8_t n);
    extern uint16_t ADD_I(uint8_t imm, uint8_t n);
    extern uint16_t SUB(uint8_t m, uint8_t n);
    extern uint16_t AND(uint8_t m, uint8_t n);
    extern uint16_t OR(uint8_t m, uint8_t n);
    extern uint16_t SHLL(uint8_t n);
    extern uint16_t SHLR(uint8_t n);
}

std::vector<TestCase> BlockTestGenerator::GenerateRandomBlocks(size_t length, size_t count) {
    std::vector<TestCase> tests;
    std::mt19937 rng(12345);  // Deterministic
    
    for (size_t i = 0; i < count; i++) {
        TestCase test;
        test.name = "random_block_" + std::to_string(length) + "_" + std::to_string(i);
        test.description = "Random " + std::to_string(length) + "-instruction sequence";
        
        test.initial_state = CreateRandomState(rng());
        test.initial_state.PC = 0x06004000;
        
        // Generate random instructions (non-branch only)
        for (size_t j = 0; j < length; j++) {
            uint8_t op = rng() % 10;
            uint8_t r1 = rng() % 8;
            uint8_t r2 = rng() % 8;
            uint8_t imm = rng() & 0xFF;
            
            switch (op) {
                case 0: test.code.push_back(sh2::NOP()); break;
                case 1: test.code.push_back(sh2::MOV_R(r1, r2)); break;
                case 2: test.code.push_back(sh2::MOV_I(imm, r1)); break;
                case 3: test.code.push_back(sh2::ADD(r1, r2)); break;
                case 4: test.code.push_back(sh2::ADD_I(imm, r1)); break;
                case 5: test.code.push_back(sh2::SUB(r1, r2)); break;
                case 6: test.code.push_back(sh2::AND(r1, r2)); break;
                case 7: test.code.push_back(sh2::OR(r1, r2)); break;
                case 8: test.code.push_back(sh2::SHLL(r1)); break;
                case 9: test.code.push_back(sh2::SHLR(r1)); break;
            }
        }
        
        tests.push_back(test);
    }
    
    return tests;
}

std::vector<TestCase> BlockTestGenerator::GenerateCommonPatterns() {
    std::vector<TestCase> tests;
    
    // Pattern 1: Load-Compute-Store
    {
        TestCase test;
        test.name = "pattern_load_compute_store";
        test.description = "Common pattern: MOV, ADD, MOV";
        test.initial_state = CreateRandomState(40000);
        test.initial_state.PC = 0x06004000;
        test.code = {
            sh2::MOV_I(5, 0),      // MOV #5, R0
            sh2::MOV_I(10, 1),     // MOV #10, R1
            sh2::ADD(1, 0)         // ADD R1, R0 (R0 = 15)
        };
        tests.push_back(test);
    }
    
    // Pattern 2: Register Shuffle
    {
        TestCase test;
        test.name = "pattern_register_shuffle";
        test.description = "Register shuffling pattern";
        test.initial_state = CreateRandomState(40001);
        test.initial_state.PC = 0x06004000;
        test.code = {
            sh2::MOV_I(1, 0),      // R0 = 1
            sh2::MOV_I(2, 1),      // R1 = 2
            sh2::MOV_I(3, 2),      // R2 = 3
            sh2::MOV_R(0, 3),      // R3 = R0
            sh2::MOV_R(1, 4),      // R4 = R1
            sh2::MOV_R(2, 5)       // R5 = R2
        };
        tests.push_back(test);
    }
    
    // Pattern 3: Arithmetic Chain
    {
        TestCase test;
        test.name = "pattern_arithmetic_chain";
        test.description = "Chained arithmetic operations";
        test.initial_state = CreateRandomState(40002);
        test.initial_state.PC = 0x06004000;
        test.code = {
            sh2::MOV_I(10, 0),     // R0 = 10
            sh2::ADD_I(5, 0),      // R0 = 15
            sh2::ADD_I(3, 0),      // R0 = 18
            sh2::SUB(1, 0),        // R0 = R0 - R1
            sh2::AND(2, 0)         // R0 = R0 & R2
        };
        tests.push_back(test);
    }
    
    // Pattern 4: Bit Manipulation
    {
        TestCase test;
        test.name = "pattern_bit_manipulation";
        test.description = "Shift and logic operations";
        test.initial_state = CreateRandomState(40003);
        test.initial_state.PC = 0x06004000;
        test.code = {
            sh2::MOV_I(0xFF, 0),   // R0 = 0xFF
            sh2::SHLL(0),          // R0 = 0x1FE
            sh2::SHLL(0),          // R0 = 0x3FC
            sh2::SHLR(0),          // R0 = 0x1FE
            sh2::AND(1, 0)         // R0 = R0 & R1
        };
        tests.push_back(test);
    }
    
    // Pattern 5: Register Dependency Chain
    {
        TestCase test;
        test.name = "pattern_dependency_chain";
        test.description = "Instructions with register dependencies";
        test.initial_state = CreateRandomState(40004);
        test.initial_state.PC = 0x06004000;
        test.code = {
            sh2::MOV_I(1, 0),      // R0 = 1
            sh2::ADD(0, 0),        // R0 = R0 + R0 = 2
            sh2::ADD(0, 0),        // R0 = R0 + R0 = 4
            sh2::ADD(0, 0),        // R0 = R0 + R0 = 8
            sh2::ADD(0, 0)         // R0 = R0 + R0 = 16
        };
        tests.push_back(test);
    }
    
    // Pattern 6: Independent Instructions
    {
        TestCase test;
        test.name = "pattern_independent";
        test.description = "Independent instructions (no dependencies)";
        test.initial_state = CreateRandomState(40005);
        test.initial_state.PC = 0x06004000;
        test.code = {
            sh2::MOV_I(1, 0),      // R0 = 1
            sh2::MOV_I(2, 1),      // R1 = 2
            sh2::MOV_I(3, 2),      // R2 = 3
            sh2::MOV_I(4, 3),      // R3 = 4
            sh2::MOV_I(5, 4)       // R4 = 5
        };
        tests.push_back(test);
    }
    
    // Pattern 7: Long Sequence
    {
        TestCase test;
        test.name = "pattern_long_sequence";
        test.description = "Long instruction sequence (20 instructions)";
        test.initial_state = CreateRandomState(40006);
        test.initial_state.PC = 0x06004000;
        
        for (int i = 0; i < 10; i++) {
            test.code.push_back(sh2::MOV_I(i, i % 8));
            test.code.push_back(sh2::ADD_I(1, i % 8));
        }
        
        tests.push_back(test);
    }
    
    // Pattern 8: Alternating Operations
    {
        TestCase test;
        test.name = "pattern_alternating";
        test.description = "Alternating ALU and MOV operations";
        test.initial_state = CreateRandomState(40007);
        test.initial_state.PC = 0x06004000;
        test.code = {
            sh2::MOV_I(10, 0),
            sh2::ADD_I(5, 0),
            sh2::MOV_R(0, 1),
            sh2::SUB(1, 0),
            sh2::MOV_R(0, 2),
            sh2::AND(2, 0),
            sh2::MOV_R(0, 3),
            sh2::OR(3, 0)
        };
        tests.push_back(test);
    }
    
    return tests;
}

} // namespace brimir::jit::test

