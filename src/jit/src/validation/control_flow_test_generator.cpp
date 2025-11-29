/**
 * @file control_flow_test_generator.cpp
 * @brief Generate test cases for control flow (Level 3 tests)
 * 
 * Tests branches, delay slots, jumps - the trickiest part of SH-2 emulation!
 */

#include "../../include/jit_test_framework.hpp"
#include <random>

namespace brimir::jit::test {

// Reuse instruction encoders
namespace sh2 {
    extern uint16_t NOP();
    extern uint16_t MOV_I(uint8_t imm, uint8_t n);
    extern uint16_t ADD_I(uint8_t imm, uint8_t n);
    extern uint16_t BT(int8_t disp);
    extern uint16_t BF(int8_t disp);
    extern uint16_t CMP_EQ(uint8_t m, uint8_t n);
    extern uint16_t SETT();
    extern uint16_t CLRT();
    extern uint16_t RTS();
    extern uint16_t JSR(uint8_t n);
    extern uint16_t JMP(uint8_t n);
    extern uint16_t BRAF(uint8_t n);
    extern uint16_t BSRF(uint8_t n);
}

std::vector<TestCase> ControlFlowTestGenerator::GenerateBranchTests() {
    std::vector<TestCase> tests;
    
    // Test 1: BT taken (T=1)
    {
        TestCase test;
        test.name = "BT_taken";
        test.description = "BT with T=1 (branch should be taken)";
        test.initial_state = CreateRandomState(50000);
        test.initial_state.PC = 0x06004000;
        test.initial_state.T = true;  // Branch will be taken
        test.code = {
            sh2::BT(2),            // If T=1, skip next 2 instructions
            sh2::MOV_I(99, 0),     // Should be skipped
            sh2::MOV_I(99, 1),     // Should be skipped
            sh2::MOV_I(1, 2),      // Should execute (target)
            sh2::NOP()
        };
        tests.push_back(test);
    }
    
    // Test 2: BT not taken (T=0)
    {
        TestCase test;
        test.name = "BT_not_taken";
        test.description = "BT with T=0 (branch should not be taken)";
        test.initial_state = CreateRandomState(50001);
        test.initial_state.PC = 0x06004000;
        test.initial_state.T = false;  // Branch will NOT be taken
        test.code = {
            sh2::BT(2),            // If T=1, skip next 2 instructions
            sh2::MOV_I(1, 0),      // Should execute
            sh2::MOV_I(2, 1),      // Should execute
            sh2::MOV_I(3, 2),      // Should execute
            sh2::NOP()
        };
        tests.push_back(test);
    }
    
    // Test 3: BF taken (T=0)
    {
        TestCase test;
        test.name = "BF_taken";
        test.description = "BF with T=0 (branch should be taken)";
        test.initial_state = CreateRandomState(50002);
        test.initial_state.PC = 0x06004000;
        test.initial_state.T = false;  // Branch will be taken
        test.code = {
            sh2::BF(2),            // If T=0, skip next 2 instructions
            sh2::MOV_I(99, 0),     // Should be skipped
            sh2::MOV_I(99, 1),     // Should be skipped
            sh2::MOV_I(1, 2),      // Should execute (target)
            sh2::NOP()
        };
        tests.push_back(test);
    }
    
    // Test 4: BF not taken (T=1)
    {
        TestCase test;
        test.name = "BF_not_taken";
        test.description = "BF with T=1 (branch should not be taken)";
        test.initial_state = CreateRandomState(50003);
        test.initial_state.PC = 0x06004000;
        test.initial_state.T = true;  // Branch will NOT be taken
        test.code = {
            sh2::BF(2),            // If T=0, skip next 2 instructions
            sh2::MOV_I(1, 0),      // Should execute
            sh2::MOV_I(2, 1),      // Should execute
            sh2::MOV_I(3, 2),      // Should execute
            sh2::NOP()
        };
        tests.push_back(test);
    }
    
    // Test 5: Backward branch
    // Note: This is tricky - needs careful PC handling
    {
        TestCase test;
        test.name = "BT_backward";
        test.description = "BT with backward displacement";
        test.initial_state = CreateRandomState(50004);
        test.initial_state.PC = 0x06004000;
        test.initial_state.T = false;  // Don't take branch initially
        test.code = {
            sh2::MOV_I(0, 0),      // R0 = 0
            sh2::ADD_I(1, 0),      // R0 = 1
            sh2::SETT(),           // T = 1
            sh2::BT(-2)            // Branch back (would create loop - careful!)
        };
        tests.push_back(test);
    }
    
    // Test 6: CMP followed by branch
    {
        TestCase test;
        test.name = "CMP_then_BT";
        test.description = "CMP/EQ followed by conditional branch";
        test.initial_state = CreateRandomState(50005);
        test.initial_state.PC = 0x06004000;
        test.initial_state.R[0] = 5;
        test.initial_state.R[1] = 5;  // Equal
        test.code = {
            sh2::CMP_EQ(1, 0),     // Compare R0 and R1, sets T=1 if equal
            sh2::BT(1),            // Branch if equal
            sh2::MOV_I(99, 2),     // Should be skipped
            sh2::MOV_I(1, 3),      // Should execute
            sh2::NOP()
        };
        tests.push_back(test);
    }
    
    return tests;
}

std::vector<TestCase> ControlFlowTestGenerator::GenerateDelaySlotTests() {
    std::vector<TestCase> tests;
    
    // Test 1: Branch with delay slot
    // Note: In SH-2, the instruction AFTER a branch always executes!
    {
        TestCase test;
        test.name = "BT_delay_slot";
        test.description = "BT with delay slot (next instruction always executes)";
        test.initial_state = CreateRandomState(51000);
        test.initial_state.PC = 0x06004000;
        test.initial_state.T = true;
        test.code = {
            sh2::BT(2),            // Branch to +2 (with delay slot)
            sh2::MOV_I(1, 0),      // DELAY SLOT - always executes!
            sh2::MOV_I(99, 1),     // Should be skipped
            sh2::MOV_I(99, 2),     // Should be skipped
            sh2::MOV_I(2, 3),      // Branch target
            sh2::NOP()
        };
        
        // Custom validator to ensure delay slot executed
        test.custom_validator = [](const SH2State& before, const SH2State& after) -> std::string {
            // R0 should be 1 (from delay slot)
            if (after.R[0] != 1) {
                return "Delay slot did not execute! R0 should be 1, got " + std::to_string(after.R[0]);
            }
            // R1 should NOT be 99 (instruction was skipped)
            if (after.R[1] == 99) {
                return "Branch target was wrong - skipped instruction executed!";
            }
            return "";
        };
        
        tests.push_back(test);
    }
    
    // Test 2: Delay slot modifies branch register
    {
        TestCase test;
        test.name = "delay_slot_modifies_register";
        test.description = "Delay slot that modifies a register";
        test.initial_state = CreateRandomState(51001);
        test.initial_state.PC = 0x06004000;
        test.initial_state.R[0] = 5;
        test.initial_state.T = true;
        test.code = {
            sh2::BT(1),            // Branch
            sh2::ADD_I(10, 0),     // DELAY SLOT: R0 = R0 + 10 = 15
            sh2::MOV_I(99, 1),     // Skipped
            sh2::MOV_I(1, 2),      // Target
            sh2::NOP()
        };
        
        test.custom_validator = [](const SH2State& before, const SH2State& after) -> std::string {
            if (after.R[0] != 15) {
                return "R0 should be 15 (delay slot executed), got " + std::to_string(after.R[0]);
            }
            return "";
        };
        
        tests.push_back(test);
    }
    
    // Test 3: Multiple branches (complex delay slot interaction)
    {
        TestCase test;
        test.name = "multiple_branches";
        test.description = "Multiple branches in sequence";
        test.initial_state = CreateRandomState(51002);
        test.initial_state.PC = 0x06004000;
        test.initial_state.T = true;
        test.code = {
            sh2::BT(1),            // First branch
            sh2::MOV_I(1, 0),      // Delay slot
            sh2::MOV_I(99, 1),     // Skipped
            sh2::BT(1),            // Second branch (target of first)
            sh2::MOV_I(2, 2),      // Delay slot
            sh2::MOV_I(99, 3),     // Skipped
            sh2::NOP()             // Final target
        };
        tests.push_back(test);
    }
    
    return tests;
}

std::vector<TestCase> ControlFlowTestGenerator::GenerateJumpTests() {
    std::vector<TestCase> tests;
    
    // Test 1: JMP @Rn (indirect jump)
    {
        TestCase test;
        test.name = "JMP_indirect";
        test.description = "JMP @Rn (jump to address in register)";
        test.initial_state = CreateRandomState(52000);
        test.initial_state.PC = 0x06004000;
        test.initial_state.R[5] = 0x06004010;  // Jump target
        test.code = {
            sh2::JMP(5),           // JMP @R5
            sh2::MOV_I(1, 0),      // Delay slot
            sh2::NOP(),
            sh2::NOP(),
            // Target at 0x06004010 (offset +16 bytes = +8 instructions)
            sh2::MOV_I(2, 1),
            sh2::NOP()
        };
        
        test.custom_validator = [](const SH2State& before, const SH2State& after) -> std::string {
            if (after.R[0] != 1) {
                return "Delay slot should have executed";
            }
            // PC should be at jump target + next instruction
            // This is tricky to validate without knowing exact execution
            return "";
        };
        
        tests.push_back(test);
    }
    
    // Test 2: JSR @Rn (jump to subroutine)
    {
        TestCase test;
        test.name = "JSR_indirect";
        test.description = "JSR @Rn (saves return address in PR)";
        test.initial_state = CreateRandomState(52001);
        test.initial_state.PC = 0x06004000;
        test.initial_state.R[5] = 0x06004010;  // Subroutine address
        test.code = {
            sh2::JSR(5),           // JSR @R5
            sh2::MOV_I(1, 0),      // Delay slot
            sh2::MOV_I(2, 1),      // Return point (PR should point here)
            sh2::NOP(),
            // Subroutine at +16 bytes
            sh2::MOV_I(3, 2),
            sh2::RTS(),            // Return
            sh2::NOP()             // RTS delay slot
        };
        
        test.custom_validator = [](const SH2State& before, const SH2State& after) -> std::string {
            // PR should be set to return address
            if (after.PR == 0) {
                return "PR was not set by JSR";
            }
            return "";
        };
        
        tests.push_back(test);
    }
    
    // Test 3: RTS (return from subroutine)
    {
        TestCase test;
        test.name = "RTS_return";
        test.description = "RTS (return from subroutine)";
        test.initial_state = CreateRandomState(52002);
        test.initial_state.PC = 0x06004000;
        test.initial_state.PR = 0x06004008;  // Return address
        test.code = {
            sh2::RTS(),            // Return to PR
            sh2::MOV_I(1, 0),      // Delay slot
            sh2::MOV_I(99, 1),     // Shouldn't execute
            sh2::NOP(),
            // Return target at +8 bytes
            sh2::MOV_I(2, 2),
            sh2::NOP()
        };
        
        test.custom_validator = [](const SH2State& before, const SH2State& after) -> std::string {
            if (after.R[0] != 1) {
                return "RTS delay slot should execute";
            }
            if (after.R[1] == 99) {
                return "Instruction after RTS should not execute";
            }
            return "";
        };
        
        tests.push_back(test);
    }
    
    // Test 4: BRAF (branch far)
    {
        TestCase test;
        test.name = "BRAF_far_branch";
        test.description = "BRAF Rn (PC = PC + 4 + Rn)";
        test.initial_state = CreateRandomState(52003);
        test.initial_state.PC = 0x06004000;
        test.initial_state.R[3] = 8;  // Branch forward 8 bytes
        test.code = {
            sh2::BRAF(3),          // BRAF R3
            sh2::MOV_I(1, 0),      // Delay slot
            sh2::MOV_I(99, 1),     // Skipped
            sh2::MOV_I(99, 2),     // Skipped
            // Target at PC + 4 + R3
            sh2::MOV_I(2, 3),
            sh2::NOP()
        };
        tests.push_back(test);
    }
    
    // Test 5: BSRF (branch to subroutine far)
    {
        TestCase test;
        test.name = "BSRF_far_call";
        test.description = "BSRF Rn (PC = PC + 4 + Rn, save return in PR)";
        test.initial_state = CreateRandomState(52004);
        test.initial_state.PC = 0x06004000;
        test.initial_state.R[4] = 12;  // Branch forward 12 bytes
        test.code = {
            sh2::BSRF(4),          // BSRF R4
            sh2::MOV_I(1, 0),      // Delay slot
            sh2::MOV_I(2, 1),      // Return point
            sh2::NOP(),
            sh2::NOP(),
            sh2::NOP(),
            // Target at PC + 4 + R4
            sh2::MOV_I(3, 2),
            sh2::NOP()
        };
        
        test.custom_validator = [](const SH2State& before, const SH2State& after) -> std::string {
            if (after.PR == 0) {
                return "PR should be set by BSRF";
            }
            return "";
        };
        
        tests.push_back(test);
    }
    
    return tests;
}

} // namespace brimir::jit::test

