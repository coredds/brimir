# SH-2 JIT Milestone 1D — Full-State Lockstep, Instruction Coverage and Game Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish milestone 1. Lockstep compares the whole emulated system, CI exercises interrupts through a synthetic workload, the JIT compiles every non-exception SH-2 instruction except multiply/divide-step/MAC, the memory forms of LDC/LDS/STC/STS and TAS, and the BIOS plus six games run 36,000 frames each in lockstep with the interpreter.

**Architecture:**
- Lockstep gains a full `Saturn::SaveState` comparison (per subsystem) and an audio comparison.
- The IR gains logic, shift, compare and system-register ops.
- The fork's JIT context gains MAC pointers and an `setSR` callback.
- The front end lowers each new opcode exactly as given by its row in `design/sh2-jit-handler-table.md`.
- A table-driven differential test covers every opcode, normally and in delay slots.

**Tech Stack:** C++20, CMake 3.28+, Catch2 (amalgamated), MSVC 2022 / GCC 14 / Apple Clang.

**Spec:** `design/sh2-jit.md`. **Lowering reference:** `design/sh2-jit-handler-table.md` (exact interpreter semantics, cycles, write-back stalls, bus-wait behavior and IR rows per opcode, with `sh2.cpp` line numbers). Prerequisite: plans 1A–1C (merged).

## Global Constraints

- **Fork scope:** exactly `src/core/include/ymir/hw/sh2/*` and `src/core/src/ymir/hw/sh2/*`. Mark fork edits with `// Brimir:` and log them in `src/core/BRIMIR_FORK.md`.
- **JIT off:** emulation is byte-identical to today.
- **JIT on:** the emulated system must be identical to the interpreter at every `Advance()` return (architectural state, memory, bus access sequence, cycles, peripherals). The only exceptions are the deviations in `design/sh2-jit.md` section 6.5.
- **Includes:** `brimir-jit` includes only `ymir/core/types.hpp`, `ymir/hw/sh2/sh2_jit_iface.hpp` and `ymir/hw/sh2/sh2_decode.hpp` from the core.
- **Exact lowering:** every opcode is lowered exactly as its IR row in `design/sh2-jit-handler-table.md`. If a row turns out to be wrong, the interpreter handler in `sh2.cpp` is the reference: fix the lowering and correct the table row in the same commit.
- **Tests:** Catch2 in `tests/unit/`, tagged `[jit]` for JIT tests. BIOS/content-dependent tests `SKIP` when the content is missing.
- **Commits:** `type(scope): subject`.

## Scope decisions

- **Not compiled in milestone 1** (they keep using the interpreter fallback):
  - MUL, MULS, MULU, DMULS, DMULU, MAC.W, MAC.L, DIV0S, DIV0U, DIV1 (multiply/divide step; milestone 2)
  - TAS
  - TRAPA, RTE, SLEEP
  - the memory forms LDC.L, LDS.L, STC.L, STS.L
  - Illegal opcodes
- **Interrupt-allow handling:** after an instruction that clears `intrAllow`, the next instruction's `CheckBoundary` runs as usual, followed by `SetIntrAllow`. This reproduces `InterpretNext` exactly (handler table section 6).

## Build and test commands (Windows)

From a *Developer PowerShell for VS 2022* (or `pwsh -NoProfile -File $env:TEMP\opencode\msvc.ps1 -Cmd "<command>"`):

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBRIMIR_BUILD_TESTS=ON
cmake --build build --target brimir_tests brimir_libretro brimir_bench
ctest --test-dir build --output-on-failure
build\bin\brimir_tests.exe "[jit]"
```

---

### Task 1: Full-state and audio lockstep comparison

**Files:**
- Modify: `include/brimir/lockstep.hpp`, `src/bridge/lockstep.cpp`
- Modify: `tests/unit/test_jit_lockstep.cpp`
- Modify: `design/sh2-jit.md` (section 7.2)

**Interfaces:**
- Consumes: `CompareCores`, `RunLockstep` (plan 1C); `ymir::Saturn::SaveState(savestate::SaveState&) const` (`saturn.hpp:257`); `CoreWrapper::GetAudioSamples(int16_t*, size_t)`.
- Produces:
  - `CompareCores(a, b)` additionally compares the full save state, per subsystem.
  - `RunLockstep` additionally drains and compares both cores' audio every frame.

**Rules:**
- **Allocation.** `ymir::savestate::SaveState` is several MB, so allocate it on the heap (`std::make_unique`).
- **Padding.** Before calling `Saturn::SaveState`, `memset` each struct to 0.
- **Comparison order.** Compare these sub-structs with `memcmp` (sizes via `sizeof`) in this order: `scheduler`, `system`, `scu`, `smpc`, `vdp`, `scsp`, `cdblockLLE`, then:
  - if `cdblockLLE` is false: `cdblock`;
  - else: `sh1`, `ygr`, `cddrive`, `cdblockDRAM`.
  
  After those come `msh2SpilloverCycles`, `ssh2SpilloverCycles`, `sh1SpilloverCycles` and `sh1FracCycles`. `msh2`/`ssh2` are already compared field by field and `discHash` is identical by construction, so skip all three.
- **Message.** Report the first mismatch as `"<subsystem> state differs (byte offset N)"`.
- **False positives from padding.** The interpreter-vs-interpreter control runs must stay clean. If a sub-struct differs in a control run only because of padding (live objects copied whole into the state carry indeterminate padding), compare that sub-struct field by field for the fields that hold data and document which one in a comment. Never drop a sub-struct entirely.
- **Audio.** Each frame, drain both cores with `GetAudioSamples(buf, 4096)` (stereo `int16_t[4096 * 2]`). Compare the sample counts, then the samples. Report `"audio differs at sample N"` or `"audio sample count differs: a=.. b=.."`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_jit_lockstep.cpp`:

```cpp
TEST_CASE("CompareCores detects differences outside the SH-2s and WRAM", "[lockstep]") {
    auto a = MakeLockstepCore(false);
    auto b = MakeLockstepCore(false);
    a->RunFrame();
    b->RunFrame();
    REQUIRE(brimir::CompareCores(*a, *b).empty());

    // VDP2 VRAM is not visible to the SH-2 diff or WRAM comparison.
    b->GetSaturn()->VDP.GetProbe().VDP2WriteVRAM<uint8>(0x100, 0x5A);
    const std::string diff = brimir::CompareCores(*a, *b);
    INFO(diff);
    REQUIRE(diff.find("vdp state differs") != std::string::npos);
}
```

The implementer must check the VDP probe API (`src/core/include/ymir/hw/vdp/vdp.hpp`, class `VDP::Probe`) and use whichever accessor writes one byte of VDP2 VRAM, or another VDP-only field reachable from tests. The assertion stays the same: a change visible only in the VDP state must be reported as `vdp state differs`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target brimir_tests` then `build\bin\brimir_tests.exe "CompareCores detects differences outside the SH-2s and WRAM"`
Expected: FAIL. `CompareCores` returns `""` (or a frame-row diff) instead of `vdp state differs`.

- [ ] **Step 3: Implement**

In `src/bridge/lockstep.cpp`, add `#include <ymir/savestate/savestate.hpp>` and `#include <memory>`. Add a static helper `std::string DiffSaturnState(const ymir::Saturn &sa, const ymir::Saturn &sb)` that implements the rules above. Call it from `CompareCores` after the WRAM comparison and before the frame comparison.

In `RunLockstep`, after both `RunFrame` calls and before `CompareCores`, add the audio comparison from the rules above. Keep the two sample buffers as `static thread_local` (`std::vector<int16_t>(4096 * 2)`) so nothing is allocated per frame.

Update the header comment of `CompareCores` in `include/brimir/lockstep.hpp` to list everything it compares, and the comment of `RunLockstep` to mention audio.

- [ ] **Step 4: Run all lockstep tests (control runs must stay clean)**

Run: `build\bin\brimir_tests.exe "[lockstep]"`, then `"[bios]"`.
Expected: all pass. If a control run (interpreter vs interpreter) now fails:
- **Padding:** if it is padding in a sub-struct, apply the padding rule above.
- **Real non-determinism:** otherwise it is a real source of non-determinism, as the RTC was in plan 1C. Investigate it. If a `CoreWrapper`/configuration setting removes it, set that in `PrepareLockstepCore` with a comment. Otherwise report BLOCKED with the subsystem and offset.

Then run the full suite and `ctest --test-dir build --output-on-failure`.

- [ ] **Step 5: Verify on real content**

Run: `build\bin\brimir_bench.exe --bios tests\fixtures\sega_101.bin --lockstep 1200`
Expected: exit 0.

- [ ] **Step 6: Update the spec and commit**

In `design/sh2-jit.md` section 7.2, replace the list of compared state with: both SH-2s (field level, including timers, DMAC, cache arrays and the pending interrupt), work RAM, the full save state of every other subsystem (scheduler, SCU, SMPC, VDP, SCSP, CD block, spillover counters; compared per subsystem), audio samples and the output frame.

```bash
git add include/brimir/lockstep.hpp src/bridge/lockstep.cpp tests/unit/test_jit_lockstep.cpp design/sh2-jit.md
git commit -m "feat(jit): compare full system state and audio in lockstep"
```

---

### Task 2: Synthetic interrupt-driven CI lockstep workload

**Files:**
- Modify: `tests/unit/test_jit_lockstep.cpp`
- Modify: `design/sh2-jit.md` (section 7.2, CI bullet)

**Interfaces:**
- Consumes: `CompareCores`/`RunLockstep` (Task 1); `ymir::sh2::SH2::GetProbe()` (`PC()`, `SR()`, `VBR()`, `R()`), `SH2::GetJitContext().write` (on-chip register writes, as in `test_jit_diff.cpp` "Interrupts raised inside a block are taken at the same instruction"); `Saturn::mem.WRAMHigh` (SH-2 address `0x06000000`, mirrored every 1 MiB up to `0x07FFFFFF`; cache-through alias `0x26000000`).

**Workload** (installed identically on both cores right after `Initialize` + `PrepareLockstepCore`, with the null IPL):

1. **Main loop** at `0x06004000`, using only opcodes the JIT compiles after plan 1C:
   - `mov.l @(disp,PC)` loads two pointers into R8 and R9 from a literal pool: a source table at `0x26010000` and a destination area at `0x26020000`.
   - Then, in a loop: `mov.l @R8,R1 ; add R1,R2 ; mov.l R2,@R9 ; add #4,R8 ; add #4,R9 ; dt R3 ; bf loop`.
   - When R3 reaches 0: reload R3 from the literal pool (64) and R8/R9 from the pool, then `bra` back to the loop.
2. **Interrupt handler** at `0x06005000`: `add #1,R10`, clear the FRT compare-match A flag (read FTCSR, write 0 to the flag bit), then `rte ; nop`. `rte` is interpreted.
3. **On-chip FRT setup** through the JIT context `write` callback on the master SH-2 (byte writes):
   - TIER: OCIAE = 1
   - OCRA: a compare value that gives an interrupt every few thousand cycles
   - FTCSR: CCLRA = 1, so the counter clears on compare match A
   - TCR: internal clock / 8
   - IPRB: FRT level 10
   - VCRC: FRT OCI vector `0x60`
   
   Verify every register address, bit position and the OCRA/OCRB selection (TOCR) against `OnChipRegWrite*` and the FRT code in `src/core/src/ymir/hw/sh2/sh2.cpp` and `sh2_frt.hpp`. The table in `design/sh2-jit-handler-table.md` does not cover these.
4. **Vector table:** VBR = `0x06008000`, and the longword at `VBR + 0x60 * 4` = `0x06005000`.
5. **Master CPU state:** PC = `0x06004000`, SR = `0x00` (mask 0), R2 = R3 = R10 = 0, a valid stack R15 = `0x0600F000`. Refill the master's fetch buffer to match memory at the new PC: write the state with `SaveState` → modify → `LoadState` on `masterSH2`, setting `fetchedOpcodes` to the two halfwords at PC.

Encode the program with the encoder helpers at the top of `tests/unit/test_jit_diff.cpp`. Copy the ones you need into an anonymous namespace in `test_jit_lockstep.cpp`; do not include the other test file. Check every displacement by hand in a comment.

- [ ] **Step 1: Write the test**

Append to `tests/unit/test_jit_lockstep.cpp` a helper `void InstallFrtWorkload(CoreWrapper &core)` that implements the workload above, and:

```cpp
TEST_CASE("Lockstep: interrupt-driven synthetic workload, JIT vs interpreter", "[lockstep][jit]") {
    auto jit = MakeLockstepCore(true);
    auto ref = MakeLockstepCore(false);
    InstallFrtWorkload(*jit);
    InstallFrtWorkload(*ref);

    const auto result = brimir::RunLockstep(*jit, *ref, 240);
    INFO("frame " << result.framesRun << ": " << result.divergence);
    REQUIRE(result.divergence.empty());

    // The workload really ran: interrupts were taken and compiled code ran.
    auto &probe = ref->GetSaturn()->masterSH2.GetProbe();
    CHECK(probe.R(10) > 100u);                 // FRT interrupts serviced
    CHECK(ref->GetSaturn()->mem.WRAMHigh[0x20003] != 0); // destination area written
    const auto &stats = jit->GetSH2JitExecutor(true)->GetStats();
    CHECK(stats.blocksRun > 10000u);
}

TEST_CASE("Lockstep control: synthetic workload on two interpreter cores", "[lockstep]") {
    auto a = MakeLockstepCore(false);
    auto b = MakeLockstepCore(false);
    InstallFrtWorkload(*a);
    InstallFrtWorkload(*b);
    const auto result = brimir::RunLockstep(*a, *b, 240);
    INFO("frame " << result.framesRun << ": " << result.divergence);
    REQUIRE(result.divergence.empty());
}
```

Before relying on them, adjust the progress thresholds (`> 100` interrupts, `> 10000` blocks) to about 50% of the values measured in a passing run, and write the measured values in a comment.

- [ ] **Step 2: Run and make the workload work**

Run: `cmake --build build --target brimir_tests` then `build\bin\brimir_tests.exe "[lockstep]"`

Expected: both new cases pass.
- **If the progress checks fail** (no interrupts, no RAM writes), the workload setup is wrong: wrong register addresses, a refill missing, or a wrong displacement. Debug it on the interpreter core (`probe.PC()`, `R(10)`) and fix the setup.
- **If the JIT case diverges but the control passes,** that is a real JIT bug. Reduce it to an isolated reproduction in `test_jit_diff.cpp`, fix it with interpreter evidence, and keep the regression test.

- [ ] **Step 3: Prove the test catches a JIT timing bug**

Temporarily change `CheckBoundary` in `src/jit/src/interp_backend.cpp` to ignore pending interrupts (budget check only). Rebuild and confirm the synthetic JIT lockstep test FAILS. Restore the code exactly (`git diff` on that file must be empty) and confirm the test passes again. Record both outputs in the report.

- [ ] **Step 4: Update the spec and commit**

In `design/sh2-jit.md` section 7.2, replace the CI bullet with: CI runs the null-IPL smoke test plus a synthetic interrupt-driven workload (master SH-2 loop with WRAM traffic and FRT compare-match interrupts, 240 frames), each with an interpreter-vs-interpreter control. The slave SH-2 and the other subsystems' interrupts are covered only by BIOS and game runs.

```bash
git add tests/unit/test_jit_lockstep.cpp design/sh2-jit.md
git commit -m "test(jit): add interrupt-driven synthetic lockstep workload"
```

---

### Task 3: IR ops, backend and fork context for the remaining instructions

**Files:**
- Modify (fork): `src/core/include/ymir/hw/sh2/sh2_jit_iface.hpp`, `src/core/include/ymir/hw/sh2/sh2.hpp`, `src/core/src/ymir/hw/sh2/sh2.cpp`, `src/core/BRIMIR_FORK.md`
- Modify: `src/jit/include/brimir/jit/ir.hpp`, `src/jit/src/ir.cpp`, `src/jit/src/interp_backend.cpp`
- Modify: `tests/unit/test_jit_ir.cpp`, `tests/unit/test_jit_backend.cpp`, `tests/unit/test_sh2_jit_iface.cpp`

**Interfaces:**
- **Produces (fork):** in `SH2JitContext`, add `uint32 *MACL = nullptr; uint32 *MACH = nullptr;` after `SR`, and the callback `void (*setSR)(void *sh2, uint32 value, bool delaySlot) = nullptr;` after `endDelaySlot`. Wire them in `SH2::InitJitContext` as follows:
  - `MACL = &MAC.L`, `MACH = &MAC.H`
  - `setSR = &SH2::JitSetSR`. `JitSetSR` performs exactly the state change in `LDCSR` (`sh2.cpp`, handler `SH2::LDCSR`):

```cpp
void SH2::JitSetSR(void *ctx, uint32 value, bool delaySlot) {
    auto &sh2 = *static_cast<SH2 *>(ctx);
    sh2.SR.u32 = value & 0x000003F3;
    sh2.m_intrFlags = std::bit_cast<IntrFlags>(std::bit_cast<uint16>(IntrFlags{
        .pending = !delaySlot && sh2.INTC.pending.level > sh2.SR.ILevel,
        .allow = false,
    }));
}
```

  Declare `static void JitSetSR(void *ctx, uint32 value, bool delaySlot);` next to the other `Jit*` statics.
- **Fork comment fix (same commit):** in `SH2::Advance`, the comment above the JIT hook still says the executor works "at block granularity". Change it to: `// Brimir: SH-2 JIT hook. Only the plain configuration (no debug tracing, no cache emulation) is compiled; the executor stops at the same instruction boundaries as the loop below.`
- **Produces (IR), new `Op` enumerators** inserted before `Load` in this order: `And, Or, Xor, Not, Shl, Shr, Sar, CmpGtU, CmpGeU, CmpGtS, CmpGeS, GetGBR, SetGBR, GetVBR, SetVBR, SetPR, GetSR, SetSR, GetMACH, GetMACL, SetMACH, SetMACL, ClearIntrAllow, SetIntrAllow, GetDelayTarget`. Matching `Builder` methods:
  - `ValueId And(ValueId, ValueId)`, `Or`, `Xor`
  - `ValueId Not(ValueId)`
  - `ValueId Shl(ValueId, uint32_t amount)`, `Shr`, `Sar` (amount 1..31, stored in `imm`)
  - `ValueId CmpGtU(ValueId, ValueId)`, `CmpGeU`, `CmpGtS`, `CmpGeS`
  - `ValueId GetGBR()`, `void SetGBR(ValueId)`, `ValueId GetVBR()`, `void SetVBR(ValueId)`, `void SetPR(ValueId)`
  - `ValueId GetSR()`, `void SetSR(ValueId, bool delaySlot)` (flag = delaySlot)
  - `ValueId GetMACH()`, `ValueId GetMACL()`, `void SetMACH(ValueId)`, `void SetMACL(ValueId)`
  - `void ClearIntrAllow()`, `void SetIntrAllow()`, `ValueId GetDelayTarget()`
- **Semantics:** exactly the table in `design/sh2-jit-handler-table.md` section 1. Backend: `GetMACH`/`SetMACH` use `*ctx.MACH`, `SetSR` calls `ctx.setSR(ctx.sh2, v[a], in.flag)`, `GetDelayTarget` reads `*ctx.delaySlotTarget`.
- **Verifier:** `Shl`/`Shr`/`Sar` with `imm` outside 1..31 are invalid (`"shift amount out of range"`).

- [ ] **Step 1: Write the failing tests**

In `tests/unit/test_jit_ir.cpp`, add a test that checks `OpName` for every new op. It must also check that the verifier rejects `Shl` with amount 0 and amount 32, with an error containing `"shift amount"`.

In `tests/unit/test_jit_backend.cpp`, add one test case per op group, using the `Fixture` and explicit expected values:

```cpp
TEST_CASE("Backend: logic, shifts and compares", "[jit][backend]") {
    Fixture f;
    auto s = f.rig->BaseState(kCode);
    s.R[1] = 0xF0F0000F;
    s.R[2] = 0x8000FF01;
    f.rig->Load(s);
    const ValueId a = f.b.GetReg(1);
    const ValueId b = f.b.GetReg(2);
    f.b.SetReg(3, f.b.And(a, b));
    f.b.SetReg(4, f.b.Or(a, b));
    f.b.SetReg(5, f.b.Xor(a, b));
    f.b.SetReg(6, f.b.Not(a));
    f.b.SetReg(7, f.b.Shl(b, 4));
    f.b.SetReg(8, f.b.Shr(b, 4));
    f.b.SetReg(9, f.b.Sar(b, 4));
    f.b.SetReg(10, f.b.CmpGtU(b, a)); // 0x8000FF01 > 0xF0F0000F unsigned? no -> 0
    f.b.SetReg(11, f.b.CmpGtS(a, b)); // -252706801 > -2147418367 -> 1
    f.b.SetReg(12, f.b.CmpGeU(a, a)); // 1
    f.b.SetReg(13, f.b.CmpGeS(b, a)); // 0
    f.b.Exit(kCode + 2, 1);
    f.Run();
    const auto st = f.rig->State();
    CHECK(st.R[3] == 0x80000001u);
    CHECK(st.R[4] == 0xF0F0FF0Fu);
    CHECK(st.R[5] == 0x70F0FF0Eu);
    CHECK(st.R[6] == 0x0F0FFFF0u);
    CHECK(st.R[7] == 0x000FF010u);
    CHECK(st.R[8] == 0x08000FF0u);
    CHECK(st.R[9] == 0xF8000FF0u);
    CHECK(st.R[10] == 0u);
    CHECK(st.R[11] == 1u);
    CHECK(st.R[12] == 1u);
    CHECK(st.R[13] == 0u);
}

TEST_CASE("Backend: system registers, MAC and interrupt-allow", "[jit][backend]") {
    Fixture f;
    auto s = f.rig->BaseState(kCode);
    s.R[1] = 0x06001234;
    s.SR = 0xF1; // mask 15, T = 1
    s.delaySlotTarget = 0x06003000;
    f.rig->Load(s);
    auto &ctx = f.rig->sh2->GetJitContext();
    const ValueId v = f.b.GetReg(1);
    f.b.SetGBR(v);
    f.b.SetVBR(f.b.Add(v, f.b.Const(4)));
    f.b.SetPR(f.b.Add(v, f.b.Const(8)));
    f.b.SetMACH(f.b.Const(0x11112222));
    f.b.SetMACL(f.b.Const(0x33334444));
    f.b.SetReg(2, f.b.GetGBR());
    f.b.SetReg(3, f.b.GetVBR());
    f.b.SetReg(4, f.b.GetMACH());
    f.b.SetReg(5, f.b.GetMACL());
    f.b.SetReg(6, f.b.GetSR());
    f.b.SetReg(7, f.b.GetDelayTarget());
    f.b.ClearIntrAllow();
    f.b.Exit(kCode + 2, 1);
    f.Run();
    const auto st = f.rig->State();
    CHECK(st.GBR == 0x06001234u);
    CHECK(st.VBR == 0x06001238u);
    CHECK(st.PR == 0x0600123Cu);
    CHECK(st.MACH == 0x11112222u);
    CHECK(st.MACL == 0x33334444u);
    CHECK(st.R[2] == 0x06001234u);
    CHECK(st.R[4] == 0x11112222u);
    CHECK(st.R[6] == 0xF1u);
    CHECK(st.R[7] == 0x06003000u);
    CHECK_FALSE(st.intrAllow);
    CHECK_FALSE(*ctx.intrAllow);
}

TEST_CASE("Backend: SetSR masks reserved bits and recomputes interrupt flags", "[jit][backend]") {
    Fixture f;
    auto s = f.rig->BaseState(kCode);
    s.R[1] = 0xFFFFFFFF;
    f.rig->Load(s);
    f.b.SetSR(f.b.GetReg(1), false);
    f.b.SetIntrAllow();
    f.b.Exit(kCode + 2, 1);
    f.Run();
    CHECK(f.rig->State().SR == 0x3F3u);
    CHECK(f.rig->State().intrAllow);
}
```

In `tests/unit/test_sh2_jit_iface.cpp`, extend "JIT context callbacks mirror interpreter memory semantics":
- `*ctx.MACH` and `*ctx.MACL` alias the SH-2's MAC registers. Write through the pointers and read back via `rig->State().MACH/MACL`.
- `ctx.setSR(ctx.sh2, 0xFFFFFFFF, false)` leaves `State().SR == 0x3F3` and `State().intrAllow == false`.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build --target brimir_tests`
Expected: compile errors (the new ops, builder methods and context fields don't exist).

- [ ] **Step 3: Implement**

Add the context fields and `JitSetSR` to the fork, fix the comment, and log one row in `src/core/BRIMIR_FORK.md`: `MAC pointers and setSR callback in SH2JitContext; Advance hook comment updated`.

Add the ops:
- the enumerators;
- the `kOpInfo` entries. Keep the same order as the enum; `static_assert` guards the count only, so check the order by eye:
  - `And`/`Or`/`Xor`/`CmpGt*`/`CmpGe*`: dst, 2 srcs
  - `Not`/`Shl`/`Shr`/`Sar`: dst, 1 src
  - `Get*` and `GetDelayTarget`: dst, 0 srcs
  - `Set*`: no dst, 1 src
  - `ClearIntrAllow`/`SetIntrAllow`: no dst, 0 srcs
- the `Builder` methods;
- the verifier rule;
- one `case` per op in `RunBlock`:
  - `Sar`: `static_cast<uint32_t>(static_cast<int32_t>(v[in.a]) >> in.imm)`
  - signed compares: cast to `int32_t`

- [ ] **Step 4: Run to verify they pass**

Run: `build\bin\brimir_tests.exe "[jit]"`, the full suite, and `ctest --test-dir build --output-on-failure`.
Expected: all pass. `brimir_libretro` also builds.

- [ ] **Step 5: Commit**

```bash
git add src/core/include/ymir/hw/sh2/sh2_jit_iface.hpp src/core/include/ymir/hw/sh2/sh2.hpp src/core/src/ymir/hw/sh2/sh2.cpp src/core/BRIMIR_FORK.md src/jit tests/unit/test_jit_ir.cpp tests/unit/test_jit_backend.cpp tests/unit/test_sh2_jit_iface.cpp
git commit -m "feat(jit): add logic, shift, compare and system-register IR ops"
```

---

### Task 4: Table-driven differential tests and data-transfer instructions

**Files:**
- Create: `tests/unit/jit_opcode_specs.hpp`, `tests/unit/jit_opcode_specs.cpp`
- Create: `tests/unit/test_jit_opcodes.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `src/jit/src/frontend.cpp`
- Modify: `design/sh2-jit-handler-table.md` (only if a row is corrected)

**Interfaces:**
- Consumes: `Pair`-style comparison. Define a local `Pair` in `test_jit_opcodes.cpp` with the same members and `Step()` behavior as in `test_jit_diff.cpp` (two `Rig`s plus an `Executor`; `Step` compares cycles and `DiffRigs`, then sets `lastStepMatched`). Also consumes `RandomDataAddress` semantics (cache-through RAM `0x2604xxxx`, cached RAM `0x0604xxxx`, MMIO `0x2200xxxx`).
- Produces (`jit_opcode_specs.hpp`, namespace `jitspec`):

```cpp
#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <span>

namespace jitspec {

// How an instruction's operand fields are encoded.
enum class Fmt : uint8_t { Z, N, M, NM, MD, ND4, NMD, D, ND8, I, NI };

// Which registers form a data address (fixed up to point at valid memory before running).
enum class Addr : uint8_t { None, Rm, Rn, RmR0, RnR0, RmDisp, RnDisp, GbrDisp, GbrR0, RnPreDec, RmPostInc };

struct OpSpec {
    const char *name;
    uint16_t base;   // encoding with all operand fields zero
    Fmt fmt;
    Addr addr;
    uint8_t size;    // data access size in bytes (0 = none)
    bool slotOk;     // legal in a delay slot
};

// Random operands for `spec`; fixes up address registers / GBR / R0 so that every data access
// is aligned and lands in RAM (cached or cache-through) or the rig's MMIO page.
uint16_t Encode(const OpSpec &spec, std::mt19937 &rng, std::array<uint32_t, 16> &regs, uint32_t &gbr);

// All opcodes the JIT compiles (grows with tasks 4-6).
std::span<const OpSpec> CompiledOpcodes();

} // namespace jitspec
```

**`Encode` rules:**
- **Operand fields:**
  - n: bits 11..8, m: bits 7..4
  - `MD`: m in bits 7..4, disp4 in bits 3..0
  - `ND4`: n in bits 7..4, disp4 in bits 3..0
  - `NMD`: n, m, disp4
  - `D`: disp8 in bits 7..0
  - `ND8`: n plus disp8
  - `I`: imm8
  - `NI`: n plus imm8
  - `M`: m in bits 11..8
- **Address fixup:** pick a target address `A = RandomDataAddress(rng, size)` (cache-through RAM, cached RAM or MMIO, aligned), then derive the register values from it:
  - `Rm`/`Rn`: reg = A
  - `RmR0`/`RnR0`: R0 = random aligned offset 0..0xF0 and reg = A − R0. If the base register is R0 itself, use A directly.
  - `RmDisp`/`RnDisp`: reg = A − disp4·size
  - `GbrDisp`: gbr = A − disp8·size
  - `GbrR0`: R0 = small aligned offset, gbr = A − R0
  - `RnPreDec`: Rn = A + size
  - `RmPostInc`: Rm = A
  
  If two operands alias the same register in a way that breaks the address (for example n == m where one register is the address and the other the data), keep the encoding. The interpreter defines what happens and the JIT must match it, so these cases are valuable. Only the address register's value is fixed up.

**`CompiledOpcodes()` after this task:** the 19 existing opcodes, plus every opcode in sections 2 and 3 of the handler table (MOVW_L, MOVB_L0, MOVW_L0, MOVL_L0, MOVB_L4, MOVW_L4, MOVL_L4, MOVB_LG, MOVW_LG, MOVL_LG, MOVB_P, MOVW_P, MOVL_P, MOVW_I, MOVA, MOVW_S, MOVB_M, MOVW_M, MOVL_M, MOVB_S0, MOVW_S0, MOVL_S0, MOVB_S4, MOVW_S4, MOVL_S4, MOVB_SG, MOVW_SG, MOVL_SG), and MOVT/CLRT/SETT. Get the base encodings from the handler table headings (for example `0110nnnnmmmm0001` → `0x6001`). Branch opcodes (`slotOk = false`) are not exercised by the per-instruction test; the delay-slot test uses them as branches.

**Tests (`test_jit_opcodes.cpp`):**
1. `"Every compiled opcode matches the interpreter"`, tag `[jit][diff][opcodes]`: for each non-branch spec, run 150 random instances at `kCode` or `kCode + 2`, with random registers, SR T bit, `wbReg` (0..16 or 0xFF), PR and GBR. Use one `Pair::Step` per instance and require `retired == 1` and no divergence. The RNG seed is derived from the spec index, so a failing spec is reproducible on its own. `INFO` prints the spec name and the instruction word.
2. `"Every slot-capable opcode matches the interpreter in a delay slot"`, tag `[jit][diff][opcodes]`: every `slotOk` spec × branches {BRA, BT/S (taken and not taken), JMP, RTS}, 20 random instances each. The target area is refilled every iteration, as in `test_jit_diff.cpp`. Two `Pair::Step` calls per instance.
3. `"Bus-wait retries match for every word and long access"`, tag `[jit][diff][opcodes]`: every spec with `size >= 2`, address forced into MMIO, `busWaitEvery` = 1, 2 and 3, and 6 steps.

**Lowering:**
- **Rows:** in `frontend.cpp`, extend `BaseOp` (normal and `Delay_` mappings) and `LowerPlain` with one `case` per new opcode, transcribing its IR row from the handler table.
- **PC-relative loads in delay slots:** MOVW_I, MOVL_I and MOVA in a delay slot must use base `delaySlotTarget − 2`. Pass the slot's branch target into `LowerPlain` as an optional `ValueId` (the constant for BRA/BT/S/BF/S/BSR, `kNoValue` for dynamic targets). Use `GetDelayTarget()` when it is `kNoValue`. Then allow MOVL_I in delay slots, which `BaseOp` currently excludes.
- **Fields:** use `Fmt` decoding helpers in `frontend.cpp` matching the handler table's `DECODE_*` macros (section 0).

- [ ] **Step 1:** Write `jit_opcode_specs.*` and `test_jit_opcodes.cpp`, initially listing only the existing 19 opcodes. Register both files in `tests/CMakeLists.txt`. Run `build\bin\brimir_tests.exe "[opcodes]"`. Expected: PASS. This confirms the framework against already-correct lowering.
- [ ] **Step 2:** Add the new specs. Run `"[opcodes]"`. Expected: FAIL for every new opcode, because it falls back to the interpreter and `retired`/`blocksRun` expectations don't hold. To make that failure visible, the per-instruction test also asserts `p.exec.GetStats().blocksRun` increased for each instance.
- [ ] **Step 3:** Implement the lowering for all section 2 and 3 opcodes plus MOVT/CLRT/SETT. Run `"[opcodes]"` and `"[jit]"` until everything passes. The interpreter is the reference. When a row was wrong, fix the row in the handler table too.
- [ ] **Step 4:** Run the full suite and `ctest --test-dir build --output-on-failure`. Then run `build\bin\brimir_bench.exe --bios tests\fixtures\sega_101.bin --lockstep 1800` (exit 0).
- [ ] **Step 5: Commit**

```bash
git add tests/unit/jit_opcode_specs.hpp tests/unit/jit_opcode_specs.cpp tests/unit/test_jit_opcodes.cpp tests/CMakeLists.txt src/jit/src/frontend.cpp design/sh2-jit-handler-table.md
git commit -m "feat(jit): compile all data-transfer instructions"
```

---

### Task 5: ALU, shift, compare and @(R0,GBR) instructions

**Files:**
- Modify: `src/jit/src/frontend.cpp`, `tests/unit/jit_opcode_specs.cpp`
- Modify: `design/sh2-jit-handler-table.md` (only if a row is corrected)

**Interfaces:** consumes `jitspec` (Task 4) and the IR ops (Task 3).

**Opcodes:**
- every row of handler table section 4 except MOVT/CLRT/SETT (done in Task 4): EXTSB, EXTSW, EXTUB, EXTUW, SWAPB, SWAPW, XTRCT, ADDC, ADDV, AND_R, AND_I, NEG, NEGC, NOT, OR_R, OR_I, ROTCL, ROTCR, ROTL, ROTR, SHAL, SHAR, SHLL, SHLL2, SHLL8, SHLL16, SHLR, SHLR2, SHLR8, SHLR16, SUB, SUBC, SUBV, XOR_R, XOR_I, CMP_EQ_I, CMP_GE, CMP_GT, CMP_HI, CMP_HS, CMP_PL, CMP_PZ, CMP_STR, TST_R, TST_I, CLRMAC;
- all of section 5: AND_M, OR_M, XOR_M, TST_M. These use `Addr::GbrR0` with `size = 1`. Their cycle quirks (OR_M uses 16-bit, XOR_M 32-bit access cycles) are already in the rows.

- [ ] **Step 1:** Add the specs to `CompiledOpcodes()`. Run `"[opcodes]"`. Expected: FAIL for the new opcodes (no compiled blocks).
- [ ] **Step 2:** Implement the lowering rows in `LowerPlain` and the `BaseOp` mappings. Run `"[opcodes]"` and `"[jit]"` until everything passes.
- [ ] **Step 3: Edge values.** Add a test case `"ALU edge values match the interpreter"` (tag `[jit][diff][opcodes]`) that runs every section 4 opcode with registers drawn from {0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x0000FFFF, 0xFF00FF00} and both T values. Random registers rarely hit carry, overflow and byte-equality edges, so cover them deliberately. Run: all pass.
- [ ] **Step 4:** Run the full suite, ctest, and `brimir_bench --bios tests\fixtures\sega_101.bin --lockstep 1800` (exit 0).
- [ ] **Step 5: Commit**

```bash
git add src/jit/src/frontend.cpp tests/unit/jit_opcode_specs.cpp tests/unit/test_jit_opcodes.cpp design/sh2-jit-handler-table.md
git commit -m "feat(jit): compile ALU, shift, compare and GBR-indexed logic instructions"
```

---

### Task 6: System-register transfers and remaining branches

**Files:**
- Modify: `src/jit/src/frontend.cpp`, `tests/unit/jit_opcode_specs.cpp`, `tests/unit/test_jit_opcodes.cpp`
- Modify: `tests/unit/test_jit_diff.cpp` (interrupt-allow test)
- Modify: `design/sh2-jit.md` (sections 5.1, 5.2)

**Opcodes:**
- handler table section 6: LDC_GBR_R, LDC_VBR_R, LDC_SR_R, LDS_MACH_R, LDS_MACL_R, LDS_PR_R, STC_GBR_R, STC_VBR_R, STC_SR_R, STS_MACH_R, STS_MACL_R, STS_PR_R;
- section 7: BSR, BRAF, BSRF, JSR (add them to `IsDelayedBranch`; their slot handling matches BRA/JMP).

**Interrupt-allow lowering** (handler table section 6, "intrAllow handling"):
- `BuildBlock` tracks a flag `allowCleared`, set by these opcodes, which emit `ClearIntrAllow`.
- Before the next instruction, emit `CheckBoundary` as usual, then `SetIntrAllow`, then clear the flag.
- LDC SR emits `SetSR(value, delaySlot)`, which clears allow and recomputes pending.
- If the allow-clearing instruction is the last one in the block, emit nothing more. The executor's pre-entry check and its `intrAllow = true` at block entry then reproduce `InterpretNext`.

- [ ] **Step 1: Write the failing tests**
  - Add the specs. LDC/LDS/STC/STS use `Fmt::M` or `Fmt::N` and `Addr::None`. BSR/BRAF/BSRF/JSR get `slotOk = false` and are added to the delay-slot test's branch list.
  - LDC VBR with random values is harmless in this rig, since no exception is raised. LDC SR with random values can unmask interrupts, but the rig has none pending, so it is safe. Keep the random values.
  - Append to `tests/unit/test_jit_diff.cpp`:

```cpp
// LDC Rm,SR that unmasks a pending interrupt: the interpreter still executes the next instruction
// (interrupt-allow is cleared for one instruction) and takes the interrupt before the one after.
TEST_CASE("Interrupts unmasked by LDC SR are taken one instruction later", "[jit][diff][exact]") {
    constexpr uint32_t kVbr = 0x06008000;
    constexpr uint32_t kVector = 0x50;
    constexpr uint32_t kHandler = 0x06009000;
    Pair p;
    for (Rig *rig : {p.ref.get(), p.jit.get()}) {
        rig->Write32(kVbr + kVector * 4, kHandler);
        rig->WriteCode(kHandler, {static_cast<uint16_t>(kSleep)});
        auto &ctx = rig->sh2->GetJitContext();
        ctx.write(ctx.sh2, 0xFFFFFF00, 4, 0);       // DVSR = 0
        ctx.write(ctx.sh2, 0xFFFFFF08, 4, 0x2);     // DVCR.OVFIE = 1
        ctx.write(ctx.sh2, 0xFFFFFF0C, 4, kVector); // VCRDIV
        ctx.write(ctx.sh2, 0xFFFFFEE2, 1, 0xF0);    // IPRA: DIVU level 15
        ctx.write(ctx.sh2, 0xFFFFFF04, 4, 1234);    // DVDNT: divide by zero -> overflow raised
    }
    // ldc R5,SR (R5 = 0: unmask) ; add #1,R3 ; add #1,R3 ; add #1,R3 ; sleep
    p.WriteCode(kCode, {0x450E, AddI(3, 1), AddI(3, 1), AddI(3, 1), static_cast<uint16_t>(kSleep)});
    auto state = p.ref->BaseState(kCode);
    state.SR = 0xF0; // mask 15: the level-15 DIVU interrupt is not pending yet
    state.VBR = kVbr;
    state.R[3] = 0;
    state.R[5] = 0;
    state.R[15] = 0x0600F000;
    p.Load(state);
    REQUIRE_FALSE(*p.jit->sh2->GetJitContext().intrPending);
    p.jit->sh2->SetJitExecutor(&p.exec);

    const uint64 refCycles = p.ref->sh2->Advance<false, false>(200);
    const uint64 jitCycles = p.jit->sh2->Advance<false, false>(200);
    REQUIRE(jitCycles == refCycles);
    const std::string diff = sh2test::DiffRigs(*p.ref, *p.jit, true);
    INFO(diff);
    REQUIRE(diff.empty());
    REQUIRE(p.ref->State().R[3] == 1u); // exactly one add ran before the interrupt
    REQUIRE(p.exec.GetStats().blocksRun > 0);
}
```

  The implementer must check the LDC SR encoding (`0100mmmm00001110`, so `ldc R5,SR` = `0x450E`). They must also check that a DIVU overflow raised while SR masks it leaves the interrupt pending at the INTC (`RaiseInterrupt` records `INTC.pending` regardless of the mask). Adjust the setup if not, keeping the assertion that exactly one instruction runs between the unmask and the interrupt.

- [ ] **Step 2:** Run `"[opcodes]"` and `"[exact]"`. Expected: FAIL for the new opcodes.
- [ ] **Step 3:** Implement the lowering rows, the interrupt-allow rule and the four branches. Run `"[jit]"` until everything passes.
- [ ] **Step 4: Prove the interrupt-allow test is sensitive.** Temporarily remove the `SetIntrAllow` emission and confirm the new `[exact]` test fails. Then temporarily remove the `ClearIntrAllow` emission instead and confirm it fails as well. Restore both and confirm `git diff` on `frontend.cpp` shows only the intended changes. Record the outputs.
- [ ] **Step 5: Update the spec.**
  - Section 5.1: remove the "while they are not supported" wording for SR/VBR writes, and state the interrupt-allow rule.
  - Section 5.2: list the new ops.
  - Run the full suite, ctest and the BIOS lockstep for 1800 frames.
- [ ] **Step 6: Commit**

```bash
git add src/jit/src/frontend.cpp tests/unit/jit_opcode_specs.cpp tests/unit/test_jit_opcodes.cpp tests/unit/test_jit_diff.cpp design/sh2-jit.md
git commit -m "feat(jit): compile system-register transfers and BSR/BRAF/BSRF/JSR"
```

---

### Task 7: Fuzzing over the full instruction set and test gaps

**Files:**
- Modify: `tests/unit/test_jit_diff.cpp` (fuzz test), `tests/unit/test_jit_executor.cpp`, `tests/unit/test_sh2_state_diff.cpp`, `tests/unit/test_jit_lockstep.cpp`

- [ ] **Step 1: Extend the fuzz generator.** In "JIT matches the interpreter on random programs":
  - Draw non-branch instructions from `jitspec::CompiledOpcodes()` (`Encode` with address fixups). This replaces the hand-written `switch` for those kinds.
  - Keep the existing branch kinds and add BSR/BRAF/BSRF/JSR. BRAF/BSRF targets are computed into R12 like JMP: R12 holds the displacement to a random program instruction.
  - Keep R8–R12 reserved as today: exclude them as destinations of non-branch instructions by remapping n ∈ {8..12} to n − 8.
  - Keep the program length and the SLEEP tail.
  - Re-measure coverage and set the thresholds at about 65% of the measured values, with the numbers in a comment.
  
  Run `"[fuzz]"`. Expected: passes, runtime under 5 s in Release (record it).
- [ ] **Step 2: Store-abort recovery.** In `test_jit_executor.cpp`, append to "flush during a store aborts the block after the store": the next `Step` recompiles and runs the rest of the block (`R[3] == 2`, `PC == kCode + 6`, `Cache().Size() == 1`).
- [ ] **Step 3: More diff coverage.**
  - In `test_sh2_state_diff.cpp`, add a test that mutates `dmac.channels[1].TCR` and checks the reported name `dmac.channels[1].TCR`.
  - Add a test that builds two rigs whose MMIO logs differ in one entry (same length) and checks the message contains `MMIO log entry`.
- [ ] **Step 4: CompareCores branches.** In `test_jit_lockstep.cpp`, extend "CompareCores detects a difference":
  - Mutating the slave SH-2's R3 reports `slave SH-2 R3`. Do it via `SaveState`/`LoadState` on `slaveSH2` with a modified copy.
  - A modified framebuffer pixel reports `frame row`. Write through `const_cast` to the pointer returned by `GetFramebuffer()` on one core, after the frame.
- [ ] **Step 5: DIVU interrupt over a target sweep.** In `test_jit_diff.cpp`, run "Interrupts raised inside a block are taken at the same instruction" over targets 1..60, not only 200. Wrap the existing body in a loop over the target, with a fresh `Pair` each time, and keep all assertions for targets large enough to reach the handler (`target >= 40`; record the actual threshold in a comment).
- [ ] **Step 6:** Run `"[jit]"`, `"[lockstep]"`, the full suite and ctest.
- [ ] **Step 7: Commit**

```bash
git add tests/unit
git commit -m "test(jit): fuzz the full compiled instruction set and close test gaps"
```

---

### Task 8: Real-game lockstep validation and milestone 1 report

**Files:**
- Create: `design/sh2-validation.md`
- Modify: `design/sh2-jit.md` (status line, section 10), `README.md` (SH-2 JIT limitation bullet), `CHANGELOG.md`

This task needs the user's content. Use exactly:
- **BIOS** (in `F:\OneDrive\Roms\BIOS\`): US = `mpr-17933.bin` (US v1.00; the file named `sega_101.bin` there is actually JP v1.01), JP = `Sega Saturn BIOS v1.01 (JAP).bin`.
- **Titles** (in `F:\OneDrive\Roms\saturn\`):
  - `Virtua Fighter 2 (Japan) (Rev B).chd` (JP)
  - `Panzer Dragoon II Zwei (USA).chd` (US)
  - `Sega Rally Championship (USA).chd` (US)
  - `Burning Rangers (USA).chd` (US)
  - `Guardian Heroes (USA).chd` (US)
  - `Street Fighter Zero 3 (Japan).chd` (JP)
- **System dir:** a scratch copy at `$env:TEMP\opencode\sysdir` containing `F:\OneDrive\Roms\BIOS\brimir_saturn_rtc_us_eu.smpc`, copied both as `brimir_saturn_rtc_us_eu.smpc` and as `brimir_saturn_rtc_jp.smpc`. Delete it at the end.

- [ ] **Step 1: Build an optimized benchmark binary.**

```powershell
cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DBRIMIR_LTO=ON -DBrimir_ENABLE_IPO=ON
cmake --build build-bench --target brimir_bench
```

Check that `build-bench\CMakeCache.txt` has `/O2` in `CMAKE_CXX_FLAGS_RELEASE`.
- [ ] **Step 2: Lockstep runs.** For the BIOS menu and each title, run:

```powershell
build-bench\bin\brimir_bench.exe --bios <bios> [--game <game>] --system-dir $env:TEMP\opencode\sysdir --lockstep 36000
```

  Run one title per command (each can take 15–30 minutes; use timeouts up to 1800000 ms and run sequentially). Record the exit code, the final line and wall time.
  - **Exit 3 (divergence):** stop the validation and debug.
    1. Reproduce with a smaller N to find the frame.
    2. Use `--dump-at <frame − 1> --dump-state` on the interpreter path, then `--state` with `--lockstep 2` to reproduce quickly.
    3. Find the instruction (temporarily log the JIT block PCs around the divergence).
    4. Reduce it to an isolated reproduction in `test_jit_diff.cpp` and fix the JIT.
    5. Commit the fix with the regression test (`fix(jit): ...`), then restart the validation for that title.
    
    Report BLOCKED only if the root cause can't be found after a real investigation, and include the frame, the diff line and the analysis.
- [ ] **Step 3: Default-settings smoke run.** For each title, run `build-bench\bin\brimir_bench.exe --bios <bios> --game <game> --system-dir $env:TEMP\opencode\sysdir --sh2-jit --warmup 600 --frames 3600`. These are normal user settings: threaded VDP and host RTC. Expected: exit 0. Record the ms/frame alongside an interpreter run with the same settings. That compares speed only; the IR interpreter is not expected to be faster.
- [ ] **Step 4: Write `design/sh2-validation.md`.**
  - A table with one row per title: lockstep frames, result, jit master/slave `blocksRun` and `interpreted` from the final line, wall time, smoke-run ms/frame with JIT vs interpreter.
  - The machine, commit and build flags.
  - The list of opcodes still interpreted.
  - A short "Findings" section listing any bugs fixed during validation, with commit SHAs.
- [ ] **Step 5: Docs.**
  - `design/sh2-jit.md`: status line becomes `Milestone 1 complete (plans 1A–1D); next: milestone 2 (x64 backend)`. In section 10, mark the milestone 1 done criteria as met and link `design/sh2-validation.md`.
  - `README.md`: the SH-2 JIT limitation bullet says the JIT is validated against the interpreter in lockstep and compiles most instructions, but is an IR interpreter and not faster yet. The option stays off by default.
  - `CHANGELOG.md` `[Unreleased]`: an entry summarizing the coverage and validation.
- [ ] **Step 6: Clean up and commit.** Delete the scratch system dir. Run the full suite and ctest once more.

```bash
git add design/sh2-validation.md design/sh2-jit.md README.md CHANGELOG.md
git commit -m "docs(design): record SH-2 JIT milestone 1 lockstep validation"
```
