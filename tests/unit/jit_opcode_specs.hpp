#pragma once

// Brimir - table of SH-2 opcodes compiled by the JIT, for table-driven differential tests.
// Licensed under GPL-3.0

#include <array>
#include <cstdint>
#include <random>
#include <span>

namespace jitspec {

// How an instruction's operand fields are encoded.
enum class Fmt : uint8_t { Z, N, M, NM, MD, ND4, NMD, D, ND8, I, NI };

// Which registers form a data address (fixed up to point at valid memory before running).
enum class Addr : uint8_t { None, Rm, Rn, RmR0, RnR0, RmDisp, RnDisp, GbrDisp, GbrR0, RnPreDec, RmPostInc };

struct OpSpec {
    const char *name;
    uint16_t base; // encoding with all operand fields zero
    Fmt fmt;
    Addr addr;
    uint8_t size; // data access size in bytes (0 = none)
    bool slotOk;  // legal in a delay slot
};

constexpr uint32_t kMmio = 0x22000000; // the rig's MMIO page (cache-through)

// A data address in cache-through RAM, cached RAM or MMIO (or MMIO only), aligned to `align`.
uint32_t RandomDataAddress(std::mt19937 &rng, uint32_t align, bool mmioOnly = false);

// Random operands for `spec`; fixes up address registers / GBR / R0 so that every data access
// is aligned and lands in RAM (cached or cache-through) or the rig's MMIO page. With
// `forceMmio`, the access always lands in the MMIO page.
uint16_t Encode(const OpSpec &spec, std::mt19937 &rng, std::array<uint32_t, 16> &regs, uint32_t &gbr,
                bool forceMmio = false);

// All opcodes the JIT compiles (grows with tasks 4-6).
std::span<const OpSpec> CompiledOpcodes();

} // namespace jitspec
