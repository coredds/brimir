#include "../include/jit_x64_backend.hpp"

namespace brimir::jit {

X64RegisterAllocator::X64RegisterAllocator() {
    // Initialize SH-2 to x64 register mapping
    // Strategy: Map hottest SH-2 registers to x64 registers
    //
    // SH-2 Register Usage (from typical Saturn games):
    // - R0: Very hot (used for return values, comparisons)
    // - R1-R7: Hot (general purpose, function args)
    // - R8-R14: Warm (local variables)
    // - R15 (SP): Cold (stack pointer, accessed infrequently)
    //
    // x64 Register Allocation:
    // - RBP: Reserved for SH-2 context pointer
    // - RSP: x64 stack pointer
    // - RAX, RCX, RDX: Scratch (used for intermediate computations)
    // - RBX, R12-R15, RDI, RSI: Callee-saved (good for SH-2 registers)
    
    // Hot SH-2 registers → x64 callee-saved registers
    sh2_to_x64_[0]  = X64Register::RBX;   // R0 → RBX
    sh2_to_x64_[1]  = X64Register::R12;   // R1 → R12
    sh2_to_x64_[2]  = X64Register::R13;   // R2 → R13
    sh2_to_x64_[3]  = X64Register::R14;   // R3 → R14
    sh2_to_x64_[4]  = X64Register::R15;   // R4 → R15
    sh2_to_x64_[5]  = X64Register::RDI;   // R5 → RDI
    sh2_to_x64_[6]  = X64Register::RSI;   // R6 → RSI
    sh2_to_x64_[7]  = X64Register::R8;    // R7 → R8
    
    // Cold SH-2 registers → memory-backed (loaded on-demand)
    for (int i = 8; i < 16; i++) {
        sh2_to_x64_[i] = X64Register::NONE; // Not in register
    }
    
    // Initialize temporary register pool
    available_temps_ = {
        X64Register::RAX,
        X64Register::RCX,
        X64Register::RDX,
        X64Register::R9,
        X64Register::R10,
        X64Register::R11
    };
}

X64Register X64RegisterAllocator::GetSH2Register(uint8 sh2_reg) {
    if (sh2_reg >= 16) {
        return X64Register::NONE;
    }
    return sh2_to_x64_[sh2_reg];
}

X64Register X64RegisterAllocator::AllocTemp() {
    if (available_temps_.empty()) {
        // TODO: Spill a temporary register
        // For now, just return RAX (this is a limitation)
        return X64Register::RAX;
    }
    
    X64Register reg = available_temps_.back();
    available_temps_.pop_back();
    used_temps_.push_back(reg);
    return reg;
}

void X64RegisterAllocator::FreeTemp(X64Register reg) {
    // Remove from used temps
    auto it = std::find(used_temps_.begin(), used_temps_.end(), reg);
    if (it != used_temps_.end()) {
        used_temps_.erase(it);
        available_temps_.push_back(reg);
    }
}

bool X64RegisterAllocator::IsInRegister(uint8 sh2_reg) const {
    if (sh2_reg >= 16) return false;
    return sh2_to_x64_[sh2_reg] != X64Register::NONE;
}

void X64RegisterAllocator::Spill(uint8 sh2_reg) {
    // TODO: Implement spilling SH-2 register to memory
    // This would store the current x64 register value back to the SH-2 context
}

void X64RegisterAllocator::Load(uint8 sh2_reg) {
    // TODO: Implement loading SH-2 register from memory
    // This would load from the SH-2 context into an x64 register
}

} // namespace brimir::jit

