#include "ymir_sh2_wrapper.hpp"

// Ymir includes
#include <ymir/hw/sh2/sh2.hpp>
#include <ymir/core/scheduler.hpp>
#include <ymir/sys/bus.hpp>
#include <ymir/sys/system_features.hpp>

#include <sstream>
#include <cstring>
#include <algorithm>

namespace jit {

// ============================================================================
// SH2StateSnapshot implementation
// ============================================================================

bool SH2StateSnapshot::operator==(const SH2StateSnapshot& other) const {
    return R == other.R &&
           PC == other.PC &&
           PR == other.PR &&
           GBR == other.GBR &&
           VBR == other.VBR &&
           MACH == other.MACH &&
           MACL == other.MACL &&
           SR == other.SR &&
           T == other.T &&
           S == other.S &&
           ILevel == other.ILevel &&
           Q == other.Q &&
           M == other.M &&
           inDelaySlot == other.inDelaySlot &&
           delaySlotTarget == other.delaySlotTarget &&
           cycles == other.cycles;
}

std::string SH2StateSnapshot::Diff(const SH2StateSnapshot& other) const {
    std::ostringstream oss;
    bool hasDiff = false;
    
    // Check general registers
    for (size_t i = 0; i < 16; ++i) {
        if (R[i] != other.R[i]) {
            oss << "  R" << i << ": 0x" << std::hex << R[i] 
                << " vs 0x" << other.R[i] << std::dec << "\n";
            hasDiff = true;
        }
    }
    
    // Check control registers
    if (PC != other.PC) {
        oss << "  PC: 0x" << std::hex << PC << " vs 0x" << other.PC << std::dec << "\n";
        hasDiff = true;
    }
    if (PR != other.PR) {
        oss << "  PR: 0x" << std::hex << PR << " vs 0x" << other.PR << std::dec << "\n";
        hasDiff = true;
    }
    if (GBR != other.GBR) {
        oss << "  GBR: 0x" << std::hex << GBR << " vs 0x" << other.GBR << std::dec << "\n";
        hasDiff = true;
    }
    if (VBR != other.VBR) {
        oss << "  VBR: 0x" << std::hex << VBR << " vs 0x" << other.VBR << std::dec << "\n";
        hasDiff = true;
    }
    
    // Check MAC registers
    if (MACH != other.MACH) {
        oss << "  MACH: 0x" << std::hex << MACH << " vs 0x" << other.MACH << std::dec << "\n";
        hasDiff = true;
    }
    if (MACL != other.MACL) {
        oss << "  MACL: 0x" << std::hex << MACL << " vs 0x" << other.MACL << std::dec << "\n";
        hasDiff = true;
    }
    
    // Check SR and flags
    if (SR != other.SR) {
        oss << "  SR: 0x" << std::hex << SR << " vs 0x" << other.SR << std::dec << "\n";
        hasDiff = true;
    }
    if (T != other.T) {
        oss << "  T-bit: " << T << " vs " << other.T << "\n";
        hasDiff = true;
    }
    if (S != other.S) {
        oss << "  S-bit: " << S << " vs " << other.S << "\n";
        hasDiff = true;
    }
    if (ILevel != other.ILevel) {
        oss << "  ILevel: " << (int)ILevel << " vs " << (int)other.ILevel << "\n";
        hasDiff = true;
    }
    if (Q != other.Q) {
        oss << "  Q-bit: " << Q << " vs " << other.Q << "\n";
        hasDiff = true;
    }
    if (M != other.M) {
        oss << "  M-bit: " << M << " vs " << other.M << "\n";
        hasDiff = true;
    }
    
    // Check delay slot state
    if (inDelaySlot != other.inDelaySlot) {
        oss << "  inDelaySlot: " << inDelaySlot << " vs " << other.inDelaySlot << "\n";
        hasDiff = true;
    }
    if (delaySlotTarget != other.delaySlotTarget) {
        oss << "  delaySlotTarget: 0x" << std::hex << delaySlotTarget 
            << " vs 0x" << other.delaySlotTarget << std::dec << "\n";
        hasDiff = true;
    }
    
    // Check cycles (CRITICAL!)
    if (cycles != other.cycles) {
        oss << "  cycles: " << cycles << " vs " << other.cycles << "\n";
        hasDiff = true;
    }
    
    return hasDiff ? oss.str() : "";
}

// ============================================================================
// YmirSH2Wrapper::Impl - Private implementation
// ============================================================================

struct YmirSH2Wrapper::Impl {
    ymir::core::Scheduler scheduler;
    ymir::sys::SH2Bus bus;
    ymir::sys::SystemFeatures features;
    std::unique_ptr<ymir::sh2::SH2> sh2;
    
    Impl()
        : scheduler()
        , bus()
        , features()
        , sh2(nullptr)
    {
        // Create minimal system features (no special hardware)
        features.hasSTV = false;
        features.hasVDP2_FBE = false;
        
        // Create SH-2 instance (slave, for testing)
        sh2 = std::make_unique<ymir::sh2::SH2>(scheduler, bus, false, features);
    }
};

// ============================================================================
// YmirSH2Wrapper implementation
// ============================================================================

YmirSH2Wrapper::YmirSH2Wrapper(size_t ramSize)
    : m_impl(std::make_unique<Impl>())
    , m_ram(ramSize, 0)
    , m_cycleCount(0)
{
    // Map RAM to bus starting at 0x00000000
    // For testing, we map the entire RAM as a simple array
    m_impl->bus.MapArray(0x00000000, ramSize - 1, m_ram.data(), m_ram.size());
    
    // Map the SH-2's memory handlers to the bus
    m_impl->sh2->MapMemory(m_impl->bus);
}

YmirSH2Wrapper::~YmirSH2Wrapper() = default;

void YmirSH2Wrapper::Reset(uint32_t pc) {
    m_impl->sh2->Reset(true);
    m_impl->sh2->GetProbe().PC() = pc;
    m_cycleCount = 0;
    
    // Clear RAM
    std::fill(m_ram.begin(), m_ram.end(), 0);
}

void YmirSH2Wrapper::WriteMemory(uint32_t address, const void* data, size_t size) {
    // Simple bounds check
    if (address + size > m_ram.size()) {
        return; // Silently ignore out-of-bounds writes for now
    }
    
    std::memcpy(&m_ram[address], data, size);
}

void YmirSH2Wrapper::ReadMemory(uint32_t address, void* data, size_t size) {
    // Simple bounds check
    if (address + size > m_ram.size()) {
        return; // Silently ignore out-of-bounds reads for now
    }
    
    std::memcpy(data, &m_ram[address], size);
}

void YmirSH2Wrapper::WriteInstruction(uint32_t address, uint16_t instruction) {
    // SH-2 is big-endian
    uint16_t bigEndian = ((instruction & 0xFF) << 8) | ((instruction >> 8) & 0xFF);
    WriteMemory(address, &bigEndian, sizeof(uint16_t));
}

void YmirSH2Wrapper::SetRegister(uint8_t reg, uint32_t value) {
    if (reg < 16) {
        m_impl->sh2->GetProbe().R(reg) = value;
    }
}

void YmirSH2Wrapper::SetPC(uint32_t value) {
    m_impl->sh2->GetProbe().PC() = value;
}

void YmirSH2Wrapper::SetPR(uint32_t value) {
    m_impl->sh2->GetProbe().PR() = value;
}

void YmirSH2Wrapper::SetGBR(uint32_t value) {
    m_impl->sh2->GetProbe().GBR() = value;
}

void YmirSH2Wrapper::SetVBR(uint32_t value) {
    m_impl->sh2->GetProbe().VBR() = value;
}

void YmirSH2Wrapper::SetMACH(uint32_t value) {
    m_impl->sh2->GetProbe().MAC().H = value;
}

void YmirSH2Wrapper::SetMACL(uint32_t value) {
    m_impl->sh2->GetProbe().MAC().L = value;
}

void YmirSH2Wrapper::SetSR(uint32_t value) {
    m_impl->sh2->GetProbe().SR().u32 = value;
}

void YmirSH2Wrapper::SetTBit(bool value) {
    m_impl->sh2->GetProbe().SR().T = value ? 1 : 0;
}

uint64_t YmirSH2Wrapper::ExecuteInstruction() {
    // Execute one instruction via Ymir's Step function
    // debug=false, enableCache=false (for deterministic testing)
    uint64_t cycles = m_impl->sh2->Step<false, false>();
    m_cycleCount += cycles;
    return cycles;
}

uint64_t YmirSH2Wrapper::ExecuteInstructions(size_t count) {
    uint64_t totalCycles = 0;
    for (size_t i = 0; i < count; ++i) {
        totalCycles += ExecuteInstruction();
    }
    return totalCycles;
}

uint64_t YmirSH2Wrapper::ExecuteCycles(uint64_t cycles) {
    // Execute via Advance function
    // debug=false, enableCache=false
    uint64_t executed = m_impl->sh2->Advance<false, false>(cycles);
    m_cycleCount += executed;
    return executed;
}

SH2StateSnapshot YmirSH2Wrapper::CaptureState() const {
    SH2StateSnapshot state;
    
    auto& probe = m_impl->sh2->GetProbe();
    
    // Capture general registers
    state.R = probe.R();
    
    // Capture control registers
    state.PC = probe.PC();
    state.PR = probe.PR();
    state.GBR = probe.GBR();
    state.VBR = probe.VBR();
    
    // Capture MAC registers
    state.MACH = probe.MAC().H;
    state.MACL = probe.MAC().L;
    
    // Capture SR and flags
    auto sr = probe.SR();
    state.SR = sr.u32;
    state.T = sr.T;
    state.S = sr.S;
    state.ILevel = sr.ILevel;
    state.Q = sr.Q;
    state.M = sr.M;
    
    // Capture delay slot state
    state.inDelaySlot = probe.IsInDelaySlot();
    state.delaySlotTarget = probe.DelaySlotTarget();
    
    // Capture cycle count
    state.cycles = m_cycleCount;
    
    return state;
}

void YmirSH2Wrapper::RestoreState(const SH2StateSnapshot& state) {
    auto& probe = m_impl->sh2->GetProbe();
    
    // Restore general registers
    for (size_t i = 0; i < 16; ++i) {
        probe.R(i) = state.R[i];
    }
    
    // Restore control registers
    probe.PC() = state.PC;
    probe.PR() = state.PR;
    probe.GBR() = state.GBR;
    probe.VBR() = state.VBR;
    
    // Restore MAC registers
    probe.MAC().H = state.MACH;
    probe.MAC().L = state.MACL;
    
    // Restore SR
    probe.SR().u32 = state.SR;
    
    // Note: Delay slot state cannot be directly restored via public API
    // This is OK for now since we reset before each test
    
    // Restore cycle count
    m_cycleCount = state.cycles;
}

} // namespace jit

