#include "sh2_test_rig.hpp"

#include <algorithm>
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
        [](uint32_t address, void *ctx) -> uint8_t {
            auto &m = *static_cast<Mmio *>(ctx);
            const uint8_t value = m.data[address & 0xFFFF];
            m.log.push_back({'R', 1, address, value});
            return value;
        },
        [](uint32_t address, void *ctx) -> uint16_t {
            auto &m = *static_cast<Mmio *>(ctx);
            const uint16_t value = ReadBE16(&m.data[address & 0xFFFE]);
            m.log.push_back({'R', 2, address, value});
            return value;
        },
        [](uint32_t address, void *ctx) -> uint32_t {
            auto &m = *static_cast<Mmio *>(ctx);
            const uint32_t value = ReadBE32(&m.data[address & 0xFFFC]);
            m.log.push_back({'R', 4, address, value});
            return value;
        },
        [](uint32_t address, uint8_t value, void *ctx) {
            auto &m = *static_cast<Mmio *>(ctx);
            m.data[address & 0xFFFF] = value;
            m.log.push_back({'W', 1, address, value});
        },
        [](uint32_t address, uint16_t value, void *ctx) {
            auto &m = *static_cast<Mmio *>(ctx);
            WriteBE16(&m.data[address & 0xFFFE], value);
            m.log.push_back({'W', 2, address, value});
        },
        [](uint32_t address, uint32_t value, void *ctx) {
            auto &m = *static_cast<Mmio *>(ctx);
            WriteBE32(&m.data[address & 0xFFFC], value);
            m.log.push_back({'W', 4, address, value});
        },
        [](uint32_t address, uint32_t size, bool, void *ctx) -> bool {
            auto &m = *static_cast<Mmio *>(ctx);
            m.log.push_back({'B', static_cast<uint8_t>(size), address, 0});
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

std::string DiffRigs(const Rig &a, const Rig &b, bool comparePeripherals) {
    const auto scope = comparePeripherals ? brimir::SH2DiffScope::CpuAndPeripherals : brimir::SH2DiffScope::Cpu;
    if (std::string diff = brimir::DiffSH2State(a.State(), b.State(), scope); !diff.empty()) {
        return diff;
    }
    char buf[160];
    auto diff = [&](const char *name, uint64_t x, uint64_t y) -> std::string {
        std::snprintf(buf, sizeof(buf), "%s differs: interpreter=0x%llX jit=0x%llX", name,
                      static_cast<unsigned long long>(x), static_cast<unsigned long long>(y));
        return buf;
    };
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
    const size_t common = std::min(a.mmio.log.size(), b.mmio.log.size());
    for (size_t i = 0; i < common; ++i) {
        const MmioAccess &x = a.mmio.log[i];
        const MmioAccess &y = b.mmio.log[i];
        if (x.kind != y.kind || x.size != y.size || x.address != y.address || x.value != y.value) {
            std::snprintf(buf, sizeof(buf),
                          "MMIO log entry %zu differs: interpreter=%c%u@%08X=%08X jit=%c%u@%08X=%08X", i, x.kind,
                          x.size, x.address, x.value, y.kind, y.size, y.address, y.value);
            return buf;
        }
    }
    if (a.mmio.log.size() != b.mmio.log.size()) {
        return diff("MMIO log length", a.mmio.log.size(), b.mmio.log.size());
    }
    return {};
}

} // namespace sh2test

