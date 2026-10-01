#include "sh2_test_rig.hpp"

#include <cstdio>
#include <cstring>

namespace sh2test {

namespace {

uint16_t ReadBE16(const uint8_t *p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t ReadBE32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void WriteBE16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void WriteBE32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

uint32_t RamOffset(uint32_t address) {
    return address & (kRamSize - 1);
}

} // namespace

Rig::Rig()
    : ram(std::make_unique<std::array<uint8_t, kRamSize>>()) {
    ram->fill(0);
    bus.MapArray(0x0000000, 0x1FFFFFF, *ram, true);
    bus.MapArray(0x4000000, 0x7FFFFFF, *ram, true);
    bus.SetAccessCycles(0x0000000, 0x1FFFFFF, 2, 3, 4, 5, 6, 7);
    bus.SetAccessCycles(0x4000000, 0x7FFFFFF, 2, 3, 4, 5, 6, 7);

    bus.MapNormal(
        0x2000000, 0x3FFFFFF, &mmio,
        [](uint32_t address, void *ctx) -> uint8_t { return static_cast<Mmio *>(ctx)->data[address & 0xFFFF]; },
        [](uint32_t address, void *ctx) -> uint16_t {
            return ReadBE16(&static_cast<Mmio *>(ctx)->data[address & 0xFFFE]);
        },
        [](uint32_t address, void *ctx) -> uint32_t {
            return ReadBE32(&static_cast<Mmio *>(ctx)->data[address & 0xFFFC]);
        },
        [](uint32_t address, uint8_t value, void *ctx) { static_cast<Mmio *>(ctx)->data[address & 0xFFFF] = value; },
        [](uint32_t address, uint16_t value, void *ctx) {
            WriteBE16(&static_cast<Mmio *>(ctx)->data[address & 0xFFFE], value);
        },
        [](uint32_t address, uint32_t value, void *ctx) {
            WriteBE32(&static_cast<Mmio *>(ctx)->data[address & 0xFFFC], value);
        },
        [](uint32_t, uint32_t, bool, void *ctx) -> bool {
            auto &m = *static_cast<Mmio *>(ctx);
            if (m.busWaitEvery == 0) {
                return false;
            }
            return (++m.busWaitQueries % m.busWaitEvery) == 0;
        });
    bus.SetAccessCycles(0x2000000, 0x3FFFFFF, 8, 9, 10, 11, 12, 13);

    sh2 = std::make_unique<ymir::sh2::SH2>(bus, true);
}

void Rig::WriteCode(uint32_t address, const std::vector<uint16_t> &words) {
    for (size_t i = 0; i < words.size(); ++i) {
        WriteBE16(&(*ram)[RamOffset(address + static_cast<uint32_t>(i * 2))], words[i]);
    }
}

void Rig::Write32(uint32_t address, uint32_t value) {
    WriteBE32(&(*ram)[RamOffset(address & ~3u)], value);
}

uint16_t Rig::Read16(uint32_t address) const {
    return ReadBE16(&(*ram)[RamOffset(address & ~1u)]);
}

uint32_t Rig::Read32(uint32_t address) const {
    return ReadBE32(&(*ram)[RamOffset(address & ~3u)]);
}

ymir::savestate::SH2SaveState Rig::State() const {
    ymir::savestate::SH2SaveState state{};
    sh2->SaveState(state);
    return state;
}

void Rig::Load(const ymir::savestate::SH2SaveState &state) {
    sh2->LoadState(state);
}

ymir::savestate::SH2SaveState Rig::BaseState(uint32_t pc) const {
    auto state = State();
    state.PC = pc;
    state.SR = 0xF0;
    state.delaySlot = false;
    state.delaySlotTarget = 0;
    state.intrAllow = true;
    state.wbReg = 0xFF;
    state.sleep = false;
    state.fetchedOpcodes = Read32(pc);
    return state;
}

std::string DiffRigs(const Rig &a, const Rig &b) {
    const auto sa = a.State();
    const auto sb = b.State();
    char buf[160];
    auto diff = [&](const char *name, uint64_t x, uint64_t y) -> std::string {
        std::snprintf(buf, sizeof(buf), "%s differs: interpreter=0x%llX jit=0x%llX", name,
                      static_cast<unsigned long long>(x), static_cast<unsigned long long>(y));
        return buf;
    };
    for (int i = 0; i < 16; ++i) {
        if (sa.R[i] != sb.R[i]) {
            std::snprintf(buf, sizeof(buf), "R%d", i);
            return diff(buf, sa.R[i], sb.R[i]);
        }
    }
    if (sa.PC != sb.PC) return diff("PC", sa.PC, sb.PC);
    if (sa.PR != sb.PR) return diff("PR", sa.PR, sb.PR);
    if (sa.MACL != sb.MACL) return diff("MACL", sa.MACL, sb.MACL);
    if (sa.MACH != sb.MACH) return diff("MACH", sa.MACH, sb.MACH);
    if (sa.SR != sb.SR) return diff("SR", sa.SR, sb.SR);
    if (sa.GBR != sb.GBR) return diff("GBR", sa.GBR, sb.GBR);
    if (sa.VBR != sb.VBR) return diff("VBR", sa.VBR, sb.VBR);
    if (sa.delaySlot != sb.delaySlot) return diff("delaySlot", sa.delaySlot, sb.delaySlot);
    if (sa.delaySlotTarget != sb.delaySlotTarget) return diff("delaySlotTarget", sa.delaySlotTarget, sb.delaySlotTarget);
    if (sa.intrAllow != sb.intrAllow) return diff("intrAllow", sa.intrAllow, sb.intrAllow);
    if (sa.fetchedOpcodes != sb.fetchedOpcodes) return diff("fetchedOpcodes", sa.fetchedOpcodes, sb.fetchedOpcodes);
    if (sa.wbReg != sb.wbReg) return diff("wbReg", sa.wbReg, sb.wbReg);
    if (sa.sleep != sb.sleep) return diff("sleep", sa.sleep, sb.sleep);
    // memcmp fast path; the byte loops only run to locate the first difference.
    if (std::memcmp(a.ram->data(), b.ram->data(), kRamSize) != 0) {
        for (uint32_t i = 0; i < kRamSize; ++i) {
            if ((*a.ram)[i] != (*b.ram)[i]) {
                std::snprintf(buf, sizeof(buf), "RAM[0x%05X]", i);
                return diff(buf, (*a.ram)[i], (*b.ram)[i]);
            }
        }
    }
    if (std::memcmp(a.mmio.data.data(), b.mmio.data.data(), a.mmio.data.size()) != 0) {
        for (size_t i = 0; i < a.mmio.data.size(); ++i) {
            if (a.mmio.data[i] != b.mmio.data[i]) {
                std::snprintf(buf, sizeof(buf), "MMIO[0x%04zX]", i);
                return diff(buf, a.mmio.data[i], b.mmio.data[i]);
            }
        }
    }
    if (a.mmio.busWaitQueries != b.mmio.busWaitQueries) {
        return diff("busWaitQueries", a.mmio.busWaitQueries, b.mmio.busWaitQueries);
    }
    return {};
}

} // namespace sh2test
