// Brimir - brimir_bench instruction and I/O traces (--trace-msh2, --trace-io)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Header-only so the unit tests check the line formats and the address decoding. Only compiled into brimir_bench when
// BRIMIR_TRACE or BRIMIR_BUILD_TESTS is on (BRIMIR_BENCH_TRACE); the libretro core never includes it.
//
// Trace lines (frame = the bench's frame index from 0, PC = the master SH-2's PC as it executes, unmasked):
//   --trace-msh2: "<frame> <PC hex8>" per master SH-2 instruction
//   --trace-io:   "<frame> <PC hex8> <R|W><8|16|32> <addr hex8> <value hex>" per access of the master SH-2 (or its DMA
//                 controller) to SMPC, SCU registers, IOGA, the A-bus CS2/CD block area and the cartridge beyond the
//                 game's ROM data. The address is physical (cache-through mirror folded); the value has 2, 4 or 8 digits.
// The same fields come from MAME's debugger (docs/superpowers/records/2026-11-21-stv-support/trace/), which is how
// the two traces are compared.

#pragma once

#include <ymir/debug/sh2_tracer_base.hpp>
#include <ymir/hw/sh2/sh2.hpp>
#include <ymir/hw/sh2/sh2_decode.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace brimir_tools {

// The traced I/O windows (physical addresses). cartROMEnd is 0x02000000 + the end of the game's ROM data (where
// MAME's cartridge region turns to zeros): cartridge accesses at or above it (up to 0x04FFFFFF, the 315-5881
// registers included) are traced, ROM reads below it are not.
inline bool IsTracedIOAddress(uint32_t phys, uint32_t cartROMEnd) {
    return (phys >= 0x0010'0000u && phys <= 0x0010'007Fu) ||                 // SMPC
           (phys >= 0x0040'0000u && phys <= 0x0040'007Fu) ||                 // IOGA (ST-V I/O board)
           (phys >= cartROMEnd && phys >= 0x0200'0000u && phys <= 0x04FF'FFFFu) || // cartridge beyond the ROM image
           (phys >= 0x0500'0000u && phys <= 0x058F'FFFFu) ||                 // A-bus CS2 (0x057FFFFC) and CD block
           (phys >= 0x05FE'0000u && phys <= 0x05FE'00CFu);                   // SCU registers
}

// The physical address of an SH-2 access, or false for areas that never reach the bus as a plain access (associative
// purge, cache address/data arrays, on-chip registers). Areas 0 (cached) and 1 (cache-through) share the 27-bit bus.
inline bool PhysicalAddress(uint32_t address, uint32_t &phys) {
    if ((address >> 29) > 1) {
        return false;
    }
    phys = address & 0x07FF'FFFFu;
    return true;
}

// "<frame> <PC hex8> <R|W><bits> <addr hex8> <value hex>" plus a newline, for an access of `size` bytes (1, 2 or 4).
// Returns the length written (buf needs 48 bytes).
inline int FormatIOLine(char *buf, size_t len, int frame, uint32_t pc, bool write, unsigned size, uint32_t address,
                        uint32_t value) {
    const unsigned digits = size * 2;
    const uint32_t mask = size >= 4 ? 0xFFFF'FFFFu : (1u << (size * 8)) - 1;
    return std::snprintf(buf, len, "%d %08X %c%u %08X %0*X\n", frame, pc, write ? 'W' : 'R', size * 8, address,
                         static_cast<int>(digits), value & mask);
}

// "<frame> <PC hex8>" plus a newline. Returns the length written (buf needs 24 bytes).
inline int FormatPCLine(char *buf, int frame, uint32_t pc) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    int n = std::snprintf(buf, 16, "%d ", frame);
    for (int shift = 28; shift >= 0; shift -= 4) {
        buf[n++] = kHex[(pc >> shift) & 0xF];
    }
    buf[n++] = '\n';
    return n;
}

// Where an instruction's data goes or comes from, for the I/O trace values. Plain MOV loads and stores name a general
// register (stores: the source before the instruction; loads: the destination after it). For anything else
// (read-modify-write, MAC, LDS/LDC/STS/STC to memory) the value is peeked: before the instruction for reads, after it
// for writes.
struct DataRegister {
    int reg = -1; // -1: peek the memory instead
};

inline DataRegister IOLoadStoreRegister(uint16_t op) {
    const unsigned hi = op >> 12;
    const unsigned n = (op >> 8) & 0xF;
    const unsigned m = (op >> 4) & 0xF;
    const unsigned lo = op & 0xF;
    switch (hi) {
    case 0x0:
        if (lo >= 0x4 && lo <= 0x6) {
            return {static_cast<int>(m)}; // mov.x Rm, @(R0,Rn)
        }
        if (lo >= 0xC && lo <= 0xE) {
            return {static_cast<int>(n)}; // mov.x @(R0,Rm), Rn
        }
        break;
    case 0x1: return {static_cast<int>(m)}; // mov.l Rm, @(disp,Rn)
    case 0x2:
        if (lo <= 0x2 || (lo >= 0x4 && lo <= 0x6)) {
            return {static_cast<int>(m)}; // mov.x Rm, @Rn / mov.x Rm, @-Rn
        }
        break;
    case 0x5: return {static_cast<int>(n)}; // mov.l @(disp,Rm), Rn
    case 0x6:
        if (lo <= 0x2 || (lo >= 0x4 && lo <= 0x6)) {
            return {static_cast<int>(n)}; // mov.x @Rm, Rn / mov.x @Rm+, Rn
        }
        break;
    case 0x8:
        if (n == 0x0 || n == 0x1 || n == 0x4 || n == 0x5) {
            return {0}; // mov.b/w R0, @(disp,Rn) / mov.b/w @(disp,Rm), R0
        }
        break;
    case 0xC:
        if (n <= 0x2 || (n >= 0x4 && n <= 0x6)) {
            return {0}; // mov.x R0, @(disp,GBR) / mov.x @(disp,GBR), R0
        }
        break;
    default: break;
    }
    return {};
}

// Writes the master SH-2's trace lines. Attach with sh2.UseTracer(&tracer) on a Saturn with debug tracing enabled;
// call SetFrame before each frame and Flush at the end (an access whose value is only known after its instruction
// is written when the next instruction starts).
class SH2TraceWriter final : public ymir::debug::ISH2Tracer {
public:
    // pcOut / ioOut may be null (that trace is off). The files stay owned by the caller.
    SH2TraceWriter(ymir::sh2::SH2 &sh2, std::FILE *pcOut, std::FILE *ioOut, uint32_t cartROMEnd)
        : m_sh2(sh2)
        , m_pcOut(pcOut)
        , m_ioOut(ioOut)
        , m_cartROMEnd(cartROMEnd) {}

    void SetFrame(int frame) { m_frame = frame; }
    // Turns the instruction trace on or off (the I/O trace always runs)
    void SetPCTraceEnabled(bool enabled) { m_pcEnabled = enabled; }
    uint64_t IOLines() const { return m_ioLines; }

    void Flush() {
        ResolvePending();
        if (m_pcOut != nullptr) {
            std::fflush(m_pcOut);
        }
        if (m_ioOut != nullptr) {
            std::fflush(m_ioOut);
        }
    }

    void ExecuteInstruction(uint32_t pc, uint16_t opcode, bool delaySlot) override {
        (void)delaySlot;
        ResolvePending();
        m_pc = pc;
        if (m_pcOut != nullptr && m_pcEnabled) {
            char buf[24];
            std::fwrite(buf, 1, static_cast<size_t>(FormatPCLine(buf, m_frame, pc)), m_pcOut);
        }
        if (m_ioOut == nullptr) {
            return;
        }
        const auto &mem = ymir::sh2::DecodeTable::s_instance.mem[opcode];
        if (!mem.anyAccess) {
            return;
        }
        // The decode table puts most loads in `first` and stores in `second`; a plain MOV has exactly one of them
        using Type = ymir::sh2::DecodedMemAccesses::Type;
        const bool single = (mem.first.type == Type::None) != (mem.second.type == Type::None);
        const DataRegister data = single ? IOLoadStoreRegister(opcode) : DataRegister{};
        Access(mem.first, data);
        Access(mem.second, data);
    }

    void DMAXferData(uint32_t channel, uint32_t srcAddress, uint32_t dstAddress, uint32_t data,
                     uint32_t unitSize) override {
        (void)channel;
        if (m_ioOut == nullptr) {
            return;
        }
        uint32_t phys;
        if (PhysicalAddress(srcAddress, phys) && IsTracedIOAddress(phys, m_cartROMEnd)) {
            Write(false, unitSize, phys, data);
        }
        if (PhysicalAddress(dstAddress, phys) && IsTracedIOAddress(phys, m_cartROMEnd)) {
            Write(true, unitSize, phys, data);
        }
    }

private:
    struct Pending {
        bool write;
        unsigned size;
        uint32_t phys;
        uint32_t address; // as the CPU issued it (for the peek)
        int reg;          // -1: peek
    };

    void Access(const ymir::sh2::DecodedMemAccesses::Access &access, DataRegister data) {
        using Type = ymir::sh2::DecodedMemAccesses::Type;
        auto &probe = m_sh2.GetProbe();
        const auto &r = probe.R();
        uint32_t address;
        switch (access.type) {
        case Type::AtReg: address = r[access.reg]; break;
        case Type::AtR0Reg: address = r[0] + r[access.reg]; break;
        case Type::AtR0GBR: address = r[0] + probe.GBR(); break;
        case Type::AtDispReg: address = r[access.reg] + static_cast<uint32_t>(static_cast<int32_t>(access.disp)); break;
        case Type::AtDispGBR: address = probe.GBR() + static_cast<uint32_t>(static_cast<int32_t>(access.disp)); break;
        default: return; // none, or a PC-relative literal (always ROM/RAM)
        }
        uint32_t phys;
        if (!PhysicalAddress(address, phys) || !IsTracedIOAddress(phys, m_cartROMEnd)) {
            return;
        }
        if (access.write && data.reg >= 0) {
            Write(true, access.size, phys, r[data.reg]);
        } else if (!access.write && data.reg < 0) {
            Write(false, access.size, phys, Peek(address, access.size));
        } else {
            // a load (value in the register after it) or a store whose value is only in memory afterwards
            m_pending.push_back({access.write, access.size, phys, address, data.reg});
        }
    }

    uint32_t Peek(uint32_t address, unsigned size) const {
        const auto &probe = m_sh2.GetProbe();
        switch (size) {
        case 1: return probe.MemPeekByte(address, true);
        case 2: return probe.MemPeekWord(address, true);
        default: return probe.MemPeekLong(address, true);
        }
    }

    void ResolvePending() {
        if (m_pending.empty()) {
            return;
        }
        const auto &r = m_sh2.GetProbe().R();
        for (const Pending &p : m_pending) {
            Write(p.write, p.size, p.phys, p.reg >= 0 ? r[p.reg] : Peek(p.address, p.size));
        }
        m_pending.clear();
    }

    void Write(bool write, unsigned size, uint32_t phys, uint32_t value) {
        char buf[48];
        const int n = FormatIOLine(buf, sizeof(buf), m_frame, m_pc, write, size, phys, value);
        std::fwrite(buf, 1, static_cast<size_t>(n), m_ioOut);
        ++m_ioLines;
    }

    ymir::sh2::SH2 &m_sh2;
    std::FILE *m_pcOut;
    std::FILE *m_ioOut;
    uint32_t m_cartROMEnd;
    int m_frame = 0;
    uint32_t m_pc = 0;
    bool m_pcEnabled = true;
    uint64_t m_ioLines = 0;
    std::vector<Pending> m_pending;
};

} // namespace brimir_tools
