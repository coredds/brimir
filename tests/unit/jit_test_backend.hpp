#pragma once

// Backend selection for the SH-2 JIT tests. The environment variable BRIMIR_JIT_BACKEND (ir|x64)
// picks the backend the executor-level tests run on; unset, they use DefaultBackend().

#include <brimir/jit/backend.hpp>
#include <brimir/jit/interp_backend.hpp>
#include <brimir/jit/ir.hpp>

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include "sh2_test_rig.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace sh2test {

// BRIMIR_JIT_BACKEND if set (fails the test if it names an unknown or unavailable backend),
// else DefaultBackend().
brimir::jit::BackendKind TestBackend();

// Every backend available in this build (Ir first).
std::vector<brimir::jit::BackendKind> AvailableBackends();

// Runs one verified block on `kind`: RunBlock for Ir, otherwise REQUIREs that the native backend
// compiles it and runs the generated code. The backend lives for the duration of the call.
brimir::jit::ExitInfo RunOnBackend(brimir::jit::BackendKind kind, const brimir::jit::Block &block,
                                   ymir::sh2::SH2JitContext &ctx,
                                   uint64_t target = brimir::jit::kNoCycleTarget,
                                   const bool *abortRequested = nullptr);

// Bus fast-path test pages, on top of the rig's RAM and MMIO: a read-only 64 KiB ROM page at bus
// 0x4000000-0x400FFFF (replacing a RAM mirror page) and an unmapped page at 0x5010000-0x501FFFF.
// SH-2 address offsets (low 29 bits) into each kind of page:
constexpr uint32_t kFastRamOffset = 0x06001230;
constexpr uint32_t kFastMmioOffset = 0x02000010;
constexpr uint32_t kFastRomOffset = 0x04000120;
constexpr uint32_t kFastUnmappedOffset = 0x05010040;
using FastRom = std::array<uint8_t, 0x10000>;

// Fills `rom` with a fixed pattern and maps it and the unmapped page on `rig`'s bus.
void MapFastPathTestPages(Rig &rig, FastRom &rom);

// Every partition (top 3 bits 0-7) x {RAM, MMIO, ROM, unmapped offset} x {+0, +1, +2, +3}.
std::vector<uint32_t> FastPathTestAddresses();

} // namespace sh2test
