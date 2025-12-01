/**
 * @file jit_integration.cpp
 * @brief Integration layer - connects JIT components and runs tests
 */

#include "../include/jit_validator.hpp"
#include "../include/jit_x64_backend.hpp"
#include "../include/sh2_spec.hpp"
#include <brimir/hw/sh2/sh2.hpp>
#include <brimir/core/scheduler.hpp>
#include <iostream>

namespace brimir::jit {

/**
 * @brief Simplified SH-2 wrapper for isolated instruction testing
 */
class SimpleSH2Executor {
public:
    SimpleSH2Executor() 
        : scheduler_()
        , bus_()
        , features_()
        , sh2_(scheduler_, bus_, true, features_)
    {
        // Set up minimal system features
        features_.enableDebugTracing = false;
        features_.emulateSH2Cache = false;
        
        // Allocate test RAM
        ram_.resize(1024 * 1024); // 1MB test RAM
        
        // Map RAM to bus
        MapRAM();
    }
    
    /**
     * @brief Execute a single SH-2 instruction and capture state
     */
    SH2State ExecuteInstruction(uint16 instruction, const SH2State& initial_state) {
        // Set initial state
        SetState(initial_state);
        
        // Write instruction to memory
        uint32 pc = initial_state.PC;
        ram_[pc & 0xFFFFF] = (instruction >> 8) & 0xFF;
        ram_[(pc + 1) & 0xFFFFF] = instruction & 0xFF;
        
        // Execute one instruction
        #ifdef BRIMIR_ENABLE_JIT_TESTING
        uint64 cycles = sh2_.JIT_ExecuteSingleInstruction();
        #else
        uint64 cycles = 0;
        #endif
        
        // Capture final state
        SH2State final_state = GetState();
        final_state.cycles = cycles;
        
        return final_state;
    }
    
private:
    void MapRAM() {
        // Map RAM using function handlers
        uint32 start = 0x00000000;
        uint32 end = 0x000FFFFF; // 1MB
        
        auto read8 = [this](uint32 addr) -> uint8 {
            return ram_[addr & 0xFFFFF];
        };
        
        auto read16 = [this](uint32 addr) -> uint16 {
            uint32 offset = addr & 0xFFFFF;
            return (ram_[offset] << 8) | ram_[offset + 1];
        };
        
        auto read32 = [this](uint32 addr) -> uint32 {
            uint32 offset = addr & 0xFFFFF;
            return (ram_[offset] << 24) | 
                   (ram_[offset + 1] << 16) | 
                   (ram_[offset + 2] << 8) | 
                   ram_[offset + 3];
        };
        
        auto write8 = [this](uint32 addr, uint8 value) {
            ram_[addr & 0xFFFFF] = value;
        };
        
        auto write16 = [this](uint32 addr, uint16 value) {
            uint32 offset = addr & 0xFFFFF;
            ram_[offset] = (value >> 8) & 0xFF;
            ram_[offset + 1] = value & 0xFF;
        };
        
        auto write32 = [this](uint32 addr, uint32 value) {
            uint32 offset = addr & 0xFFFFF;
            ram_[offset] = (value >> 24) & 0xFF;
            ram_[offset + 1] = (value >> 16) & 0xFF;
            ram_[offset + 2] = (value >> 8) & 0xFF;
            ram_[offset + 3] = value & 0xFF;
        };
        
        bus_.MapBoth(start, end, this, read8, read16, read32, write8, write16, write32);
    }
    
    void SetState(const SH2State& state) {
        #ifdef BRIMIR_ENABLE_JIT_TESTING
        for (int i = 0; i < 16; i++) {
            sh2_.JIT_SetR(i, state.R[i]);
        }
        sh2_.JIT_SetPC(state.PC);
        sh2_.JIT_SetPR(state.PR);
        sh2_.JIT_SetGBR(state.GBR);
        sh2_.JIT_SetVBR(state.VBR);
        
        // Set SR
        brimir::sh2::RegSR sr;
        sr.u32 = state.SR;
        sr.T = state.T;
        sr.S = state.S;
        sr.ILevel = state.IMASK;
        sr.Q = state.Q;
        sr.M = state.M;
        sh2_.JIT_SetSR(sr);
        
        // Set MAC
        brimir::sh2::RegMAC mac;
        mac.H = state.MACH;
        mac.L = state.MACL;
        sh2_.JIT_SetMAC(mac);
        #endif
    }
    
    SH2State GetState() {
        SH2State state;
        #ifdef BRIMIR_ENABLE_JIT_TESTING
        for (int i = 0; i < 16; i++) {
            state.R[i] = sh2_.JIT_GetR(i);
        }
        state.PC = sh2_.JIT_GetPC();
        state.PR = sh2_.JIT_GetPR();
        state.GBR = sh2_.JIT_GetGBR();
        state.VBR = sh2_.JIT_GetVBR();
        
        // Get SR
        auto sr = sh2_.JIT_GetSR();
        state.SR = sr.u32;
        state.T = sr.T;
        state.S = sr.S;
        state.IMASK = sr.ILevel;
        state.Q = sr.Q;
        state.M = sr.M;
        
        // Get MAC
        auto mac = sh2_.JIT_GetMAC();
        state.MACH = mac.H;
        state.MACL = mac.L;
        #endif
        return state;
    }
    
    core::Scheduler scheduler_;
    sys::SH2Bus bus_;
    sys::SystemFeatures features_;
    brimir::sh2::SH2 sh2_;
    std::vector<uint8> ram_;
};

/**
 * @brief Simple test runner - executes one test and reports result
 */
bool RunSingleTest(const InstructionTest& test) {
    std::cout << "Testing: " << test.mnemonic << " - " << test.description << "\n";
    
    try {
        // Create executor
        SimpleSH2Executor executor;
        
        // Execute in interpreter
        SH2State interpreter_result = executor.ExecuteInstruction(test.instruction, test.initial_state);
        
        // TODO: Execute in JIT (not yet implemented)
        
        // For now, just compare with expected state
        bool passed = (interpreter_result == test.expected_state);
        
        if (passed) {
            std::cout << "  ✅ PASS\n";
        } else {
            std::cout << "  ❌ FAIL\n";
            std::cout << interpreter_result.Diff(test.expected_state);
        }
        
        return passed;
        
    } catch (const std::exception& e) {
        std::cout << "  ❌ EXCEPTION: " << e.what() << "\n";
        return false;
    }
}

/**
 * @brief Run all tests in a suite
 */
void RunTestSuite(const InstructionTestSuite& suite) {
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "Test Suite: " << suite.category << "\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
    
    size_t passed = 0;
    size_t total = suite.Count();
    
    for (const auto& test : suite.tests) {
        if (RunSingleTest(test)) {
            passed++;
        }
    }
    
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "Results: " << passed << "/" << total << " passed";
    if (passed == total) {
        std::cout << " ✅\n";
    } else {
        std::cout << " ❌\n";
    }
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
}

} // namespace brimir::jit

