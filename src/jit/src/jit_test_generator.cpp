#include "../include/jit_validator.hpp"
#include "../include/sh2_spec.hpp"
#include <random>

namespace brimir::jit {

// Helper: Create a test with initial state
static InstructionTest CreateTest(const SH2InstructionSpec& spec, 
                                   uint16 encoded_instruction,
                                   const std::string& description) {
    InstructionTest test;
    test.instruction = encoded_instruction;
    test.mnemonic = spec.mnemonic;
    test.description = description;
    test.expected_cycles = spec.issue_cycles;
    test.sh2_address = 0x06004000; // Default test address in LWRAM
    
    // Initialize to known state
    for (int i = 0; i < 16; i++) {
        test.initial_state.R[i] = 0;
    }
    test.initial_state.PC = test.sh2_address;
    test.initial_state.PR = 0;
    test.initial_state.GBR = 0;
    test.initial_state.VBR = 0;
    test.initial_state.MACH = 0;
    test.initial_state.MACL = 0;
    test.initial_state.SR = 0;
    test.initial_state.T = false;
    test.initial_state.S = false;
    test.initial_state.IMASK = 0;
    test.initial_state.Q = false;
    test.initial_state.M = false;
    test.initial_state.cycles = 0;
    test.initial_state.in_delay_slot = false;
    
    return test;
}

InstructionTestSuite TestGenerator::GenerateInstructionTests(
    const std::string& mnemonic,
    uint16 opcode_pattern,
    uint16 opcode_mask) {
    
    auto spec_opt = SH2SpecDatabase::GetByMnemonic(mnemonic);
    if (!spec_opt) {
        return InstructionTestSuite("Unknown");
    }
    
    const auto& spec = *spec_opt;
    InstructionTestSuite suite(spec.category + " - " + spec.mnemonic);
    
    // Generate tests based on instruction type
    AddNormalTests(suite, mnemonic, opcode_pattern);
    AddEdgeTests(suite, mnemonic, opcode_pattern);
    AddFlagTests(suite, mnemonic, opcode_pattern);
    
    return suite;
}

void TestGenerator::AddNormalTests(InstructionTestSuite& suite,
                                    const std::string& mnemonic,
                                    uint16 pattern) {
    auto spec_opt = SH2SpecDatabase::GetByMnemonic(mnemonic);
    if (!spec_opt) return;
    const auto& spec = *spec_opt;
    
    // Generate tests based on instruction format
    if (mnemonic == "MOV" && spec.format == InstrFormat::TWO_REG) {
        // MOV Rm, Rn - Test 1: Normal case
        uint16 instr = 0x6023; // MOV R2, R3
        auto test = CreateTest(spec, instr, "MOV R2, R3 - normal case");
        test.initial_state.R[2] = 0x12345678;
        test.expected_state = test.initial_state;
        test.expected_state.R[3] = 0x12345678;
        test.expected_state.PC += 2;
        suite.AddTest(test);
        
        // Test 2: Move zero
        instr = 0x6013; // MOV R1, R3
        test = CreateTest(spec, instr, "MOV R1, R3 - move zero");
        test.initial_state.R[1] = 0;
        test.expected_state = test.initial_state;
        test.expected_state.R[3] = 0;
        test.expected_state.PC += 2;
        suite.AddTest(test);
        
        // Test 3: Move to same register (R0 = R0)
        instr = 0x6003; // MOV R0, R0
        test = CreateTest(spec, instr, "MOV R0, R0 - same register");
        test.initial_state.R[0] = 0xDEADBEEF;
        test.expected_state = test.initial_state;
        test.expected_state.R[0] = 0xDEADBEEF;
        test.expected_state.PC += 2;
        suite.AddTest(test);
    }
    else if (mnemonic == "MOV" && spec.format == InstrFormat::REG_IMM) {
        // MOV #imm, Rn - Test 1: Positive immediate
        uint16 instr = 0xE105; // MOV #5, R1
        auto test = CreateTest(spec, instr, "MOV #5, R1 - positive immediate");
        test.expected_state = test.initial_state;
        test.expected_state.R[1] = 5;
        test.expected_state.PC += 2;
        suite.AddTest(test);
        
        // Test 2: Negative immediate (sign extension)
        instr = 0xE1FF; // MOV #-1, R1
        test = CreateTest(spec, instr, "MOV #-1, R1 - negative immediate");
        test.expected_state = test.initial_state;
        test.expected_state.R[1] = 0xFFFFFFFF; // Sign-extended
        test.expected_state.PC += 2;
        suite.AddTest(test);
        
        // Test 3: Zero immediate
        instr = 0xE200; // MOV #0, R2
        test = CreateTest(spec, instr, "MOV #0, R2 - zero");
        test.expected_state = test.initial_state;
        test.expected_state.R[2] = 0;
        test.expected_state.PC += 2;
        suite.AddTest(test);
    }
    else if (mnemonic == "ADD" && spec.format == InstrFormat::TWO_REG) {
        // ADD Rm, Rn - Test 1: Normal addition
        uint16 instr = 0x312C; // ADD R2, R1
        auto test = CreateTest(spec, instr, "ADD R2, R1 - normal");
        test.initial_state.R[1] = 5;
        test.initial_state.R[2] = 3;
        test.expected_state = test.initial_state;
        test.expected_state.R[1] = 8;
        test.expected_state.PC += 2;
        suite.AddTest(test);
        
        // Test 2: Add zero
        instr = 0x310C; // ADD R0, R1
        test = CreateTest(spec, instr, "ADD R0, R1 - add zero");
        test.initial_state.R[0] = 0;
        test.initial_state.R[1] = 100;
        test.expected_state = test.initial_state;
        test.expected_state.R[1] = 100;
        test.expected_state.PC += 2;
        suite.AddTest(test);
        
        // Test 3: Result zero
        instr = 0x312C; // ADD R2, R1
        test = CreateTest(spec, instr, "ADD R2, R1 - result zero");
        test.initial_state.R[1] = 5;
        test.initial_state.R[2] = -5;
        test.expected_state = test.initial_state;
        test.expected_state.R[1] = 0;
        test.expected_state.PC += 2;
        suite.AddTest(test);
    }
    else if (mnemonic == "ADD" && spec.format == InstrFormat::REG_IMM) {
        // ADD #imm, Rn - Test 1: Positive immediate
        uint16 instr = 0x7105; // ADD #5, R1
        auto test = CreateTest(spec, instr, "ADD #5, R1");
        test.initial_state.R[1] = 10;
        test.expected_state = test.initial_state;
        test.expected_state.R[1] = 15;
        test.expected_state.PC += 2;
        suite.AddTest(test);
        
        // Test 2: Negative immediate
        instr = 0x71FF; // ADD #-1, R1
        test = CreateTest(spec, instr, "ADD #-1, R1");
        test.initial_state.R[1] = 10;
        test.expected_state = test.initial_state;
        test.expected_state.R[1] = 9;
        test.expected_state.PC += 2;
        suite.AddTest(test);
    }
}

void TestGenerator::AddEdgeTests(InstructionTestSuite& suite,
                                  const std::string& mnemonic,
                                  uint16 pattern) {
    auto spec_opt = SH2SpecDatabase::GetByMnemonic(mnemonic);
    if (!spec_opt) return;
    const auto& spec = *spec_opt;
    
    // Edge case: Maximum values
    if (mnemonic == "ADD" && spec.format == InstrFormat::TWO_REG) {
        uint16 instr = 0x312C; // ADD R2, R1
        auto test = CreateTest(spec, instr, "ADD R2, R1 - overflow");
        test.initial_state.R[1] = 0xFFFFFFFF;
        test.initial_state.R[2] = 1;
        test.expected_state = test.initial_state;
        test.expected_state.R[1] = 0; // Wraps around
        test.expected_state.PC += 2;
        // Note: T-bit NOT affected for ADD (only ADDC sets it)
        suite.AddTest(test);
    }
    
    // Edge case: Negative numbers
    if (mnemonic == "SUB" && spec.format == InstrFormat::TWO_REG) {
        uint16 instr = 0x3128; // SUB R2, R1
        auto test = CreateTest(spec, instr, "SUB R2, R1 - negative result");
        test.initial_state.R[1] = 5;
        test.initial_state.R[2] = 10;
        test.expected_state = test.initial_state;
        test.expected_state.R[1] = -5;
        test.expected_state.PC += 2;
        suite.AddTest(test);
    }
}

void TestGenerator::AddFlagTests(InstructionTestSuite& suite,
                                 const std::string& mnemonic,
                                 uint16 pattern) {
    auto spec_opt = SH2SpecDatabase::GetByMnemonic(mnemonic);
    if (!spec_opt) return;
    const auto& spec = *spec_opt;
    
    // Flag test: ADDC (affects T-bit)
    if (mnemonic == "ADDC") {
        // Test 1: No carry
        uint16 instr = 0x312E; // ADDC R2, R1
        auto test = CreateTest(spec, instr, "ADDC R2, R1 - no carry");
        test.initial_state.R[1] = 5;
        test.initial_state.R[2] = 3;
        test.initial_state.T = false;
        test.expected_state = test.initial_state;
        test.expected_state.R[1] = 8;
        test.expected_state.T = false;
        test.expected_state.PC += 2;
        suite.AddTest(test);
        
        // Test 2: With input carry
        test = CreateTest(spec, instr, "ADDC R2, R1 - with input carry");
        test.initial_state.R[1] = 5;
        test.initial_state.R[2] = 3;
        test.initial_state.T = true;
        test.expected_state = test.initial_state;
        test.expected_state.R[1] = 9; // 5 + 3 + 1
        test.expected_state.T = false;
        test.expected_state.PC += 2;
        suite.AddTest(test);
        
        // Test 3: Overflow generates carry
        test = CreateTest(spec, instr, "ADDC R2, R1 - overflow");
        test.initial_state.R[1] = 0xFFFFFFFF;
        test.initial_state.R[2] = 1;
        test.initial_state.T = false;
        test.expected_state = test.initial_state;
        test.expected_state.R[1] = 0;
        test.expected_state.T = true; // Carry out
        test.expected_state.PC += 2;
        suite.AddTest(test);
    }
    
    // Flag test: SUBC (affects T-bit)
    if (mnemonic == "SUBC") {
        // Test 1: No borrow
        uint16 instr = 0x312A; // SUBC R2, R1
        auto test = CreateTest(spec, instr, "SUBC R2, R1 - no borrow");
        test.initial_state.R[1] = 10;
        test.initial_state.R[2] = 3;
        test.initial_state.T = false;
        test.expected_state = test.initial_state;
        test.expected_state.R[1] = 7;
        test.expected_state.T = false;
        test.expected_state.PC += 2;
        suite.AddTest(test);
        
        // Test 2: With input borrow
        test = CreateTest(spec, instr, "SUBC R2, R1 - with input borrow");
        test.initial_state.R[1] = 10;
        test.initial_state.R[2] = 3;
        test.initial_state.T = true;
        test.expected_state = test.initial_state;
        test.expected_state.R[1] = 6; // 10 - 3 - 1
        test.expected_state.T = false;
        test.expected_state.PC += 2;
        suite.AddTest(test);
        
        // Test 3: Underflow generates borrow
        test = CreateTest(spec, instr, "SUBC R2, R1 - underflow");
        test.initial_state.R[1] = 0;
        test.initial_state.R[2] = 1;
        test.initial_state.T = false;
        test.expected_state = test.initial_state;
        test.expected_state.R[1] = 0xFFFFFFFF;
        test.expected_state.T = true; // Borrow
        test.expected_state.PC += 2;
        suite.AddTest(test);
    }
}

std::vector<InstructionTestSuite> TestGenerator::GenerateAllTests() {
    std::vector<InstructionTestSuite> suites;
    
    const auto& specs = SH2SpecDatabase::GetAllInstructions();
    
    for (const auto& spec : specs) {
        auto suite = GenerateInstructionTests(spec.mnemonic, 
                                               spec.opcode_pattern,
                                               spec.opcode_mask);
        if (suite.Count() > 0) {
            suites.push_back(std::move(suite));
        }
    }
    
    return suites;
}

} // namespace brimir::jit

