#include <brimir/jit/bus_fast_path.hpp>

#include <cstring>

namespace brimir::jit {

namespace {

// Partitions whose accesses go straight to the bus with cache emulation off.
constexpr bool IsBusPartition(uint32_t address) {
    const uint32_t partition = address >> 29;
    return partition == 0b000 || partition == 0b001 || partition == 0b101;
}

uint8_t *PageArray(const uint8_t *entry, const ymir::sh2::SH2JitBusLayout &bus) {
    uint8_t *array = nullptr;
    std::memcpy(&array, entry + bus.arrayOffset, sizeof(array));
    return array;
}

uint32_t SizeIndex(uint32_t size) {
    return size == 1 ? 0 : size == 2 ? 1 : 2;
}

} // namespace

const uint8_t *PageEntry(const ymir::sh2::SH2JitBusLayout &bus, uint32_t address) {
    const uint32_t index = (address & bus.addressMask) >> bus.pageShift;
    return bus.pages + static_cast<size_t>(index) * bus.pageStride;
}

uint8_t *FastArrayPointer(const ymir::sh2::SH2JitBusLayout &bus, uint32_t address, uint32_t size, bool &writable) {
    if (!IsBusPartition(address)) {
        return nullptr;
    }
    const uint32_t aligned = address & ~(size - 1);
    const uint8_t *entry = PageEntry(bus, aligned);
    uint8_t *array = PageArray(entry, bus);
    if (array == nullptr) {
        return nullptr;
    }
    bool w = false;
    std::memcpy(&w, entry + bus.arrayWritableOffset, sizeof(w));
    writable = w;
    const uint32_t pageMask = (1u << bus.pageShift) - 1;
    return array + (aligned & pageMask);
}

uint64_t FastAccessCycles(const ymir::sh2::SH2JitBusLayout &bus, uint32_t address, uint32_t size, bool write) {
    switch (address >> 29) {
    case 0b001:
    case 0b101: {
        const uint8_t *entry = PageEntry(bus, address);
        const uint32_t offset = write ? bus.writeCyclesOffset[SizeIndex(size)] : bus.readCyclesOffset[SizeIndex(size)];
        uint64_t cycles = 0;
        std::memcpy(&cycles, entry + offset, sizeof(cycles));
        return cycles;
    }
    case 0b111: return 4; // I/O area
    default: return 1;    // cached area (all hits without cache emulation), purge, cache arrays
    }
}

bool FastBusWait(const ymir::sh2::SH2JitBusLayout &bus, uint32_t address, bool &needsCallback) {
    needsCallback = PageArray(PageEntry(bus, address), bus) == nullptr;
    return false;
}

bool FastPeek16(const ymir::sh2::SH2JitBusLayout &bus, uint32_t address, uint16_t &out) {
    bool writable = false;
    const uint8_t *p = FastArrayPointer(bus, address, 2, writable);
    if (p == nullptr) {
        return false;
    }
    out = static_cast<uint16_t>((p[0] << 8) | p[1]);
    return true;
}

} // namespace brimir::jit
