// Quick JIT validation test
#include <iostream>
#include <brimir/hw/sh2/sh2.hpp>
#include <brimir/core/scheduler.hpp>
#include <brimir/sys/bus.hpp>
#include <brimir/sys/system_features.hpp>

int main() {
    std::cout << "\n";
    std::cout << "╔════════════════════════════════════════════════════╗\n";
    std::cout << "║  Brimir SH-2 JIT Test Interface Validation        ║\n";
    std::cout << "╚════════════════════════════════════════════════════╝\n\n";
    
    #ifdef BRIMIR_ENABLE_JIT_TESTING
    std::cout << "✅ BRIMIR_ENABLE_JIT_TESTING is defined\n\n";
    
    try {
        // Create minimal SH-2 environment
        brimir::core::Scheduler scheduler;
        brimir::sys::SH2Bus bus;
        brimir::sys::SystemFeatures features;
        
        features.enableDebugTracing = false;
        features.emulateSH2Cache = false;
        
        brimir::sh2::SH2 sh2(scheduler, bus, true, features);
        
        std::cout << "Test 1: Register Access\n";
        std::cout << "  Setting R0 = 0x12345678...\n";
        sh2.JIT_SetR(0, 0x12345678);
        
        uint32_t value = sh2.JIT_GetR(0);
        std::cout << "  Reading R0 = 0x" << std::hex << value << std::dec << "\n";
        
        if (value == 0x12345678) {
            std::cout << "  ✅ PASS: Register access works!\n\n";
        } else {
            std::cout << "  ❌ FAIL: Expected 0x12345678, got 0x" 
                      << std::hex << value << std::dec << "\n\n";
            return 1;
        }
        
        std::cout << "Test 2: PC Access\n";
        std::cout << "  Setting PC = 0x06004000...\n";
        sh2.JIT_SetPC(0x06004000);
        
        uint32_t pc = sh2.JIT_GetPC();
        std::cout << "  Reading PC = 0x" << std::hex << pc << std::dec << "\n";
        
        if (pc == 0x06004000) {
            std::cout << "  ✅ PASS: PC access works!\n\n";
        } else {
            std::cout << "  ❌ FAIL: Expected 0x06004000, got 0x"
                      << std::hex << pc << std::dec << "\n\n";
            return 1;
        }
        
        std::cout << "Test 3: SR Access\n";
        brimir::sh2::RegSR sr;
        sr.u32 = 0;
        sr.T = 1;
        sr.S = 0;
        sr.ILevel = 5;
        
        std::cout << "  Setting SR with T=1, S=0, ILevel=5...\n";
        sh2.JIT_SetSR(sr);
        
        auto sr_read = sh2.JIT_GetSR();
        std::cout << "  Reading SR: T=" << sr_read.T 
                  << ", S=" << sr_read.S
                  << ", ILevel=" << (int)sr_read.ILevel << "\n";
        
        if (sr_read.T == 1 && sr_read.S == 0 && sr_read.ILevel == 5) {
            std::cout << "  ✅ PASS: SR access works!\n\n";
        } else {
            std::cout << "  ❌ FAIL: SR mismatch\n\n";
            return 1;
        }
        
        std::cout << "╔════════════════════════════════════════════════════╗\n";
        std::cout << "║            ✅ ALL TESTS PASSED! ✅                  ║\n";
        std::cout << "╚════════════════════════════════════════════════════╝\n\n";
        
        std::cout << "JIT Test Interface Status: OPERATIONAL\n";
        std::cout << "Ready for full validation testing!\n\n";
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cout << "❌ EXCEPTION: " << e.what() << "\n\n";
        return 1;
    }
    
    #else
    std::cout << "❌ BRIMIR_ENABLE_JIT_TESTING is NOT defined!\n";
    std::cout << "   Rebuild with: cmake .. -DBRIMIR_ENABLE_JIT_TESTING=ON\n\n";
    return 1;
    #endif
}

