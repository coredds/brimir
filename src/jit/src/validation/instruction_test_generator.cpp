/**
 * @file instruction_test_generator.cpp
 * @brief Generate test cases for individual SH-2 instructions
 * 
 * This file contains the test case generators for Level 1 testing:
 * exhaustive coverage of all SH-2 instructions with various operand combinations.
 */

#include "../../include/jit_test_framework.hpp"
#include <random>
#include <algorithm>

namespace brimir::jit::test {

// -----------------------------------------------------------------------------
// SH-2 Instruction Encoding Helpers
// -----------------------------------------------------------------------------

namespace sh2 {

// Instruction encoding functions for test generation

/**
 * @brief Encode NOP instruction
 */
inline uint16_t NOP() {
    return 0x0009;
}

/**
 * @brief Encode MOV Rm, Rn
 */
inline uint16_t MOV_R(uint8_t m, uint8_t n) {
    return 0x6003 | (n << 8) | (m << 4);
}

/**
 * @brief Encode MOV #imm, Rn
 */
inline uint16_t MOV_I(uint8_t imm, uint8_t n) {
    return 0xE000 | (n << 8) | imm;
}

/**
 * @brief Encode MOV.L @Rm, Rn
 */
inline uint16_t MOVL_L(uint8_t m, uint8_t n) {
    return 0x6002 | (n << 8) | (m << 4);
}

/**
 * @brief Encode MOV.L Rm, @Rn
 */
inline uint16_t MOVL_S(uint8_t m, uint8_t n) {
    return 0x2002 | (n << 8) | (m << 4);
}

/**
 * @brief Encode ADD Rm, Rn
 */
inline uint16_t ADD(uint8_t m, uint8_t n) {
    return 0x300C | (n << 8) | (m << 4);
}

/**
 * @brief Encode ADD #imm, Rn
 */
inline uint16_t ADD_I(uint8_t imm, uint8_t n) {
    return 0x7000 | (n << 8) | imm;
}

/**
 * @brief Encode ADDC Rm, Rn (add with carry)
 */
inline uint16_t ADDC(uint8_t m, uint8_t n) {
    return 0x300E | (n << 8) | (m << 4);
}

/**
 * @brief Encode SUB Rm, Rn
 */
inline uint16_t SUB(uint8_t m, uint8_t n) {
    return 0x3008 | (n << 8) | (m << 4);
}

/**
 * @brief Encode SUBC Rm, Rn (subtract with carry)
 */
inline uint16_t SUBC(uint8_t m, uint8_t n) {
    return 0x300A | (n << 8) | (m << 4);
}

/**
 * @brief Encode AND Rm, Rn
 */
inline uint16_t AND(uint8_t m, uint8_t n) {
    return 0x2009 | (n << 8) | (m << 4);
}

/**
 * @brief Encode OR Rm, Rn
 */
inline uint16_t OR(uint8_t m, uint8_t n) {
    return 0x200B | (n << 8) | (m << 4);
}

/**
 * @brief Encode XOR Rm, Rn
 */
inline uint16_t XOR(uint8_t m, uint8_t n) {
    return 0x200A | (n << 8) | (m << 4);
}

/**
 * @brief Encode NOT Rm, Rn
 */
inline uint16_t NOT(uint8_t m, uint8_t n) {
    return 0x6007 | (n << 8) | (m << 4);
}

/**
 * @brief Encode SHLL Rn (shift left logical)
 */
inline uint16_t SHLL(uint8_t n) {
    return 0x4000 | (n << 8);
}

/**
 * @brief Encode SHLR Rn (shift right logical)
 */
inline uint16_t SHLR(uint8_t n) {
    return 0x4001 | (n << 8);
}

/**
 * @brief Encode SHAL Rn (shift left arithmetic)
 */
inline uint16_t SHAL(uint8_t n) {
    return 0x4020 | (n << 8);
}

/**
 * @brief Encode SHAR Rn (shift right arithmetic)
 */
inline uint16_t SHAR(uint8_t n) {
    return 0x4021 | (n << 8);
}

/**
 * @brief Encode CMP/EQ Rm, Rn
 */
inline uint16_t CMP_EQ(uint8_t m, uint8_t n) {
    return 0x3000 | (n << 8) | (m << 4);
}

/**
 * @brief Encode CMP/GT Rm, Rn (signed)
 */
inline uint16_t CMP_GT(uint8_t m, uint8_t n) {
    return 0x3007 | (n << 8) | (m << 4);
}

/**
 * @brief Encode CMP/HI Rm, Rn (unsigned)
 */
inline uint16_t CMP_HI(uint8_t m, uint8_t n) {
    return 0x3006 | (n << 8) | (m << 4);
}

/**
 * @brief Encode BT label (branch if T=1)
 * @param disp Displacement (signed 8-bit)
 */
inline uint16_t BT(int8_t disp) {
    return 0x8900 | (disp & 0xFF);
}

/**
 * @brief Encode BF label (branch if T=0)
 * @param disp Displacement (signed 8-bit)
 */
inline uint16_t BF(int8_t disp) {
    return 0x8B00 | (disp & 0xFF);
}

/**
 * @brief Encode RTS (return from subroutine)
 */
inline uint16_t RTS() {
    return 0x000B;
}

} // namespace sh2

// -----------------------------------------------------------------------------
// InstructionTestGenerator Implementation
// -----------------------------------------------------------------------------

std::vector<TestCase> InstructionTestGenerator::GenerateForOpcode(uint16_t opcode, size_t count) {
    std::vector<TestCase> tests;
    std::mt19937 rng(opcode);  // Deterministic based on opcode
    
    for (size_t i = 0; i < count; i++) {
        TestCase test;
        test.name = "opcode_" + std::to_string(opcode) + "_var_" + std::to_string(i);
        test.description = "Test opcode 0x" + std::to_string(opcode) + " with random state";
        
        // Random initial state
        test.initial_state = CreateRandomState(rng());
        test.initial_state.PC = 0x06004000;  // Valid PC in RAM
        
        // Single instruction
        test.code = {opcode};
        
        tests.push_back(test);
    }
    
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateForInstruction(const std::string& mnemonic) {
    std::vector<TestCase> tests;
    
    // Route to specific instruction generators based on mnemonic
    if (mnemonic == "NOP") {
        return GenerateNOPTests();
    } else if (mnemonic == "MOV") {
        return GenerateMOVTests();
    } else if (mnemonic == "ADD") {
        return GenerateADDTests();
    } else if (mnemonic == "SUB") {
        return GenerateSUBTests();
    } else if (mnemonic == "AND") {
        return GenerateANDTests();
    } else if (mnemonic == "OR") {
        return GenerateORTests();
    } else if (mnemonic == "XOR") {
        return GenerateXORTests();
    } else if (mnemonic == "NOT") {
        return GenerateNOTTests();
    } else if (mnemonic == "SHLL") {
        return GenerateSHIFTTests();
    } else if (mnemonic == "CMP") {
        return GenerateCMPTests();
    }
    
    // TODO: Add more instructions
    
    return tests;
}

// -----------------------------------------------------------------------------
// Specific Instruction Test Generators
// -----------------------------------------------------------------------------

std::vector<TestCase> InstructionTestGenerator::GenerateNOPTests() {
    std::vector<TestCase> tests;
    
    TestCase test;
    test.name = "NOP_basic";
    test.description = "NOP should not modify any state except PC and cycles";
    
    test.initial_state = CreateRandomState(12345);
    test.initial_state.PC = 0x06004000;
    
    test.code = {sh2::NOP()};
    
    // Custom validator: ensure nothing changed except PC
    test.custom_validator = [](const SH2State& before, const SH2State& after) -> std::string {
        // PC should advance by 2
        if (after.PC != before.PC + 2) {
            return "PC should advance by 2, but went from " + 
                std::to_string(before.PC) + " to " + std::to_string(after.PC);
        }
        
        // All registers should be unchanged
        for (int i = 0; i < 16; i++) {
            if (after.R[i] != before.R[i]) {
                return "R" + std::to_string(i) + " was modified";
            }
        }
        
        // All other state unchanged
        if (after.SR != before.SR || after.T != before.T) {
            return "SR/T-bit was modified";
        }
        
        return "";  // Valid
    };
    
    tests.push_back(test);
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateMOVTests() {
    std::vector<TestCase> tests;
    
    // Test MOV Rm, Rn for all register combinations
    for (uint8_t src = 0; src < 16; src++) {
        for (uint8_t dst = 0; dst < 16; dst++) {
            TestCase test;
            test.name = "MOV_R" + std::to_string(src) + "_R" + std::to_string(dst);
            test.description = "MOV R" + std::to_string(src) + ", R" + std::to_string(dst);
            
            test.initial_state = CreateRandomState(src * 16 + dst);
            test.initial_state.PC = 0x06004000;
            
            test.code = {sh2::MOV_R(src, dst)};
            
            tests.push_back(test);
        }
    }
    
    // Test MOV #imm, Rn for various immediates
    for (uint8_t reg = 0; reg < 16; reg++) {
        for (uint8_t imm : {0, 1, 0x7F, 0x80, 0xFF}) {
            TestCase test;
            test.name = "MOV_IMM_" + std::to_string(imm) + "_R" + std::to_string(reg);
            test.description = "MOV #" + std::to_string(imm) + ", R" + std::to_string(reg);
            
            test.initial_state = CreateRandomState(reg * 256 + imm);
            test.initial_state.PC = 0x06004000;
            
            test.code = {sh2::MOV_I(imm, reg)};
            
            tests.push_back(test);
        }
    }
    
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateADDTests() {
    std::vector<TestCase> tests;
    
    // Test ADD Rm, Rn with various value combinations
    for (uint8_t src = 0; src < 4; src++) {  // Test subset of registers
        for (uint8_t dst = 0; dst < 4; dst++) {
            TestCase test;
            test.name = "ADD_R" + std::to_string(src) + "_R" + std::to_string(dst);
            test.description = "ADD R" + std::to_string(src) + ", R" + std::to_string(dst);
            
            test.initial_state = CreateRandomState(src * 16 + dst);
            test.initial_state.PC = 0x06004000;
            
            test.code = {sh2::ADD(src, dst)};
            
            tests.push_back(test);
            
            // Test overflow cases
            TestCase overflow_test = test;
            overflow_test.name += "_overflow";
            overflow_test.description += " (overflow case)";
            overflow_test.initial_state.R[src] = 0xFFFFFFFF;
            overflow_test.initial_state.R[dst] = 0x00000001;
            
            tests.push_back(overflow_test);
        }
    }
    
    // Test ADD #imm, Rn
    for (uint8_t reg = 0; reg < 8; reg++) {
        for (int8_t imm : {0, 1, -1, 127, -128}) {
            TestCase test;
            test.name = "ADD_IMM_" + std::to_string(imm) + "_R" + std::to_string(reg);
            test.description = "ADD #" + std::to_string(imm) + ", R" + std::to_string(reg);
            
            test.initial_state = CreateRandomState(reg * 256 + (imm & 0xFF));
            test.initial_state.PC = 0x06004000;
            
            test.code = {sh2::ADD_I(imm & 0xFF, reg)};
            
            tests.push_back(test);
        }
    }
    
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateSUBTests() {
    // Similar to ADD but for SUB
    std::vector<TestCase> tests;
    
    for (uint8_t src = 0; src < 4; src++) {
        for (uint8_t dst = 0; dst < 4; dst++) {
            TestCase test;
            test.name = "SUB_R" + std::to_string(src) + "_R" + std::to_string(dst);
            test.description = "SUB R" + std::to_string(src) + ", R" + std::to_string(dst);
            
            test.initial_state = CreateRandomState(src * 16 + dst + 1000);
            test.initial_state.PC = 0x06004000;
            
            test.code = {sh2::SUB(src, dst)};
            
            tests.push_back(test);
        }
    }
    
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateANDTests() {
    std::vector<TestCase> tests;
    
    for (uint8_t src = 0; src < 4; src++) {
        for (uint8_t dst = 0; dst < 4; dst++) {
            TestCase test;
            test.name = "AND_R" + std::to_string(src) + "_R" + std::to_string(dst);
            test.description = "AND R" + std::to_string(src) + ", R" + std::to_string(dst);
            
            test.initial_state = CreateRandomState(src * 16 + dst + 2000);
            test.initial_state.PC = 0x06004000;
            
            test.code = {sh2::AND(src, dst)};
            
            tests.push_back(test);
        }
    }
    
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateORTests() {
    std::vector<TestCase> tests;
    
    for (uint8_t src = 0; src < 4; src++) {
        for (uint8_t dst = 0; dst < 4; dst++) {
            TestCase test;
            test.name = "OR_R" + std::to_string(src) + "_R" + std::to_string(dst);
            test.description = "OR R" + std::to_string(src) + ", R" + std::to_string(dst);
            
            test.initial_state = CreateRandomState(src * 16 + dst + 3000);
            test.initial_state.PC = 0x06004000;
            
            test.code = {sh2::OR(src, dst)};
            
            tests.push_back(test);
        }
    }
    
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateXORTests() {
    std::vector<TestCase> tests;
    
    for (uint8_t src = 0; src < 4; src++) {
        for (uint8_t dst = 0; dst < 4; dst++) {
            TestCase test;
            test.name = "XOR_R" + std::to_string(src) + "_R" + std::to_string(dst);
            test.description = "XOR R" + std::to_string(src) + ", R" + std::to_string(dst);
            
            test.initial_state = CreateRandomState(src * 16 + dst + 4000);
            test.initial_state.PC = 0x06004000;
            
            test.code = {sh2::XOR(src, dst)};
            
            tests.push_back(test);
        }
    }
    
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateNOTTests() {
    std::vector<TestCase> tests;
    
    for (uint8_t src = 0; src < 8; src++) {
        for (uint8_t dst = 0; dst < 8; dst++) {
            TestCase test;
            test.name = "NOT_R" + std::to_string(src) + "_R" + std::to_string(dst);
            test.description = "NOT R" + std::to_string(src) + ", R" + std::to_string(dst);
            
            test.initial_state = CreateRandomState(src * 16 + dst + 5000);
            test.initial_state.PC = 0x06004000;
            
            test.code = {sh2::NOT(src, dst)};
            
            tests.push_back(test);
        }
    }
    
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateSHIFTTests() {
    std::vector<TestCase> tests;
    
    // Test all shift instructions
    for (uint8_t reg = 0; reg < 8; reg++) {
        // SHLL
        {
            TestCase test;
            test.name = "SHLL_R" + std::to_string(reg);
            test.description = "SHLL R" + std::to_string(reg);
            test.initial_state = CreateRandomState(reg + 6000);
            test.initial_state.PC = 0x06004000;
            test.code = {sh2::SHLL(reg)};
            tests.push_back(test);
        }
        
        // SHLR
        {
            TestCase test;
            test.name = "SHLR_R" + std::to_string(reg);
            test.description = "SHLR R" + std::to_string(reg);
            test.initial_state = CreateRandomState(reg + 7000);
            test.initial_state.PC = 0x06004000;
            test.code = {sh2::SHLR(reg)};
            tests.push_back(test);
        }
        
        // SHAL
        {
            TestCase test;
            test.name = "SHAL_R" + std::to_string(reg);
            test.description = "SHAL R" + std::to_string(reg);
            test.initial_state = CreateRandomState(reg + 8000);
            test.initial_state.PC = 0x06004000;
            test.code = {sh2::SHAL(reg)};
            tests.push_back(test);
        }
        
        // SHAR
        {
            TestCase test;
            test.name = "SHAR_R" + std::to_string(reg);
            test.description = "SHAR R" + std::to_string(reg);
            test.initial_state = CreateRandomState(reg + 9000);
            test.initial_state.PC = 0x06004000;
            test.code = {sh2::SHAR(reg)};
            tests.push_back(test);
        }
    }
    
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateCMPTests() {
    std::vector<TestCase> tests;
    
    for (uint8_t src = 0; src < 4; src++) {
        for (uint8_t dst = 0; dst < 4; dst++) {
            // CMP/EQ
            {
                TestCase test;
                test.name = "CMP_EQ_R" + std::to_string(src) + "_R" + std::to_string(dst);
                test.description = "CMP/EQ R" + std::to_string(src) + ", R" + std::to_string(dst);
                test.initial_state = CreateRandomState(src * 16 + dst + 10000);
                test.initial_state.PC = 0x06004000;
                test.code = {sh2::CMP_EQ(src, dst)};
                tests.push_back(test);
            }
            
            // CMP/GT
            {
                TestCase test;
                test.name = "CMP_GT_R" + std::to_string(src) + "_R" + std::to_string(dst);
                test.description = "CMP/GT R" + std::to_string(src) + ", R" + std::to_string(dst);
                test.initial_state = CreateRandomState(src * 16 + dst + 11000);
                test.initial_state.PC = 0x06004000;
                test.code = {sh2::CMP_GT(src, dst)};
                tests.push_back(test);
            }
            
            // CMP/HI
            {
                TestCase test;
                test.name = "CMP_HI_R" + std::to_string(src) + "_R" + std::to_string(dst);
                test.description = "CMP/HI R" + std::to_string(src) + ", R" + std::to_string(dst);
                test.initial_state = CreateRandomState(src * 16 + dst + 12000);
                test.initial_state.PC = 0x06004000;
                test.code = {sh2::CMP_HI(src, dst)};
                tests.push_back(test);
            }
        }
    }
    
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateAllInstructionTests() {
    std::vector<TestCase> all_tests;
    
    // Generate tests for each instruction family
    std::vector<std::string> instructions = {
        "NOP", "MOV", "ADD", "SUB", "AND", "OR", "XOR", "NOT",
        "SHLL", "CMP"
        // TODO: Add more instructions
    };
    
    for (const auto& instr : instructions) {
        auto tests = GenerateForInstruction(instr);
        all_tests.insert(all_tests.end(), tests.begin(), tests.end());
    }
    
    return all_tests;
}

} // namespace brimir::jit::test

