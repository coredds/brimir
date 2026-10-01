# SH-2 JIT Milestone 2A — Remaining Instructions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Compile every remaining SH-2 instruction except `TRAPA`/`RTE`/`SLEEP`/illegal: multiplies, `MAC.W`/`MAC.L`, `DIV0S`/`DIV0U`/`DIV1`, `TAS`, and the memory forms of `LDC`/`LDS`/`STC`/`STS`. The JIT must stay bit-identical to the interpreter.

**Architecture:** New IR ops for multiplies, partial SR updates and the multi-step operations. Those are implemented through pure helper functions shared with the future x64 backend. There is one fork addition, a context callback for TAS's read-modify-write cycles. Lowering rows come from `design/sh2-jit-handler-table.md` §9. Validation uses the milestone 1D framework: table-driven opcode tests, edge values, fuzz and lockstep.

**Tech Stack:** C++20, CMake 3.28+, Catch2 (amalgamated), MSVC 2022 / GCC 14 / Apple Clang.

**Spec:** `design/sh2-jit-m2.md` §3. **Lowering reference:** `design/sh2-jit-handler-table.md` §9 (exact semantics, cycles, IR rows, helper code, quirks and test notes).

## Global Constraints

- **Fork scope:** exactly `src/core/include/ymir/hw/sh2/*` and `src/core/src/ymir/hw/sh2/*`. Fork edits are marked `// Brimir:` and logged in `src/core/BRIMIR_FORK.md`.
- **JIT off:** emulation is byte-identical to today.
- **JIT on:** identical to the interpreter (state, memory, bus access sequence, cycles, peripherals) at every `Advance()` return. The only accepted deviations are `design/sh2-jit.md` §6.5.
- **Includes:** `brimir-jit` includes only `ymir/core/types.hpp`, `ymir/hw/sh2/sh2_jit_iface.hpp` and `ymir/hw/sh2/sh2_decode.hpp` from the core.
- **Lowering:** every opcode is lowered exactly as its §9 row. If a row is wrong, the interpreter in `sh2.cpp` is the reference: fix the lowering and the row in the same commit.
- **Tests:** Catch2 in `tests/unit/`, JIT tests tagged `[jit]`. Never weaken an assertion to make a test pass.
- **Workflow:** work on branch `feature/sh2-jit-2a`. Push the branch to get Windows/Linux/macOS CI; commit messages use `type(scope): subject`.

## Build and test commands (Windows)

`pwsh -NoProfile -File $env:TEMP\opencode\msvc.ps1 -Cmd "<command>"` loads MSVC. Commands:

```powershell
cmake --build build --target brimir_tests brimir_libretro brimir_bench
build\bin\brimir_tests.exe "[jit]"
build\bin\brimir_tests.exe
ctest --test-dir build --output-on-failure
```

---

### Task 1: IR ops, helpers and the RMW-cycles callback

**Files:**
- Create: `src/jit/include/brimir/jit/sh2_helpers.hpp`, `src/jit/src/sh2_helpers.cpp`
- Modify: `src/jit/CMakeLists.txt`, `src/jit/include/brimir/jit/ir.hpp`, `src/jit/src/ir.cpp`, `src/jit/src/interp_backend.cpp`
- Modify (fork): `src/core/include/ymir/hw/sh2/sh2_jit_iface.hpp`, `src/core/include/ymir/hw/sh2/sh2.hpp`, `src/core/src/ymir/hw/sh2/sh2.cpp`, `src/core/BRIMIR_FORK.md`
- Test: `tests/unit/test_jit_helpers.cpp` (new; register it in `tests/CMakeLists.txt`), `tests/unit/test_jit_ir.cpp`, `tests/unit/test_jit_backend.cpp`, `tests/unit/test_sh2_jit_iface.cpp`

**Interfaces:**
- **Produces (`brimir/jit/sh2_helpers.hpp`, namespace `brimir::jit`):**

```cpp
#pragma once
// Pure transcriptions of SH-2 interpreter handlers (src/core/src/ymir/hw/sh2/sh2.cpp) used by
// every JIT backend. Reference: design/sh2-jit-handler-table.md section 9.
#include <cstdint>

namespace brimir::jit {

// DIV1 step. rmIsRn: the instruction has n == m, so Rm is read after Rn was shifted.
// Returns the new Rn; updates only Q (bit 8) and T (bit 0) of sr.
uint32_t Div1Step(uint32_t rn, uint32_t rm, bool rmIsRn, uint32_t &sr);
// MAC.W accumulate. op1 = sext16(@Rm), op2 = sext16(@Rn). Returns the new MAC (MACH:MACL).
uint64_t MacWStep(uint64_t mac, bool s, int32_t op1, int32_t op2);
// MAC.L accumulate. op1 = @Rm, op2 = @Rn. Returns the new MAC (MACH:MACL).
uint64_t MacLStep(uint64_t mac, bool s, int32_t op1, int32_t op2);

} // namespace brimir::jit
```

  The implementations are the exact transcriptions in handler table §9.4 (`Div1Step`) and §9.5 (`MacWStep`, `MacLStep`). For `MacLStep`, follow the handler at `sh2.cpp:4158-4190` literally: the saturation test is done on the 64-bit sum as unsigned, and the direction comes from `int32_t(op1 ^ op2) < 0`.
- **Produces (IR):** new `Op` enumerators inserted before `Load`, in this order: `Mul, MulHiS, MulHiU, SetSRBits, Div1, MacW, MacL, AddAccessCyclesRMWByte`. Builder methods:
  - `ValueId Mul(ValueId a, ValueId b)`, `ValueId MulHiS(ValueId, ValueId)`, `ValueId MulHiU(ValueId, ValueId)`
  - `void SetSRBits(ValueId value, uint32_t mask)` (mask in `imm`; the verifier rejects `mask & ~0x303u`)
  - `ValueId Div1(ValueId rn, ValueId rm, bool rmIsRn)` (flag)
  - `void MacW(ValueId op1, ValueId op2)`, `void MacL(ValueId op1, ValueId op2)`
  - `void AddAccessCyclesRMWByte(ValueId address)`

  `kOpInfo` rows:
  - `Mul`/`MulHiS`/`MulHiU`/`Div1`: dst, 2 srcs
  - `SetSRBits`: no dst, 1 src
  - `MacW`/`MacL`: no dst, 2 srcs
  - `AddAccessCyclesRMWByte`: no dst, 1 src
  - none `usesSize`, none `isExit`

  Backend semantics are as in handler table §9.1. `MacW`/`MacL` assemble `mac = (uint64_t(*ctx.MACH) << 32) | *ctx.MACL`, read `s = (*ctx.SR >> 1) & 1`, and write both halves back.
- **Produces (fork):** `uint64 (*accessCyclesRMWByte)(void *sh2, uint32 address) = nullptr;` in `SH2JitContext` after `setSR`, wired to `SH2::JitAccessCyclesRMWByte`, which returns `AccessCyclesRMWByte<false>(address)`.

- [ ] **Step 1: Write the failing tests**

`tests/unit/test_jit_helpers.cpp` checks each helper against the interpreter. Build a `sh2test::Rig` per case and run the real instruction with the interpreter (`sh2->Step<false,false>()`). Compare its resulting Rn/SR (or MAC) with the helper's output for the same inputs.
- **DIV1:** all 8 (oldQ, M, T) combinations × operand values {0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678} for Rn and Rm, plus n == m (encoding `0x3n n4`, e.g. `0x3554` for `div1 R5,R5`).
- **MAC.W:** S ∈ {0, 1}, MAC values {0, 0x7FFFFFFF, 0x80000000, 0x00000001_7FFFFFFF, 0xFFFFFFFF_80000000, MACH bit 0 set}, operands {0x8000, 0x7FFF, 0xFFFF, 1}.
- **MAC.L:** S ∈ {0, 1}, MAC at ±2^47 boundaries (`0x00007FFFFFFFFFFF`, that +1, `0xFFFF800000000000`, that −1), operands {0x80000000, 0x7FFFFFFF, 0xFFFFFFFF, 0, 1}, and the "zero product with one negative operand while MAC is out of range" case.

The interpreter runs use operands placed in rig RAM at two addresses held in Rm/Rn, with `mac.w @Rm+,@Rn+` = `0x4nmF` and `mac.l` = `0x0nmF`.

In `test_jit_ir.cpp`, add `OpName` checks for the eight new ops, and check that the verifier rejects `SetSRBits` with mask `0x10` (an ILevel bit).

In `test_jit_backend.cpp`, using the `Fixture`:
- `Mul`/`MulHiS`/`MulHiU` on (0x80000000, 0x80000000), (0xFFFFFFFF, 0xFFFFFFFF) and (0x12345678, 0x9ABCDEF0), with the expected values written out;
- `SetSRBits(C(0xFFFFFFFF), 0x301)` changes only T/Q/M;
- `Div1`, `MacW` and `MacL` each produce the helper's result through the context;
- `AddAccessCyclesRMWByte` adds `ctx.accessCyclesRMWByte(...)` for a cache-through RAM address and an MMIO address.

In `test_sh2_jit_iface.cpp`, check `accessCyclesRMWByte` for partitions 000 (bus byte-read cycles − 1, not 1), 001, 101 and 111 (= 8).

- [ ] **Step 2:** Build. Expected: compile errors (missing ops, helpers and the context field).
- [ ] **Step 3:** Implement the helpers, ops, verifier rule, backend cases and fork callback. Log the fork change in `BRIMIR_FORK.md` (`accessCyclesRMWByte callback for TAS`).
- [ ] **Step 4:** Run `"[jit]"`, the full suite and ctest. Expected: all pass.
- [ ] **Step 5: Commit** — `feat(jit): add multiply, DIV1, MAC and RMW-cycle IR ops with shared helpers`

---

### Task 2: Multiply and divide instructions

**Files:** `src/jit/src/frontend.cpp`, `tests/unit/jit_opcode_specs.cpp`, `tests/unit/test_jit_opcodes.cpp`, `tests/unit/test_jit_diff.cpp`

**Opcodes** (handler table §9.2–9.4): MUL (`OpcodeType::MUL` → handler `MULL`), MULS, MULU, DMULS, DMULU, DIV0S, DIV0U, DIV1. All are slot-capable and all have `Delay_` mappings. Encodings:

| Opcode | Base encoding |
|---|---|
| MUL | `0x0007` |
| MULS | `0x200F` |
| MULU | `0x200E` |
| DMULS | `0x300D` |
| DMULU | `0x3005` |
| DIV0S | `0x2007` |
| DIV0U | `0x0019` |
| DIV1 | `0x3004` |

- [ ] **Step 1:** Add the specs (`Fmt::NM`/`Fmt::Z`, `Addr::None`) to `CompiledOpcodes()`. Run `"[opcodes]"`. Expected: FAIL for the new opcodes, because no blocks are compiled.
- [ ] **Step 2:** Add these ops to the edge-value test's opcode set. MULS/MULU need dirty upper halves (for example `0xABCD8000`), and DIV1 needs all Q/M/T combinations. If the current edge test doesn't randomize SR.Q/M, extend it so SR ∈ {T, Q, M combinations}.
- [ ] **Step 3: Division sequences.** Add a `[jit][diff][exact]` test that runs full division sequences in one block pair and compares JIT against interpreter through `Advance` with `DiffRigs(..., true)`:
  - unsigned: `div0u` then 32× (`rotcl R0`; `div1 R1,R2`), dividend in R0, divisor in R1;
  - signed: `div0s R1,R2` then 32× (`rotcl R0`; `div1 R1,R2`);
  - inputs from {0, 1, 7, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678} for dividend, high part and divisor.
  
  Blocks cap at 32 instructions, so the sequence spans several blocks. That is intended.
- [ ] **Step 4:** Implement the lowering rows from §9.2–9.4 in `LowerPlain`/`BaseOp`. Run `"[opcodes]"`, `"[exact]"` and `"[jit]"` until all pass.
- [ ] **Step 5:** Run the full suite and ctest, and `build\bin\brimir_bench.exe --bios tests\fixtures\sega_101.bin --lockstep 1200` (exit 0).
- [ ] **Step 6: Commit** — `feat(jit): compile multiply and divide-step instructions`

---

### Task 3: MAC.W, MAC.L and TAS

**Files:** `src/jit/src/frontend.cpp`, `tests/unit/jit_opcode_specs.hpp`, `tests/unit/jit_opcode_specs.cpp`, `tests/unit/test_jit_opcodes.cpp`, `tests/unit/test_jit_diff.cpp`

**Opcodes** (§9.5, §9.6): MACW `0x400F`, MACL `0x000F` (`Fmt::NM`), TAS `0x401B` (`Fmt::N`, `Addr::Rn`, size 1). All are slot-capable.

- [ ] **Step 1:** Add `Addr::MacPair` to `jitspec::Addr`. `Encode` points Rm and Rn at two independently chosen aligned data addresses of the spec's size (2 for MAC.W, 4 for MAC.L). When n == m, one register points at an address with room for both reads. Add the three specs and run `"[opcodes]"`. Expected: FAIL for the new opcodes.
- [ ] **Step 2:** Add a `[jit][diff][exact]` test `"MAC saturation edges match the interpreter"`. For S ∈ {0, 1}, preload MAC (via the rig state's MACH/MACL) and memory operands from the §9.x notes 2–3 lists, run one `mac.w` or `mac.l` through `Advance` on both rigs, and compare with `DiffRigs(..., true)` and cycles. Include n == m and overlapping addresses.
- [ ] **Step 3:** Add a test `"TAS matches the interpreter for every partition"`: byte values {0x00, 0x80, 0x7F}, with the address in cached RAM (`0x06...`), cache-through RAM (`0x26...`) and MMIO (`0x22...`). Compare with `DiffRigs` and cycles, and assert the stored byte and T on the reference rig (0 → T=1 and byte 0x80; 0x80 → T=0 and byte 0x80; 0x7F → T=0 and byte 0xFF).
- [ ] **Step 4:** Implement the lowering rows (§9.5 MAC.W/MAC.L, using the compile-time n==m address; §9.6 TAS). Run `"[opcodes]"`, `"[exact]"` and `"[jit]"` until all pass.
- [ ] **Step 5:** Run the full suite, ctest, and the BIOS lockstep for 1200 frames.
- [ ] **Step 6: Commit** — `feat(jit): compile MAC.W, MAC.L and TAS`

---

### Task 4: Memory forms of LDC/LDS/STC/STS

**Files:** `src/jit/src/frontend.cpp`, `tests/unit/jit_opcode_specs.cpp`, `tests/unit/test_jit_diff.cpp`

**Opcodes** (§9.7, §9.8):
- LDC_GBR_M `0x4017`, LDC_SR_M `0x4007`, LDC_VBR_M `0x4027`, LDS_MACH_M `0x4006`, LDS_MACL_M `0x4016`, LDS_PR_M `0x4026`: `Fmt::M`, `Addr::RmPostInc`, size 4.
- STC_GBR_M `0x4013`, STC_SR_M `0x4003`, STC_VBR_M `0x4023`, STS_MACH_M `0x4002`, STS_MACL_M `0x4012`, STS_PR_M `0x4022`: `Fmt::N`, `Addr::RnPreDec`, size 4.

All are slot-capable, all clear interrupt-allow (add them to `ClearsIntrAllow`), and LDC.L SR uses `SetSR(value, delaySlot)`.

- [ ] **Step 1:** Add the specs and run `"[opcodes]"`. Expected: FAIL. The bus-wait test must also show that these 32-bit accesses never exit on a bus wait. They have no bus-wait check, so the existing bus-wait test runs them against MMIO and compares; confirm it covers them.
- [ ] **Step 2:** Add `[jit][diff][exact]` tests:
  - `"LDC.L SR unmasking a pending interrupt"`: like the existing LDC SR test, with `ldc.l @R5+,SR` loading 0 from RAM. Exactly one following instruction runs before the interrupt, and the stacked PC is checked.
  - The same instruction in a delay slot (`bra target ; ldc.l @R5+,SR`): interrupt after the first instruction at the target.
  - `"LDS.L PR write-back stall"`: `lds.l @R1+,PR ; rts ; nop`. The stall cycles are compared via `Advance`, and `m_wbReg` = PR after the first instruction.
- [ ] **Step 3:** Implement the rows. Run `"[opcodes]"`, `"[exact]"` and `"[jit]"` until all pass.
- [ ] **Step 4:** Run the full suite, ctest, and the BIOS lockstep for 1200 frames.
- [ ] **Step 5: Commit** — `feat(jit): compile memory forms of LDC/LDS/STC/STS`

---

### Task 5: Fuzz coverage, game lockstep and docs

**Files:** `tests/unit/test_jit_diff.cpp` (fuzz), `design/sh2-validation.md`, `design/sh2-jit.md`, `README.md`, `CHANGELOG.md`, `src/libretro/options.cpp`

- [ ] **Step 1: Fuzz.** The fuzz generator draws from `CompiledOpcodes()`, so it picks up the new specs automatically. Make the needed adjustments:
  - LDC.L GBR/VBR/SR and LDS.L PR change state that the fuzz relies on (GBR invariant, PR absolute mode). Restrict them the same way the register forms are restricted today (read the existing rules in the generator), and keep those rules documented in comments.
  - Re-measure coverage and set the thresholds at about 65% of the measured values. Expected: passes, runtime still under 5 s in Release.
- [ ] **Step 2: Game lockstep.** Use the Release + LTO build `build-bench` (`cmake --build build-bench --target brimir_bench`), the BIOS and title list from `design/plans/2026-10-01-sh2-jit-m1d-coverage-validation.md` Task 8, and a scratch system dir as there. Run lockstep for 36,000 frames on:
  - **Sega Rally Championship** and **Burning Rangers**, which interpret the most today;
  - **Virtua Fighter 2** and **Panzer Dragoon II Zwei**;
  - **Guardian Heroes**, **Street Fighter Zero 3** and the BIOS menu, for 1,800 frames each.
  
  Run one title per command, in the background (`Start-Process` with redirected output under `.superpowers\sdd\`), polling until it exits. On divergence (exit 3), follow 1D Task 8's debugging procedure: reduce it to an isolated regression test, fix it, and restart that title.
- [ ] **Step 3: Smoke runs.** Run the default-settings smoke runs (`--sh2-jit --warmup 600 --frames 3600` and the interpreter equivalent) on the six titles. Record the executor stats. The `interpreted` counts should drop sharply; only `TRAPA`/`RTE`/`SLEEP` remain.
- [ ] **Step 4: Docs.**
  - `design/sh2-validation.md`: add a "Milestone 2A" section with the lockstep results, the new smoke-run stats, and the updated list of interpreted instructions (`TRAPA`, `RTE`, `SLEEP`, illegal).
  - `design/sh2-jit.md`: status line becomes `Milestone 2 in progress: full instruction coverage done (plan 2A); x64 backend next (plan 2B)`.
  - `README.md`: SH-2 JIT bullet with the updated coverage.
  - `src/libretro/options.cpp`: the option description says "compiles most SH-2 instructions" → "compiles all SH-2 instructions except exception entry/return and SLEEP".
  - `CHANGELOG.md`: `[Unreleased]` entry.
- [ ] **Step 5:** Delete the scratch dir. Run the full suite and ctest.
- [ ] **Step 6: Commit** — `docs(design): record milestone 2A coverage validation`
