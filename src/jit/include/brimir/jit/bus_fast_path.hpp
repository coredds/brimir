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

} // namespace brimir::jit
