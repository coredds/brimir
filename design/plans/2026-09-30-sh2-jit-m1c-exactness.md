# SH-2 JIT Milestone 1C — Instruction-Exact Execution and Lockstep Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the JIT stop and take interrupts at exactly the same instructions as the interpreter, and prove it with whole-system lockstep runs (JIT core vs interpreter core, identical state every frame).

**Architecture:** A new IR op `CheckBoundary` runs before every instruction after the first in a block and makes the interpreter's two per-instruction checks: the `Advance()` cycle budget and the interrupt check. A shared SH-2 state diff (in the bridge) is used by the isolated test rig and by a new whole-system `CompareCores`. `brimir_bench --lockstep` and lockstep tests run two cores side by side. A first control run (interpreter vs interpreter) proves the emulator is deterministic.

**Tech Stack:** C++20, CMake 3.28+, Catch2 (amalgamated), MSVC 2022 / GCC 14 / Apple Clang.

**Spec:** `design/sh2-jit.md`. Prerequisite: plans 1A and 1B (merged).

## Global Constraints

- Fork scope is exactly `src/core/include/ymir/hw/sh2/*` and `src/core/src/ymir/hw/sh2/*`; this plan needs no fork changes.
- With the JIT off, emulation is byte-identical to today.
- With the JIT on, the emulated system must be identical to the interpreter: architectural state, memory, bus access sequence, cycle counts and on-chip peripheral state, at every `Advance()` return. The only accepted deviations are those in `design/sh2-jit.md` section 6.5.
- `brimir-jit` includes only `ymir/core/types.hpp`, `ymir/hw/sh2/sh2_jit_iface.hpp` and `ymir/hw/sh2/sh2_decode.hpp` from the core.
- Tests: Catch2 in `tests/unit/`, registered in `tests/CMakeLists.txt`. JIT tests are tagged `[jit]`; BIOS-dependent tests `SKIP` without a BIOS.
- Commit messages: `type(scope): subject`.

## Scope decisions

- **Instruction-exact boundaries** (user decision, replaces the spec's block-granular checks). Cost: one compare and one byte load per instruction, cheap in native code later.
- **Lockstep replaces shadow-verify** (spec 7.2). With exact boundaries, a JIT core and an interpreter core must stay identical, so a whole-system comparison is stronger and simpler than re-running blocks.
- **The game-database "interpreter only" flag is dropped** unless lockstep validation finds a game that needs it.
- **Opcode coverage and real-game validation move to plan 1D**, which builds on the lockstep harness from this plan.

## Build and test commands (Windows)

From a *Developer PowerShell for VS 2022* (or `pwsh -NoProfile -File $env:TEMP\opencode\msvc.ps1 -Cmd "<command>"`):

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBRIMIR_BUILD_TESTS=ON
cmake --build build --target brimir_tests brimir_libretro brimir_bench
ctest --test-dir build --output-on-failure
build\bin\brimir_tests.exe "[jit]"
```

---

### Task 1: Instruction-exact block boundaries

**Files:**
- Modify: `src/jit/include/brimir/jit/ir.hpp`, `src/jit/src/ir.cpp`
- Modify: `src/jit/include/brimir/jit/interp_backend.hpp`, `src/jit/src/interp_backend.cpp`
- Modify: `src/jit/src/frontend.cpp` (`BuildBlock`)
- Modify: `src/jit/include/brimir/jit/executor.hpp`, `src/jit/src/executor.cpp`
- Modify: `tests/unit/test_jit_ir.cpp`, `tests/unit/test_jit_backend.cpp`, `tests/unit/test_jit_diff.cpp`
- Modify: `design/sh2-jit.md` (sections 2, 4.3, 5.1, 5.2, 11)

**Interfaces:**
- Produces (`brimir/jit/ir.hpp`): `Op::CheckBoundary` (enumerator directly after `SyncCycles`), `void Builder::CheckBoundary(uint32_t pc, uint8_t retired)`.
- Produces (`brimir/jit/interp_backend.hpp`): `constexpr uint64_t kNoCycleTarget = ~uint64_t{0};`, `ExitInfo::boundary` (`bool`), and the signature `ExitInfo RunBlock(const Block &block, ymir::sh2::SH2JitContext &ctx, uint64_t target = kNoCycleTarget, const bool *abortRequested = nullptr);`
- Produces (`brimir/jit/executor.hpp`): `ExitInfo Executor::Step(ymir::sh2::SH2JitContext &ctx, uint64 target = kNoCycleTarget);`

**IR semantics (new row):**

| Op | Operands | Semantics |
|---|---|---|
| `CheckBoundary` | imm = pc, retired | if `entryCycles + cycles >= target` or (`*ctx.intrPending && *ctx.intrAllow`): `PC = imm`, exit with `boundary = true` |

**Front-end rule:** emit `CheckBoundary(pc, retiredSoFar)` before every instruction except the first of the block, including before a delay-slot instruction, and always before that instruction's `Refill`. (Interpreter order: the `Advance` loop condition, then `InterpretNext`'s interrupt check, then the fetch.) Inside a delay slot, `SetupDelaySlot` has already cleared `intrPending`, so only the budget part can fire there. That matches the interpreter, whose loop can stop between a branch and its slot.

- [ ] **Step 1: Write the failing tests**

In `tests/unit/test_jit_ir.cpp`, inside "IR printer lists every instruction", add after the `SyncCycles` name check:

```cpp
    REQUIRE(std::string(OpName(Op::CheckBoundary)) == "CheckBoundary");
```

Append to `tests/unit/test_jit_backend.cpp`:

```cpp
TEST_CASE("Backend: CheckBoundary stops at the cycle target or a pending interrupt", "[jit][backend]") {
    Fixture f;
    auto &ctx = f.rig->sh2->GetJitContext();
    f.b.AddCycles(5);
    f.b.CheckBoundary(kCode + 2, 1);
    f.b.AddCycles(7);
    f.b.Exit(kCode + 4, 2);
    REQUIRE(VerifyBlock(f.block).empty());
    *ctx.cyclesExecuted = 100;

    SECTION("budget left") {
        const ExitInfo info = RunBlock(f.block, ctx, 106);
        REQUIRE_FALSE(info.boundary);
        REQUIRE(info.cycles == 12);
        REQUIRE(info.retired == 2);
        REQUIRE(*ctx.PC == kCode + 4);
    }
    SECTION("budget used up") {
        const ExitInfo info = RunBlock(f.block, ctx, 105);
        REQUIRE(info.boundary);
        REQUIRE(info.cycles == 5);
        REQUIRE(info.retired == 1);
        REQUIRE(*ctx.PC == kCode + 2);
    }
    SECTION("interrupt pending and allowed") {
        *ctx.intrPending = true;
        *ctx.intrAllow = true;
        const ExitInfo info = RunBlock(f.block, ctx);
        REQUIRE(info.boundary);
        REQUIRE(info.cycles == 5);
        REQUIRE(*ctx.PC == kCode + 2);
    }
    SECTION("interrupt pending but not allowed") {
        *ctx.intrPending = true;
        *ctx.intrAllow = false;
        const ExitInfo info = RunBlock(f.block, ctx);
        REQUIRE_FALSE(info.boundary);
        REQUIRE(info.cycles == 12);
    }
}
```

Append to `tests/unit/test_jit_diff.cpp`:

```cpp
// With instruction-exact boundaries, Advance() through the JIT must stop at exactly the same
// instruction as the interpreter for any cycle target, including between a branch and its slot.
TEST_CASE("JIT Advance matches interpreter Advance for every cycle target", "[jit][diff][exact]") {
    // loop: mov.l @R8,R1 ; add R1,R2 ; mov.l R2,@R9 ; dt R3 ; bf/s loop ; add #1,R4 ; bra loop ; nop
    const std::vector<uint16_t> loop = {MovLL(1, 8), Add(2, 1), MovLS(9, 2), Dt(3),
                                        Bfs(0xFA),   AddI(4, 1), Bra(0xFF8), kNop};
    bool sawDelaySlotStop = false;
    for (uint32_t target = 1; target <= 400; ++target) {
        Pair p;
        p.SetBusWaitEvery(target % 3 == 0 ? 2u : 0u);
        p.WriteCode(kCode, loop);
        p.Write32(0x06040000, 0x01020304);
        auto state = p.ref->BaseState(kCode);
        state.R[3] = 3;
        state.R[8] = 0x26040000;
        state.R[9] = (target & 1u) ? kMmio + 0x10 : 0x26040010;
        p.Load(state);
        p.jit->sh2->SetJitExecutor(&p.exec);

        const uint64 refCycles = p.ref->sh2->Advance<false, false>(target);
        const uint64 jitCycles = p.jit->sh2->Advance<false, false>(target);
        INFO("target " << target);
        REQUIRE(jitCycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*p.ref, *p.jit);
        INFO(diff);
        REQUIRE(diff.empty());
        sawDelaySlotStop = sawDelaySlotStop || p.jit->State().delaySlot;
    }
    CHECK(sawDelaySlotStop); // some target stopped between bf/s and its slot
}

// A 32/32 division by zero (write to DVDNT) raises the DIVU overflow interrupt synchronously,
// inside the store. The interpreter takes it before the next instruction; so must the JIT.
TEST_CASE("Interrupts raised inside a block are taken at the same instruction", "[jit][diff][exact]") {
    constexpr uint32_t kVbr = 0x06008000;
    constexpr uint32_t kVector = 0x40;
    constexpr uint32_t kHandler = 0x06009000;
    Pair p;
    for (Rig *rig : {p.ref.get(), p.jit.get()}) {
        rig->Write32(kVbr + kVector * 4, kHandler);
        rig->WriteCode(kHandler, {static_cast<uint16_t>(kSleep)});
        auto &ctx = rig->sh2->GetJitContext();
        ctx.write(ctx.sh2, 0xFFFFFF00, 4, 0);       // DVSR = 0: the next division overflows
        ctx.write(ctx.sh2, 0xFFFFFF08, 4, 0x2);     // DVCR.OVFIE = 1
        ctx.write(ctx.sh2, 0xFFFFFF0C, 4, kVector); // VCRDIV: vector number
        ctx.write(ctx.sh2, 0xFFFFFEE2, 1, 0xF0);    // IPRA: DIVU interrupt level 15
    }
    // mov.l R1,@R2 (R2 = DVDNT) ; add #1,R3 ; add #1,R3 ; sleep
    p.WriteCode(kCode, {MovLS(2, 1), AddI(3, 1), AddI(3, 1), static_cast<uint16_t>(kSleep)});
    auto state = p.ref->BaseState(kCode);
    state.SR = 0x00; // interrupt mask 0
    state.VBR = kVbr;
    state.R[1] = 1234;
    state.R[2] = 0xFFFFFF04;
    state.R[3] = 0;
    state.R[15] = 0x0600F000; // stack for exception entry
    p.Load(state);
    p.jit->sh2->SetJitExecutor(&p.exec);

    const uint64 refCycles = p.ref->sh2->Advance<false, false>(200);
    const uint64 jitCycles = p.jit->sh2->Advance<false, false>(200);
    REQUIRE(jitCycles == refCycles);
    const std::string diff = sh2test::DiffRigs(*p.ref, *p.jit);
    INFO(diff);
    REQUIRE(diff.empty());
    REQUIRE(p.ref->State().R[3] == 0u); // the interpreter took the interrupt before the adds
    REQUIRE(p.ref->State().sleep);      // and ran the handler
}
```

In the existing test "On-chip timer reads see the same cycle counts as the interpreter", replace:

```cpp
    const uint64 jitCycles = jit->sh2->Advance<false, false>(50000);
    const uint64 refCycles = ref->sh2->Advance<false, false>(jitCycles);
```

with:

```cpp
    const uint64 jitCycles = jit->sh2->Advance<false, false>(50000);
    const uint64 refCycles = ref->sh2->Advance<false, false>(50000);
```

and update the comment above those lines to: `// With instruction-exact boundaries, both stop at the same instruction for the same target.`

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target brimir_tests`
Expected: compile errors (`Op::CheckBoundary`, `Builder::CheckBoundary`, the `RunBlock` target parameter and `ExitInfo::boundary` do not exist).

- [ ] **Step 3: Add the IR op**

In `src/jit/include/brimir/jit/ir.hpp`, add `CheckBoundary,` to `enum class Op` directly after `SyncCycles,`, and to `class Builder` directly after `void SyncCycles();`:

```cpp
    void CheckBoundary(uint32_t pc, uint8_t retired);
```

In `src/jit/src/ir.cpp`, add to `kOpInfo` directly after the `{"SyncCycles", false, 0, false, false},` entry:

```cpp
    {"CheckBoundary", false, 0, false, false},
```

and directly after `Builder::SyncCycles()`:

```cpp
void Builder::CheckBoundary(uint32_t pc, uint8_t retired) {
    Inst &inst = Emit(Op::CheckBoundary);
    inst.imm = pc;
    inst.retired = retired;
}
```

- [ ] **Step 4: Implement it in the backend**

In `src/jit/include/brimir/jit/interp_backend.hpp`:
- add directly after `namespace brimir::jit {`:

```cpp
// Cycle target meaning "no budget limit" (tests that run a single block).
constexpr uint64_t kNoCycleTarget = ~uint64_t{0};
```

- add to `ExitInfo` after `bool aborted = false; ...`:

```cpp
    bool boundary = false; // stopped before an instruction: cycle target reached or interrupt pending
```

- replace the `RunBlock` declaration (and its comment) with:

```cpp
// Executes a verified, non-empty block against the live SH-2 state.
// Before every instruction after the first, the block makes the interpreter's two checks
// (CheckBoundary): *ctx.cyclesExecuted at entry plus the cycles so far must stay below `target`,
// and no interrupt may be pending and allowed; otherwise it stops there with boundary = true.
// If *abortRequested becomes true during a memory access or pipeline refill (for example a WDT
// register access that resets the CPU and flushes the cache), the block stops right there: PC is
// left untouched and the result has aborted = true with the cycles accumulated so far.
ExitInfo RunBlock(const Block &block, ymir::sh2::SH2JitContext &ctx, uint64_t target = kNoCycleTarget,
                  const bool *abortRequested = nullptr);
```

In `src/jit/src/interp_backend.cpp`, change the definition's signature to match (`uint64_t target, const bool *abortRequested`) and add this case to the switch, after `case Op::SyncCycles:`:

```cpp
        case Op::CheckBoundary:
            // The interpreter's per-instruction checks: Advance's budget (m_cyclesExecuted < target)
            // and InterpretNext's interrupt test (pending && allowed).
            if (entryCycles + info.cycles >= target || (*ctx.intrPending && *ctx.intrAllow)) {
                *ctx.PC = in.imm;
                info.retired = in.retired;
                info.boundary = true;
                return info;
            }
            break;
```

- [ ] **Step 5: Emit it in the front end**

In `src/jit/src/frontend.cpp`, inside `BuildBlock`, add after the `refillIfAligned` lambda:

```cpp
    // The interpreter checks the cycle budget and pending interrupts before every instruction;
    // the first instruction of a block is covered by the executor's own checks.
    const auto boundary = [&](uint32_t address, uint8_t retired) {
        if (retired > 0) {
            b.CheckBoundary(address, retired);
        }
    };
```

Then insert a `boundary(...)` call directly before each instruction's first refill:
- delayed branch: `boundary(pc, count);` before `refillIfAligned(pc);`, and `b.CheckBoundary(pc + 2, static_cast<uint8_t>(count + 1));` before `refillIfAligned(pc + 2);`. The slot always follows a completed branch, so its check is unconditional.
- `BT`/`BF`: `boundary(pc, count);` before `refillIfAligned(pc);`
- plain instructions: `boundary(pc, count);` before `refillIfAligned(pc);`

- [ ] **Step 6: Pass the budget through the executor**

In `src/jit/include/brimir/jit/executor.hpp`, change the `Step` declaration and comment to:

```cpp
    // Runs one compiled block, or one interpreter instruction when no block applies
    // (pending interrupt, delay slot, or unsupported first instruction; reported as retired = 1).
    // A compiled block stops before any instruction at which *ctx.cyclesExecuted + its cycles
    // would reach `target`, exactly where the interpreter's Advance loop stops.
    ExitInfo Step(ymir::sh2::SH2JitContext &ctx, uint64 target = kNoCycleTarget);
```

In `src/jit/src/executor.cpp`:
- change the definition to `ExitInfo Executor::Step(ymir::sh2::SH2JitContext &ctx, uint64 target)`
- in `Run`, change `executed += Step(ctx).cycles;` to `executed += Step(ctx, target).cycles;`
- change `RunBlock(block, ctx, &m_flushPending)` to `RunBlock(block, ctx, target, &m_flushPending)`

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build build --target brimir_tests` then `build\bin\brimir_tests.exe "[jit]"`
Expected: all pass, including the 2 new `[exact]` cases and the new backend case.
If "Interrupts raised inside a block" fails only on `R[3] == 0` / `sleep` (both rigs agree, but no interrupt was taken), the DIVU setup is wrong. Read `OnChipRegWrite*` (the 0x100–0x10C and 0xE2 cases) and `ExecuteDiv32` in `src/core/src/ymir/hw/sh2/sh2.cpp` and fix the test setup so the overflow interrupt is raised. Keep the JIT-vs-interpreter assertions.
Run: `ctest --test-dir build --output-on-failure` — Expected: `100% tests passed`.

- [ ] **Step 8: Update the spec**

In `design/sh2-jit.md`:

Replace section `## 2. Timing model` (its bullet list, up to `## 3.`) with:

```markdown
## 2. Timing model

- Each compiled block accumulates exactly the cycles Ymir's `InterpretNext()` would have returned for the same instructions (fixed costs, `AccessCycles` wait states from the bus page table, pipeline refills, `WritebackCycles` load-use stalls).
- **Instruction-exact boundaries.** Before every instruction after the first, a block makes the same two checks the interpreter makes before every instruction (IR op `CheckBoundary`): the `Advance()` cycle budget (`m_cyclesExecuted < target`) and the interrupt check (pending and allowed). A block therefore stops at exactly the instruction where the interpreter stops and takes interrupts at the same instruction, so a JIT-enabled system runs identically to the interpreter. This is verified by whole-system lockstep runs (section 7.2).
- This replaced the original block-granular checks (changed during milestone 1). Those let a block overshoot the master/slave sync step in `Saturn::Run`, and the overshoot delayed the slave SH-2, SCU, VDP and scheduler by up to a whole block.
- The remaining deviations are listed in section 6.5.
```

In section 4.3, add this line directly after the executor pseudocode block:

```markdown
`backend.Run` receives `target`; the block itself stops before any later instruction once the budget is used up or an interrupt becomes pending (`CheckBoundary`), exactly like the interpreter loop.
```

In section 5.1, replace the bullet `- after \`SLEEP\`, \`TRAPA\`, \`RTE\`, or any instruction that writes \`SR\` or \`VBR\` (for example \`LDC Rm,SR\`), so a newly unmasked interrupt is seen promptly` with:

```markdown
- before `SLEEP`, `TRAPA`, `RTE` and instructions that write `SR` or `VBR` while they are not supported (interpreter fallback); once supported, `CheckBoundary` before the next instruction already sees a newly unmasked interrupt
```

In section 5.2, add to the **control** bullet: `, boundary check (\`CheckBoundary\`: cycle budget and pending interrupt before an instruction)`.

In section 11, delete the bullet that starts with `- Bound block overshoot (plan 1C`.

- [ ] **Step 9: Commit**

```bash
git add src/jit tests/unit/test_jit_ir.cpp tests/unit/test_jit_backend.cpp tests/unit/test_jit_diff.cpp design/sh2-jit.md
git commit -m "feat(jit): stop blocks at the interpreter's instruction boundaries"
```

---

### Task 2: Shared SH-2 state diff, bus access log, and peripheral comparison

**Files:**
- Create: `include/brimir/lockstep.hpp`, `src/bridge/lockstep.cpp`
- Modify: `src/bridge/CMakeLists.txt`
- Modify: `tests/unit/sh2_test_rig.hpp`, `tests/unit/sh2_test_rig.cpp`
- Modify: `tests/unit/test_sh2_jit_iface.cpp`, `tests/unit/test_jit_diff.cpp` (Advance-level comparisons)
- Create: `tests/unit/test_sh2_state_diff.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ymir::savestate::SH2SaveState` (`src/core/include/ymir/savestate/savestate_sh2.hpp`).
- Produces (`brimir/lockstep.hpp`, namespace `brimir`):
  - `enum class SH2DiffScope { Cpu, CpuAndPeripherals };`
  - `std::string DiffSH2State(const ymir::savestate::SH2SaveState &a, const ymir::savestate::SH2SaveState &b, SH2DiffScope scope, const char *label = "");` returns `""` when equal, otherwise `"<label><field> differs: a=0x.. b=0x.."` for the first differing field.
- Produces (test rig):
  - `struct sh2test::MmioAccess { char kind; uint8_t size; uint32_t address; uint32_t value; };` with kind `'R'` read, `'W'` write, `'B'` bus-wait query
  - `Mmio::log` (`std::vector<MmioAccess>`)
  - `std::string DiffRigs(const Rig &a, const Rig &b, bool comparePeripherals = false);`

**Field scopes:**
- `Cpu`:
  - R0–R15, PC, PR, MACL, MACH, SR, GBR, VBR
  - delaySlot, delaySlotTarget, intrAllow, fetchedOpcodes, wbReg
  - sleep, SBYCR
  - BSC registers
  - DIVU registers
  - INTC ICR, levels, vectors, NMI, extVec
  - cache CCR
- `CpuAndPeripherals` adds:
  - FRT: all fields
  - WDT: all fields
  - DMAC: DMAOR and both channels
  - INTC: pendingSource, pendingLevel
  
  These advance with time inside `SH2::Step`/`Advance` (timers, DMA) or depend on them (pending interrupt), so they are compared only at `Advance()` boundaries, not in per-step tests. In per-step tests, the reference rig's `SH2::Step` advances timers and the JIT's step does not.

- [ ] **Step 1: Write the failing tests**

Create `tests/unit/test_sh2_state_diff.cpp`:

```cpp
// Brimir - SH-2 state diff and test rig bus log tests
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/lockstep.hpp>

#include <memory>

using brimir::DiffSH2State;
using brimir::SH2DiffScope;

TEST_CASE("DiffSH2State reports the first differing field", "[jit][diff]") {
    auto rig = std::make_unique<sh2test::Rig>();
    const auto a = rig->State();
    auto b = a;
    REQUIRE(DiffSH2State(a, b, SH2DiffScope::CpuAndPeripherals).empty());

    b.R[5] ^= 1;
    const std::string diff = DiffSH2State(a, b, SH2DiffScope::Cpu, "master ");
    REQUIRE(diff.find("master R5 differs") != std::string::npos);
}

TEST_CASE("DiffSH2State compares peripherals only in the peripheral scope", "[jit][diff]") {
    auto rig = std::make_unique<sh2test::Rig>();
    const auto a = rig->State();
    auto b = a;
    b.frt.FRC ^= 1;
    REQUIRE(DiffSH2State(a, b, SH2DiffScope::Cpu).empty());
    REQUIRE(DiffSH2State(a, b, SH2DiffScope::CpuAndPeripherals).find("frt.FRC") != std::string::npos);

    b = a;
    b.wdt.WTCNT ^= 1;
    REQUIRE(DiffSH2State(a, b, SH2DiffScope::CpuAndPeripherals).find("wdt.WTCNT") != std::string::npos);
    b = a;
    b.intc.pendingLevel ^= 1;
    REQUIRE(DiffSH2State(a, b, SH2DiffScope::Cpu).empty());
    REQUIRE_FALSE(DiffSH2State(a, b, SH2DiffScope::CpuAndPeripherals).empty());
}

TEST_CASE("Test rig logs MMIO accesses and DiffRigs compares the logs", "[jit][diff]") {
    auto a = std::make_unique<sh2test::Rig>();
    auto b = std::make_unique<sh2test::Rig>();
    auto &ctx = a->sh2->GetJitContext();
    ctx.write(ctx.sh2, 0x22000010, 4, 0xCAFEF00D);
    ctx.read(ctx.sh2, 0x22000010, 2, false);
    ctx.busWait(ctx.sh2, 0x22000010, 4, false);
    REQUIRE(a->mmio.log.size() == 3);
    CHECK(a->mmio.log[0].kind == 'W');
    CHECK(a->mmio.log[0].size == 4);
    CHECK(a->mmio.log[0].value == 0xCAFEF00Du);
    CHECK(a->mmio.log[1].kind == 'R');
    CHECK(a->mmio.log[1].value == 0xCAFEu);
    CHECK(a->mmio.log[2].kind == 'B');

    // Same final MMIO contents, different access sequence: still a difference.
    auto &ctxB = b->sh2->GetJitContext();
    ctxB.write(ctxB.sh2, 0x22000010, 4, 0xCAFEF00D);
    REQUIRE(sh2test::DiffRigs(*a, *b).find("MMIO log") != std::string::npos);
}
```

Register it in `tests/CMakeLists.txt` after `unit/test_jit_executor.cpp` (or after the last JIT test file):

```cmake
    unit/test_sh2_state_diff.cpp
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake -S . -B build && cmake --build build --target brimir_tests`
Expected: compile errors — `brimir/lockstep.hpp` not found, no `Mmio::log`.

- [ ] **Step 3: Implement the shared diff**

Create `include/brimir/lockstep.hpp`:

```cpp
#pragma once

// State comparison for SH-2 JIT validation (design/sh2-jit.md section 7):
// SH-2 state diffs (used by the isolated SH-2 test rig and by whole-system lockstep runs).

#include <ymir/savestate/savestate_sh2.hpp>

#include <string>

namespace brimir {

enum class SH2DiffScope {
    Cpu,               // CPU, pipeline and register-programmed on-chip state
    CpuAndPeripherals, // also timers (FRT, WDT), DMAC and the pending interrupt, which advance with time
};

// Returns "" if equal, otherwise "<label><field> differs: a=0x.. b=0x.." for the first difference.
std::string DiffSH2State(const ymir::savestate::SH2SaveState &a, const ymir::savestate::SH2SaveState &b,
                         SH2DiffScope scope, const char *label = "");

} // namespace brimir
```

Create `src/bridge/lockstep.cpp`:

```cpp
// Brimir - state comparison for SH-2 JIT validation
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "brimir/lockstep.hpp"

#include <cstdint>
#include <cstdio>

namespace brimir {

namespace {

std::string Describe(const char *label, const char *field, uint64_t a, uint64_t b) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%s%s differs: a=0x%llX b=0x%llX", label, field,
                  static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return buf;
}

} // namespace

std::string DiffSH2State(const ymir::savestate::SH2SaveState &a, const ymir::savestate::SH2SaveState &b,
                         SH2DiffScope scope, const char *label) {
#define BRIMIR_DIFF(field)                                                                                   \
    if (a.field != b.field) {                                                                                \
        return Describe(label, #field, static_cast<uint64_t>(a.field), static_cast<uint64_t>(b.field));       \
    }

    char name[32];
    for (int i = 0; i < 16; ++i) {
        if (a.R[i] != b.R[i]) {
            std::snprintf(name, sizeof(name), "R%d", i);
            return Describe(label, name, a.R[i], b.R[i]);
        }
    }
    BRIMIR_DIFF(PC)
    BRIMIR_DIFF(PR)
    BRIMIR_DIFF(MACL)
    BRIMIR_DIFF(MACH)
    BRIMIR_DIFF(SR)
    BRIMIR_DIFF(GBR)
    BRIMIR_DIFF(VBR)
    BRIMIR_DIFF(delaySlot)
    BRIMIR_DIFF(delaySlotTarget)
    BRIMIR_DIFF(intrAllow)
    BRIMIR_DIFF(fetchedOpcodes)
    BRIMIR_DIFF(wbReg)
    BRIMIR_DIFF(sleep)
    BRIMIR_DIFF(SBYCR)
    BRIMIR_DIFF(bsc.BCR1)
    BRIMIR_DIFF(bsc.BCR2)
    BRIMIR_DIFF(bsc.WCR)
    BRIMIR_DIFF(bsc.MCR)
    BRIMIR_DIFF(bsc.RTCSR)
    BRIMIR_DIFF(bsc.RTCNT)
    BRIMIR_DIFF(bsc.RTCOR)
    BRIMIR_DIFF(divu.DVSR)
    BRIMIR_DIFF(divu.DVDNT)
    BRIMIR_DIFF(divu.DVCR)
    BRIMIR_DIFF(divu.VCRDIV)
    BRIMIR_DIFF(divu.DVDNTH)
    BRIMIR_DIFF(divu.DVDNTL)
    BRIMIR_DIFF(divu.DVDNTUH)
    BRIMIR_DIFF(divu.DVDNTUL)
    BRIMIR_DIFF(intc.ICR)
    BRIMIR_DIFF(intc.NMI)
    BRIMIR_DIFF(intc.extVec)
    for (int i = 0; i < 16; ++i) {
        if (a.intc.levels[i] != b.intc.levels[i]) {
            std::snprintf(name, sizeof(name), "intc.levels[%d]", i);
            return Describe(label, name, a.intc.levels[i], b.intc.levels[i]);
        }
        if (a.intc.vectors[i] != b.intc.vectors[i]) {
            std::snprintf(name, sizeof(name), "intc.vectors[%d]", i);
            return Describe(label, name, a.intc.vectors[i], b.intc.vectors[i]);
        }
    }
    BRIMIR_DIFF(cache.CCR)

    if (scope == SH2DiffScope::CpuAndPeripherals) {
        BRIMIR_DIFF(intc.pendingSource)
        BRIMIR_DIFF(intc.pendingLevel)
        BRIMIR_DIFF(frt.TIER)
        BRIMIR_DIFF(frt.FTCSR)
        BRIMIR_DIFF(frt.FRC)
        BRIMIR_DIFF(frt.OCRA)
        BRIMIR_DIFF(frt.OCRB)
        BRIMIR_DIFF(frt.TCR)
        BRIMIR_DIFF(frt.TOCR)
        BRIMIR_DIFF(frt.ICR)
        BRIMIR_DIFF(frt.TEMP)
        BRIMIR_DIFF(frt.cycleCount)
        BRIMIR_DIFF(frt.FTCSR_mask)
        BRIMIR_DIFF(wdt.WTCSR)
        BRIMIR_DIFF(wdt.WTCNT)
        BRIMIR_DIFF(wdt.RSTCSR)
        BRIMIR_DIFF(wdt.cycleCount)
        BRIMIR_DIFF(wdt.WTCSR_mask)
        BRIMIR_DIFF(wdt.busValue)
        BRIMIR_DIFF(dmac.DMAOR)
        for (int ch = 0; ch < 2; ++ch) {
            const auto &ca = a.dmac.channels[ch];
            const auto &cb = b.dmac.channels[ch];
            const struct {
                const char *field;
                uint64_t x, y;
            } fields[] = {{"SAR", ca.SAR, cb.SAR},
                          {"DAR", ca.DAR, cb.DAR},
                          {"TCR", ca.TCR, cb.TCR},
                          {"CHCR", ca.CHCR, cb.CHCR},
                          {"DRCR", ca.DRCR, cb.DRCR}};
            for (const auto &f : fields) {
                if (f.x != f.y) {
                    std::snprintf(name, sizeof(name), "dmac.channels[%d].%s", ch, f.field);
                    return Describe(label, name, f.x, f.y);
                }
            }
        }
    }
#undef BRIMIR_DIFF
    return {};
}

} // namespace brimir
```

In `src/bridge/CMakeLists.txt`, change the source list of `brimir_bridge` to:

```cmake
add_library(brimir_bridge OBJECT
    core_wrapper.cpp
    lockstep.cpp
)
```

- [ ] **Step 4: Use it in the test rig and add the MMIO log**

In `tests/unit/sh2_test_rig.hpp`:
- add `#include <brimir/lockstep.hpp>` after the ymir includes
- replace `struct Mmio { ... };` with:

```cpp
struct MmioAccess {
    char kind;    // 'R' read, 'W' write, 'B' bus-wait query
    uint8_t size; // bytes
    uint32_t address;
    uint32_t value; // value read or written (0 for bus-wait queries)
};

struct Mmio {
    std::array<uint8_t, 0x10000> data{};
    uint32_t busWaitQueries = 0;
    uint32_t busWaitEvery = 0;
    std::vector<MmioAccess> log; // every access in order, for bus-sequence comparison
};
```

- replace the `DiffRigs` declaration and comment with:

```cpp
// Returns a description of the first difference, or "" if identical: SH-2 state (a = interpreter,
// b = JIT), RAM, MMIO contents and the MMIO access log. comparePeripherals also compares timers,
// DMAC and the pending interrupt; use it only when both rigs ran through SH2::Advance (per-step
// tests use SH2::Step on the reference, which advances timers while the JIT's step does not).
std::string DiffRigs(const Rig &a, const Rig &b, bool comparePeripherals = false);
```

In `tests/unit/sh2_test_rig.cpp`, make every MMIO handler log its access. For example, the 16-bit read becomes:

```cpp
        [](uint32_t address, void *ctx) -> uint16_t {
            auto &m = *static_cast<Mmio *>(ctx);
            const uint16_t value = ReadBE16(&m.data[address & 0xFFFE]);
            m.log.push_back({'R', 2, address, value});
            return value;
        },
```

Apply the same pattern to all six read/write handlers (`'R'` or `'W'`, size 1/2/4, the value read or written). The bus-wait handler logs `{'B', static_cast<uint8_t>(size), address, 0}` on every query, before the `busWaitEvery` check. Its counting behavior stays the same.

Replace the body of `DiffRigs` with:

```cpp
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
```

(Add `#include <algorithm>` to `sh2_test_rig.cpp` for `std::min`.) Remove the old per-field SH-2 comparison code, which `DiffSH2State` now covers.

- [ ] **Step 5: Compare peripherals in the Advance-level tests**

Pass `true` as the third argument to `DiffRigs` in every test that runs both rigs through `SH2::Advance`:
- `test_sh2_jit_iface.cpp`: "Advance routes through an attached executor with identical results"
- `test_jit_diff.cpp`:
  - "On-chip timer reads see the same cycle counts as the interpreter"
  - "JIT Advance matches interpreter Advance for every cycle target"
  - "Interrupts raised inside a block are taken at the same instruction"

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build --target brimir_tests` then `build\bin\brimir_tests.exe "[jit]"`
Expected: all pass. A failure in an Advance-level test with `comparePeripherals` means the JIT path differs from the interpreter in timer, DMA or interrupt state. That is a real bug: fix the JIT (with interpreter evidence), not the test. A failure caused by the MMIO log in a per-step test means a differing bus access order, which is also a real bug.
Run: `ctest --test-dir build --output-on-failure` — Expected: `100% tests passed`.

- [ ] **Step 7: Commit**

```bash
git add include/brimir/lockstep.hpp src/bridge/lockstep.cpp src/bridge/CMakeLists.txt tests/unit/sh2_test_rig.hpp tests/unit/sh2_test_rig.cpp tests/unit/test_sh2_state_diff.cpp tests/unit/test_sh2_jit_iface.cpp tests/unit/test_jit_diff.cpp tests/CMakeLists.txt
git commit -m "test(jit): compare SH-2 peripherals and the MMIO access sequence"
```

---

### Task 3: Whole-system lockstep (tests and `brimir_bench --lockstep`)

**Files:**
- Modify: `include/brimir/lockstep.hpp`, `src/bridge/lockstep.cpp`
- Create: `tests/unit/test_jit_lockstep.cpp`
- Modify: `tests/unit/test_bios_integration.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `tools/brimir_bench.cpp`, `tools/README.md`
- Modify: `design/sh2-jit.md` (status line, sections 7.2, 7.3, 10), `CHANGELOG.md`

**Interfaces:**
- Consumes: `DiffSH2State` (Task 2); `CoreWrapper::GetSaturn`, `SetSH2JitEnabled`, `SetThreadedVDP1/2`, `RunFrame`, `GetFramebuffer/Width/Height/Pitch`; `ymir::Saturn::masterSH2/slaveSH2/slaveSH2Enabled/mem.WRAMLow/mem.WRAMHigh`.
- Produces (`brimir/lockstep.hpp`):
  - `void PrepareLockstepCore(CoreWrapper &core);` turns off threaded VDP1/VDP2 rendering (call after `Initialize`)
  - `std::string CompareCores(CoreWrapper &a, CoreWrapper &b);` compares both SH-2s (`CpuAndPeripherals`), `slaveSH2Enabled`, WRAM low/high and the last output frame (size and pixels)
  - `struct LockstepResult { int framesRun = 0; std::string divergence; };`
  - `LockstepResult RunLockstep(CoreWrapper &a, CoreWrapper &b, int frames);` runs both cores frame by frame and stops at the first difference
- Produces (bench): option `--lockstep N`. It runs N frames on a JIT core and an interpreter core loaded identically, prints progress every 600 frames, exits 0 if they stayed identical and exits 3 with the frame number and difference on divergence. It cannot be combined with `--dump-at`.

- [ ] **Step 1: Write the failing tests**

Create `tests/unit/test_jit_lockstep.cpp`:

```cpp
// Brimir - whole-system lockstep: a JIT core and an interpreter core must stay identical
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"

#include <brimir/core_wrapper.hpp>
#include <brimir/jit/executor.hpp>
#include <brimir/lockstep.hpp>

#include <memory>

using brimir::CoreWrapper;

namespace {

std::unique_ptr<CoreWrapper> MakeLockstepCore(bool jit) {
    auto core = std::make_unique<CoreWrapper>();
    core->SetSH2JitEnabled(jit);
    REQUIRE(core->Initialize());
    brimir::PrepareLockstepCore(*core);
    return core;
}

} // namespace

// Control: proves the emulator itself is deterministic, so any JIT lockstep failure is the JIT's.
TEST_CASE("Lockstep control: two interpreter cores stay identical", "[lockstep]") {
    // Uses the built-in null IPL program (no BIOS loaded).
    auto a = MakeLockstepCore(false);
    auto b = MakeLockstepCore(false);
    const auto result = brimir::RunLockstep(*a, *b, 120);
    INFO("frame " << result.framesRun << ": " << result.divergence);
    REQUIRE(result.divergence.empty());
    REQUIRE(result.framesRun == 120);
}

TEST_CASE("Lockstep: JIT core matches interpreter core", "[lockstep][jit]") {
    auto jit = MakeLockstepCore(true);
    auto ref = MakeLockstepCore(false);
    const auto result = brimir::RunLockstep(*jit, *ref, 300);
    INFO("frame " << result.framesRun << ": " << result.divergence);
    REQUIRE(result.divergence.empty());
    const auto &stats = jit->GetSH2JitExecutor(true)->GetStats();
    REQUIRE(stats.blocksRun + stats.interpreted > 0);
}

TEST_CASE("CompareCores detects a difference", "[lockstep]") {
    auto a = MakeLockstepCore(false);
    auto b = MakeLockstepCore(false);
    a->RunFrame();
    b->RunFrame();
    REQUIRE(brimir::CompareCores(*a, *b).empty());
    b->GetSaturn()->mem.WRAMHigh[0x123] ^= 0xFF;
    REQUIRE(brimir::CompareCores(*a, *b).find("WRAMHigh") != std::string::npos);
}
```

Append to `tests/unit/test_bios_integration.cpp` (add `#include <brimir/lockstep.hpp>` to the includes):

```cpp
TEST_CASE("BIOS integration - JIT core and interpreter core stay identical", "[bios][integration][jit][lockstep]") {
    const auto biosFiles = AvailableBIOS();
    if (biosFiles.empty()) {
        SKIP("No BIOS files found in " << FixturesDir().string());
    }
    const auto &biosPath = biosFiles.front();
    INFO("BIOS: " << biosPath.filename().string());

    CoreWrapper jit;
    CoreWrapper ref;
    jit.SetSH2JitEnabled(true);
    for (CoreWrapper *core : {&jit, &ref}) {
        REQUIRE(core->Initialize());
        brimir::PrepareLockstepCore(*core);
        REQUIRE(core->LoadIPLFromFile(biosPath.string().c_str()));
    }
    const auto result = brimir::RunLockstep(jit, ref, 600);
    INFO("frame " << result.framesRun << ": " << result.divergence);
    REQUIRE(result.divergence.empty());
    REQUIRE(jit.GetSH2JitExecutor(true)->GetStats().blocksRun > 0);
}
```

Register the new file in `tests/CMakeLists.txt` after `unit/test_jit_integration.cpp`:

```cmake
    unit/test_jit_lockstep.cpp
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target brimir_tests`
Expected: compile errors — `PrepareLockstepCore`, `CompareCores`, `RunLockstep` are not declared.

- [ ] **Step 3: Implement the whole-system comparison**

Append to `include/brimir/lockstep.hpp`, inside `namespace brimir` and after `DiffSH2State`:

```cpp

class CoreWrapper;

// Prepares a core for lockstep runs (call after Initialize): threaded VDP rendering is turned off
// so frames are produced on the emulation thread.
void PrepareLockstepCore(CoreWrapper &core);

// Compares the emulated state of two cores after the same frames: both SH-2s (CPU and on-chip
// peripherals), the slave SH-2 enable flag, low and high work RAM, and the last output frame.
// Returns "" if identical, otherwise the first difference ("a" is the first core).
std::string CompareCores(CoreWrapper &a, CoreWrapper &b);

struct LockstepResult {
    int framesRun = 0;      // frames run on both cores (including the diverging one)
    std::string divergence; // "" if the cores stayed identical
};

// Runs both cores frame by frame and compares them after every frame; stops at the first difference.
LockstepResult RunLockstep(CoreWrapper &a, CoreWrapper &b, int frames);
```

In `src/bridge/lockstep.cpp`, add `#include "brimir/core_wrapper.hpp"` and `#include <cstring>`, and append inside `namespace brimir`:

```cpp
void PrepareLockstepCore(CoreWrapper &core) {
    core.SetThreadedVDP1(false);
    core.SetThreadedVDP2(false);
}

std::string CompareCores(CoreWrapper &a, CoreWrapper &b) {
    ymir::Saturn *sa = a.GetSaturn();
    ymir::Saturn *sb = b.GetSaturn();
    if (sa == nullptr || sb == nullptr) {
        return "core not initialized";
    }

    ymir::savestate::SH2SaveState x{};
    ymir::savestate::SH2SaveState y{};
    sa->masterSH2.SaveState(x);
    sb->masterSH2.SaveState(y);
    if (std::string diff = DiffSH2State(x, y, SH2DiffScope::CpuAndPeripherals, "master SH-2 "); !diff.empty()) {
        return diff;
    }
    sa->slaveSH2.SaveState(x);
    sb->slaveSH2.SaveState(y);
    if (std::string diff = DiffSH2State(x, y, SH2DiffScope::CpuAndPeripherals, "slave SH-2 "); !diff.empty()) {
        return diff;
    }
    if (sa->slaveSH2Enabled != sb->slaveSH2Enabled) {
        return Describe("", "slaveSH2Enabled", sa->slaveSH2Enabled, sb->slaveSH2Enabled);
    }

    const auto compareRam = [](const char *name, const auto &ra, const auto &rb) -> std::string {
        if (std::memcmp(ra.data(), rb.data(), ra.size()) == 0) {
            return {};
        }
        for (size_t i = 0; i < ra.size(); ++i) {
            if (ra[i] != rb[i]) {
                char field[48];
                std::snprintf(field, sizeof(field), "%s[0x%05zX]", name, i);
                return Describe("", field, ra[i], rb[i]);
            }
        }
        return {};
    };
    if (std::string diff = compareRam("WRAMLow", sa->mem.WRAMLow, sb->mem.WRAMLow); !diff.empty()) {
        return diff;
    }
    if (std::string diff = compareRam("WRAMHigh", sa->mem.WRAMHigh, sb->mem.WRAMHigh); !diff.empty()) {
        return diff;
    }

    const unsigned w = a.GetFramebufferWidth();
    const unsigned h = a.GetFramebufferHeight();
    const unsigned pitch = a.GetFramebufferPitch();
    if (w != b.GetFramebufferWidth() || h != b.GetFramebufferHeight() || pitch != b.GetFramebufferPitch()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "frame size differs: a=%ux%u (pitch %u) b=%ux%u (pitch %u)", w, h, pitch,
                      b.GetFramebufferWidth(), b.GetFramebufferHeight(), b.GetFramebufferPitch());
        return buf;
    }
    const auto *fa = static_cast<const uint8_t *>(a.GetFramebuffer());
    const auto *fb = static_cast<const uint8_t *>(b.GetFramebuffer());
    if (fa != nullptr && fb != nullptr) {
        for (unsigned row = 0; row < h; ++row) {
            const uint8_t *ra = fa + static_cast<size_t>(row) * pitch;
            const uint8_t *rb = fb + static_cast<size_t>(row) * pitch;
            if (std::memcmp(ra, rb, static_cast<size_t>(w) * 4) != 0) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "frame row %u differs", row);
                return buf;
            }
        }
    }
    return {};
}

LockstepResult RunLockstep(CoreWrapper &a, CoreWrapper &b, int frames) {
    LockstepResult result;
    for (int frame = 0; frame < frames; ++frame) {
        a.RunFrame();
        b.RunFrame();
        ++result.framesRun;
        result.divergence = CompareCores(a, b);
        if (!result.divergence.empty()) {
            break;
        }
    }
    return result;
}
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build --target brimir_tests` then `build\bin\brimir_tests.exe "[lockstep]"`
Expected: all pass.
- **If the control test fails:** the emulator is not deterministic under these settings, and that must be solved before any JIT conclusion. Find the source (likely a worker thread or a time-based value). If it can be made deterministic through `CoreWrapper` settings, do that in `PrepareLockstepCore` and document the setting in a comment. Otherwise stop and report BLOCKED with the diverging field and frame.
- **If only the JIT test fails:** that is a real JIT divergence. Find the first diverging frame and field, reduce it to an isolated SH-2 reproduction in `test_jit_diff.cpp`, fix the JIT, and keep the reproduction as a regression test.

- [ ] **Step 5: Add `--lockstep` to `brimir_bench`**

In `tools/brimir_bench.cpp`:
- add `#include <brimir/lockstep.hpp>` and the field `int lockstep = -1;` to `Args`
- parse `--lockstep N` with `ParseInt` (reject 0: `Invalid --lockstep value`) next to `--frames`
- after the `--dump-at`/`--dump-state` check in `ParseArgs`, add:

```cpp
    if (args.lockstep >= 0 && args.dumpAt >= 0) {
        std::fprintf(stderr, "--lockstep cannot be combined with --dump-at\n");
        return ParseResult::Error;
    }
```

- add to `PrintUsage` (usage line and option list):

```
        "       brimir_bench --bios <file> [--game <file>] [--system-dir <dir>] [--state <file>]\n"
        "                    --lockstep N\n"
...
        "  --lockstep    run N frames on a JIT core and an interpreter core side by side and require\n"
        "                identical state after every frame (exit 3 on divergence)\n"
```

- extract the content loading at the start of `Run` (from `brimir::CoreWrapper core;` through the save-state load) into:

```cpp
// Initializes `core` and loads BIOS, game and save state per `args`. Returns 0 or exit code 2.
int LoadContent(brimir::CoreWrapper &core, const Args &args, const std::filesystem::path &saveDir,
                const std::filesystem::path &systemDir, bool sh2Jit, bool lockstepCore) {
    core.SetSH2JitEnabled(sh2Jit);
    if (!core.Initialize()) {
        std::fprintf(stderr, "Failed to initialize the core\n");
        return 2;
    }
    if (lockstepCore) {
        brimir::PrepareLockstepCore(core);
    }
    // ... the existing BIOS, game and save-state loading, unchanged ...
    return 0;
}
```

and start `Run` with:

```cpp
    if (args.lockstep > 0) {
        brimir::CoreWrapper jitCore;
        brimir::CoreWrapper refCore;
        if (const int rc = LoadContent(jitCore, args, saveDir, systemDir, true, true); rc != 0) {
            return rc;
        }
        if (const int rc = LoadContent(refCore, args, saveDir, systemDir, false, true); rc != 0) {
            return rc;
        }
        constexpr int kChunk = 600;
        int done = 0;
        while (done < args.lockstep) {
            const int frames = std::min(kChunk, args.lockstep - done);
            const auto result = brimir::RunLockstep(jitCore, refCore, frames);
            done += result.framesRun;
            if (!result.divergence.empty()) {
                std::printf("lockstep divergence at frame %d: %s\n", done - 1, result.divergence.c_str());
                return 3;
            }
            std::printf("lockstep: %d/%d frames identical\n", done, args.lockstep);
            std::fflush(stdout);
        }
        const auto &stats = jitCore.GetSH2JitExecutor(true)->GetStats();
        std::printf("lockstep: OK, %d frames identical (jit master blocksRun %llu, interpreted %llu)\n",
                    args.lockstep, static_cast<unsigned long long>(stats.blocksRun),
                    static_cast<unsigned long long>(stats.interpreted));
        return 0;
    }

    brimir::CoreWrapper core;
    if (const int rc = LoadContent(core, args, saveDir, systemDir, args.sh2Jit, false); rc != 0) {
        return rc;
    }
```

Leave the rest of `Run` (dump/measure) unchanged.

- In `tools/README.md`, add a section `### Lockstep validation` with the command `build\bin\brimir_bench.exe --bios <bios> --game <game> --system-dir <dir> --lockstep 36000`. Explain:
  - it runs a JIT core and an interpreter core side by side and compares them after every frame;
  - what is compared: both SH-2s including timers and DMA, work RAM, and the output frame;
  - threaded VDP rendering is off in this mode;
  - progress is printed every 600 frames;
  - it exits 3 with the first difference on divergence.

  Add exit code 3 to the exit-code list.

- [ ] **Step 6: Verify the tool**

Run: `cmake --build build --target brimir_bench brimir_tests` (expected: no errors).
- `build\bin\brimir_bench.exe --bios tests\fixtures\sega_101.bin --lockstep 1200`: exit 0, prints two progress lines and `lockstep: OK`.
- `build\bin\brimir_bench.exe --bios tests\fixtures\sega_101.bin --lockstep 10 --dump-at 5 --dump-state x`: exit 1.

Run: `build\bin\brimir_tests.exe "[jit]"`, the full `build\bin\brimir_tests.exe`, and `ctest --test-dir build --output-on-failure`. All pass.

- [ ] **Step 7: Update the docs**

In `design/sh2-jit.md`:
- replace section `### 7.2 Shadow-verify mode (debug, real games)` (heading and body) with:

```markdown
### 7.2 Whole-system lockstep (CI and real games)

Because blocks stop at the interpreter's instruction boundaries (section 2), a core running the JIT and a core running the interpreter must stay identical. `brimir::RunLockstep` runs two `CoreWrapper` instances frame by frame and compares, after every frame: both SH-2s (CPU, pipeline, on-chip timers, DMAC, pending interrupt), the slave SH-2 enable flag, low and high work RAM, and the output frame. Threaded VDP rendering is turned off for lockstep runs.

- A control run (two interpreter cores) proves the emulator is deterministic, so a JIT divergence points at the JIT.
- CI: null IPL program (300 frames) and, when a BIOS is present in `tests/fixtures/`, the BIOS (600 frames).
- Real games: `brimir_bench --lockstep N` (section 7.3).

This replaces the shadow-verify mode planned earlier: lockstep checks the whole system, including bus side effects that shadow-verify had to skip.
```

- replace the body of `### 7.3 Game-level regression (local)` with:

```markdown
`brimir_bench --bios <bios> --game <game> --system-dir <dir> --lockstep N` runs N frames of real content on a JIT core and an interpreter core and reports the first divergence (exit code 3). It requires the user's own BIOS and discs, so it does not run in CI. Game validation for milestone 1 is plan 1D.
```

- in section 10, replace the done-criteria bullets that mention running games for 10 minutes and shadow-verify with:

```markdown
- with the JIT on, the BIOS and a set of games run 10 minutes each (36000 frames) in lockstep with the interpreter without divergence
```

- update the `**Status**:` line to `**Status**: Milestone 1 in progress: foundation and instruction-exact execution implemented (plans 1A–1C); instruction coverage and game validation remain (plan 1D)`

In `CHANGELOG.md` under `## [Unreleased]` → `### Added`, add:

```markdown
- **SH-2 JIT lockstep validation** - compiled blocks now stop at exactly the interpreter's instruction boundaries (cycle budget and interrupts checked before every instruction), so a JIT core and an interpreter core stay identical. `brimir_bench --lockstep N` and new lockstep tests compare both cores after every frame (SH-2 state incl. timers and DMA, work RAM, output frame).
```

- [ ] **Step 8: Commit**

```bash
git add include/brimir/lockstep.hpp src/bridge/lockstep.cpp tests/unit/test_jit_lockstep.cpp tests/unit/test_bios_integration.cpp tests/CMakeLists.txt tools/brimir_bench.cpp tools/README.md design/sh2-jit.md CHANGELOG.md
git commit -m "feat(jit): add whole-system lockstep validation against the interpreter"
```

---

### Task 4: Hardening and test hygiene

**Files:**
- Modify: `src/jit/src/executor.cpp`
- Modify: `tests/unit/test_jit_executor.cpp`
- Modify: `tests/unit/test_jit_diff.cpp` (fuzz test)
- Modify: `design/sh2-jit.md` (section 6.5)

**Interfaces:** no API changes.

- [ ] **Step 1: Write the failing test (Store abort site)**

In `tests/unit/test_jit_executor.cpp`, add after `FlushingRead`:

```cpp
void (*g_origWrite)(void *, uint32, uint32, uint32) = nullptr;

void FlushingWrite(void *sh2, uint32 address, uint32 size, uint32 value) {
    g_origWrite(sh2, address, size, value);
    g_exec->Flush();
}
```

Add `constexpr uint16_t kMovL_R2_atR1 = 0x2122; // mov.l R2,@R1` next to the other opcodes, and append:

```cpp
TEST_CASE("Executor: flush during a store aborts the block after the store", "[jit][executor]") {
    auto rig = std::make_unique<Rig>();
    brimir::jit::Executor exec;
    rig->WriteCode(kCode, {kMovL_R2_atR1, kAdd1_R3, kAdd1_R3, kSleep});
    auto state = rig->BaseState(kCode);
    state.R[1] = kData;
    state.R[2] = 0xA5A5A5A5;
    state.R[3] = 0;
    rig->Load(state);

    auto &ctx = rig->sh2->GetJitContext();
    g_exec = &exec;
    g_origWrite = ctx.write;
    ctx.write = FlushingWrite;
    const auto info = exec.Step(ctx);
    ctx.write = g_origWrite;
    g_exec = nullptr;

    CHECK(info.aborted);
    CHECK(rig->Read32(kData) == 0xA5A5A5A5u); // the store itself completed
    CHECK(rig->State().R[3] == 0u);           // nothing after it ran
    CHECK(rig->State().PC == kCode);
    CHECK(exec.Cache().Size() == 0);
}
```

Also add `g_exec = nullptr;` right after `ctx.read = g_origRead;` in the existing flush test, so the global never points at a destroyed executor.

Run: `cmake --build build --target brimir_tests` then `build\bin\brimir_tests.exe "[executor]"`
Expected: the new case passes already (the Store abort check exists). This step adds coverage of an existing path; record the output.

- [ ] **Step 2: Make the in-block flag exception-safe**

In `src/jit/src/executor.cpp`, replace the block-running part of `Step`:

```cpp
    m_inBlock = true;
    m_flushPending = false;
    const ExitInfo info = RunBlock(block, ctx, target, &m_flushPending);
    m_inBlock = false;
    if (m_flushPending) {
        m_flushPending = false;
        m_cache.Flush();
    }
    return info;
```

with:

```cpp
    // Clears m_inBlock and applies a deferred flush on every exit, including an exception thrown
    // by a memory callback, so later flushes are never deferred forever.
    struct BlockScope {
        Executor &self;
        ~BlockScope() {
            self.m_inBlock = false;
            if (self.m_flushPending) {
                self.m_flushPending = false;
                self.m_cache.Flush();
            }
        }
    };
    m_flushPending = false;
    m_inBlock = true;
    const BlockScope scope{*this};
    return RunBlock(block, ctx, target, &m_flushPending);
```

(`block` is not used after `RunBlock` returns, so flushing in the destructor is safe.)

- [ ] **Step 3: Fix the fuzz test's minor issues**

In `tests/unit/test_jit_diff.cpp`, test "JIT matches the interpreter on random programs":
1. Remove `kMinCompiles` and `CHECK(totalCompiles >= kMinCompiles);`. Compile attempts include empty fallback blocks, so the count doesn't show that compiled code ran. Keep `totalCompiles` in the `WARN` output for information.
2. Evaluate register choices into named locals before building each instruction, so the generated program does not depend on argument evaluation order (MSVC, GCC and Clang differ). For example, `case 1: { const uint32_t n = dataReg(); const uint32_t m = dataReg(); instr = MovR(n, m); break; }`. Do the same for every case with two register draws.
3. Hoist the per-program `failed` flag into a test-level `bool diverged = false;` that is set when a program fails. Run the coverage `CHECK`s only `if (!diverged)`, so a real divergence is not buried under follow-on threshold failures.
4. Re-measure the coverage (the generated programs change with item 2), record the new numbers in a comment, and set the thresholds at about 65% of the measured values.

Run: `build\bin\brimir_tests.exe "[fuzz]"` — Expected: passes. Record the measured coverage.

- [ ] **Step 4: Document the remaining abort details**

In `design/sh2-jit.md` section 6.5, append to the "Reset inside an instruction" bullet:

```markdown
 An aborted block returns only the cycles accumulated before the abort; the interpreter would return the whole instruction's cost.
```

and add a bullet:

```markdown
- Dev-log lines that print the current PC (for example on-chip register access traces) show the block's start PC for accesses made by compiled code, because the JIT does not update `PC` inside a block. Emulated state is unaffected.
```

- [ ] **Step 5: Run everything**

Run: `cmake --build build --target brimir_tests brimir_libretro brimir_bench`, then `build\bin\brimir_tests.exe` and `ctest --test-dir build --output-on-failure`.
Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add src/jit/src/executor.cpp tests/unit/test_jit_executor.cpp tests/unit/test_jit_diff.cpp design/sh2-jit.md
git commit -m "test(jit): harden executor flush scope and fuzz determinism"
```

---

## After this plan: plan 1D

1. Front-end coverage for the rest of the milestone-1 subset (all MOV addressing modes, `MOVA`, `MOVT`, `CLRT`/`SETT`, extensions and swaps, logic and `TST`, `SUB`/`NEG`, carry/overflow variants, remaining compares, shifts and rotates, `BSR`/`BRAF`/`BSRF`/`JSR`, `MOV.L @(disp,PC)` in delay slots). Each opcode gets rows in the per-instruction and delay-slot differential tests and joins the fuzz generator.
2. Real-game lockstep validation: BIOS plus the baseline titles, 36000 frames each with `brimir_bench --lockstep` (milestone 1 done criteria), results committed as `design/sh2-validation.md`.
