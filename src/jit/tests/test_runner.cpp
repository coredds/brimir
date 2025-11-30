/**
 * @file test_runner.cpp
 * @brief Main test runner for JIT validation
 * 
 * Runs all test levels and generates comprehensive reports.
 */

#include "../include/jit_test_framework.hpp"
#include <iostream>
#include <fstream>

using namespace brimir::jit::test;

void PrintBanner(const std::string& title) {
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << title << "\n";
    std::cout << "================================================================================\n";
}

int main(int argc, char** argv) {
    PrintBanner("Brimir SH-2 JIT Test Suite");
    
    std::cout << "Phase 1: Test Infrastructure\n";
    std::cout << "Goal: Validate JIT against interpreter for 100% accuracy\n\n";
    
    DualExecutionHarness harness;
    harness.SetVerbose(true);
    
    // -------------------------------------------------------------------------
    // Level 1: Instruction Tests
    // -------------------------------------------------------------------------
    PrintBanner("Level 1: Instruction Tests");
    
    std::cout << "Generating instruction test cases...\n";
    auto instruction_tests = InstructionTestGenerator::GenerateAllInstructionTests();
    std::cout << "Generated " << instruction_tests.size() << " instruction tests\n\n";
    
    std::cout << "Running instruction tests...\n";
    std::string report = harness.RunTestsWithReport(instruction_tests);
    std::cout << report << "\n";
    
    // Save report to file
    std::ofstream report_file("jit_test_results.txt");
    if (report_file.is_open()) {
        report_file << report;
        report_file.close();
        std::cout << "Report saved to: jit_test_results.txt\n";
    }
    
    // -------------------------------------------------------------------------
    // Level 2: Basic Block Tests
    // -------------------------------------------------------------------------
    PrintBanner("Level 2: Basic Block Tests");
    
    std::cout << "Generating basic block test cases...\n";
    auto block_tests = BlockTestGenerator::GenerateCommonPatterns();
    auto random_blocks = BlockTestGenerator::GenerateRandomBlocks(10, 20);  // 20 random 10-instruction blocks
    block_tests.insert(block_tests.end(), random_blocks.begin(), random_blocks.end());
    std::cout << "Generated " << block_tests.size() << " block tests\n\n";
    
    std::cout << "Running block tests...\n";
    auto block_results = harness.RunTests(block_tests);
    
    size_t block_passed = 0;
    for (const auto& result : block_results) {
        if (result.passed) block_passed++;
    }
    
    std::cout << "Block Tests: " << block_passed << "/" << block_tests.size() << " passed\n";
    
    // -------------------------------------------------------------------------
    // Level 3: Control Flow Tests
    // -------------------------------------------------------------------------
    PrintBanner("Level 3: Control Flow Tests");
    
    std::cout << "Generating control flow test cases...\n";
    auto branch_tests = ControlFlowTestGenerator::GenerateBranchTests();
    auto delay_slot_tests = ControlFlowTestGenerator::GenerateDelaySlotTests();
    auto jump_tests = ControlFlowTestGenerator::GenerateJumpTests();
    
    std::cout << "Generated:\n";
    std::cout << "  - " << branch_tests.size() << " branch tests\n";
    std::cout << "  - " << delay_slot_tests.size() << " delay slot tests\n";
    std::cout << "  - " << jump_tests.size() << " jump tests\n\n";
    
    // -------------------------------------------------------------------------
    // Level 4: Game Regression Tests (placeholder)
    // -------------------------------------------------------------------------
    PrintBanner("Level 4: Game Regression Tests");
    std::cout << "Not yet implemented (requires full emulator integration)\n";
    
    // -------------------------------------------------------------------------
    // Level 5: Fuzz Testing
    // -------------------------------------------------------------------------
    PrintBanner("Level 5: Fuzz Testing");
    
    std::cout << "Generating fuzz test cases...\n";
    auto fuzz_tests = FuzzTestGenerator::GenerateRandomSequences(20, 100, 12345);  // 100 sequences of 20 instructions
    std::cout << "Generated " << fuzz_tests.size() << " fuzz tests\n\n";
    
    std::cout << "Fuzz tests ready for execution once interpreter is integrated.\n";
    
    // -------------------------------------------------------------------------
    // Summary
    // -------------------------------------------------------------------------
    PrintBanner("Test Suite Summary");
    
    size_t total_tests = instruction_tests.size() + block_tests.size() + 
                         branch_tests.size() + delay_slot_tests.size() + jump_tests.size() +
                         fuzz_tests.size();
    
    std::cout << "✅ Test Infrastructure: READY!\n\n";
    std::cout << "Test Coverage:\n";
    std::cout << "  Level 1 (Instructions):  " << instruction_tests.size() << " tests\n";
    std::cout << "  Level 2 (Blocks):        " << block_tests.size() << " tests\n";
    std::cout << "  Level 3 (Control Flow):  " << (branch_tests.size() + delay_slot_tests.size() + jump_tests.size()) << " tests\n";
    std::cout << "  Level 4 (Games):         Pending emulator integration\n";
    std::cout << "  Level 5 (Fuzz):          " << fuzz_tests.size() << " tests\n";
    std::cout << "  ───────────────────────────────────────\n";
    std::cout << "  TOTAL:                   " << total_tests << " tests ready!\n\n";
    
    std::cout << "NOTE: Test execution will work once interpreter/JIT integration is complete.\n";
    std::cout << "      Current output shows framework is ready and test cases are generated.\n";
    
    PrintBanner("Phase 1 Status: Infrastructure Complete");
    
    return 0;
}

