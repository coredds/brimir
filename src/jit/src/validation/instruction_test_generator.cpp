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

/**
 * @brief Encode JSR @Rn (jump to subroutine)
 */
inline uint16_t JSR(uint8_t n) {
    return 0x400B | (n << 8);
}

/**
 * @brief Encode JMP @Rn
 */
inline uint16_t JMP(uint8_t n) {
    return 0x402B | (n << 8);
}

/**
 * @brief Encode BRAF Rn (branch far)
 */
inline uint16_t BRAF(uint8_t n) {
    return 0x0023 | (n << 8);
}

/**
 * @brief Encode BSRF Rn (branch to subroutine far)
 */
inline uint16_t BSRF(uint8_t n) {
    return 0x0003 | (n << 8);
}

/**
 * @brief Encode TST Rm, Rn
 */
inline uint16_t TST(uint8_t m, uint8_t n) {
    return 0x2008 | (n << 8) | (m << 4);
}

/**
 * @brief Encode NEG Rm, Rn
 */
inline uint16_t NEG(uint8_t m, uint8_t n) {
    return 0x600B | (n << 8) | (m << 4);
}

/**
 * @brief Encode NEGC Rm, Rn (negate with carry)
 */
inline uint16_t NEGC(uint8_t m, uint8_t n) {
    return 0x600A | (n << 8) | (m << 4);
}

/**
 * @brief Encode EXTS.B Rm, Rn (sign extend byte)
 */
inline uint16_t EXTSB(uint8_t m, uint8_t n) {
    return 0x600E | (n << 8) | (m << 4);
}

/**
 * @brief Encode EXTS.W Rm, Rn (sign extend word)
 */
inline uint16_t EXTSW(uint8_t m, uint8_t n) {
    return 0x600F | (n << 8) | (m << 4);
}

/**
 * @brief Encode EXTU.B Rm, Rn (zero extend byte)
 */
inline uint16_t EXTUB(uint8_t m, uint8_t n) {
    return 0x600C | (n << 8) | (m << 4);
}

/**
 * @brief Encode EXTU.W Rm, Rn (zero extend word)
 */
inline uint16_t EXTUW(uint8_t m, uint8_t n) {
    return 0x600D | (n << 8) | (m << 4);
}

/**
 * @brief Encode SWAP.B Rm, Rn (swap bytes)
 */
inline uint16_t SWAPB(uint8_t m, uint8_t n) {
    return 0x6008 | (n << 8) | (m << 4);
}

/**
 * @brief Encode SWAP.W Rm, Rn (swap words)
 */
inline uint16_t SWAPW(uint8_t m, uint8_t n) {
    return 0x6009 | (n << 8) | (m << 4);
}

/**
 * @brief Encode XTRCT Rm, Rn (extract)
 */
inline uint16_t XTRCT(uint8_t m, uint8_t n) {
    return 0x200D | (n << 8) | (m << 4);
}

/**
 * @brief Encode MUL.L Rm, Rn
 */
inline uint16_t MULL(uint8_t m, uint8_t n) {
    return 0x0007 | (n << 8) | (m << 4);
}

/**
 * @brief Encode MULS.W Rm, Rn (signed multiply)
 */
inline uint16_t MULSW(uint8_t m, uint8_t n) {
    return 0x200F | (n << 8) | (m << 4);
}

/**
 * @brief Encode MULU.W Rm, Rn (unsigned multiply)
 */
inline uint16_t MULUW(uint8_t m, uint8_t n) {
    return 0x200E | (n << 8) | (m << 4);
}

/**
 * @brief Encode DMULS.L Rm, Rn (signed 64-bit multiply)
 */
inline uint16_t DMULSL(uint8_t m, uint8_t n) {
    return 0x300D | (n << 8) | (m << 4);
}

/**
 * @brief Encode DMULU.L Rm, Rn (unsigned 64-bit multiply)
 */
inline uint16_t DMULUL(uint8_t m, uint8_t n) {
    return 0x3005 | (n << 8) | (m << 4);
}

/**
 * @brief Encode DIV0S Rm, Rn
 */
inline uint16_t DIV0S(uint8_t m, uint8_t n) {
    return 0x2007 | (n << 8) | (m << 4);
}

/**
 * @brief Encode DIV0U
 */
inline uint16_t DIV0U() {
    return 0x0019;
}

/**
 * @brief Encode DIV1 Rm, Rn
 */
inline uint16_t DIV1(uint8_t m, uint8_t n) {
    return 0x3004 | (n << 8) | (m << 4);
}

/**
 * @brief Encode DT Rn (decrement and test)
 */
inline uint16_t DT(uint8_t n) {
    return 0x4010 | (n << 8);
}

/**
 * @brief Encode ROTL Rn (rotate left)
 */
inline uint16_t ROTL(uint8_t n) {
    return 0x4004 | (n << 8);
}

/**
 * @brief Encode ROTR Rn (rotate right)
 */
inline uint16_t ROTR(uint8_t n) {
    return 0x4005 | (n << 8);
}

/**
 * @brief Encode ROTCL Rn (rotate left through carry)
 */
inline uint16_t ROTCL(uint8_t n) {
    return 0x4024 | (n << 8);
}

/**
 * @brief Encode ROTCR Rn (rotate right through carry)
 */
inline uint16_t ROTCR(uint8_t n) {
    return 0x4025 | (n << 8);
}

/**
 * @brief Encode SETT (set T-bit)
 */
inline uint16_t SETT() {
    return 0x0018;
}

/**
 * @brief Encode CLRT (clear T-bit)
 */
inline uint16_t CLRT() {
    return 0x0008;
}

/**
 * @brief Encode CLRMAC (clear MAC register)
 */
inline uint16_t CLRMAC() {
    return 0x0028;
}

/**
 * @brief Encode MOVT Rn (move T-bit to register)
 */
inline uint16_t MOVT(uint8_t n) {
    return 0x0029 | (n << 8);
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

std::vector<TestCase> InstructionTestGenerator::GenerateExtensionTests() {
    std::vector<TestCase> tests;
    
    // Test sign/zero extension instructions
    for (uint8_t src = 0; src < 8; src++) {
        for (uint8_t dst = 0; dst < 8; dst++) {
            // EXTS.B
            {
                TestCase test;
                test.name = "EXTS_B_R" + std::to_string(src) + "_R" + std::to_string(dst);
                test.description = "EXTS.B R" + std::to_string(src) + ", R" + std::to_string(dst);
                test.initial_state = CreateRandomState(src * 16 + dst + 13000);
                test.initial_state.PC = 0x06004000;
                test.code = {sh2::EXTSB(src, dst)};
                tests.push_back(test);
            }
            
            // EXTS.W
            {
                TestCase test;
                test.name = "EXTS_W_R" + std::to_string(src) + "_R" + std::to_string(dst);
                test.description = "EXTS.W R" + std::to_string(src) + ", R" + std::to_string(dst);
                test.initial_state = CreateRandomState(src * 16 + dst + 14000);
                test.initial_state.PC = 0x06004000;
                test.code = {sh2::EXTSW(src, dst)};
                tests.push_back(test);
            }
            
            // EXTU.B
            {
                TestCase test;
                test.name = "EXTU_B_R" + std::to_string(src) + "_R" + std::to_string(dst);
                test.description = "EXTU.B R" + std::to_string(src) + ", R" + std::to_string(dst);
                test.initial_state = CreateRandomState(src * 16 + dst + 15000);
                test.initial_state.PC = 0x06004000;
                test.code = {sh2::EXTUB(src, dst)};
                tests.push_back(test);
            }
            
            // EXTU.W
            {
                TestCase test;
                test.name = "EXTU_W_R" + std::to_string(src) + "_R" + std::to_string(dst);
                test.description = "EXTU.W R" + std::to_string(src) + ", R" + std::to_string(dst);
                test.initial_state = CreateRandomState(src * 16 + dst + 16000);
                test.initial_state.PC = 0x06004000;
                test.code = {sh2::EXTUW(src, dst)};
                tests.push_back(test);
            }
        }
    }
    
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateSwapTests() {
    std::vector<TestCase> tests;
    
    for (uint8_t src = 0; src < 8; src++) {
        for (uint8_t dst = 0; dst < 8; dst++) {
            // SWAP.B
            {
                TestCase test;
                test.name = "SWAP_B_R" + std::to_string(src) + "_R" + std::to_string(dst);
                test.description = "SWAP.B R" + std::to_string(src) + ", R" + std::to_string(dst);
                test.initial_state = CreateRandomState(src * 16 + dst + 17000);
                test.initial_state.PC = 0x06004000;
                test.code = {sh2::SWAPB(src, dst)};
                tests.push_back(test);
            }
            
            // SWAP.W
            {
                TestCase test;
                test.name = "SWAP_W_R" + std::to_string(src) + "_R" + std::to_string(dst);
                test.description = "SWAP.W R" + std::to_string(src) + ", R" + std::to_string(dst);
                test.initial_state = CreateRandomState(src * 16 + dst + 18000);
                test.initial_state.PC = 0x06004000;
                test.code = {sh2::SWAPW(src, dst)};
                tests.push_back(test);
            }
            
            // XTRCT
            {
                TestCase test;
                test.name = "XTRCT_R" + std::to_string(src) + "_R" + std::to_string(dst);
                test.description = "XTRCT R" + std::to_string(src) + ", R" + std::to_string(dst);
                test.initial_state = CreateRandomState(src * 16 + dst + 19000);
                test.initial_state.PC = 0x06004000;
                test.code = {sh2::XTRCT(src, dst)};
                tests.push_back(test);
            }
        }
    }
    
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateMultiplyTests() {
    std::vector<TestCase> tests;
    
    for (uint8_t src = 0; src < 4; src++) {
        for (uint8_t dst = 0; dst < 4; dst++) {
            // MUL.L
            {
                TestCase test;
                test.name = "MUL_L_R" + std::to_string(src) + "_R" + std::to_string(dst);
                test.description = "MUL.L R" + std::to_string(src) + ", R" + std::to_string(dst);
                test.initial_state = CreateRandomState(src * 16 + dst + 20000);
                test.initial_state.PC = 0x06004000;
                test.code = {sh2::MULL(src, dst)};
                tests.push_back(test);
            }
            
            // MULS.W
            {
                TestCase test;
                test.name = "MULS_W_R" + std::to_string(src) + "_R" + std::to_string(dst);
                test.description = "MULS.W R" + std::to_string(src) + ", R" + std::to_string(dst);
                test.initial_state = CreateRandomState(src * 16 + dst + 21000);
                test.initial_state.PC = 0x06004000;
                test.code = {sh2::MULSW(src, dst)};
                tests.push_back(test);
            }
            
            // MULU.W
            {
                TestCase test;
                test.name = "MULU_W_R" + std::to_string(src) + "_R" + std::to_string(dst);
                test.description = "MULU.W R" + std::to_string(src) + ", R" + std::to_string(dst);
                test.initial_state = CreateRandomState(src * 16 + dst + 22000);
                test.initial_state.PC = 0x06004000;
                test.code = {sh2::MULUW(src, dst)};
                tests.push_back(test);
            }
            
            // DMULS.L
            {
                TestCase test;
                test.name = "DMULS_L_R" + std::to_string(src) + "_R" + std::to_string(dst);
                test.description = "DMULS.L R" + std::to_string(src) + ", R" + std::to_string(dst);
                test.initial_state = CreateRandomState(src * 16 + dst + 23000);
                test.initial_state.PC = 0x06004000;
                test.code = {sh2::DMULSL(src, dst)};
                tests.push_back(test);
            }
            
            // DMULU.L
            {
                TestCase test;
                test.name = "DMULU_L_R" + std::to_string(src) + "_R" + std::to_string(dst);
                test.description = "DMULU.L R" + std::to_string(src) + ", R" + std::to_string(dst);
                test.initial_state = CreateRandomState(src * 16 + dst + 24000);
                test.initial_state.PC = 0x06004000;
                test.code = {sh2::DMULUL(src, dst)};
                tests.push_back(test);
            }
        }
    }
    
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateRotateTests() {
    std::vector<TestCase> tests;
    
    for (uint8_t reg = 0; reg < 8; reg++) {
        // ROTL
        {
            TestCase test;
            test.name = "ROTL_R" + std::to_string(reg);
            test.description = "ROTL R" + std::to_string(reg);
            test.initial_state = CreateRandomState(reg + 25000);
            test.initial_state.PC = 0x06004000;
            test.code = {sh2::ROTL(reg)};
            tests.push_back(test);
        }
        
        // ROTR
        {
            TestCase test;
            test.name = "ROTR_R" + std::to_string(reg);
            test.description = "ROTR R" + std::to_string(reg);
            test.initial_state = CreateRandomState(reg + 26000);
            test.initial_state.PC = 0x06004000;
            test.code = {sh2::ROTR(reg)};
            tests.push_back(test);
        }
        
        // ROTCL
        {
            TestCase test;
            test.name = "ROTCL_R" + std::to_string(reg);
            test.description = "ROTCL R" + std::to_string(reg);
            test.initial_state = CreateRandomState(reg + 27000);
            test.initial_state.PC = 0x06004000;
            test.code = {sh2::ROTCL(reg)};
            tests.push_back(test);
        }
        
        // ROTCR
        {
            TestCase test;
            test.name = "ROTCR_R" + std::to_string(reg);
            test.description = "ROTCR R" + std::to_string(reg);
            test.initial_state = CreateRandomState(reg + 28000);
            test.initial_state.PC = 0x06004000;
            test.code = {sh2::ROTCR(reg)};
            tests.push_back(test);
        }
    }
    
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateFlagTests() {
    std::vector<TestCase> tests;
    
    // SETT
    {
        TestCase test;
        test.name = "SETT";
        test.description = "SETT - Set T-bit to 1";
        test.initial_state = CreateRandomState(29000);
        test.initial_state.PC = 0x06004000;
        test.initial_state.T = false;  // Start with T=0
        test.code = {sh2::SETT()};
        tests.push_back(test);
    }
    
    // CLRT
    {
        TestCase test;
        test.name = "CLRT";
        test.description = "CLRT - Clear T-bit to 0";
        test.initial_state = CreateRandomState(30000);
        test.initial_state.PC = 0x06004000;
        test.initial_state.T = true;  // Start with T=1
        test.code = {sh2::CLRT()};
        tests.push_back(test);
    }
    
    // MOVT
    for (uint8_t reg = 0; reg < 8; reg++) {
        TestCase test;
        test.name = "MOVT_R" + std::to_string(reg);
        test.description = "MOVT R" + std::to_string(reg);
        test.initial_state = CreateRandomState(reg + 31000);
        test.initial_state.PC = 0x06004000;
        test.initial_state.T = (reg & 1) != 0;  // Alternate T-bit
        test.code = {sh2::MOVT(reg)};
        tests.push_back(test);
    }
    
    return tests;
}

std::vector<TestCase> InstructionTestGenerator::GenerateAllInstructionTests() {
    std::vector<TestCase> all_tests;
    
    // Generate tests for each instruction family
    std::vector<std::string> instructions = {
        "NOP", "MOV", "ADD", "SUB", "AND", "OR", "XOR", "NOT",
        "SHLL", "CMP"
    };
    
    for (const auto& instr : instructions) {
        auto tests = GenerateForInstruction(instr);
        all_tests.insert(all_tests.end(), tests.begin(), tests.end());
    }
    
    // Add new instruction families
    auto ext_tests = GenerateExtensionTests();
    all_tests.insert(all_tests.end(), ext_tests.begin(), ext_tests.end());
    
    auto swap_tests = GenerateSwapTests();
    all_tests.insert(all_tests.end(), swap_tests.begin(), swap_tests.end());
    
    auto mul_tests = GenerateMultiplyTests();
    all_tests.insert(all_tests.end(), mul_tests.begin(), mul_tests.end());
    
    auto rot_tests = GenerateRotateTests();
    all_tests.insert(all_tests.end(), rot_tests.begin(), rot_tests.end());
    
    auto flag_tests = GenerateFlagTests();
    all_tests.insert(all_tests.end(), flag_tests.begin(), flag_tests.end());
    
    return all_tests;
}

} // namespace brimir::jit::test

