// Brimir - table of SH-2 opcodes compiled by the JIT, for table-driven differential tests.
// Licensed under GPL-3.0

#include "jit_opcode_specs.hpp"

namespace jitspec {

namespace {

// clang-format off
constexpr OpSpec kSpecs[] = {
    // name        base    fmt       addr             size slotOk
    // Milestone 1B/1C opcodes
    {"NOP",      0x0009, Fmt::Z,   Addr::None,      0, true},
    {"MOV_R",    0x6003, Fmt::NM,  Addr::None,      0, true},
    {"MOV_I",    0xE000, Fmt::NI,  Addr::None,      0, true},
    {"MOVB_L",   0x6000, Fmt::NM,  Addr::Rm,        1, true},
    {"MOVL_L",   0x6002, Fmt::NM,  Addr::Rm,        4, true},
    {"MOVB_S",   0x2000, Fmt::NM,  Addr::Rn,        1, true},
    {"MOVL_S",   0x2002, Fmt::NM,  Addr::Rn,        4, true},
    {"MOVL_I",   0xD000, Fmt::ND8, Addr::None,      4, true},  // PC-relative
    {"ADD",      0x300C, Fmt::NM,  Addr::None,      0, true},
    {"ADD_I",    0x7000, Fmt::NI,  Addr::None,      0, true},
    {"CMP_EQ_R", 0x3000, Fmt::NM,  Addr::None,      0, true},
    {"DT",       0x4010, Fmt::N,   Addr::None,      0, true},
    {"BRA",      0xA000, Fmt::D,   Addr::None,      0, false}, // disp12; not encoded randomly
    {"BT",       0x8900, Fmt::D,   Addr::None,      0, false},
    {"BF",       0x8B00, Fmt::D,   Addr::None,      0, false},
    {"BTS",      0x8D00, Fmt::D,   Addr::None,      0, false},
    {"BFS",      0x8F00, Fmt::D,   Addr::None,      0, false},
    {"JMP",      0x402B, Fmt::M,   Addr::None,      0, false},
    {"RTS",      0x000B, Fmt::Z,   Addr::None,      0, false},
    // Milestone 1D task 4: data transfer
    {"MOVW_L",   0x6001, Fmt::NM,  Addr::Rm,        2, true},
    {"MOVB_L0",  0x000C, Fmt::NM,  Addr::RmR0,      1, true},
    {"MOVW_L0",  0x000D, Fmt::NM,  Addr::RmR0,      2, true},
    {"MOVL_L0",  0x000E, Fmt::NM,  Addr::RmR0,      4, true},
    {"MOVB_L4",  0x8400, Fmt::MD,  Addr::RmDisp,    1, true},
    {"MOVW_L4",  0x8500, Fmt::MD,  Addr::RmDisp,    2, true},
    {"MOVL_L4",  0x5000, Fmt::NMD, Addr::RmDisp,    4, true},
    {"MOVB_LG",  0xC400, Fmt::D,   Addr::GbrDisp,   1, true},
    {"MOVW_LG",  0xC500, Fmt::D,   Addr::GbrDisp,   2, true},
    {"MOVL_LG",  0xC600, Fmt::D,   Addr::GbrDisp,   4, true},
    {"MOVB_P",   0x6004, Fmt::NM,  Addr::RmPostInc, 1, true},
    {"MOVW_P",   0x6005, Fmt::NM,  Addr::RmPostInc, 2, true},
    {"MOVL_P",   0x6006, Fmt::NM,  Addr::RmPostInc, 4, true},
    {"MOVW_I",   0x9000, Fmt::ND8, Addr::None,      2, true},  // PC-relative
    {"MOVA",     0xC700, Fmt::D,   Addr::None,      0, true},
    {"MOVW_S",   0x2001, Fmt::NM,  Addr::Rn,        2, true},
    {"MOVB_M",   0x2004, Fmt::NM,  Addr::RnPreDec,  1, true},
    {"MOVW_M",   0x2005, Fmt::NM,  Addr::RnPreDec,  2, true},
    {"MOVL_M",   0x2006, Fmt::NM,  Addr::RnPreDec,  4, true},
    {"MOVB_S0",  0x0004, Fmt::NM,  Addr::RnR0,      1, true},
    {"MOVW_S0",  0x0005, Fmt::NM,  Addr::RnR0,      2, true},
    {"MOVL_S0",  0x0006, Fmt::NM,  Addr::RnR0,      4, true},
    {"MOVB_S4",  0x8000, Fmt::ND4, Addr::RnDisp,    1, true},
    {"MOVW_S4",  0x8100, Fmt::ND4, Addr::RnDisp,    2, true},
    {"MOVL_S4",  0x1000, Fmt::NMD, Addr::RnDisp,    4, true},
    {"MOVB_SG",  0xC000, Fmt::D,   Addr::GbrDisp,   1, true},
    {"MOVW_SG",  0xC100, Fmt::D,   Addr::GbrDisp,   2, true},
    {"MOVL_SG",  0xC200, Fmt::D,   Addr::GbrDisp,   4, true},
    {"MOVT",     0x0029, Fmt::N,   Addr::None,      0, true},
    {"CLRT",     0x0008, Fmt::Z,   Addr::None,      0, true},
    {"SETT",     0x0018, Fmt::Z,   Addr::None,      0, true},
};
// clang-format on

// Random R0 offset 0..0xF0, aligned to `align`.
uint32_t RandomOffset(std::mt19937 &rng, uint32_t align) {
    return (rng() % 0xF1u) & ~(align - 1);
}

} // namespace

uint32_t RandomDataAddress(std::mt19937 &rng, uint32_t align, bool mmioOnly) {
    const uint32_t offset = (rng() & 0xFFF0u) | (rng() & 0xFu & ~(align - 1));
    const uint32_t region = rng() % 3;
    if (mmioOnly) {
        return kMmio + offset;
    }
    switch (region) {
    case 0: return 0x26040000 + offset;
    case 1: return 0x06040000 + offset;
    default: return kMmio + offset;
    }
}

uint16_t Encode(const OpSpec &spec, std::mt19937 &rng, std::array<uint32_t, 16> &regs, uint32_t &gbr,
                bool forceMmio) {
    const uint32_t f8 = rng() % 16; // bits 11..8
    const uint32_t f4 = rng() % 16; // bits 7..4
    const uint32_t d4 = rng() & 0xFu;
    const uint32_t d8 = rng() & 0xFFu;

    uint32_t word = spec.base;
    uint32_t n = 0;
    uint32_t m = 0;
    uint32_t disp = 0;
    switch (spec.fmt) {
    case Fmt::Z: break;
    case Fmt::N: word |= f8 << 8; n = f8; break;
    case Fmt::M: word |= f8 << 8; m = f8; break;
    case Fmt::NM: word |= (f8 << 8) | (f4 << 4); n = f8; m = f4; break;
    case Fmt::MD: word |= (f4 << 4) | d4; m = f4; disp = d4; break;
    case Fmt::ND4: word |= (f4 << 4) | d4; n = f4; disp = d4; break;
    case Fmt::NMD: word |= (f8 << 8) | (f4 << 4) | d4; n = f8; m = f4; disp = d4; break;
    case Fmt::D: word |= d8; disp = d8; break;
    case Fmt::ND8: word |= (f8 << 8) | d8; n = f8; disp = d8; break;
    case Fmt::I: word |= d8; break;
    case Fmt::NI: word |= (f8 << 8) | d8; n = f8; break;
    }

    if (spec.addr != Addr::None) {
        const uint32_t size = spec.size;
        const uint32_t a = RandomDataAddress(rng, size, forceMmio);
        const auto indexed = [&](uint32_t base) {
            if (base == 0) {
                // @(R0,R0): address = 2 * R0. Halving keeps the access aligned and in the same
                // region (A & ~1 for bytes, A otherwise).
                regs[0] = a >> 1;
            } else {
                const uint32_t off = RandomOffset(rng, size);
                regs[0] = off;
                regs[base] = a - off;
            }
        };
        switch (spec.addr) {
        case Addr::None: break;
        case Addr::Rm: regs[m] = a; break;
        case Addr::Rn: regs[n] = a; break;
        case Addr::RmR0: indexed(m); break;
        case Addr::RnR0: indexed(n); break;
        case Addr::RmDisp: regs[m] = a - disp * size; break;
        case Addr::RnDisp: regs[n] = a - disp * size; break;
        case Addr::GbrDisp: gbr = a - disp * size; break;
        case Addr::GbrR0: {
            const uint32_t off = RandomOffset(rng, size);
            regs[0] = off;
            gbr = a - off;
            break;
        }
        case Addr::RnPreDec: regs[n] = a + size; break;
        case Addr::RmPostInc: regs[m] = a; break;
        }
    }
    return static_cast<uint16_t>(word);
}

std::span<const OpSpec> CompiledOpcodes() {
    return kSpecs;
}

} // namespace jitspec
