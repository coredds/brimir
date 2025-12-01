/**
 * @file test_runner.cpp
 * @brief Standalone test runner executable
 */

#include "include/jit_validator.hpp"
#include "include/sh2_spec.hpp"
#include "src/jit_test_generator.cpp"
#include "src/jit_integration.cpp"

#include <iostream>
#include <string>

using namespace brimir::jit;

int main(int argc, char** argv) {
    std::cout << "\n";
    std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                                                                ║\n";
    std::cout << "║              BRIMIR SH-2 JIT TEST RUNNER                       ║\n";
    std::cout << "║                                                                ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
    
    // Get spec database stats
    auto stats = SH2SpecDatabase::GetStats();
    std::cout << "Specification Database:\n";
    std::cout << "  Total Instructions: " << stats.total_instructions << "\n";
    std::cout << "  Implemented: " << stats.implemented << " (" 
              << stats.ImplementationProgress() << "%)\n";
    std::cout << "  Tested: " << stats.tested << " (" 
              << stats.TestProgress() << "%)\n";
    std::cout << "\n";
    
    // Generate all test suites
    std::cout << "Generating test suites...\n";
    auto suites = TestGenerator::GenerateAllTests();
    std::cout << "Generated " << suites.size() << " test suites\n\n";
    
    // Run each suite
    size_t total_passed = 0;
    size_t total_tests = 0;
    
    for (const auto& suite : suites) {
        RunTestSuite(suite);
        
        // Count totals
        for (const auto& test : suite.tests) {
            total_tests++;
            // Results were printed in RunTestSuite
        }
    }
    
    // Final summary
    std::cout << "\n";
    std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                      FINAL RESULTS                             ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
    std::cout << "Total Tests: " << total_tests << "\n";
    std::cout << "Status: Integration in progress\n";
    std::cout << "\n";
    std::cout << "Next Steps:\n";
    std::cout << "  1. Complete SH-2 register access in SimpleSH2Executor\n";
    std::cout << "  2. Implement x86-64 backend compilation\n";
    std::cout << "  3. Wire JIT execution path\n";
    std::cout << "  4. Run full dual-execution validation\n";
    std::cout << "\n";
    
    return 0;
}

