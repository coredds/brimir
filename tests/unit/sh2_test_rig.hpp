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

#include <brimir/lockstep.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sh2test {

constexpr uint32_t kRamSize = 0x100000;
constexpr uint32_t kSleep = 0x001B; // SLEEP: never supported by the JIT, ends test programs

struct MmioAccess {
    char kind;    // 'R' read, 'W' write, 'B' bus-wait query
    uint8_t size; // bytes
    uint32_t address;
    uint32_t value; // value read or written (0 for bus-wait queries)
};

struct Mmio {
    std::array<uint8_t, 0x10000> data{};
    uint32_t busWaitQueries = 0;
    uint32_t busWaitEvery = 0;
    std::vector<MmioAccess> log; // every access in order, for bus-sequence comparison
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

// Returns a description of the first difference, or "" if identical: SH-2 state (a = interpreter,
// b = JIT), RAM, MMIO contents and the MMIO access log. comparePeripherals also compares timers,
// DMAC and the pending interrupt; use it only when both rigs ran through SH2::Advance (per-step
// tests use SH2::Step on the reference, which advances timers while the JIT's step does not).
std::string DiffRigs(const Rig &a, const Rig &b, bool comparePeripherals = false);

} // namespace sh2test
