#pragma once

// Isolated SH-2 on a synthetic bus, for JIT/interpreter differential tests.
//
// Bus layout (SH-2 bus addresses, 27 bits):
//   0x0000000-0x1FFFFFF  1 MiB RAM, mirrored. Wait states r8=2 w8=3 r16=4 w16=5 r32=6 w32=7
//   0x2000000-0x3FFFFFF  64 KiB MMIO with handlers, mirrored. Wait states 8/9/10/11/12/13.
//                        Bus wait: every Nth 16/32-bit query returns "wait" (N = busWaitEvery, 0 = never)
//   0x4000000-0x7FFFFFF  the same 1 MiB RAM, same wait states
// With cache emulation off, the SH-2 cached area (0x0xxxxxxx) costs 1 cycle per access;
// the cache-through area (0x2xxxxxxx) uses the wait states above.

#include <ymir/hw/sh2/sh2.hpp>
#include <ymir/savestate/savestate_sh2.hpp>
#include <ymir/sys/bus.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sh2test {

constexpr uint32_t kRamSize = 0x100000;
constexpr uint32_t kSleep = 0x001B; // SLEEP: never supported by the JIT, ends test programs

struct Mmio {
    std::array<uint8_t, 0x10000> data{};
    uint32_t busWaitQueries = 0;
    uint32_t busWaitEvery = 0;
};

class Rig {
public:
    Rig();
    Rig(const Rig &) = delete;
    Rig &operator=(const Rig &) = delete;

    // Writes big-endian halfwords at an SH-2 RAM address.
    void WriteCode(uint32_t address, const std::vector<uint16_t> &words);
    void Write32(uint32_t address, uint32_t value);
    uint16_t Read16(uint32_t address) const;
    uint32_t Read32(uint32_t address) const;

    ymir::savestate::SH2SaveState State() const;
    void Load(const ymir::savestate::SH2SaveState &state);

    // Current state with PC = pc, SR = 0xF0 (interrupts masked), no delay slot, interrupts allowed,
    // no write-back register, and a fetch buffer consistent with memory at pc.
    ymir::savestate::SH2SaveState BaseState(uint32_t pc) const;

    ymir::sys::SH2Bus bus;
    std::unique_ptr<std::array<uint8_t, kRamSize>> ram;
    Mmio mmio;
    std::unique_ptr<ymir::sh2::SH2> sh2;
};

// Returns a description of the first difference in CPU state, RAM or MMIO, or "" if identical.
std::string DiffRigs(const Rig &a, const Rig &b);

} // namespace sh2test
