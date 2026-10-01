# SH-2 JIT Milestone 1B — Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the validated JIT pipeline end to end: the fork's executor hook, an IR, a front end for a 19-opcode vertical slice, a block cache, an IR-interpreter backend, an executor, strict differential tests in CI, and a core option to turn it on.

**Architecture:** The forked `SH2` exposes an `SH2JitContext` (pointers to live state and callbacks that reuse the interpreter's own memory, timing and delay-slot helpers with cache emulation off). `SH2::Advance<false, false>` hands control to an optional `ISH2Executor`. The new `brimir-jit` library decodes guest blocks into IR, caches them per PC with check-on-entry validation, and runs them on an IR interpreter. Anything not supported falls back to one interpreter instruction.

**Tech Stack:** C++20, CMake 3.28+, Catch2 (amalgamated), MSVC 2022 / GCC 14 / Apple Clang.

**Spec:** `design/sh2-jit.md`. Prerequisite: plan `2026-09-30-sh2-jit-m1a-measurement.md` Task 1 (creates `src/core/BRIMIR_FORK.md`).

## Global Constraints

- Fork scope is exactly `src/core/include/ymir/hw/sh2/*` and `src/core/src/ymir/hw/sh2/*`. No other file under `src/core/` changes. Mark fork edits with `// Brimir:` and log them in `src/core/BRIMIR_FORK.md`.
- With no executor attached (the default), emulation is byte-identical to today.
- Compiled blocks must produce exactly the interpreter's architectural state (R0–R15, PC, PR, SR, GBR, VBR, MACH/MACL, delay-slot flag and target, fetched-opcode buffer, write-back register, interrupt-allow flag), memory contents, bus call sequence and cycle total for the same instructions.
- The JIT runs only in `Advance<false, false>` (no debug tracing, no cache emulation).
- `brimir-jit` includes only `ymir/core/types.hpp`, `ymir/hw/sh2/sh2_jit_iface.hpp` and `ymir/hw/sh2/sh2_decode.hpp` from the core.
- Tests: Catch2 in `tests/unit/`, tagged `[jit]`, registered in `tests/CMakeLists.txt`, no BIOS required (BIOS-dependent cases must `SKIP`).
- Commit messages: `type(scope): subject`.

## Scope decisions (differences from the spec, agreed during planning)

- **Opcode coverage is a vertical slice** of 19 opcodes that exercises every hard pattern (loads with and without bus-wait retry, stores, PC-relative literal loads, write-back stalls, T bit, conditional branches, all delay-slot shapes): `NOP, MOV Rm,Rn, MOV #imm,Rn, MOV.B @Rm,Rn, MOV.L @Rm,Rn, MOV.B Rm,@Rn, MOV.L Rm,@Rn, MOV.L @(disp,PC),Rn, ADD Rm,Rn, ADD #imm,Rn, CMP/EQ Rm,Rn, DT Rn, BT, BF, BT/S, BF/S, BRA, JMP @Rm, RTS`. The rest of the milestone-1 subset and shadow-verify mode (spec 7.2) are plan 1C.
- **Memory operations call the fork's interpreter helpers** (`MemRead`/`MemWrite`/`AccessCycles`/`IsBusWait`) instead of inlining a RAM fast path. This is exact by construction; the inline fast path is a milestone-2 optimization.
- **The block cache is keyed by the full guest PC**, not a normalized address: block exits write constant PCs that include the partition bits, so aliases must not share a block. Task 4 updates the spec.
- **Pipeline refills are emitted exactly where the interpreter performs them** (every instruction at a 4-byte-aligned PC, taken non-delayed branches, delay-slot exits to `PC & 2` targets).
- The game-database "interpreter only" flag is deferred to plan 1C (nothing needs it before real-game validation).

## Build and test commands (Windows)

From a *Developer PowerShell for VS 2022*:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBRIMIR_BUILD_TESTS=ON
cmake --build build --target brimir_tests brimir_libretro
ctest --test-dir build --output-on-failure
build\bin\brimir_tests.exe "[jit]"
```

Linux: add `-DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_C_COMPILER=gcc-14`; binary is `./build/bin/brimir_tests`.

## Reference: interpreter facts the JIT must reproduce

Read these in `src/core/src/ymir/hw/sh2/sh2.cpp` before Task 4 (line numbers as of this plan):

- `InterpretNext` (2198): services an interrupt when `pending && allow`, otherwise sets `allow = true`, fetches (`FetchInstruction`: refills the 32-bit buffer when `(PC & 2) == 0`), decodes with `DecodeTable::s_instance.opcodes[m_delaySlot][instr]`, dispatches.
- `AdvancePC<.., delaySlot>` (2159): normal: `PC += 2`. In a delay slot: `PC = m_delaySlotTarget`, refill if `PC & 2`, `m_delaySlot = false`, recompute `m_intrFlags.pending`.
- `SetupDelaySlot` (2152): sets the flag and target and clears `m_intrFlags.pending`.
- `AccessCycles` (956): with cache emulation off, the cached partition (`0b000`) costs 1 cycle; cache-through uses the bus page table; I/O partition costs 4.
- `WritebackCycles(regs...)` (1779): 1 if any listed register equals `m_wbReg` (`0x10` = PR, `0xFF` = none).
- 16/32-bit load/store handlers check `m_bus.IsBusWait(address, size, write)` after adding access cycles; on a wait they return only the cycles so far and leave PC, registers and `m_wbReg` unchanged (the instruction retries). 8-bit handlers never check bus wait.
- Handlers used by the slice: `NOP` 2608, `MOV` 2643, `MOVBL` 2655, `MOVLL` 2684, `MOVBS` 2933, `MOVLS` 2960, `MOVI` 3101, `MOVLI` 3127, `ADD` 3605, `ADDI` 3618, `DT` 4049, `CMPEQ` 4276, `BF` 4421, `BFS` 4438, `BT` 4452, `BTS` 4469, `BRA` 4483, `JMP` 4536, `RTS` 4605.

---

### Task 1: Fork executor hook, context, and SH-2 test rig

**Files:**
- Create: `src/core/include/ymir/hw/sh2/sh2_jit_iface.hpp`
- Modify: `src/core/include/ymir/hw/sh2/sh2.hpp`
- Modify: `src/core/src/ymir/hw/sh2/sh2.cpp` (constructor 322, `Reset` 330, `Advance` 435, `LoadState` 580, new section before `// Probe implementation` ~4639)
- Modify: `src/core/BRIMIR_FORK.md` (Brimir changes table)
- Create: `tests/unit/sh2_test_rig.hpp`, `tests/unit/sh2_test_rig.cpp`
- Create: `tests/unit/test_sh2_jit_iface.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces (`namespace ymir::sh2`, header `ymir/hw/sh2/sh2_jit_iface.hpp`):
  - `struct SH2JitContext` with fields `R, PC, PR, GBR, VBR, SR` (`uint32*`), `delaySlotTarget` (`uint32*`), `delaySlot` (`bool*`), `wbReg` (`uint8*`), `intrPending`, `intrAllow` (`bool*`), `fetchedOpcodes` (`uint32*`), `cyclesExecuted` (`uint64*`), `sh2` (`void*`), and callbacks `interpretOne`, `read`, `write`, `peekInstruction`, `accessCycles`, `busWait`, `refillPipeline`, `setupDelaySlot`, `endDelaySlot` (signatures below).
  - `class ISH2Executor { virtual uint64 Run(SH2JitContext&, uint64 executed, uint64 target) = 0; virtual void Flush() = 0; }`
- Produces (`ymir::sh2::SH2`, public): `void SetJitExecutor(ISH2Executor*)`, `ISH2Executor* GetJitExecutor() const`, `SH2JitContext& GetJitContext()`.
- Produces (tests, `namespace sh2test`): `class Rig` (isolated SH-2 on a synthetic bus), `struct Mmio`, constants `kRamSize`, `std::string DiffRigs(const Rig&, const Rig&)`.

- [ ] **Step 1: Create the interface header**

Create `src/core/include/ymir/hw/sh2/sh2_jit_iface.hpp`:

```cpp
#pragma once

// Brimir: interface between the forked SH-2 and the SH-2 JIT.
// See src/core/BRIMIR_FORK.md and design/sh2-jit.md.
//
// The SH-2 fills in SH2JitContext with pointers to its live state and with
// callbacks that reuse the interpreter's own helpers with cache emulation
// disabled, so compiled code has exactly the interpreter's memory, timing and
// delay-slot behavior. The JIT library implements ISH2Executor.

#include <ymir/core/types.hpp>

namespace ymir::sh2 {

struct SH2JitContext {
    // Live CPU state of the owning SH2
    uint32 *R = nullptr; // R0..R15
    uint32 *PC = nullptr;
    uint32 *PR = nullptr;
    uint32 *GBR = nullptr;
    uint32 *VBR = nullptr;
    uint32 *SR = nullptr; // RegSR::u32, T is bit 0
    uint32 *delaySlotTarget = nullptr;
    bool *delaySlot = nullptr;
    uint8 *wbReg = nullptr; // 0x0..0xF: R0..R15, 0x10: PR, 0xFF: none
    bool *intrPending = nullptr;
    bool *intrAllow = nullptr;
    uint32 *fetchedOpcodes = nullptr; // 32-bit instruction fetch buffer

    // Cycles executed so far in the current Advance() call. On-chip timers (WDT, FRT) read it to
    // sync, so the executor must keep it current before every interpreter call and memory access.
    uint64 *cyclesExecuted = nullptr;

    void *sh2 = nullptr; // opaque owner, passed to every callback

    // Executes exactly one instruction (or interrupt entry) with the interpreter; returns cycles.
    uint64 (*interpretOne)(void *sh2) = nullptr;
    // SH2::MemRead / MemWrite with cache emulation off. size is 1, 2 or 4; reads are zero-extended.
    uint32 (*read)(void *sh2, uint32 address, uint32 size, bool instrFetch) = nullptr;
    void (*write)(void *sh2, uint32 address, uint32 size, uint32 value) = nullptr;
    // Side-effect-free instruction read, for decoding and block validation.
    uint16 (*peekInstruction)(void *sh2, uint32 address) = nullptr;
    // SH2::AccessCycles with cache emulation off.
    uint64 (*accessCycles)(void *sh2, uint32 address, uint32 size, bool write) = nullptr;
    // SH2Bus::IsBusWait.
    bool (*busWait)(void *sh2, uint32 address, uint32 size, bool write) = nullptr;
    // Loads the 32-bit instruction fetch buffer from address (SH2::RefillPipeline).
    void (*refillPipeline)(void *sh2, uint32 address) = nullptr;
    // SH2::SetupDelaySlot.
    void (*setupDelaySlot)(void *sh2, uint32 target) = nullptr;
    // SH2::AdvancePC for an instruction executed in a delay slot.
    void (*endDelaySlot)(void *sh2) = nullptr;
};

class ISH2Executor {
public:
    virtual ~ISH2Executor() = default;

    // Runs code until executed >= target and returns the new executed count.
    virtual uint64 Run(SH2JitContext &ctx, uint64 executed, uint64 target) = 0;

    // Drops all compiled code (reset, save-state load, executor attach).
    virtual void Flush() = 0;
};

} // namespace ymir::sh2
```

- [ ] **Step 2: Write the test rig**

Create `tests/unit/sh2_test_rig.hpp`:

```cpp
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

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sh2test {

constexpr uint32_t kRamSize = 0x100000;
constexpr uint32_t kSleep = 0x001B; // SLEEP: never supported by the JIT, ends test programs

struct Mmio {
    std::array<uint8_t, 0x10000> data{};
    uint32_t busWaitQueries = 0;
    uint32_t busWaitEvery = 0;
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

// Returns a description of the first difference in CPU state, RAM or MMIO, or "" if identical.
std::string DiffRigs(const Rig &a, const Rig &b);

} // namespace sh2test
```

Create `tests/unit/sh2_test_rig.cpp`:

```cpp
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
    for (uint32_t i = 0; i < kRamSize; ++i) {
        if ((*a.ram)[i] != (*b.ram)[i]) {
            std::snprintf(buf, sizeof(buf), "RAM[0x%05X]", i);
            return diff(buf, (*a.ram)[i], (*b.ram)[i]);
        }
    }
    for (size_t i = 0; i < a.mmio.data.size(); ++i) {
        if (a.mmio.data[i] != b.mmio.data[i]) {
            std::snprintf(buf, sizeof(buf), "MMIO[0x%04zX]", i);
            return diff(buf, a.mmio.data[i], b.mmio.data[i]);
        }
    }
    if (a.mmio.busWaitQueries != b.mmio.busWaitQueries) {
        return diff("busWaitQueries", a.mmio.busWaitQueries, b.mmio.busWaitQueries);
    }
    return {};
}

} // namespace sh2test
```

- [ ] **Step 3: Write the failing interface tests**

Create `tests/unit/test_sh2_jit_iface.cpp`:

```cpp
// Brimir - SH-2 fork JIT hook tests
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "sh2_test_rig.hpp"

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <memory>

using sh2test::Rig;

namespace {

constexpr uint32_t kCode = 0x06001000;

// Minimal executor: runs every instruction through the interpreter callback.
class DelegatingExecutor final : public ymir::sh2::ISH2Executor {
public:
    uint64 Run(ymir::sh2::SH2JitContext &ctx, uint64 executed, uint64 target) override {
        ++runCalls;
        while (executed < target) {
            *ctx.cyclesExecuted = executed;
            executed += ctx.interpretOne(ctx.sh2);
        }
        return executed;
    }
    void Flush() override {
        ++flushes;
    }

    int runCalls = 0;
    int flushes = 0;
};

// add #1,R0 ; bra kCode ; nop  (infinite loop)
const std::vector<uint16_t> kLoop = {0x7001, 0xAFFD, 0x0009};

} // namespace

TEST_CASE("Advance routes through an attached executor with identical results", "[jit][sh2]") {
    auto ref = std::make_unique<Rig>();
    auto jit = std::make_unique<Rig>();
    ref->WriteCode(kCode, kLoop);
    jit->WriteCode(kCode, kLoop);
    const auto state = ref->BaseState(kCode);
    ref->Load(state);
    jit->Load(state);

    DelegatingExecutor exec;
    jit->sh2->SetJitExecutor(&exec);
    REQUIRE(jit->sh2->GetJitExecutor() == &exec);

    const uint64 refCycles = ref->sh2->Advance<false, false>(1000);
    const uint64 jitCycles = jit->sh2->Advance<false, false>(1000);

    REQUIRE(exec.runCalls == 1);
    REQUIRE(refCycles == jitCycles);
    const std::string diff = sh2test::DiffRigs(*ref, *jit);
    INFO(diff);
    REQUIRE(diff.empty());
}

TEST_CASE("Advance with cache emulation ignores the executor", "[jit][sh2]") {
    auto rig = std::make_unique<Rig>();
    rig->WriteCode(kCode, kLoop);
    rig->Load(rig->BaseState(kCode));

    DelegatingExecutor exec;
    rig->sh2->SetJitExecutor(&exec);
    rig->sh2->Advance<false, true>(200);
    rig->sh2->Advance<true, false>(200);
    REQUIRE(exec.runCalls == 0);
}

TEST_CASE("Attaching, Reset and LoadState flush the executor", "[jit][sh2]") {
    auto rig = std::make_unique<Rig>();
    DelegatingExecutor exec;
    rig->sh2->SetJitExecutor(&exec);
    REQUIRE(exec.flushes == 1);
    rig->sh2->Reset(true);
    REQUIRE(exec.flushes == 2);
    rig->Load(rig->BaseState(kCode));
    REQUIRE(exec.flushes == 3);
    rig->sh2->SetJitExecutor(nullptr);
    rig->sh2->Reset(true);
    REQUIRE(exec.flushes == 3);
}

TEST_CASE("JIT context callbacks mirror interpreter memory semantics", "[jit][sh2]") {
    auto rig = std::make_unique<Rig>();
    rig->Load(rig->BaseState(kCode));
    auto &ctx = rig->sh2->GetJitContext();
    REQUIRE(ctx.sh2 == rig->sh2.get());
    REQUIRE(*ctx.PC == kCode);
    REQUIRE(*ctx.fetchedOpcodes == rig->State().fetchedOpcodes);
    REQUIRE(ctx.cyclesExecuted != nullptr);

    rig->Write32(0x06040000, 0x11223344);
    REQUIRE(ctx.read(ctx.sh2, 0x26040000, 4, false) == 0x11223344u);
    REQUIRE(ctx.read(ctx.sh2, 0x26040001, 1, false) == 0x22u);
    REQUIRE(ctx.read(ctx.sh2, 0x26040002, 2, false) == 0x3344u);

    ctx.write(ctx.sh2, 0x26040004, 2, 0xBEEF);
    REQUIRE(rig->Read16(0x06040004) == 0xBEEF);
    ctx.write(ctx.sh2, 0x22000010, 4, 0xCAFEF00D);
    REQUIRE(ctx.read(ctx.sh2, 0x22000010, 4, false) == 0xCAFEF00Du);

    // Wait states: cache-through uses the bus table, cached area costs 1, I/O area costs 4.
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x26040000, 1, false) == 2);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x26040000, 1, true) == 3);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x26040000, 2, false) == 4);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x26040000, 2, true) == 5);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x26040000, 4, false) == 6);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x26040000, 4, true) == 7);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x22000000, 4, true) == 13);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0x06040000, 4, false) == 1);
    REQUIRE(ctx.accessCycles(ctx.sh2, 0xFFFFFE10, 4, false) == 4);

    REQUIRE_FALSE(ctx.busWait(ctx.sh2, 0x26040000, 4, false));
    rig->mmio.busWaitEvery = 1;
    REQUIRE(ctx.busWait(ctx.sh2, 0x22000000, 4, false));
    REQUIRE(rig->mmio.busWaitQueries == 1);

    rig->WriteCode(kCode, {0x1234, 0x5678});
    REQUIRE(ctx.peekInstruction(ctx.sh2, kCode + 2) == 0x5678);
    ctx.refillPipeline(ctx.sh2, kCode);
    REQUIRE(rig->State().fetchedOpcodes == 0x12345678u);

    ctx.setupDelaySlot(ctx.sh2, kCode + 0x102);
    REQUIRE(*ctx.delaySlot);
    REQUIRE(*ctx.delaySlotTarget == kCode + 0x102);
    REQUIRE_FALSE(*ctx.intrPending);
    rig->WriteCode(kCode + 0x100, {0xAAAA, 0xBBBB});
    ctx.endDelaySlot(ctx.sh2);
    REQUIRE_FALSE(*ctx.delaySlot);
    REQUIRE(*ctx.PC == kCode + 0x102);
    REQUIRE(rig->State().fetchedOpcodes == 0xAAAABBBBu); // target & 2 -> refill

    // interpretOne executes exactly one instruction with the interpreter.
    rig->WriteCode(kCode + 0x200, {0x7005}); // add #5,R0
    auto s = rig->BaseState(kCode + 0x200);
    s.R[0] = 1;
    rig->Load(s);
    REQUIRE(ctx.interpretOne(ctx.sh2) == 1);
    REQUIRE(rig->State().R[0] == 6u);
    REQUIRE(*ctx.PC == kCode + 0x202);
}
```

Add to `tests/CMakeLists.txt`, in the `add_executable(brimir_tests ...)` source list after `unit/test_bios_integration.cpp`:

```cmake
    # SH-2 JIT (isolated SH-2 rig and differential tests)
    unit/sh2_test_rig.cpp
    unit/test_sh2_jit_iface.cpp
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `cmake --build build --target brimir_tests`
Expected: compile errors — `ymir/hw/sh2/sh2_jit_iface.hpp` defines the types but `SH2` has no `SetJitExecutor`, `GetJitExecutor`, `GetJitContext`.

- [ ] **Step 5: Add the hook to the fork**

In `src/core/include/ymir/hw/sh2/sh2.hpp`, add `#include "sh2_jit_iface.hpp"` after `#include "sh2_wdt.hpp"`. In the public section, directly after the host-time profiling block added in plan 1A (after `ConsumeHostTimeNs`), add:

```cpp

    // -------------------------------------------------------------------------
    // Brimir: SH-2 JIT hook (see src/core/BRIMIR_FORK.md)

    /// @brief Routes Advance<false, false>() through the executor. Pass nullptr to use the interpreter.
    /// Attaching flushes the executor.
    void SetJitExecutor(ISH2Executor *executor) {
        m_jitExecutor = executor;
        if (executor != nullptr) {
            executor->Flush();
        }
    }

    ISH2Executor *GetJitExecutor() const {
        return m_jitExecutor;
    }

    /// @brief Live-state view and callbacks used by the JIT (also used directly by tests).
    SH2JitContext &GetJitContext() {
        return m_jitContext;
    }
```

In the private section, directly after the `m_hostTimeNs` member added in plan 1A, add:

```cpp

    // Brimir: SH-2 JIT hook state and context callbacks
    SH2JitContext m_jitContext;
    ISH2Executor *m_jitExecutor = nullptr;

    void InitJitContext();
    static uint64 JitInterpretOne(void *ctx);
    static uint32 JitRead(void *ctx, uint32 address, uint32 size, bool instrFetch);
    static void JitWrite(void *ctx, uint32 address, uint32 size, uint32 value);
    static uint16 JitPeekInstruction(void *ctx, uint32 address);
    static uint64 JitAccessCycles(void *ctx, uint32 address, uint32 size, bool write);
    static bool JitBusWait(void *ctx, uint32 address, uint32 size, bool write);
    static void JitRefillPipeline(void *ctx, uint32 address);
    static void JitSetupDelaySlot(void *ctx, uint32 target);
    static void JitEndDelaySlot(void *ctx);
```

In `src/core/src/ymir/hw/sh2/sh2.cpp`:

1. In the constructor, make the first statement of the body (before `BCR1.MASTER = !master;`):

```cpp
    InitJitContext(); // Brimir: SH-2 JIT hook
```

2. At the end of `SH2::Reset` (after `TraceReset(...)`), add:

```cpp

    // Brimir: compiled code is invalid after a reset
    if (m_jitExecutor != nullptr) {
        m_jitExecutor->Flush();
    }
```

3. At the end of `SH2::LoadState` (after the `m_intrFlags.pending = ...` line), add the same block with the comment `// Brimir: compiled code is invalid after loading a state`.

4. In `SH2::Advance`, directly before `while (m_cyclesExecuted < cycles) {`, add:

```cpp
    // Brimir: SH-2 JIT hook. Only the plain configuration (no debug tracing, no cache emulation) is
    // compiled; the executor reproduces the loop below at block granularity.
    if constexpr (!debug && !emulateCache) {
        if (m_jitExecutor != nullptr) {
            m_cyclesExecuted = m_jitExecutor->Run(m_jitContext, m_cyclesExecuted, cycles);
            AdvanceDMA<debug, emulateCache>(m_cyclesExecuted - spilloverCycles);
            return m_cyclesExecuted;
        }
    }

```

5. Directly before the `// -----` line that precedes `// Probe implementation` (~line 4639), add:

```cpp
// -----------------------------------------------------------------------------
// Brimir: SH-2 JIT hook

void SH2::InitJitContext() {
    m_jitContext.R = R.data();
    m_jitContext.PC = &PC;
    m_jitContext.PR = &PR;
    m_jitContext.GBR = &GBR;
    m_jitContext.VBR = &VBR;
    m_jitContext.SR = &SR.u32;
    m_jitContext.delaySlotTarget = &m_delaySlotTarget;
    m_jitContext.delaySlot = &m_delaySlot;
    m_jitContext.wbReg = &m_wbReg;
    m_jitContext.intrPending = &m_intrFlags.pending;
    m_jitContext.intrAllow = &m_intrFlags.allow;
    m_jitContext.fetchedOpcodes = &m_fetchedOpcodes;
    m_jitContext.cyclesExecuted = &m_cyclesExecuted;
    m_jitContext.sh2 = this;
    m_jitContext.interpretOne = &SH2::JitInterpretOne;
    m_jitContext.read = &SH2::JitRead;
    m_jitContext.write = &SH2::JitWrite;
    m_jitContext.peekInstruction = &SH2::JitPeekInstruction;
    m_jitContext.accessCycles = &SH2::JitAccessCycles;
    m_jitContext.busWait = &SH2::JitBusWait;
    m_jitContext.refillPipeline = &SH2::JitRefillPipeline;
    m_jitContext.setupDelaySlot = &SH2::JitSetupDelaySlot;
    m_jitContext.endDelaySlot = &SH2::JitEndDelaySlot;
}

uint64 SH2::JitInterpretOne(void *ctx) {
    return static_cast<SH2 *>(ctx)->InterpretNext<false, false>();
}

uint32 SH2::JitRead(void *ctx, uint32 address, uint32 size, bool instrFetch) {
    auto &sh2 = *static_cast<SH2 *>(ctx);
    if (instrFetch) {
        switch (size) {
        case 1: return sh2.MemRead<uint8, true, false, false>(address);
        case 2: return sh2.MemRead<uint16, true, false, false>(address);
        default: return sh2.MemRead<uint32, true, false, false>(address);
        }
    }
    switch (size) {
    case 1: return sh2.MemRead<uint8, false, false, false>(address);
    case 2: return sh2.MemRead<uint16, false, false, false>(address);
    default: return sh2.MemRead<uint32, false, false, false>(address);
    }
}

void SH2::JitWrite(void *ctx, uint32 address, uint32 size, uint32 value) {
    auto &sh2 = *static_cast<SH2 *>(ctx);
    switch (size) {
    case 1: sh2.MemWrite<uint8, false, false, false>(address, static_cast<uint8>(value)); break;
    case 2: sh2.MemWrite<uint16, false, false, false>(address, static_cast<uint16>(value)); break;
    default: sh2.MemWrite<uint32, false, false, false>(address, value); break;
    }
}

uint16 SH2::JitPeekInstruction(void *ctx, uint32 address) {
    return static_cast<SH2 *>(ctx)->MemRead<uint16, true, true, false>(address);
}

uint64 SH2::JitAccessCycles(void *ctx, uint32 address, uint32 size, bool write) {
    auto &sh2 = *static_cast<SH2 *>(ctx);
    if (write) {
        switch (size) {
        case 1: return sh2.AccessCycles<uint8, true, false>(address);
        case 2: return sh2.AccessCycles<uint16, true, false>(address);
        default: return sh2.AccessCycles<uint32, true, false>(address);
        }
    }
    switch (size) {
    case 1: return sh2.AccessCycles<uint8, false, false>(address);
    case 2: return sh2.AccessCycles<uint16, false, false>(address);
    default: return sh2.AccessCycles<uint32, false, false>(address);
    }
}

bool SH2::JitBusWait(void *ctx, uint32 address, uint32 size, bool write) {
    return static_cast<SH2 *>(ctx)->m_bus.IsBusWait(address, size, write);
}

void SH2::JitRefillPipeline(void *ctx, uint32 address) {
    auto &sh2 = *static_cast<SH2 *>(ctx);
    sh2.m_fetchedOpcodes = sh2.MemRead<uint32, true, false, false>(address);
}

void SH2::JitSetupDelaySlot(void *ctx, uint32 target) {
    static_cast<SH2 *>(ctx)->SetupDelaySlot(target);
}

void SH2::JitEndDelaySlot(void *ctx) {
    static_cast<SH2 *>(ctx)->AdvancePC<false, false, true>();
}

```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build --target brimir_tests` then `build\bin\brimir_tests.exe "[jit]"`
Expected: `All tests passed` (4 test cases).
Run: `ctest --test-dir build --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 7: Log the fork change**

Append a row to the "Brimir changes" table in `src/core/BRIMIR_FORK.md`:

```markdown
| 2026-09-30 | `sh2_jit_iface.hpp` (new), `sh2.hpp`, `sh2.cpp` | SH-2 JIT hook: `SH2JitContext` + callbacks reusing interpreter helpers (cache emulation off), `SetJitExecutor`, executor dispatch in `Advance<false, false>`, flush on `Reset`/`LoadState`. |
```

- [ ] **Step 8: Commit**

```bash
git add src/core/include/ymir/hw/sh2/sh2_jit_iface.hpp src/core/include/ymir/hw/sh2/sh2.hpp src/core/src/ymir/hw/sh2/sh2.cpp src/core/BRIMIR_FORK.md tests/unit/sh2_test_rig.hpp tests/unit/sh2_test_rig.cpp tests/unit/test_sh2_jit_iface.cpp tests/CMakeLists.txt
git commit -m "feat(sh2): add JIT executor hook and context to the forked SH-2"
```

---

### Task 2: `brimir-jit` library and IR

**Files:**
- Create: `src/jit/CMakeLists.txt`
- Create: `src/jit/include/brimir/jit/ir.hpp`
- Create: `src/jit/src/ir.cpp`
- Modify: `CMakeLists.txt` (add subdirectory, link into `brimir_libretro`, IPO lists)
- Modify: `src/bridge/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Test: `tests/unit/test_jit_ir.cpp`

**Interfaces:**
- Produces (`namespace brimir::jit`, header `brimir/jit/ir.hpp`):
  - `using ValueId = uint16_t; constexpr ValueId kNoValue = 0xFFFF; constexpr uint16_t kMaxValues = 4096;`
  - `enum class Op : uint8_t { Const, GetReg, SetReg, GetPR, GetT, SetT, Add, Sub, CmpEq, SExt8, SExt16, Load, Store, AddCycles, AddAccessCycles, WbStall, SetWb, SyncCycles, Refill, SetupDelaySlot, EndDelaySlot, ExitIfBusWait, ExitIf, Exit, ExitDynamic };`
  - `struct Inst { Op op; uint8_t size; bool flag; uint8_t retired; ValueId dst, a, b; uint32_t imm, imm2; };`
  - `struct Block { uint32_t startPC; std::vector<uint16_t> guestOpcodes; uint32_t guestInstrCount; uint16_t numValues; std::vector<Inst> code; };`
  - `class Builder` (methods listed in the header below)
  - `const char* OpName(Op)`, `std::string VerifyBlock(const Block&)` (empty string = valid), `std::string PrintBlock(const Block&)`
- CMake target: `brimir-jit` (static library, `PUBLIC` include dir `src/jit/include`, links `brimir::brimir-core`).

IR semantics (the contract every backend implements; `v[x]` is value `x`, `cycles` is the block's cycle counter, `ctx` the `SH2JitContext`):

| Op | Operands | Semantics |
|---|---|---|
| `Const` | dst, imm | `v[dst] = imm` |
| `GetReg` / `SetReg` | dst / a, imm = reg | `v[dst] = R[imm]` / `R[imm] = v[a]` |
| `GetPR` | dst | `v[dst] = PR` |
| `GetT` / `SetT` | dst / a | `v[dst] = SR & 1` / `SR = (SR & ~1) \| (v[a] != 0)` |
| `Add` `Sub` `CmpEq` | dst, a, b | `v[a] + v[b]`, `v[a] - v[b]`, `v[a] == v[b] ? 1 : 0` |
| `SExt8` `SExt16` | dst, a | sign-extend low 8 / 16 bits |
| `Load` | dst, a, size, flag = instrFetch | `v[dst] = ctx.read(v[a], size, flag)` |
| `Store` | a, b, size | `ctx.write(v[a], size, v[b])` |
| `AddCycles` | imm | `cycles += imm` |
| `AddAccessCycles` | a, size, flag = write | `cycles += ctx.accessCycles(v[a], size, flag)` |
| `WbStall` | imm = mask | `cycles += 1` if `wbReg <= 16` and bit `wbReg` of `imm` is set (bit 16 = PR) |
| `SetWb` | imm | `wbReg = imm` |
| `SyncCycles` | — | `*ctx.cyclesExecuted = entryCycles + cycles` (`entryCycles` = `*ctx.cyclesExecuted` at block entry); emitted at the start of every instruction that accesses memory, so on-chip timers see the same count as with the interpreter |
| `Refill` | imm = address | `ctx.refillPipeline(imm)` |
| `SetupDelaySlot` | a | `ctx.setupDelaySlot(v[a])` |
| `EndDelaySlot` | — | `ctx.endDelaySlot()` |
| `ExitIfBusWait` | a, size, flag = write, imm = pc, retired | if `ctx.busWait(v[a], size, flag)`: `PC = imm`, exit (bus wait) |
| `ExitIf` | a, imm = pc, imm2 = taken cycles, flag = refill, retired | if `v[a] != 0`: `cycles += imm2`; if flag `ctx.refillPipeline(imm)`; `PC = imm`; exit |
| `Exit` | imm = pc, retired | `PC = imm`; exit |
| `ExitDynamic` | retired | exit (PC already set by `EndDelaySlot`) |

`retired` on exit ops is the number of guest instructions fully executed when that exit is taken (a bus-wait exit does not count the waiting instruction).

- [ ] **Step 1: Write the failing IR tests**

Create `tests/unit/test_jit_ir.cpp`:

```cpp
// Brimir - SH-2 JIT IR tests
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"

#include <brimir/jit/ir.hpp>

using namespace brimir::jit;

TEST_CASE("IR builder assigns sequential values and records operands", "[jit][ir]") {
    Block block;
    block.guestInstrCount = 1;
    Builder b(block);
    const ValueId r1 = b.GetReg(1);
    const ValueId k = b.Const(5);
    const ValueId sum = b.Add(r1, k);
    b.SetReg(1, sum);
    b.AddCycles(1);
    b.Exit(0x06000002, 1);

    REQUIRE(r1 == 0);
    REQUIRE(k == 1);
    REQUIRE(sum == 2);
    REQUIRE(block.numValues == 3);
    REQUIRE(block.code.size() == 6);
    REQUIRE(block.code[2].op == Op::Add);
    REQUIRE(block.code[2].a == r1);
    REQUIRE(block.code[2].b == k);
    REQUIRE(block.code[3].imm == 1);
    REQUIRE(block.code[5].op == Op::Exit);
    REQUIRE(block.code[5].imm == 0x06000002u);
    REQUIRE(block.code[5].retired == 1);
    REQUIRE(VerifyBlock(block).empty());
}

TEST_CASE("IR verifier accepts an empty fallback block", "[jit][ir]") {
    Block block;
    REQUIRE(VerifyBlock(block).empty());
    block.code.push_back(Inst{});
    REQUIRE_FALSE(VerifyBlock(block).empty());
}

TEST_CASE("IR verifier rejects malformed blocks", "[jit][ir]") {
    SECTION("missing exit") {
        Block block;
        block.guestInstrCount = 1;
        Builder b(block);
        b.AddCycles(1);
        REQUIRE(VerifyBlock(block).find("does not end with an exit") != std::string::npos);
    }
    SECTION("exit before the end") {
        Block block;
        block.guestInstrCount = 1;
        Builder b(block);
        b.Exit(0, 0);
        b.AddCycles(1);
        b.Exit(0, 1);
        REQUIRE(VerifyBlock(block).find("exit before end") != std::string::npos);
    }
    SECTION("use of an undefined value") {
        Block block;
        block.guestInstrCount = 1;
        Builder b(block);
        b.SetReg(0, 7);
        b.Exit(0, 1);
        block.numValues = 8;
        REQUIRE(VerifyBlock(block).find("undefined value") != std::string::npos);
    }
    SECTION("bad access size") {
        Block block;
        block.guestInstrCount = 1;
        Builder b(block);
        const ValueId addr = b.Const(0x06000000);
        b.Load(addr, 3, false);
        b.Exit(0, 1);
        REQUIRE(VerifyBlock(block).find("invalid access size") != std::string::npos);
    }
    SECTION("register index out of range") {
        Block block;
        block.guestInstrCount = 1;
        Builder b(block);
        b.GetReg(16);
        b.Exit(0, 1);
        REQUIRE(VerifyBlock(block).find("register index") != std::string::npos);
    }
}

TEST_CASE("IR printer lists every instruction", "[jit][ir]") {
    Block block;
    block.startPC = 0x06001000;
    block.guestInstrCount = 1;
    Builder b(block);
    const ValueId addr = b.GetReg(4);
    const ValueId value = b.Load(addr, 4, false);
    b.SetReg(2, value);
    b.Exit(0x06001002, 1);

    const std::string text = PrintBlock(block);
    REQUIRE(text.find("block @06001000") != std::string::npos);
    REQUIRE(text.find("v1 = Load.32 v0") != std::string::npos);
    REQUIRE(text.find("SetReg v1") != std::string::npos);
    REQUIRE(text.find("Exit") != std::string::npos);
    REQUIRE(std::string(OpName(Op::ExitIfBusWait)) == "ExitIfBusWait");
    REQUIRE(std::string(OpName(Op::SyncCycles)) == "SyncCycles");
}
```

Add to `tests/CMakeLists.txt` after `unit/test_sh2_jit_iface.cpp`:

```cmake
    unit/test_jit_ir.cpp
```

and change the `target_link_libraries(brimir_tests PRIVATE ...)` block to:

```cmake
target_link_libraries(brimir_tests PRIVATE
    brimir_bridge
    brimir-jit
    brimir-core
)
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake -S . -B build && cmake --build build --target brimir_tests`
Expected: CMake error or compile error — target `brimir-jit` / header `brimir/jit/ir.hpp` does not exist.

- [ ] **Step 3: Create the library and the IR**

Create `src/jit/CMakeLists.txt`:

```cmake
# Brimir SH-2 JIT (see design/sh2-jit.md)
add_library(brimir-jit STATIC
    src/ir.cpp
)

target_include_directories(brimir-jit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(brimir-jit PUBLIC cxx_std_20)
target_link_libraries(brimir-jit PUBLIC brimir::brimir-core)
```

Create `src/jit/include/brimir/jit/ir.hpp`:

```cpp
#pragma once

// Brimir SH-2 JIT intermediate representation. See design/sh2-jit.md section 5
// and the semantics table in design/plans/2026-09-30-sh2-jit-m1b-foundation.md.

#include <cstdint>
#include <string>
#include <vector>

namespace brimir::jit {

using ValueId = uint16_t;
constexpr ValueId kNoValue = 0xFFFF;
constexpr uint16_t kMaxValues = 4096;

enum class Op : uint8_t {
    Const,
    GetReg,
    SetReg,
    GetPR,
    GetT,
    SetT,
    Add,
    Sub,
    CmpEq,
    SExt8,
    SExt16,
    Load,
    Store,
    AddCycles,
    AddAccessCycles,
    WbStall,
    SetWb,
    SyncCycles,
    Refill,
    SetupDelaySlot,
    EndDelaySlot,
    ExitIfBusWait,
    ExitIf,
    Exit,
    ExitDynamic,
};

struct Inst {
    Op op = Op::Exit;
    uint8_t size = 0;    // access size in bytes (1, 2, 4)
    bool flag = false;   // Load: instrFetch; AddAccessCycles/ExitIfBusWait: write; ExitIf: refill
    uint8_t retired = 0; // exit ops: guest instructions completed when the exit is taken
    ValueId dst = kNoValue;
    ValueId a = kNoValue;
    ValueId b = kNoValue;
    uint32_t imm = 0;
    uint32_t imm2 = 0; // ExitIf: cycles added when taken
};

struct Block {
    uint32_t startPC = 0;
    std::vector<uint16_t> guestOpcodes; // guest code at startPC, startPC+2, ... (check-on-entry)
    uint32_t guestInstrCount = 0;       // 0: the first instruction runs on the interpreter
    uint16_t numValues = 0;
    std::vector<Inst> code;
};

class Builder {
public:
    explicit Builder(Block &block)
        : m_block(block) {}

    ValueId Const(uint32_t value);
    ValueId GetReg(uint32_t reg);
    void SetReg(uint32_t reg, ValueId value);
    ValueId GetPR();
    ValueId GetT();
    void SetT(ValueId value);
    ValueId Add(ValueId a, ValueId b);
    ValueId Sub(ValueId a, ValueId b);
    ValueId CmpEq(ValueId a, ValueId b);
    ValueId SExt8(ValueId a);
    ValueId SExt16(ValueId a);
    ValueId Load(ValueId address, uint8_t size, bool instrFetch);
    void Store(ValueId address, uint8_t size, ValueId value);
    void AddCycles(uint32_t cycles);
    void AddAccessCycles(ValueId address, uint8_t size, bool write);
    void WbStall(uint32_t mask);
    void SetWb(uint8_t reg);
    void SyncCycles();
    void Refill(uint32_t address);
    void SetupDelaySlot(ValueId target);
    void EndDelaySlot();
    void ExitIfBusWait(ValueId address, uint8_t size, bool write, uint32_t pc, uint8_t retired);
    void ExitIf(ValueId cond, uint32_t pc, uint32_t takenCycles, bool refill, uint8_t retired);
    void Exit(uint32_t pc, uint8_t retired);
    void ExitDynamic(uint8_t retired);

private:
    ValueId NewValue();
    Inst &Emit(Op op);

    Block &m_block;
};

const char *OpName(Op op);

// Returns an empty string if the block is well formed, otherwise a description of the first problem.
std::string VerifyBlock(const Block &block);

// Human-readable listing, used in test failure messages.
std::string PrintBlock(const Block &block);

} // namespace brimir::jit
```

Create `src/jit/src/ir.cpp`:

```cpp
#include <brimir/jit/ir.hpp>

#include <cstdio>
#include <iterator>

namespace brimir::jit {

namespace {

struct OpInfo {
    const char *name;
    bool hasDst;
    uint8_t numSrcs;
    bool usesSize;
    bool isExit; // must be, and may only be, the last instruction
};

constexpr OpInfo kOpInfo[] = {
    {"Const", true, 0, false, false},          {"GetReg", true, 0, false, false},
    {"SetReg", false, 1, false, false},        {"GetPR", true, 0, false, false},
    {"GetT", true, 0, false, false},           {"SetT", false, 1, false, false},
    {"Add", true, 2, false, false},            {"Sub", true, 2, false, false},
    {"CmpEq", true, 2, false, false},          {"SExt8", true, 1, false, false},
    {"SExt16", true, 1, false, false},         {"Load", true, 1, true, false},
    {"Store", false, 2, true, false},          {"AddCycles", false, 0, false, false},
    {"AddAccessCycles", false, 1, true, false}, {"WbStall", false, 0, false, false},
    {"SetWb", false, 0, false, false},         {"SyncCycles", false, 0, false, false},
    {"Refill", false, 0, false, false},
    {"SetupDelaySlot", false, 1, false, false}, {"EndDelaySlot", false, 0, false, false},
    {"ExitIfBusWait", false, 1, true, false},  {"ExitIf", false, 1, false, false},
    {"Exit", false, 0, false, true},           {"ExitDynamic", false, 0, false, true},
};
static_assert(std::size(kOpInfo) == static_cast<size_t>(Op::ExitDynamic) + 1, "kOpInfo must cover every Op");

const OpInfo &Info(Op op) {
    return kOpInfo[static_cast<size_t>(op)];
}

} // namespace

ValueId Builder::NewValue() {
    return m_block.numValues++;
}

Inst &Builder::Emit(Op op) {
    Inst &inst = m_block.code.emplace_back();
    inst.op = op;
    return inst;
}

ValueId Builder::Const(uint32_t value) {
    Inst &inst = Emit(Op::Const);
    inst.imm = value;
    return inst.dst = NewValue();
}

ValueId Builder::GetReg(uint32_t reg) {
    Inst &inst = Emit(Op::GetReg);
    inst.imm = reg;
    return inst.dst = NewValue();
}

void Builder::SetReg(uint32_t reg, ValueId value) {
    Inst &inst = Emit(Op::SetReg);
    inst.imm = reg;
    inst.a = value;
}

ValueId Builder::GetPR() {
    return Emit(Op::GetPR).dst = NewValue();
}

ValueId Builder::GetT() {
    return Emit(Op::GetT).dst = NewValue();
}

void Builder::SetT(ValueId value) {
    Emit(Op::SetT).a = value;
}

ValueId Builder::Add(ValueId a, ValueId b) {
    Inst &inst = Emit(Op::Add);
    inst.a = a;
    inst.b = b;
    return inst.dst = NewValue();
}

ValueId Builder::Sub(ValueId a, ValueId b) {
    Inst &inst = Emit(Op::Sub);
    inst.a = a;
    inst.b = b;
    return inst.dst = NewValue();
}

ValueId Builder::CmpEq(ValueId a, ValueId b) {
    Inst &inst = Emit(Op::CmpEq);
    inst.a = a;
    inst.b = b;
    return inst.dst = NewValue();
}

ValueId Builder::SExt8(ValueId a) {
    Inst &inst = Emit(Op::SExt8);
    inst.a = a;
    return inst.dst = NewValue();
}

ValueId Builder::SExt16(ValueId a) {
    Inst &inst = Emit(Op::SExt16);
    inst.a = a;
    return inst.dst = NewValue();
}

ValueId Builder::Load(ValueId address, uint8_t size, bool instrFetch) {
    Inst &inst = Emit(Op::Load);
    inst.a = address;
    inst.size = size;
    inst.flag = instrFetch;
    return inst.dst = NewValue();
}

void Builder::Store(ValueId address, uint8_t size, ValueId value) {
    Inst &inst = Emit(Op::Store);
    inst.a = address;
    inst.b = value;
    inst.size = size;
}

void Builder::AddCycles(uint32_t cycles) {
    Emit(Op::AddCycles).imm = cycles;
}

void Builder::AddAccessCycles(ValueId address, uint8_t size, bool write) {
    Inst &inst = Emit(Op::AddAccessCycles);
    inst.a = address;
    inst.size = size;
    inst.flag = write;
}

void Builder::WbStall(uint32_t mask) {
    Emit(Op::WbStall).imm = mask;
}

void Builder::SetWb(uint8_t reg) {
    Emit(Op::SetWb).imm = reg;
}

void Builder::SyncCycles() {
    Emit(Op::SyncCycles);
}

void Builder::Refill(uint32_t address) {
    Emit(Op::Refill).imm = address;
}

void Builder::SetupDelaySlot(ValueId target) {
    Emit(Op::SetupDelaySlot).a = target;
}

void Builder::EndDelaySlot() {
    Emit(Op::EndDelaySlot);
}

void Builder::ExitIfBusWait(ValueId address, uint8_t size, bool write, uint32_t pc, uint8_t retired) {
    Inst &inst = Emit(Op::ExitIfBusWait);
    inst.a = address;
    inst.size = size;
    inst.flag = write;
    inst.imm = pc;
    inst.retired = retired;
}

void Builder::ExitIf(ValueId cond, uint32_t pc, uint32_t takenCycles, bool refill, uint8_t retired) {
    Inst &inst = Emit(Op::ExitIf);
    inst.a = cond;
    inst.imm = pc;
    inst.imm2 = takenCycles;
    inst.flag = refill;
    inst.retired = retired;
}

void Builder::Exit(uint32_t pc, uint8_t retired) {
    Inst &inst = Emit(Op::Exit);
    inst.imm = pc;
    inst.retired = retired;
}

void Builder::ExitDynamic(uint8_t retired) {
    Emit(Op::ExitDynamic).retired = retired;
}

const char *OpName(Op op) {
    return static_cast<size_t>(op) < std::size(kOpInfo) ? Info(op).name : "<invalid>";
}

std::string VerifyBlock(const Block &block) {
    if (block.guestInstrCount == 0) {
        return block.code.empty() ? std::string{} : std::string{"empty block must have no code"};
    }
    if (block.code.empty()) {
        return "block has no code";
    }
    if (block.numValues > kMaxValues) {
        return "too many values";
    }

    std::vector<bool> defined(block.numValues, false);
    for (size_t i = 0; i < block.code.size(); ++i) {
        const Inst &inst = block.code[i];
        const auto error = [&](const char *message) {
            return "inst " + std::to_string(i) + " (" + OpName(inst.op) + "): " + message;
        };
        if (static_cast<size_t>(inst.op) >= std::size(kOpInfo)) {
            return error("invalid op");
        }
        const OpInfo &info = Info(inst.op);

        const ValueId srcs[2] = {inst.a, inst.b};
        for (uint8_t s = 0; s < info.numSrcs; ++s) {
            if (srcs[s] >= block.numValues || !defined[srcs[s]]) {
                return error("uses an undefined value");
            }
        }
        if (info.hasDst) {
            if (inst.dst >= block.numValues) {
                return error("destination out of range");
            }
            if (defined[inst.dst]) {
                return error("value defined twice");
            }
            defined[inst.dst] = true;
        }
        if (info.usesSize && inst.size != 1 && inst.size != 2 && inst.size != 4) {
            return error("invalid access size");
        }
        if ((inst.op == Op::GetReg || inst.op == Op::SetReg) && inst.imm > 15) {
            return error("register index out of range");
        }
        const bool last = i + 1 == block.code.size();
        if (info.isExit && !last) {
            return error("exit before end of block");
        }
        if (!info.isExit && last) {
            return error("block does not end with an exit");
        }
    }
    return {};
}

std::string PrintBlock(const Block &block) {
    std::string out;
    char line[192];
    std::snprintf(line, sizeof(line), "block @%08X: %u guest instructions, %u values\n", block.startPC,
                  block.guestInstrCount, static_cast<unsigned>(block.numValues));
    out += line;
    for (const Inst &inst : block.code) {
        if (static_cast<size_t>(inst.op) >= std::size(kOpInfo)) {
            out += "  <invalid op>\n";
            continue;
        }
        const OpInfo &info = Info(inst.op);
        std::string text = "  ";
        if (info.hasDst) {
            text += "v" + std::to_string(inst.dst) + " = ";
        }
        text += info.name;
        if (info.usesSize) {
            text += "." + std::to_string(inst.size * 8);
        }
        if (info.numSrcs >= 1) {
            text += " v" + std::to_string(inst.a);
        }
        if (info.numSrcs >= 2) {
            text += ", v" + std::to_string(inst.b);
        }
        std::snprintf(line, sizeof(line), "  imm=0x%X imm2=%u flag=%d retired=%u\n", inst.imm, inst.imm2,
                      inst.flag ? 1 : 0, static_cast<unsigned>(inst.retired));
        out += text + line;
    }
    return out;
}

} // namespace brimir::jit
```

- [ ] **Step 4: Wire the library into the build**

In the root `CMakeLists.txt`:
- directly before `# Bridge layer - adapts core to libretro`, add:

```cmake
# SH-2 JIT library (design/sh2-jit.md)
add_subdirectory(src/jit)

```

- change `target_link_libraries(brimir_libretro PRIVATE brimir::brimir-core)` to `target_link_libraries(brimir_libretro PRIVATE brimir-jit brimir::brimir-core)`
- in the IPO section change `list(APPEND brimir_ipo_targets brimir-core brimir_bench)` to `list(APPEND brimir_ipo_targets brimir-core brimir-jit brimir_bench)` and add `brimir-jit` to the `foreach(target IN ITEMS ...)` list after `brimir-core`.

In `src/bridge/CMakeLists.txt`, replace `target_link_libraries(brimir_bridge PRIVATE brimir::brimir-core)` with:

```cmake
# Link with Brimir core and the SH-2 JIT (PUBLIC so consumers of the object
# library, such as tests and tools, link the JIT too)
target_link_libraries(brimir_bridge PUBLIC brimir-jit brimir::brimir-core)
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build --target brimir_tests brimir_libretro brimir_bench`
Expected: builds with no errors.
Run: `build\bin\brimir_tests.exe "[ir]"` — Expected: `All tests passed` (4 test cases).
Run: `ctest --test-dir build --output-on-failure` — Expected: `100% tests passed`.

- [ ] **Step 6: Commit**

```bash
git add src/jit/CMakeLists.txt src/jit/include/brimir/jit/ir.hpp src/jit/src/ir.cpp CMakeLists.txt src/bridge/CMakeLists.txt tests/CMakeLists.txt tests/unit/test_jit_ir.cpp
git commit -m "feat(jit): add brimir-jit library with SH-2 block IR, verifier and printer"
```

---

### Task 3: IR interpreter backend

**Files:**
- Create: `src/jit/include/brimir/jit/interp_backend.hpp`
- Create: `src/jit/src/interp_backend.cpp`
- Modify: `src/jit/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Test: `tests/unit/test_jit_backend.cpp`

**Interfaces:**
- Consumes: `Block`, `Inst`, `Op`, `Builder` (Task 2); `ymir::sh2::SH2JitContext` (Task 1).
- Produces (`namespace brimir::jit`, header `brimir/jit/interp_backend.hpp`):
  - `struct ExitInfo { uint64_t cycles = 0; uint8_t retired = 0; bool busWait = false; };`
  - `ExitInfo RunBlock(const Block& block, ymir::sh2::SH2JitContext& ctx);` — executes a verified, non-empty block per the IR semantics table.

- [ ] **Step 1: Write the failing backend tests**

Create `tests/unit/test_jit_backend.cpp`:

```cpp
// Brimir - SH-2 JIT IR interpreter backend tests
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/jit/interp_backend.hpp>
#include <brimir/jit/ir.hpp>

#include <memory>

using namespace brimir::jit;
using sh2test::Rig;

namespace {

constexpr uint32_t kCode = 0x06001000;

struct Fixture {
    std::unique_ptr<Rig> rig = std::make_unique<Rig>();
    Block block;
    Builder b{block};

    Fixture() {
        rig->Load(rig->BaseState(kCode));
        block.startPC = kCode;
        block.guestInstrCount = 1;
    }

    ExitInfo Run() {
        INFO(PrintBlock(block));
        REQUIRE(VerifyBlock(block).empty());
        return RunBlock(block, rig->sh2->GetJitContext());
    }
};

} // namespace

TEST_CASE("Backend: registers, ALU and T bit", "[jit][backend]") {
    Fixture f;
    auto s = f.rig->BaseState(kCode);
    s.R[1] = 40;
    s.PR = 0x06002000;
    f.rig->Load(s);

    const ValueId r1 = f.b.GetReg(1);
    const ValueId two = f.b.Const(2);
    f.b.SetReg(2, f.b.Add(r1, two));
    f.b.SetReg(3, f.b.Sub(r1, two));
    f.b.SetReg(4, f.b.SExt8(f.b.Const(0x80)));
    f.b.SetReg(5, f.b.SExt16(f.b.Const(0x8001)));
    f.b.SetReg(6, f.b.GetPR());
    f.b.SetT(f.b.CmpEq(r1, f.b.Const(40)));
    f.b.SetReg(7, f.b.GetT());
    f.b.Exit(kCode + 2, 1);

    const ExitInfo info = f.Run();
    const auto st = f.rig->State();
    REQUIRE(st.R[2] == 42u);
    REQUIRE(st.R[3] == 38u);
    REQUIRE(st.R[4] == 0xFFFFFF80u);
    REQUIRE(st.R[5] == 0xFFFF8001u);
    REQUIRE(st.R[6] == 0x06002000u);
    REQUIRE((st.SR & 1u) == 1u);
    REQUIRE(st.R[7] == 1u);
    REQUIRE(st.PC == kCode + 2);
    REQUIRE(info.cycles == 0);
    REQUIRE(info.retired == 1);
    REQUIRE_FALSE(info.busWait);
}

TEST_CASE("Backend: memory, access cycles and write-back stalls", "[jit][backend]") {
    Fixture f;
    auto s = f.rig->BaseState(kCode);
    s.wbReg = 3;
    f.rig->Load(s);
    f.rig->Write32(0x06040000, 0x89ABCDEF);

    const ValueId addr = f.b.Const(0x26040000);
    f.b.AddAccessCycles(addr, 4, false); // +6
    f.b.SetReg(1, f.b.Load(addr, 4, false));
    f.b.Store(f.b.Const(0x26040010), 2, f.b.Const(0x1234));
    f.b.AddAccessCycles(addr, 1, true);  // +3
    f.b.WbStall((1u << 3) | (1u << 5)); // wbReg = 3 -> +1
    f.b.WbStall(1u << 16);              // PR not in WB -> +0
    f.b.SetWb(7);
    f.b.AddCycles(10);
    f.b.Exit(kCode + 2, 1);

    const ExitInfo info = f.Run();
    REQUIRE(f.rig->State().R[1] == 0x89ABCDEFu);
    REQUIRE(f.rig->Read16(0x06040010) == 0x1234);
    REQUIRE(f.rig->State().wbReg == 7);
    REQUIRE(info.cycles == 6 + 3 + 1 + 10);
}

TEST_CASE("Backend: SyncCycles publishes the running cycle count", "[jit][backend]") {
    Fixture f;
    auto &ctx = f.rig->sh2->GetJitContext();
    *ctx.cyclesExecuted = 100;
    f.b.AddCycles(7);
    f.b.SyncCycles();
    f.b.AddCycles(5);
    f.b.Exit(kCode + 2, 1);

    const ExitInfo info = f.Run();
    REQUIRE(info.cycles == 12);
    REQUIRE(*ctx.cyclesExecuted == 107); // the executor, not the backend, publishes the final count
}

TEST_CASE("Backend: WbStall ignores the 'no register' marker", "[jit][backend]") {
    Fixture f; // BaseState sets wbReg = 0xFF
    f.b.WbStall(0xFFFFFFFFu);
    f.b.Exit(kCode + 2, 1);
    REQUIRE(f.Run().cycles == 0);
}

TEST_CASE("Backend: bus-wait exit leaves state for a retry", "[jit][backend]") {
    Fixture f;
    f.rig->mmio.busWaitEvery = 1;
    const ValueId addr = f.b.Const(0x22000000);
    f.b.AddAccessCycles(addr, 4, false); // +12
    f.b.ExitIfBusWait(addr, 4, false, kCode, 0);
    f.b.SetReg(1, f.b.Load(addr, 4, false));
    f.b.Exit(kCode + 2, 1);

    const ExitInfo info = f.Run();
    REQUIRE(info.busWait);
    REQUIRE(info.retired == 0);
    REQUIRE(info.cycles == 12);
    REQUIRE(f.rig->State().PC == kCode);
    REQUIRE(f.rig->State().R[1] == 0u);
}

TEST_CASE("Backend: conditional exit with refill", "[jit][backend]") {
    Fixture f;
    f.rig->WriteCode(kCode + 0x40, {0x1111, 0x2222});
    const ValueId one = f.b.Const(1);
    f.b.ExitIf(one, kCode + 0x40, 3, true, 1);
    f.b.AddCycles(1);
    f.b.Exit(kCode + 2, 1);

    const ExitInfo info = f.Run();
    REQUIRE(info.cycles == 3);
    REQUIRE(info.retired == 1);
    REQUIRE(f.rig->State().PC == kCode + 0x40);
    REQUIRE(f.rig->State().fetchedOpcodes == 0x11112222u);
}

TEST_CASE("Backend: conditional exit not taken falls through", "[jit][backend]") {
    Fixture f;
    const ValueId zero = f.b.Const(0);
    f.b.ExitIf(zero, kCode + 0x40, 3, true, 1);
    f.b.AddCycles(1);
    f.b.Exit(kCode + 2, 1);

    const ExitInfo info = f.Run();
    REQUIRE(info.cycles == 1);
    REQUIRE(f.rig->State().PC == kCode + 2);
}

TEST_CASE("Backend: delay slot setup and dynamic exit", "[jit][backend]") {
    Fixture f;
    f.b.SetupDelaySlot(f.b.Const(kCode + 0x80));
    f.b.Refill(kCode);
    f.b.EndDelaySlot();
    f.b.ExitDynamic(2);

    const ExitInfo info = f.Run();
    REQUIRE(info.retired == 2);
    const auto st = f.rig->State();
    REQUIRE(st.PC == kCode + 0x80);
    REQUIRE_FALSE(st.delaySlot);
    REQUIRE(st.delaySlotTarget == kCode + 0x80);
}
```

Add to `tests/CMakeLists.txt` after `unit/test_jit_ir.cpp`:

```cmake
    unit/test_jit_backend.cpp
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target brimir_tests`
Expected: compile error — `brimir/jit/interp_backend.hpp` not found.

- [ ] **Step 3: Implement the backend**

Create `src/jit/include/brimir/jit/interp_backend.hpp`:

```cpp
#pragma once

// IR interpreter backend: executes IR blocks directly (milestone 1 backend).

#include <brimir/jit/ir.hpp>

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <cstdint>

namespace brimir::jit {

struct ExitInfo {
    uint64_t cycles = 0;
    uint8_t retired = 0;  // guest instructions fully executed
    bool busWait = false; // exited on a bus wait; the instruction at PC retries
};

// Executes a verified, non-empty block against the live SH-2 state.
ExitInfo RunBlock(const Block &block, ymir::sh2::SH2JitContext &ctx);

} // namespace brimir::jit
```

Create `src/jit/src/interp_backend.cpp`:

```cpp
#include <brimir/jit/interp_backend.hpp>

#include <vector>

namespace brimir::jit {

ExitInfo RunBlock(const Block &block, ymir::sh2::SH2JitContext &ctx) {
    thread_local std::vector<uint32_t> values;
    if (values.size() < block.numValues) {
        values.resize(block.numValues);
    }
    uint32_t *v = values.data();

    const uint64_t entryCycles = *ctx.cyclesExecuted;
    ExitInfo info;
    for (const Inst &in : block.code) {
        switch (in.op) {
        case Op::Const: v[in.dst] = in.imm; break;
        case Op::GetReg: v[in.dst] = ctx.R[in.imm]; break;
        case Op::SetReg: ctx.R[in.imm] = v[in.a]; break;
        case Op::GetPR: v[in.dst] = *ctx.PR; break;
        case Op::GetT: v[in.dst] = *ctx.SR & 1u; break;
        case Op::SetT: *ctx.SR = (*ctx.SR & ~1u) | (v[in.a] != 0 ? 1u : 0u); break;
        case Op::Add: v[in.dst] = v[in.a] + v[in.b]; break;
        case Op::Sub: v[in.dst] = v[in.a] - v[in.b]; break;
        case Op::CmpEq: v[in.dst] = v[in.a] == v[in.b] ? 1u : 0u; break;
        case Op::SExt8:
            v[in.dst] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v[in.a] & 0xFFu)));
            break;
        case Op::SExt16:
            v[in.dst] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v[in.a] & 0xFFFFu)));
            break;
        case Op::Load: v[in.dst] = ctx.read(ctx.sh2, v[in.a], in.size, in.flag); break;
        case Op::Store: ctx.write(ctx.sh2, v[in.a], in.size, v[in.b]); break;
        case Op::AddCycles: info.cycles += in.imm; break;
        case Op::AddAccessCycles: info.cycles += ctx.accessCycles(ctx.sh2, v[in.a], in.size, in.flag); break;
        case Op::WbStall: {
            const uint8_t wb = *ctx.wbReg;
            if (wb <= 16 && ((in.imm >> wb) & 1u) != 0) {
                info.cycles += 1;
            }
            break;
        }
        case Op::SetWb: *ctx.wbReg = static_cast<uint8_t>(in.imm); break;
        case Op::SyncCycles: *ctx.cyclesExecuted = entryCycles + info.cycles; break;
        case Op::Refill: ctx.refillPipeline(ctx.sh2, in.imm); break;
        case Op::SetupDelaySlot: ctx.setupDelaySlot(ctx.sh2, v[in.a]); break;
        case Op::EndDelaySlot: ctx.endDelaySlot(ctx.sh2); break;
        case Op::ExitIfBusWait:
            if (ctx.busWait(ctx.sh2, v[in.a], in.size, in.flag)) {
                *ctx.PC = in.imm;
                info.retired = in.retired;
                info.busWait = true;
                return info;
            }
            break;
        case Op::ExitIf:
            if (v[in.a] != 0) {
                info.cycles += in.imm2;
                if (in.flag) {
                    ctx.refillPipeline(ctx.sh2, in.imm);
                }
                *ctx.PC = in.imm;
                info.retired = in.retired;
                return info;
            }
            break;
        case Op::Exit:
            *ctx.PC = in.imm;
            info.retired = in.retired;
            return info;
        case Op::ExitDynamic: info.retired = in.retired; return info;
        }
    }
    return info; // unreachable for verified blocks (they always end with an exit)
}

} // namespace brimir::jit
```

In `src/jit/CMakeLists.txt`, change the source list to:

```cmake
add_library(brimir-jit STATIC
    src/ir.cpp
    src/interp_backend.cpp
)
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --target brimir_tests` then `build\bin\brimir_tests.exe "[backend]"`
Expected: `All tests passed` (8 test cases).
Run: `ctest --test-dir build --output-on-failure` — Expected: `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/jit/include/brimir/jit/interp_backend.hpp src/jit/src/interp_backend.cpp src/jit/CMakeLists.txt tests/CMakeLists.txt tests/unit/test_jit_backend.cpp
git commit -m "feat(jit): add IR interpreter backend"
```

---

### Task 4: Front end, block cache, executor, and per-instruction differential tests

**Files:**
- Create: `src/jit/include/brimir/jit/frontend.hpp`, `src/jit/src/frontend.cpp`
- Create: `src/jit/include/brimir/jit/block_cache.hpp`, `src/jit/src/block_cache.cpp`
- Create: `src/jit/include/brimir/jit/executor.hpp`, `src/jit/src/executor.cpp`
- Modify: `src/jit/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `design/sh2-jit.md` (section 6.1, block cache key)
- Test: `tests/unit/test_jit_diff.cpp`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces (`namespace brimir::jit`):
  - `frontend.hpp`: `constexpr uint32_t kMaxBlockInstructions = 32;`, `bool IsCompilableAddress(uint32_t pc);`, `Block BuildBlock(ymir::sh2::SH2JitContext& ctx, uint32_t pc);`
  - `block_cache.hpp`: `class BlockCache { const Block& Get(ymir::sh2::SH2JitContext&, uint32_t pc); void Flush(); size_t Size() const; uint64_t Compiles() const; uint64_t Invalidations() const; };`, `constexpr size_t kMaxCachedInsts = size_t{1} << 20;`
  - `executor.hpp`: `class Executor final : public ymir::sh2::ISH2Executor { uint64 Run(...) override; void Flush() override; ExitInfo Step(ymir::sh2::SH2JitContext&); const BlockCache& Cache() const; struct Stats { uint64_t blocksRun, interpreted; }; const Stats& GetStats() const; };`

**Lowering rules** (each mirrors the interpreter handler listed in the plan's reference section; `n` = bits 8–11, `m` = bits 4–7; `advance` = `EndDelaySlot` when in a delay slot, nothing otherwise; `Refill(pc)` is emitted before every instruction whose `pc & 2 == 0`; every instruction that accesses data memory starts with `SyncCycles`, omitted from the table):

| Opcode | IR sequence |
|---|---|
| `NOP` | advance; `SetWb(none)`; `AddCycles(1)` |
| `MOV Rm,Rn` | `SetReg(n, GetReg(m))`; advance; `WbStall(m,n)`; `AddCycles(1)`; `SetWb(none)` |
| `MOV #imm,Rn` | `SetReg(n, Const(sext8))`; advance; `WbStall(n)`; `AddCycles(1)`; `SetWb(none)` |
| `MOV.B @Rm,Rn` | `a=GetReg(m)`; `AddAccessCycles(a,1,r)`; `WbStall(m)`; `SetReg(n, SExt8(Load(a,1)))`; advance; `SetWb(n)` |
| `MOV.L @Rm,Rn` | `a=GetReg(m)`; `AddAccessCycles(a,4,r)`; `ExitIfBusWait(a,4,r,pc)`; `SetReg(n, Load(a,4))`; advance; `WbStall(m)`; `SetWb(n)` |
| `MOV.B Rm,@Rn` | `a=GetReg(n)`; `AddAccessCycles(a,1,w)`; `WbStall(m,n)`; `Store(a,1,GetReg(m))`; advance; `SetWb(none)` |
| `MOV.L Rm,@Rn` | `a=GetReg(n)`; `AddAccessCycles(a,4,w)`; `ExitIfBusWait(a,4,w,pc)`; `Store(a,4,GetReg(m))`; advance; `WbStall(m,n)`; `SetWb(none)` |
| `MOV.L @(d,PC),Rn` | not allowed in a delay slot; `a=Const((pc&~3)+d*4+4)`; `AddAccessCycles(a,4,r)`; `SetReg(n, Load(a,4,instrFetch))`; `SetWb(n)` |
| `ADD Rm,Rn` | `SetReg(n, Add(GetReg(n),GetReg(m)))`; advance; `WbStall(m,n)`; `AddCycles(1)`; `SetWb(none)` |
| `ADD #imm,Rn` | `SetReg(n, Add(GetReg(n),Const(sext8)))`; advance; `WbStall(n)`; `AddCycles(1)`; `SetWb(none)` |
| `CMP/EQ Rm,Rn` | `SetT(CmpEq(GetReg(n),GetReg(m)))`; advance; `WbStall(m,n)`; `AddCycles(1)`; `SetWb(none)` |
| `DT Rn` | `d=Sub(GetReg(n),1)`; `SetReg(n,d)`; `SetT(CmpEq(d,0))`; advance; `WbStall(n)`; `AddCycles(1)`; `SetWb(none)` |
| `BT` / `BF` | `SetWb(none)`; `c=GetT` (BF: `CmpEq(GetT,0)`); `ExitIf(c, pc+d*2+4, 3, refill)`; `AddCycles(1)`; `Exit(pc+2)` — ends the block |
| `BRA` | `SetupDelaySlot(Const(pc+d12*2+4))`; `SetWb(none)`; `AddCycles(2)`; then the slot; `ExitDynamic` |
| `BT/S` / `BF/S` | `SetWb(none)`; not-taken condition `c`; `ExitIf(c, pc+2, 1, no refill)`; `SetupDelaySlot(Const(pc+d*2+4))`; `AddCycles(2)`; slot; `ExitDynamic` |
| `JMP @Rm` | `SetupDelaySlot(GetReg(m))` (m = bits 8–11); `WbStall(m)`; `AddCycles(2)`; `SetWb(none)`; slot; `ExitDynamic` |
| `RTS` | `SetupDelaySlot(GetPR())`; `WbStall(PR)`; `AddCycles(2)`; `SetWb(none)`; slot; `ExitDynamic` |

A delayed branch whose slot instruction is not supported in a slot ends the block **before** the branch.

- [ ] **Step 1: Write the failing differential tests**

Create `tests/unit/test_jit_diff.cpp`:

```cpp
// Brimir - SH-2 JIT vs interpreter differential tests (per instruction)
// Licensed under GPL-3.0
//
// Every test runs the same code on two identical isolated SH-2s: one through
// the JIT executor, one through the interpreter, and requires identical state,
// memory, bus-wait query sequence and cycle totals.

#include "catch_amalgamated.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/jit/executor.hpp>

#include <array>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

using sh2test::kSleep;
using sh2test::Rig;

namespace {

constexpr uint32_t kCode = 0x06001000;
constexpr uint32_t kTarget = kCode + 0x100; // branch target area, filled with SLEEP
constexpr uint32_t kMmio = 0x22000000;

// Encoders
uint16_t Nm(uint16_t base, uint32_t n, uint32_t m) {
    return static_cast<uint16_t>(base | (n << 8) | (m << 4));
}
uint16_t NImm(uint16_t base, uint32_t n, uint32_t imm) {
    return static_cast<uint16_t>(base | (n << 8) | (imm & 0xFF));
}
constexpr uint16_t kNop = 0x0009;
constexpr uint16_t kRts = 0x000B;
uint16_t MovR(uint32_t n, uint32_t m) { return Nm(0x6003, n, m); }
uint16_t MovI(uint32_t n, uint32_t imm) { return NImm(0xE000, n, imm); }
uint16_t MovBL(uint32_t n, uint32_t m) { return Nm(0x6000, n, m); }
uint16_t MovLL(uint32_t n, uint32_t m) { return Nm(0x6002, n, m); }
uint16_t MovBS(uint32_t n, uint32_t m) { return Nm(0x2000, n, m); }
uint16_t MovLS(uint32_t n, uint32_t m) { return Nm(0x2002, n, m); }
uint16_t MovLI(uint32_t n, uint32_t disp) { return NImm(0xD000, n, disp); }
uint16_t Add(uint32_t n, uint32_t m) { return Nm(0x300C, n, m); }
uint16_t AddI(uint32_t n, uint32_t imm) { return NImm(0x7000, n, imm); }
uint16_t CmpEq(uint32_t n, uint32_t m) { return Nm(0x3000, n, m); }
uint16_t Dt(uint32_t n) { return static_cast<uint16_t>(0x4010 | (n << 8)); }
uint16_t Jmp(uint32_t m) { return static_cast<uint16_t>(0x402B | (m << 8)); }
uint16_t Bt(uint32_t d) { return static_cast<uint16_t>(0x8900 | (d & 0xFF)); }
uint16_t Bf(uint32_t d) { return static_cast<uint16_t>(0x8B00 | (d & 0xFF)); }
uint16_t Bts(uint32_t d) { return static_cast<uint16_t>(0x8D00 | (d & 0xFF)); }
uint16_t Bfs(uint32_t d) { return static_cast<uint16_t>(0x8F00 | (d & 0xFF)); }
uint16_t Bra(uint32_t d) { return static_cast<uint16_t>(0xA000 | (d & 0xFFF)); }

std::string Hex(const std::vector<uint16_t> &words) {
    std::string out;
    char buf[8];
    for (uint16_t w : words) {
        std::snprintf(buf, sizeof(buf), "%04X ", w);
        out += buf;
    }
    return out;
}

struct Pair {
    std::unique_ptr<Rig> ref = std::make_unique<Rig>();
    std::unique_ptr<Rig> jit = std::make_unique<Rig>();
    brimir::jit::Executor exec;

    void WriteCode(uint32_t address, const std::vector<uint16_t> &words) {
        ref->WriteCode(address, words);
        jit->WriteCode(address, words);
    }
    void Write32(uint32_t address, uint32_t value) {
        ref->Write32(address, value);
        jit->Write32(address, value);
    }
    void SetBusWaitEvery(uint32_t every) {
        ref->mmio.busWaitEvery = every;
        jit->mmio.busWaitEvery = every;
    }
    void Load(const ymir::savestate::SH2SaveState &state) {
        ref->Load(state);
        jit->Load(state);
    }

    // One executor step on the JIT rig and the equivalent interpreter steps on the reference rig.
    brimir::jit::ExitInfo Step() {
        const auto info = exec.Step(jit->sh2->GetJitContext());
        uint64_t refCycles = 0;
        for (uint32_t i = 0; i < info.retired; ++i) {
            refCycles += ref->sh2->Step<false, false>();
        }
        if (info.busWait) {
            refCycles += ref->sh2->Step<false, false>();
        }
        CHECK(info.cycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*ref, *jit);
        INFO(diff);
        CHECK(diff.empty());
        return info;
    }
};

// Random register file; memory-op base registers are pointed at valid data afterwards.
std::array<uint32_t, 16> RandomRegs(std::mt19937 &rng) {
    std::array<uint32_t, 16> regs{};
    for (auto &r : regs) {
        r = rng();
    }
    return regs;
}

// A data address in cache-through RAM, cached RAM, or MMIO, aligned to `align`.
uint32_t RandomDataAddress(std::mt19937 &rng, uint32_t align) {
    const uint32_t offset = (rng() & 0xFFF0u) | (rng() & 0xFu & ~(align - 1));
    switch (rng() % 3) {
    case 0: return 0x26040000 + offset;
    case 1: return 0x06040000 + offset;
    default: return kMmio + offset;
    }
}

uint8_t RandomWb(std::mt19937 &rng) {
    const uint32_t pick = rng() % 19;
    return pick < 17 ? static_cast<uint8_t>(pick) : uint8_t{0xFF};
}

enum class Kind { Nop, MovR, MovI, MovBL, MovLL, MovBS, MovLS, MovLI, Add, AddI, CmpEq, Dt };
constexpr Kind kAllKinds[] = {Kind::Nop, Kind::MovR, Kind::MovI, Kind::MovBL, Kind::MovLL, Kind::MovBS,
                              Kind::MovLS, Kind::MovLI, Kind::Add, Kind::AddI, Kind::CmpEq, Kind::Dt};

// Builds one instance of `kind` with random operands and fixes up registers used as addresses.
uint16_t MakeInstr(Kind kind, std::mt19937 &rng, std::array<uint32_t, 16> &regs) {
    const uint32_t n = rng() % 16;
    const uint32_t m = rng() % 16;
    switch (kind) {
    case Kind::Nop: return kNop;
    case Kind::MovR: return MovR(n, m);
    case Kind::MovI: return MovI(n, rng());
    case Kind::MovBL: regs[m] = RandomDataAddress(rng, 1); return MovBL(n, m);
    case Kind::MovLL: regs[m] = RandomDataAddress(rng, 4); return MovLL(n, m);
    case Kind::MovBS: regs[n] = RandomDataAddress(rng, 1); return MovBS(n, m);
    case Kind::MovLS: regs[n] = RandomDataAddress(rng, 4); return MovLS(n, m);
    case Kind::MovLI: return MovLI(n, rng());
    case Kind::Add: return Add(n, m);
    case Kind::AddI: return AddI(n, rng());
    case Kind::CmpEq: return CmpEq(n, m);
    case Kind::Dt: return Dt(n);
    }
    return kNop;
}

void FillTargetArea(Pair &p) {
    p.WriteCode(kTarget - 0x10, std::vector<uint16_t>(0x40, static_cast<uint16_t>(kSleep)));
}

} // namespace

TEST_CASE("JIT matches the interpreter for each supported instruction", "[jit][diff]") {
    Pair p;
    std::mt19937 rng(0x5EED0001);
    for (Kind kind : kAllKinds) {
        for (int iter = 0; iter < 200; ++iter) {
            const uint32_t pc = kCode + (rng() & 1u) * 2; // exercise both fetch-buffer halves
            auto regs = RandomRegs(rng);
            const uint16_t instr = MakeInstr(kind, rng, regs);
            p.WriteCode(pc, {instr, static_cast<uint16_t>(kSleep)});

            auto state = p.ref->BaseState(pc);
            state.R = regs;
            state.SR = 0xF0 | (rng() & 1u);
            state.wbReg = RandomWb(rng);
            state.PR = rng();
            p.Load(state);

            INFO("instr " << Hex({instr}) << " at PC " << std::hex << pc);
            const auto info = p.Step();
            CHECK(info.retired == 1);
        }
    }
}

TEST_CASE("JIT matches the interpreter for instructions in delay slots", "[jit][diff]") {
    Pair p;
    FillTargetArea(p);
    std::mt19937 rng(0x5EED0002);
    const Kind slotKinds[] = {Kind::Nop, Kind::MovR, Kind::MovI, Kind::MovBL, Kind::MovLL, Kind::MovBS,
                              Kind::MovLS, Kind::Add, Kind::AddI, Kind::CmpEq, Kind::Dt};
    enum class Branch { Bra, Jmp, Rts, Bts, Bfs };
    for (Branch branch : {Branch::Bra, Branch::Jmp, Branch::Rts, Branch::Bts, Branch::Bfs}) {
        for (Kind kind : slotKinds) {
            for (int iter = 0; iter < 40; ++iter) {
                auto regs = RandomRegs(rng);
                const uint16_t slot = MakeInstr(kind, rng, regs);
                // Displacements reach kTarget from kCode: (0x100 - 4) / 2 = 126.
                uint16_t br = 0;
                uint32_t pr = rng();
                switch (branch) {
                case Branch::Bra: br = Bra(126); break;
                case Branch::Bts: br = Bts(126); break;
                case Branch::Bfs: br = Bfs(126); break;
                case Branch::Jmp: {
                    const uint32_t m = rng() % 16;
                    regs[m] = kTarget;
                    br = Jmp(m);
                    break;
                }
                case Branch::Rts: pr = kTarget; br = kRts; break;
                }
                p.WriteCode(kCode, {br, slot, static_cast<uint16_t>(kSleep)});

                auto state = p.ref->BaseState(kCode);
                state.R = regs;
                state.SR = 0xF0 | (rng() & 1u);
                state.wbReg = RandomWb(rng);
                state.PR = pr;
                p.Load(state);

                INFO("branch/slot " << Hex({br, slot}));
                p.Step(); // branch (+ slot when taken)
                p.Step(); // not-taken BT/S, BF/S: the slot instruction as a normal instruction
            }
        }
    }
}

TEST_CASE("JIT matches the interpreter for BT and BF", "[jit][diff]") {
    Pair p;
    FillTargetArea(p);
    for (uint32_t t = 0; t < 2; ++t) {
        for (uint16_t br : {Bt(126), Bf(126), Bt(0xFE), Bf(0xFE)}) {
            p.WriteCode(kCode, {br, static_cast<uint16_t>(kSleep)});
            auto state = p.ref->BaseState(kCode);
            state.SR = 0xF0 | t;
            state.wbReg = 4;
            p.Load(state);
            INFO("branch " << Hex({br}) << " T=" << t);
            const auto info = p.Step();
            CHECK(info.retired == 1);
        }
    }
}

TEST_CASE("Bus-wait retries match the interpreter", "[jit][diff]") {
    for (uint32_t every : {1u, 2u, 3u}) {
        Pair p;
        FillTargetArea(p);
        p.SetBusWaitEvery(every);
        const std::vector<std::vector<uint16_t>> programs = {
            {MovLL(1, 2), static_cast<uint16_t>(kSleep)},
            {MovLS(2, 1), static_cast<uint16_t>(kSleep)},
            {AddI(3, 1), MovLL(1, 2), AddI(3, 1), static_cast<uint16_t>(kSleep)},
            {Bra(126), MovLL(1, 2), static_cast<uint16_t>(kSleep)},
            {Bra(126), MovLS(2, 1), static_cast<uint16_t>(kSleep)},
        };
        for (const auto &program : programs) {
            p.WriteCode(kCode, program);
            auto state = p.ref->BaseState(kCode);
            state.R[1] = 0x12345678;
            state.R[2] = kMmio + 0x40;
            state.wbReg = 2;
            p.Load(state);
            INFO("program " << Hex(program) << " busWaitEvery " << every);
            for (int step = 0; step < 6; ++step) {
                p.Step();
            }
        }
    }
}

TEST_CASE("JIT recompiles a block when its guest code changes", "[jit][diff]") {
    Pair p;
    p.WriteCode(kCode, {AddI(0, 1), static_cast<uint16_t>(kSleep)});
    auto state = p.ref->BaseState(kCode);
    p.Load(state);
    p.Step();
    REQUIRE(p.exec.Cache().Compiles() == 1);

    p.WriteCode(kCode, {AddI(0, 2), static_cast<uint16_t>(kSleep)});
    p.Load(state);
    p.Step();
    REQUIRE(p.exec.Cache().Invalidations() == 1);
    REQUIRE(p.exec.Cache().Compiles() == 2);
    REQUIRE(p.jit->State().R[0] == state.R[0] + 2);
}

TEST_CASE("Unsupported instructions fall back to the interpreter", "[jit][diff]") {
    Pair p;
    // mulu.w R2,R1 (unsupported) ; add #1,R0 ; sleep
    p.WriteCode(kCode, {0x212E, AddI(0, 1), static_cast<uint16_t>(kSleep)});
    auto state = p.ref->BaseState(kCode);
    state.R[1] = 7;
    state.R[2] = 9;
    p.Load(state);
    p.Step();
    REQUIRE(p.exec.GetStats().interpreted == 1);
    p.Step();
    REQUIRE(p.exec.GetStats().blocksRun == 1);
}

TEST_CASE("Delayed branch with an unsupported slot ends the block before the branch", "[jit][diff]") {
    Pair p;
    FillTargetArea(p);
    // add #1,R0 ; bra kTarget ; mulu.w R2,R1 (unsupported in the slot)
    p.WriteCode(kCode, {AddI(0, 1), Bra(125), 0x212E});
    p.Load(p.ref->BaseState(kCode));
    const auto first = p.Step();
    REQUIRE(first.retired == 1);
    p.Step(); // bra via the interpreter
    p.Step(); // slot via the interpreter (delay slot pending)
}

TEST_CASE("Blocks are capped at the maximum length", "[jit][diff]") {
    Pair p;
    std::vector<uint16_t> program(40, kNop);
    program.push_back(static_cast<uint16_t>(kSleep));
    p.WriteCode(kCode, program);
    p.Load(p.ref->BaseState(kCode));
    const auto info = p.Step();
    REQUIRE(info.retired == brimir::jit::kMaxBlockInstructions);
}

TEST_CASE("Executor::Run executes until the cycle target", "[jit][diff]") {
    Pair p;
    // add #1,R0 ; bra kCode ; nop  (infinite loop, 4 cycles per iteration)
    p.WriteCode(kCode, {AddI(0, 1), 0xAFFD, kNop});
    p.Load(p.ref->BaseState(kCode));
    auto &ctx = p.jit->sh2->GetJitContext();
    const uint64 executed = p.exec.Run(ctx, 0, 100);
    REQUIRE(executed >= 100);
    REQUIRE(executed < 100 + 4);
}

TEST_CASE("A stale fetch buffer at PC & 2 runs on the interpreter", "[jit][diff]") {
    Pair p;
    p.WriteCode(kCode, {kNop, AddI(0, 1), static_cast<uint16_t>(kSleep)});
    auto state = p.ref->BaseState(kCode + 2);
    state.fetchedOpcodes = (uint32_t{kNop} << 16) | kNop; // memory at kCode+2 now holds add #1,R0
    p.Load(state);
    p.Step();
    REQUIRE(p.exec.GetStats().interpreted == 1);
    REQUIRE(p.jit->State().R[0] == state.R[0]); // the buffered NOP ran, not the add
}

TEST_CASE("On-chip timer reads see the same cycle counts as the interpreter", "[jit][diff]") {
    auto ref = std::make_unique<Rig>();
    auto jit = std::make_unique<Rig>();
    brimir::jit::Executor exec;
    // loop: mov.b @R1,R2 (FRC high byte) ; add R2,R3 ; bra loop ; nop
    const std::vector<uint16_t> loop = {MovBL(2, 1), Add(3, 2), 0xAFFC, kNop};
    ref->WriteCode(kCode, loop);
    jit->WriteCode(kCode, loop);
    auto state = ref->BaseState(kCode);
    state.R[1] = 0xFFFFFE12;
    state.R[3] = 0;
    ref->Load(state);
    jit->Load(state);
    jit->sh2->SetJitExecutor(&exec);

    // The JIT stops on a block boundary; that point is also an instruction boundary for the
    // interpreter, so advancing the interpreter to the same count lands on the same state.
    const uint64 jitCycles = jit->sh2->Advance<false, false>(50000);
    const uint64 refCycles = ref->sh2->Advance<false, false>(jitCycles);
    REQUIRE(refCycles == jitCycles);
    const std::string diff = sh2test::DiffRigs(*ref, *jit);
    INFO(diff);
    REQUIRE(diff.empty());
    CHECK(jit->State().R[3] != 0u); // the timer actually advanced
}
```

Add to `tests/CMakeLists.txt` after `unit/test_jit_backend.cpp`:

```cmake
    unit/test_jit_diff.cpp
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target brimir_tests`
Expected: compile error — `brimir/jit/executor.hpp` not found.

- [ ] **Step 3: Implement the front end**

Create `src/jit/include/brimir/jit/frontend.hpp`:

```cpp
#pragma once

// Decodes guest SH-2 code into IR blocks (see design/sh2-jit.md section 5).

#include <brimir/jit/ir.hpp>

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <cstdint>

namespace brimir::jit {

constexpr uint32_t kMaxBlockInstructions = 32;

// Whether code at pc may be compiled: cached (0b000) and cache-through (0b001, 0b101) areas.
bool IsCompilableAddress(uint32_t pc);

// Decodes a block starting at pc. guestInstrCount == 0 means the first instruction is not
// supported and must run on the interpreter.
Block BuildBlock(ymir::sh2::SH2JitContext &ctx, uint32_t pc);

} // namespace brimir::jit
```

Create `src/jit/src/frontend.cpp`:

```cpp
#include <brimir/jit/frontend.hpp>

#include <ymir/hw/sh2/sh2_decode.hpp>

#include <optional>

namespace brimir::jit {

namespace {

using ymir::sh2::DecodeTable;
using ymir::sh2::OpcodeType;

constexpr uint8_t kWbNone = 0xFF;
constexpr uint32_t kWbPRBit = 1u << 16;

constexpr uint32_t RegBit(uint32_t reg) {
    return 1u << reg;
}

uint32_t Rn(uint16_t instr) {
    return (instr >> 8) & 0xFu;
}

uint32_t Rm(uint16_t instr) {
    return (instr >> 4) & 0xFu;
}

uint32_t SImm8(uint16_t instr) {
    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(instr & 0xFF)));
}

uint32_t Disp8x2(uint16_t instr) {
    return SImm8(instr) << 1;
}

uint32_t Disp12x2(uint16_t instr) {
    int32_t disp = instr & 0xFFF;
    if (disp & 0x800) {
        disp -= 0x1000;
    }
    return static_cast<uint32_t>(disp) << 1;
}

// Maps a supported instruction (normal or delay-slot decode) to its base opcode.
std::optional<OpcodeType> BaseOp(OpcodeType op, bool delaySlot) {
    if (!delaySlot) {
        switch (op) {
        case OpcodeType::NOP:
        case OpcodeType::MOV_R:
        case OpcodeType::MOV_I:
        case OpcodeType::MOVB_L:
        case OpcodeType::MOVL_L:
        case OpcodeType::MOVB_S:
        case OpcodeType::MOVL_S:
        case OpcodeType::MOVL_I:
        case OpcodeType::ADD:
        case OpcodeType::ADD_I:
        case OpcodeType::CMP_EQ_R:
        case OpcodeType::DT: return op;
        default: return std::nullopt;
        }
    }
    // MOVL_I is excluded in delay slots: its PC-relative base uses the delay-slot target.
    switch (op) {
    case OpcodeType::Delay_NOP: return OpcodeType::NOP;
    case OpcodeType::Delay_MOV_R: return OpcodeType::MOV_R;
    case OpcodeType::Delay_MOV_I: return OpcodeType::MOV_I;
    case OpcodeType::Delay_MOVB_L: return OpcodeType::MOVB_L;
    case OpcodeType::Delay_MOVL_L: return OpcodeType::MOVL_L;
    case OpcodeType::Delay_MOVB_S: return OpcodeType::MOVB_S;
    case OpcodeType::Delay_MOVL_S: return OpcodeType::MOVL_S;
    case OpcodeType::Delay_ADD: return OpcodeType::ADD;
    case OpcodeType::Delay_ADD_I: return OpcodeType::ADD_I;
    case OpcodeType::Delay_CMP_EQ_R: return OpcodeType::CMP_EQ_R;
    case OpcodeType::Delay_DT: return OpcodeType::DT;
    default: return std::nullopt;
    }
}

bool IsDelayedBranch(OpcodeType op) {
    return op == OpcodeType::BRA || op == OpcodeType::BTS || op == OpcodeType::BFS || op == OpcodeType::JMP ||
           op == OpcodeType::RTS;
}

// Lowers a non-branch instruction. `retiredBefore` = instructions completed before this one.
void LowerPlain(Builder &b, OpcodeType op, uint16_t instr, uint32_t pc, bool delaySlot, uint8_t retiredBefore) {
    const uint32_t n = Rn(instr);
    const uint32_t m = Rm(instr);
    const auto advance = [&] {
        if (delaySlot) {
            b.EndDelaySlot();
        }
    };

    switch (op) {
    case OpcodeType::NOP:
        advance();
        b.SetWb(kWbNone);
        b.AddCycles(1);
        break;
    case OpcodeType::MOV_R:
        b.SetReg(n, b.GetReg(m));
        advance();
        b.WbStall(RegBit(m) | RegBit(n));
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    case OpcodeType::MOV_I:
        b.SetReg(n, b.Const(SImm8(instr)));
        advance();
        b.WbStall(RegBit(n));
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    case OpcodeType::MOVB_L: {
        b.SyncCycles();
        const ValueId address = b.GetReg(m);
        b.AddAccessCycles(address, 1, false);
        b.WbStall(RegBit(m));
        b.SetReg(n, b.SExt8(b.Load(address, 1, false)));
        advance();
        b.SetWb(static_cast<uint8_t>(n));
        break;
    }
    case OpcodeType::MOVL_L: {
        b.SyncCycles();
        const ValueId address = b.GetReg(m);
        b.AddAccessCycles(address, 4, false);
        b.ExitIfBusWait(address, 4, false, pc, retiredBefore);
        b.SetReg(n, b.Load(address, 4, false));
        advance();
        b.WbStall(RegBit(m));
        b.SetWb(static_cast<uint8_t>(n));
        break;
    }
    case OpcodeType::MOVB_S: {
        b.SyncCycles();
        const ValueId address = b.GetReg(n);
        b.AddAccessCycles(address, 1, true);
        b.WbStall(RegBit(m) | RegBit(n));
        b.Store(address, 1, b.GetReg(m));
        advance();
        b.SetWb(kWbNone);
        break;
    }
    case OpcodeType::MOVL_S: {
        b.SyncCycles();
        const ValueId address = b.GetReg(n);
        b.AddAccessCycles(address, 4, true);
        b.ExitIfBusWait(address, 4, true, pc, retiredBefore);
        b.Store(address, 4, b.GetReg(m));
        advance();
        b.WbStall(RegBit(m) | RegBit(n));
        b.SetWb(kWbNone);
        break;
    }
    case OpcodeType::MOVL_I: {
        b.SyncCycles();
        const ValueId address = b.Const((pc & ~3u) + ((instr & 0xFFu) << 2) + 4u);
        b.AddAccessCycles(address, 4, false);
        b.SetReg(n, b.Load(address, 4, true));
        b.SetWb(static_cast<uint8_t>(n));
        break;
    }
    case OpcodeType::ADD:
        b.SetReg(n, b.Add(b.GetReg(n), b.GetReg(m)));
        advance();
        b.WbStall(RegBit(m) | RegBit(n));
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    case OpcodeType::ADD_I:
        b.SetReg(n, b.Add(b.GetReg(n), b.Const(SImm8(instr))));
        advance();
        b.WbStall(RegBit(n));
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    case OpcodeType::CMP_EQ_R:
        b.SetT(b.CmpEq(b.GetReg(n), b.GetReg(m)));
        advance();
        b.WbStall(RegBit(m) | RegBit(n));
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    case OpcodeType::DT: {
        const ValueId dec = b.Sub(b.GetReg(n), b.Const(1));
        b.SetReg(n, dec);
        b.SetT(b.CmpEq(dec, b.Const(0)));
        advance();
        b.WbStall(RegBit(n));
        b.AddCycles(1);
        b.SetWb(kWbNone);
        break;
    }
    default: break; // callers only pass supported opcodes
    }
}

// Lowers the branch part of a delayed branch (the slot is lowered by the caller).
void LowerDelayedBranch(Builder &b, OpcodeType op, uint16_t instr, uint32_t pc, uint8_t retiredBefore) {
    switch (op) {
    case OpcodeType::BRA:
        b.SetupDelaySlot(b.Const(pc + Disp12x2(instr) + 4u));
        b.SetWb(kWbNone);
        b.AddCycles(2);
        break;
    case OpcodeType::BTS:
    case OpcodeType::BFS: {
        b.SetWb(kWbNone);
        const ValueId t = b.GetT();
        // Not-taken condition: BT/S is not taken when T == 0, BF/S when T == 1.
        const ValueId notTaken = op == OpcodeType::BTS ? b.CmpEq(t, b.Const(0)) : t;
        b.ExitIf(notTaken, pc + 2u, 1, false, static_cast<uint8_t>(retiredBefore + 1));
        b.SetupDelaySlot(b.Const(pc + Disp8x2(instr) + 4u));
        b.AddCycles(2);
        break;
    }
    case OpcodeType::JMP: {
        const uint32_t m = Rn(instr);
        b.SetupDelaySlot(b.GetReg(m));
        b.WbStall(RegBit(m));
        b.AddCycles(2);
        b.SetWb(kWbNone);
        break;
    }
    case OpcodeType::RTS:
        b.SetupDelaySlot(b.GetPR());
        b.WbStall(kWbPRBit);
        b.AddCycles(2);
        b.SetWb(kWbNone);
        break;
    default: break;
    }
}

} // namespace

bool IsCompilableAddress(uint32_t pc) {
    const uint32_t partition = pc >> 29u;
    return partition == 0b000 || partition == 0b001 || partition == 0b101;
}

Block BuildBlock(ymir::sh2::SH2JitContext &ctx, uint32_t startPC) {
    Block block;
    block.startPC = startPC;
    Builder b(block);

    const auto &table = DecodeTable::s_instance;
    const auto peek = [&](uint32_t address) { return ctx.peekInstruction(ctx.sh2, address); };
    const auto refillIfAligned = [&](uint32_t address) {
        if ((address & 2u) == 0) {
            b.Refill(address);
        }
    };

    uint32_t pc = startPC;
    uint8_t count = 0;
    while (count < kMaxBlockInstructions && IsCompilableAddress(pc)) {
        const uint16_t instr = peek(pc);
        const OpcodeType op = table.opcodes[0][instr];

        if (IsDelayedBranch(op)) {
            if (count + 2 > kMaxBlockInstructions || !IsCompilableAddress(pc + 2)) {
                break;
            }
            const uint16_t slot = peek(pc + 2);
            const auto slotBase = BaseOp(table.opcodes[1][slot], true);
            if (!slotBase) {
                break;
            }
            block.guestOpcodes.push_back(instr);
            block.guestOpcodes.push_back(slot);
            refillIfAligned(pc);
            LowerDelayedBranch(b, op, instr, pc, count);
            refillIfAligned(pc + 2);
            LowerPlain(b, *slotBase, slot, pc + 2, true, static_cast<uint8_t>(count + 1));
            b.ExitDynamic(static_cast<uint8_t>(count + 2));
            block.guestInstrCount = count + 2u;
            return block;
        }

        if (op == OpcodeType::BT || op == OpcodeType::BF) {
            block.guestOpcodes.push_back(instr);
            refillIfAligned(pc);
            b.SetWb(kWbNone);
            const ValueId t = b.GetT();
            const ValueId taken = op == OpcodeType::BT ? t : b.CmpEq(t, b.Const(0));
            b.ExitIf(taken, pc + Disp8x2(instr) + 4u, 3, true, static_cast<uint8_t>(count + 1));
            b.AddCycles(1);
            b.Exit(pc + 2u, static_cast<uint8_t>(count + 1));
            block.guestInstrCount = count + 1u;
            return block;
        }

        const auto base = BaseOp(op, false);
        if (!base) {
            break;
        }
        block.guestOpcodes.push_back(instr);
        refillIfAligned(pc);
        LowerPlain(b, *base, instr, pc, false, count);
        ++count;
        pc += 2;
    }

    if (count == 0) {
        // Interpreter fallback. Remember the opcode so a code change triggers a rebuild.
        block = Block{};
        block.startPC = startPC;
        if (IsCompilableAddress(startPC)) {
            block.guestOpcodes.push_back(peek(startPC));
        }
        return block;
    }

    b.Exit(pc, count);
    block.guestInstrCount = count;
    return block;
}

} // namespace brimir::jit
```

- [ ] **Step 4: Implement the block cache**

Create `src/jit/include/brimir/jit/block_cache.hpp`:

```cpp
#pragma once

// Per-CPU cache of compiled blocks keyed by the full guest PC (see design/sh2-jit.md section 6).

#include <brimir/jit/ir.hpp>

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace brimir::jit {

// Total IR instructions kept before the cache is flushed (~20 MB of IR).
constexpr size_t kMaxCachedInsts = size_t{1} << 20;

class BlockCache {
public:
    // Returns the block for pc, compiling it on a miss or when its guest code changed.
    const Block &Get(ymir::sh2::SH2JitContext &ctx, uint32_t pc);

    void Flush();

    size_t Size() const {
        return m_blocks.size();
    }
    uint64_t Compiles() const {
        return m_compiles;
    }
    uint64_t Invalidations() const {
        return m_invalidations;
    }

private:
    static bool IsCurrent(const Block &block, ymir::sh2::SH2JitContext &ctx);

    std::unordered_map<uint32_t, std::unique_ptr<Block>> m_blocks;
    size_t m_totalInsts = 0;
    uint64_t m_compiles = 0;
    uint64_t m_invalidations = 0;
};

} // namespace brimir::jit
```

Create `src/jit/src/block_cache.cpp`:

```cpp
#include <brimir/jit/block_cache.hpp>

#include <brimir/jit/frontend.hpp>

#include <cassert>

namespace brimir::jit {

bool BlockCache::IsCurrent(const Block &block, ymir::sh2::SH2JitContext &ctx) {
    for (size_t i = 0; i < block.guestOpcodes.size(); ++i) {
        const uint32_t address = block.startPC + static_cast<uint32_t>(i * 2);
        if (ctx.peekInstruction(ctx.sh2, address) != block.guestOpcodes[i]) {
            return false;
        }
    }
    return true;
}

const Block &BlockCache::Get(ymir::sh2::SH2JitContext &ctx, uint32_t pc) {
    if (auto it = m_blocks.find(pc); it != m_blocks.end()) {
        if (IsCurrent(*it->second, ctx)) {
            return *it->second;
        }
        ++m_invalidations;
        m_totalInsts -= it->second->code.size();
        m_blocks.erase(it);
    }

    if (m_totalInsts >= kMaxCachedInsts) {
        Flush();
    }

    auto block = std::make_unique<Block>(BuildBlock(ctx, pc));
    if (!VerifyBlock(*block).empty()) {
        // Never run a malformed block: fall back to the interpreter for this PC.
        assert(false && "front end produced an invalid block");
        const uint16_t opcode = ctx.peekInstruction(ctx.sh2, pc);
        *block = Block{};
        block->startPC = pc;
        block->guestOpcodes.push_back(opcode);
    }
    ++m_compiles;
    m_totalInsts += block->code.size();
    const Block &ref = *block;
    m_blocks.emplace(pc, std::move(block));
    return ref;
}

void BlockCache::Flush() {
    m_blocks.clear();
    m_totalInsts = 0;
}

} // namespace brimir::jit
```

- [ ] **Step 5: Implement the executor**

Create `src/jit/include/brimir/jit/executor.hpp`:

```cpp
#pragma once

// SH-2 JIT executor: dispatches compiled blocks and falls back to the interpreter
// (see design/sh2-jit.md section 4.3).

#include <brimir/jit/block_cache.hpp>
#include <brimir/jit/frontend.hpp>
#include <brimir/jit/interp_backend.hpp>

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <cstdint>

namespace brimir::jit {

class Executor final : public ymir::sh2::ISH2Executor {
public:
    struct Stats {
        uint64_t blocksRun = 0;
        uint64_t interpreted = 0;
    };

    uint64 Run(ymir::sh2::SH2JitContext &ctx, uint64 executed, uint64 target) override;
    void Flush() override;

    // Runs one compiled block, or one interpreter instruction when no block applies
    // (pending interrupt, delay slot, or unsupported first instruction; reported as retired = 1).
    ExitInfo Step(ymir::sh2::SH2JitContext &ctx);

    const BlockCache &Cache() const {
        return m_cache;
    }
    const Stats &GetStats() const {
        return m_stats;
    }

private:
    BlockCache m_cache;
    Stats m_stats;
};

} // namespace brimir::jit
```

Create `src/jit/src/executor.cpp`:

```cpp
#include <brimir/jit/executor.hpp>

namespace brimir::jit {

uint64 Executor::Run(ymir::sh2::SH2JitContext &ctx, uint64 executed, uint64 target) {
    while (executed < target) {
        // On-chip timers read the running count, exactly as in the interpreter loop.
        *ctx.cyclesExecuted = executed;
        executed += Step(ctx).cycles;
    }
    *ctx.cyclesExecuted = executed;
    return executed;
}

void Executor::Flush() {
    m_cache.Flush();
}

ExitInfo Executor::Step(ymir::sh2::SH2JitContext &ctx) {
    const auto interpret = [&] {
        ++m_stats.interpreted;
        ExitInfo info;
        info.cycles = ctx.interpretOne(ctx.sh2);
        info.retired = 1;
        return info;
    };

    // Interrupt entry and pending delay slots are handled by the interpreter.
    if (*ctx.delaySlot || (*ctx.intrPending && *ctx.intrAllow)) {
        return interpret();
    }

    // At PC & 2 the interpreter executes the opcode already in its fetch buffer, which can differ
    // from memory if the code was modified after the fetch. Only compile when they agree.
    const uint32_t pc = *ctx.PC;
    if ((pc & 2u) != 0 && static_cast<uint16_t>(*ctx.fetchedOpcodes) != ctx.peekInstruction(ctx.sh2, pc)) {
        return interpret();
    }

    const Block &block = m_cache.Get(ctx, pc);
    if (block.guestInstrCount == 0) {
        return interpret();
    }

    // Same as InterpretNext on its non-interrupt path.
    *ctx.intrAllow = true;
    ++m_stats.blocksRun;
    return RunBlock(block, ctx);
}

} // namespace brimir::jit
```

Change the source list in `src/jit/CMakeLists.txt` to:

```cmake
add_library(brimir-jit STATIC
    src/ir.cpp
    src/interp_backend.cpp
    src/frontend.cpp
    src/block_cache.cpp
    src/executor.cpp
)
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build --target brimir_tests` then `build\bin\brimir_tests.exe "[jit]"`
Expected: `All tests passed`. If "On-chip timer reads" fails only on its final `CHECK` (R3 == 0), the FRT is not counting in the rig; change the loop to read the watchdog counter `WTCNT` at `0xFFFFFE81` instead. If a differential case fails, the message shows the first differing field and the instruction words; compare the IR (add `INFO(brimir::jit::PrintBlock(brimir::jit::BuildBlock(ctx, pc)))` temporarily) against the handler in `sh2.cpp` and fix the lowering, not the test.
Run: `ctest --test-dir build --output-on-failure` — Expected: `100% tests passed`.

- [ ] **Step 7: Record the planning decisions in the spec**

In `design/sh2-jit.md`:

Section 4.1, replace the bullet `- interrupt flags (\`m_intrFlags\`)` with:

```markdown
- interrupt flags (`m_intrFlags`) and the 32-bit instruction fetch buffer
- `m_cyclesExecuted`, kept current by the executor before every interpreter call and (via the `SyncCycles` IR op) before every memory access, because on-chip timers (WDT, FRT) read it
```

Section 6.1, replace the bullet that starts with "Keyed by a normalized guest PC" with:

```markdown
- Keyed by the full guest PC. Block exits write constant PCs that include the partition bits, so the cached (`0x0xxxxxxx`) and cache-through (`0x2xxxxxxx`) aliases of the same code get separate blocks.
- A block starting at `PC & 2` runs only if the fetch buffer's low halfword matches memory; otherwise the interpreter executes the buffered opcode, as the hardware would.
```

Section 6.3, add at the end:

```markdown
- Milestone 1 routes every access (including RAM) through the fork's own `MemRead`/`MemWrite`/`AccessCycles`/`IsBusWait` via context callbacks, which is exact by construction. The inline RAM fast path above is a milestone 2 optimization.
- Known deviation: a store into the currently executing block's own code takes effect at the next block entry (check-on-entry), not at the next instruction.
```

- [ ] **Step 8: Commit**

```bash
git add src/jit tests/CMakeLists.txt tests/unit/test_jit_diff.cpp design/sh2-jit.md
git commit -m "feat(jit): add front end, block cache and executor with differential tests"
```

---

### Task 5: Random-program differential fuzzing

**Files:**
- Modify: `tests/unit/test_jit_diff.cpp` (append one test case; reuses `Pair`, encoders and constants from Task 4)

**Interfaces:**
- Consumes: `Pair`, `kCode`, `kMmio`, `kSleep`, `Hex` and the encoder helpers in `test_jit_diff.cpp` (Task 4); `sh2test::DiffRigs` (Task 1).
- Produces: test case tagged `[jit][diff][fuzz]`, deterministic (fixed seeds), run in CI.

- [ ] **Step 1: Write the fuzz test**

Append to `tests/unit/test_jit_diff.cpp`:

```cpp
// Random programs of supported instructions, including branches, delay slots, loops, MMIO and
// bus waits. Register roles keep execution inside the program: R0-R7 data, R8-R11 data
// addresses (never written), R12 jump target, PR return target.
TEST_CASE("JIT matches the interpreter on random programs", "[jit][diff][fuzz]") {
    constexpr int kPrograms = 300;
    constexpr int kLength = 24;
    constexpr int kSteps = 80;

    for (int prog = 0; prog < kPrograms; ++prog) {
        const uint32_t seed = 0xF0220000u + static_cast<uint32_t>(prog);
        std::mt19937 rng(seed);
        Pair p;
        p.SetBusWaitEvery((rng() & 1u) ? 3u : 0u);

        const auto dataReg = [&] { return rng() % 8; };
        const auto addrReg = [&] { return 8 + rng() % 4; };
        // Displacement, in instructions, from instruction i to a random instruction of the program.
        const auto targetDisp = [&](int i) {
            return static_cast<uint32_t>(static_cast<int>(rng() % kLength) - i - 2);
        };

        std::vector<uint16_t> program;
        bool prevDelayed = false;
        for (int i = 0; i < kLength; ++i) {
            uint32_t pick = rng() % 16;
            if (prevDelayed && pick >= 12) {
                pick = rng() % 12; // delay slots never hold branches
            }
            uint16_t instr = kNop;
            switch (pick) {
            case 0: instr = kNop; break;
            case 1: instr = MovR(dataReg(), dataReg()); break;
            case 2: instr = MovI(dataReg(), rng()); break;
            case 3: instr = MovBL(dataReg(), addrReg()); break;
            case 4: instr = MovLL(dataReg(), addrReg()); break;
            case 5: instr = MovBS(addrReg(), dataReg()); break;
            case 6: instr = MovLS(addrReg(), dataReg()); break;
            case 7: instr = MovLI(dataReg(), rng() % 16); break;
            case 8: instr = Add(dataReg(), dataReg()); break;
            case 9: instr = AddI(dataReg(), rng()); break;
            case 10: instr = CmpEq(dataReg(), dataReg()); break;
            case 11: instr = Dt(dataReg()); break;
            case 12: instr = (rng() & 1u) ? Bt(targetDisp(i)) : Bf(targetDisp(i)); break;
            case 13: instr = (rng() & 1u) ? Bts(targetDisp(i)) : Bfs(targetDisp(i)); break;
            case 14: instr = Bra(targetDisp(i)); break;
            default: instr = (rng() & 1u) ? Jmp(12) : kRts; break;
            }
            program.push_back(instr);
            prevDelayed = pick >= 13;
        }
        for (int i = 0; i < 8; ++i) {
            program.push_back(static_cast<uint16_t>(kSleep));
        }
        p.WriteCode(kCode, program);

        auto state = p.ref->BaseState(kCode);
        for (int r = 0; r < 8; ++r) {
            state.R[r] = rng();
        }
        state.R[8] = 0x26040000 + (rng() & 0xFF0u);
        state.R[9] = 0x06040100 + (rng() & 0xFF0u);
        state.R[10] = kMmio + (rng() & 0xF0u);
        state.R[11] = 0x26048000;
        state.R[12] = kCode + 2 * (rng() % kLength);
        state.PR = kCode + 2 * (rng() % kLength);
        state.SR = 0xF0 | (rng() & 1u);
        state.wbReg = static_cast<uint8_t>(rng() % 17);
        p.Load(state);

        INFO("seed 0x" << std::hex << seed << " program " << Hex(program));
        for (int step = 0; step < kSteps; ++step) {
            p.Step();
            if (!sh2test::DiffRigs(*p.ref, *p.jit).empty()) {
                break; // Pair::Step already reported the difference
            }
        }
    }
}
```

- [ ] **Step 2: Run the fuzz test**

Run: `cmake --build build --target brimir_tests` then `build\bin\brimir_tests.exe "[fuzz]"`
Expected: `All tests passed`.
If it fails: the output shows the seed, the program words, the first differing field, and the failing cycle comparison. Reproduce the smallest failing instruction pair as an explicit case in "JIT matches the interpreter for instructions in delay slots" or the per-instruction test, fix the lowering in `src/jit/src/frontend.cpp` to match the interpreter handler, and rerun until the fuzz test passes.

- [ ] **Step 3: Run the full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 4: Commit**

```bash
git add tests/unit/test_jit_diff.cpp src/jit/src/frontend.cpp
git commit -m "test(jit): add random-program differential fuzzing"
```

(If `frontend.cpp` did not change, `git add` of an unmodified file is a no-op.)

---

### Task 6: `brimir_sh2_jit` core option and integration

**Files:**
- Modify: `include/brimir/core_wrapper.hpp`
- Modify: `src/bridge/core_wrapper.cpp` (`Initialize`, new methods next to `SetThreadedVDP2`)
- Modify: `src/libretro/options.cpp` (new definition before `brimir_profiling`)
- Modify: `src/libretro/libretro.cpp` (`OptionCache`, `apply_core_options`)
- Modify: `tests/unit/test_options.cpp`
- Create: `tests/unit/test_jit_integration.cpp`
- Modify: `tests/unit/test_bios_integration.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `README.md` (core options table), `CHANGELOG.md` (Unreleased), `design/sh2-jit.md` (status line)

**Interfaces:**
- Consumes: `brimir::jit::Executor` (Task 4), `SH2::SetJitExecutor` / `GetJitExecutor` (Task 1).
- Produces (`brimir::CoreWrapper`, public):
  - `void SetSH2JitEnabled(bool enable)` — attaches (or detaches) one executor per SH-2; remembered across `Initialize`
  - `bool IsSH2JitEnabled() const`
  - `const jit::Executor* GetSH2JitExecutor(bool master) const` — `nullptr` until first enabled
- Produces (libretro): core option `brimir_sh2_jit`, values `disabled` (default) / `enabled`, category `system`.

- [ ] **Step 1: Write the failing tests**

Create `tests/unit/test_jit_integration.cpp`:

```cpp
// Brimir - SH-2 JIT integration with CoreWrapper
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"

#include <brimir/core_wrapper.hpp>
#include <brimir/jit/executor.hpp>

#include <vector>

using brimir::CoreWrapper;

TEST_CASE("SH-2 JIT is off by default and toggles executors", "[jit][integration]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    REQUIRE_FALSE(core.IsSH2JitEnabled());
    REQUIRE(saturn->masterSH2.GetJitExecutor() == nullptr);

    core.SetSH2JitEnabled(true);
    REQUIRE(core.IsSH2JitEnabled());
    REQUIRE(saturn->masterSH2.GetJitExecutor() != nullptr);
    REQUIRE(saturn->slaveSH2.GetJitExecutor() != nullptr);
    REQUIRE(saturn->masterSH2.GetJitExecutor() != saturn->slaveSH2.GetJitExecutor());

    core.SetSH2JitEnabled(false);
    REQUIRE(saturn->masterSH2.GetJitExecutor() == nullptr);
    REQUIRE(saturn->slaveSH2.GetJitExecutor() == nullptr);
}

TEST_CASE("SH-2 JIT enabled before Initialize is applied", "[jit][integration]") {
    CoreWrapper core;
    core.SetSH2JitEnabled(true);
    REQUIRE(core.Initialize());
    REQUIRE(core.GetSaturn()->masterSH2.GetJitExecutor() != nullptr);
}

TEST_CASE("Frames run with the SH-2 JIT enabled", "[jit][integration]") {
    // Uses the built-in null IPL program (no BIOS loaded).
    CoreWrapper core;
    core.SetSH2JitEnabled(true);
    REQUIRE(core.Initialize());
    for (int i = 0; i < 30; ++i) {
        core.RunFrame();
    }
    REQUIRE(core.GetLastError().empty());
    const auto* master = core.GetSH2JitExecutor(true);
    REQUIRE(master != nullptr);
    REQUIRE(master->GetStats().blocksRun + master->GetStats().interpreted > 0);
}

TEST_CASE("Save state round trip with the SH-2 JIT flushes the block caches", "[jit][integration]") {
    CoreWrapper core;
    core.SetSH2JitEnabled(true);
    REQUIRE(core.Initialize());
    for (int i = 0; i < 10; ++i) {
        core.RunFrame();
    }
    std::vector<uint8_t> state(core.GetStateSize());
    REQUIRE(core.SaveState(state.data(), state.size()));
    REQUIRE(core.LoadState(state.data(), state.size()));
    REQUIRE(core.GetSH2JitExecutor(true)->Cache().Size() == 0);
    REQUIRE(core.GetSH2JitExecutor(false)->Cache().Size() == 0);
    core.RunFrame();
    REQUIRE(core.GetLastError().empty());
}
```

In `tests/unit/test_bios_integration.cpp`, add this helper inside the anonymous namespace, directly after `ReadFile`:

```cpp
// Runs up to 600 frames and returns true once the framebuffer shows a non-black pixel.
bool RendersNonBlackFrame(CoreWrapper &core) {
    for (int i = 0; i < 600; ++i) {
        core.RunFrame();
        if (i < 60 || i % 30 != 0) {
            continue;
        }
        const auto *fb = static_cast<const uint32_t *>(core.GetFramebuffer());
        const uint32_t w = core.GetFramebufferWidth();
        const uint32_t h = core.GetFramebufferHeight();
        const uint32_t stride = core.GetFramebufferPitch() / sizeof(uint32_t);
        if (fb == nullptr || w == 0 || h == 0 || stride < w) {
            continue;
        }
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                if ((fb[y * stride + x] & 0x00FFFFFF) != 0) {
                    return true;
                }
            }
        }
    }
    return false;
}
```

and append this test case at the end of the file:

```cpp
TEST_CASE("BIOS integration - real BIOS boots and renders with the SH-2 JIT", "[bios][integration][jit]") {
    const auto biosFiles = AvailableBIOS();
    if (biosFiles.empty()) {
        SKIP("No BIOS files found in " << FixturesDir().string());
    }

    const auto &biosPath = biosFiles.front();
    INFO("BIOS: " << biosPath.filename().string());

    CoreWrapper core;
    core.SetSH2JitEnabled(true);
    REQUIRE(core.Initialize());
    REQUIRE(core.LoadIPLFromFile(biosPath.string().c_str()));
    REQUIRE(RendersNonBlackFrame(core));
    REQUIRE(core.GetSH2JitExecutor(true)->GetStats().blocksRun > 0);
}
```

In `tests/unit/test_options.cpp`, add to the `expected[]` table after the `brimir_sh2_overclock` row:

```cpp
        { "brimir_sh2_jit",              "disabled" },
```

Add to `tests/CMakeLists.txt` after `unit/test_jit_diff.cpp`:

```cmake
    unit/test_jit_integration.cpp
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target brimir_tests`
Expected: compile errors — `SetSH2JitEnabled`, `IsSH2JitEnabled`, `GetSH2JitExecutor` are not members of `CoreWrapper`.

- [ ] **Step 3: Add the wrapper API**

In `include/brimir/core_wrapper.hpp`, directly before the line `namespace brimir {` that opens the `ConsoleRegion`/`CoreWrapper` declarations (line 40), add:

```cpp
namespace brimir::jit {
class Executor;
} // namespace brimir::jit

```

In the public section, directly after `void SetThreadedVDP2(bool enable);`, add:

```cpp

    /// @brief Run the SH-2 CPUs through the experimental JIT (see design/sh2-jit.md).
    /// Remembered across Initialize(). Has no effect while SH-2 cache emulation is active.
    void SetSH2JitEnabled(bool enable);
    bool IsSH2JitEnabled() const { return m_sh2JitEnabled; }

    /// @brief The JIT executor of the master or slave SH-2, or nullptr if the JIT was never enabled.
    const jit::Executor* GetSH2JitExecutor(bool master) const {
        return master ? m_jitMaster.get() : m_jitSlave.get();
    }
```

In the private section, directly after `Profiler m_profiler;`, add:

```cpp

    // SH-2 JIT executors (one per CPU), created on first enable
    bool m_sh2JitEnabled = false;
    std::unique_ptr<jit::Executor> m_jitMaster;
    std::unique_ptr<jit::Executor> m_jitSlave;
```

In `src/bridge/core_wrapper.cpp`, add `#include <brimir/jit/executor.hpp>` after `#include "brimir/core_wrapper.hpp"`. Directly after `CoreWrapper::SetThreadedVDP2`, add:

```cpp
void CoreWrapper::SetSH2JitEnabled(bool enable) {
    m_sh2JitEnabled = enable;
    if (!m_saturn) {
        return; // applied in Initialize()
    }
    if (enable) {
        if (!m_jitMaster) {
            m_jitMaster = std::make_unique<jit::Executor>();
        }
        if (!m_jitSlave) {
            m_jitSlave = std::make_unique<jit::Executor>();
        }
        m_saturn->masterSH2.SetJitExecutor(m_jitMaster.get());
        m_saturn->slaveSH2.SetJitExecutor(m_jitSlave.get());
    } else {
        m_saturn->masterSH2.SetJitExecutor(nullptr);
        m_saturn->slaveSH2.SetJitExecutor(nullptr);
    }
}
```

In `CoreWrapper::Initialize`, directly after the two `SetHostTimeProfiling(...)` lines added in plan 1A, add:

```cpp
        // Apply an SH-2 JIT state chosen before initialization
        SetSH2JitEnabled(m_sh2JitEnabled);
```

- [ ] **Step 4: Add the core option**

In `src/libretro/options.cpp`, insert before the `brimir_profiling` definition:

```cpp
    {
        "brimir_sh2_jit",
        "SH-2 JIT (Experimental)",
        nullptr,
        "Run the SH-2 CPUs through the experimental JIT. Currently an IR interpreter covering a small "
        "instruction subset: it is not faster yet and exists for testing. Not used while SH-2 cache "
        "emulation is active.",
        nullptr,
        "system",
        {
            { "disabled", "OFF" },
            { "enabled", "ON" },
            { nullptr, nullptr }
        },
        "disabled"
    },
```

In `src/libretro/libretro.cpp`, add to `struct OptionCache` after `std::string threaded_vdp2 = "enabled";`:

```cpp
    std::string sh2_jit = "disabled";
```

and to `apply_core_options` after the `brimir_threaded_vdp2` line:

```cpp
    apply("brimir_sh2_jit",                 g_options.sh2_jit,          [](const char* v){ g_core->SetSH2JitEnabled(strcmp(v, "enabled") == 0); });
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --target brimir_tests brimir_libretro brimir_bench`
Expected: builds with no errors.
Run: `build\bin\brimir_tests.exe "[jit]"` — Expected: `All tests passed` (the BIOS case runs if a BIOS is in `tests/fixtures/`, otherwise it is skipped).
Run: `ctest --test-dir build --output-on-failure` — Expected: `100% tests passed`.

- [ ] **Step 6: Update the docs**

In `README.md`, add to the core options table after the `SH-2 CPU Overclock` row:

```markdown
| System | SH-2 JIT (Experimental) | On/Off (default Off; not faster yet, for testing) |
```

In `CHANGELOG.md`, add under `## [Unreleased]` (create the `### Added` heading if missing):

```markdown
### Added
- **SH-2 JIT foundation (experimental, off by default)** - `brimir_sh2_jit` core option. The forked SH-2 hands execution to a new `brimir-jit` library: IR front end for a first instruction subset, per-CPU block cache with check-on-entry invalidation, and an IR-interpreter backend, with interpreter fallback for everything else. Differential tests compare every supported instruction, delay-slot combination, bus-wait retry and random programs against the interpreter for exact state and cycle counts. Not faster yet; see `design/sh2-jit.md`.
- **`brimir_bench`** - headless frame benchmark reporting ms/frame and the SH-2 share of emulation time.
```

In `design/sh2-jit.md`, replace the `**Status**:` line with:

```markdown
**Status**: Milestone 1 in progress — foundation implemented (plan `design/plans/2026-09-30-sh2-jit-m1b-foundation.md`); instruction coverage and shadow-verify remain (plan 1C)
```

- [ ] **Step 7: Commit**

```bash
git add include/brimir/core_wrapper.hpp src/bridge/core_wrapper.cpp src/libretro/options.cpp src/libretro/libretro.cpp tests/unit/test_options.cpp tests/unit/test_jit_integration.cpp tests/unit/test_bios_integration.cpp tests/CMakeLists.txt README.md CHANGELOG.md design/sh2-jit.md
git commit -m "feat(jit): add brimir_sh2_jit core option and wire executors into CoreWrapper"
```

---

## After this plan: remaining milestone 1 work (plan 1C)

Written after 1B lands, using what 1B reveals:

1. Front-end coverage for the rest of the milestone-1 subset: all MOV addressing modes (displacement, R0-indexed, GBR, pre-decrement, post-increment, byte/word variants), `MOVA`, `MOVT`, `CLRT`/`SETT`, `EXTS`/`EXTU`/`SWAP`/`XTRCT`, logic (`AND`/`OR`/`XOR`/`NOT`/`TST` and immediates), `SUB`, `NEG`, carry/overflow variants, remaining compares, shifts and rotates, `BSR`/`BRAF`/`BSRF`/`JSR`, `MOV.L @(disp,PC)` in delay slots. Each opcode gets a row in the per-instruction and delay-slot differential tests.
2. Shadow-verify mode (spec 7.2) as a debug core option.
3. Game-database "interpreter only" flag and the eligibility check.
4. Bound block overshoot before real-game validation: pass the remaining cycle budget into the block and exit at an instruction boundary when it runs out. Today the master's overshoot (up to one block, which can exceed 100 cycles with wait states) shifts the slave target, SCU, VDPs and scheduler in `Saturn::Run` (spec section 2).
5. Real-game validation: BIOS plus the baseline titles for 10 minutes each with the JIT on, zero shadow-verify mismatches (spec milestone 1 done criteria).
