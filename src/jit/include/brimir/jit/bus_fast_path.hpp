#pragma once

// C++ reference of the inline bus fast path that generated code uses (design/sh2-jit-m2.md §4.4).
// Every function reads only the bus page table described by SH2JitBusLayout and mirrors, with cache
// emulation off, SH2::MemRead/MemWrite (partitions 0b000/0b001/0b101 go to the bus at
// address & 0x7FFFFFF, misaligned addresses masked to the access size), SH2::AccessCycles and
// SH2Bus::IsBusWait. When the page has no array (handler page or unmapped), the access must take
// its SH2JitContext callback instead.
//
// All functions except PeekOpcode require bus.pages != nullptr.

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <cstdint>

namespace brimir::jit {

// The page entry for a bus address: pages + ((address & addressMask) >> pageShift) * pageStride.
const uint8_t *PageEntry(const ymir::sh2::SH2JitBusLayout &bus, uint32_t address);

// Partition 0b000/0b001/0b101 data access to an array page: returns the byte pointer for the
// size-aligned address and sets writable; nullptr when the access must take the callback (writable
// is then left untouched).
uint8_t *FastArrayPointer(const ymir::sh2::SH2JitBusLayout &bus, uint32_t address, uint32_t size, bool &writable);

// SH2::AccessCycles<T, write, emulateCache = false> for every partition:
// 0b000 -> 1; 0b001/0b101 -> page read/write cycles for the size; 0b010/0b011/0b100/0b110 -> 1; 0b111 -> 4.
uint64_t FastAccessCycles(const ymir::sh2::SH2JitBusLayout &bus, uint32_t address, uint32_t size, bool write);

// SH2Bus::IsBusWait (on address & addressMask, any partition): false for array pages; otherwise
// unknown (returns false and sets needsCallback).
bool FastBusWait(const ymir::sh2::SH2JitBusLayout &bus, uint32_t address, bool &needsCallback);

// Side-effect-free 16-bit instruction peek for partitions 0b000/0b001/0b101 on array pages.
// Returns false (out untouched) when the peek must take the callback.
bool FastPeek16(const ymir::sh2::SH2JitBusLayout &bus, uint32_t address, uint16_t &out);

// ctx.peekInstruction(ctx.sh2, address), read with FastPeek16 when the context has a page table and
// the address is on an array page (same result: the peek callback reads the same array).
uint16_t PeekOpcode(ymir::sh2::SH2JitContext &ctx, uint32_t address);

// Host memory holding a block's code: the bytes of `words` instruction words from `start` on their
// array pages, as at most kMaxCodeHostRanges contiguous [lo, hi) ranges. A store through any alias
// of that memory (cached, cache-through, a mirror) has its host pointer inside a range.
constexpr uint32_t kMaxCodeHostRanges = 2;
struct CodeHostRanges {
    const uint8_t *lo[kMaxCodeHostRanges] = {};
    const uint8_t *hi[kMaxCodeHostRanges] = {};
    uint32_t count = 0;

    // Whether [p, p + size) overlaps a range.
    bool Overlaps(const uint8_t *p, uint32_t size) const {
        for (uint32_t i = 0; i < count; ++i) {
            if (p < hi[i] && p + size > lo[i]) {
                return true;
            }
        }
        return false;
    }
};

// Fills `out` and returns true if every word is on an array page (FastPeek16 would succeed) and the
// code needs at most kMaxCodeHostRanges ranges; otherwise returns false.
bool FindCodeHostRanges(const ymir::sh2::SH2JitBusLayout &bus, uint32_t start, uint32_t words, CodeHostRanges &out);

} // namespace brimir::jit
