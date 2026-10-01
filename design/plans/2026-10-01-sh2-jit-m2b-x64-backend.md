# SH-2 JIT Milestone 2B — x64 Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run compiled SH-2 blocks as native x86-64 code generated with asmjit, bit-identical to the interpreter, and make SH-2 host time at least 2x lower than Ymir's interpreter on the six baseline titles.

**Architecture:**
- A native backend interface (`INativeBackend`) sits beside the existing IR interpreter (`RunBlock`). The executor and block cache compile each block natively when the backend can, and otherwise run it on `RunBlock`.
- The x64 backend lowers each IR op with asmjit's `x86::Compiler`, which does register allocation and calling conventions. Guest state is addressed relative to `ctx.R`.
- Calls out of generated code go through `noexcept` trampolines that catch exceptions and report aborts.
- Data accesses to array pages (RAM/ROM) and all access-cycle lookups are inlined from a read-only description of the bus page table. That description is the one fork addition in `ymir/sys/bus.hpp`.

**Tech Stack:** C++20, CMake 3.28+, asmjit (vendored, zlib license), Catch2 (amalgamated), MSVC 2022 / GCC 14 / Apple Clang.

**Spec:** `design/sh2-jit-m2.md` §4–§6. **Semantic reference:** `src/jit/src/interp_backend.cpp` (`RunBlock`). Every native op must have exactly the effect of its `RunBlock` case. `RunBlock` in turn matches the interpreter, as validated in milestones 1 and 2A.

## Design decisions (refinements of `sh2-jit-m2.md`, recorded there in Task 1)

1. **Register allocation:** asmjit's `x86::Compiler` (virtual registers, spilling and call ABI) replaces a hand-written linear-scan allocator (§4.3). Blocks are short (≤ 32 guest instructions) and compiled once, so compile time is not a concern, and it removes the riskiest hand-written component.
2. **Interface:** `INativeBackend` covers native code only. The "IR backend" is the absence of a native backend, so the executor runs every block with `RunBlock`. `RunBlock` stays the fallback and the reference (§4.2, §4.8).
3. **Guest state addressing:** generated code loads `ctx->R` once and addresses every other state field as `[R + offset]`. Each offset is computed at compile time from the `SH2JitContext` pointers. If an offset does not fit in 32 bits, compilation fails and the block runs on `RunBlock` (§4.3).
4. **Compile failures** are counted in `Executor::Stats::compileFallbacks` (printed by `brimir_bench`) instead of logged (§4.8).
5. **Exceptions:** trampolines catch every exception, store it, and stop the block. The backend rethrows it after the generated code returns, so callers see the same exception as with `RunBlock` (§4.5).
6. **Block linking (§4.6)** is not in this plan. If Task 6 misses the speed target, the profile goes into a follow-up plan (2C) that adds linking.

## Global Constraints

- **Fork scope:** `src/core/include/ymir/hw/sh2/*`, `src/core/src/ymir/hw/sh2/*`, and one read-only accessor in `src/core/include/ymir/sys/bus.hpp`. Fork edits are marked `// Brimir:` and logged in `src/core/BRIMIR_FORK.md`.
- **JIT off:** emulation is byte-identical to today.
- **JIT on, either backend:** identical to the interpreter (state, memory, bus access sequence, cycles, peripherals) at every `Advance()` return. The only accepted deviations are `design/sh2-jit.md` §6.5. Lockstep is the oracle: any divergence is a bug.
- **Includes:** `brimir-jit` includes only `ymir/core/types.hpp`, `ymir/hw/sh2/sh2_jit_iface.hpp` and `ymir/hw/sh2/sh2_decode.hpp` from the core. asmjit headers are included only by files under `src/jit/src/x64/`.
- **Portability:** everything builds and passes on hosts without x64 support (macOS ARM64 CI). There, `DefaultBackend()` is `Ir` and the x64 tests `SKIP`.
- **No exceptions cross generated code.** No generated code runs while its memory is freed: flushes stay deferred while a block runs.
- **Tests:** Catch2 in `tests/unit/`, JIT tests tagged `[jit]`. Never weaken an assertion to make a test pass.
- **Workflow:** branch `feature/sh2-jit-2b`. Push the branch to get Windows/Linux/macOS CI. Commit messages use `type(scope): subject`. Never stage `ROADMAP.md`, BIOS/ROM or `.smpc` files.

## Build and test commands (Windows)

`pwsh -NoProfile -File $env:TEMP\opencode\msvc.ps1 -Cmd "<command>"` loads MSVC. Commands:

```powershell
cmake --build build --target brimir_tests brimir_libretro brimir_bench
build\bin\brimir_tests.exe "[jit]"
build\bin\brimir_tests.exe
ctest --test-dir build --output-on-failure
build\bin\brimir_bench.exe --bios tests\fixtures\sega_101.bin --lockstep 1200
```

To run the JIT tests on a specific backend, set the environment variable first: `$env:BRIMIR_JIT_BACKEND='ir'` (or `'x64'`) before `brimir_tests.exe "[jit]"`.

## File structure

| File | Responsibility |
|---|---|
| `vendor/asmjit/` (new) | asmjit sources at a pinned commit, `LICENSE.md`, `BRIMIR_VENDOR.md` (commit, date, options) |
| `src/jit/include/brimir/jit/backend.hpp` (new) | `BackendKind`, `INativeBackend`, `NativeCode`, factory and name helpers |
| `src/jit/src/backend.cpp` (new) | factory, `DefaultBackend()`, parsing |
| `src/jit/src/x64/x64_backend.hpp/.cpp` (new) | `X64Backend`: `JitRuntime`, `Compile`, `Run`, `Reset`, `X64Frame`, trampolines |
| `src/jit/src/x64/x64_emitter.hpp/.cpp` (new) | IR → asmjit lowering of one block |
| `src/jit/include/brimir/jit/bus_fast_path.hpp`, `src/jit/src/bus_fast_path.cpp` (new, Task 4) | C++ reference of the inline fast path, also used for fast peeks |
| `src/jit/include/brimir/jit/block_cache.hpp`, `src/jit/src/block_cache.cpp` | cached entries carry `NativeCode`; code-size cap; recent-lookup table (Task 5) |
| `src/jit/include/brimir/jit/executor.hpp`, `src/jit/src/executor.cpp` | backend choice, native dispatch, stats |
| `include/brimir/core_wrapper.hpp`, `src/bridge/core_wrapper.cpp` | `SetSH2JitBackend` |
| `tools/brimir_bench.cpp`, `tools/README.md` | `--jit-backend ir\|x64`, backend stats |
| `tests/unit/jit_test_backend.hpp/.cpp` (new) | `TestBackend()` (env var), `RunOnBackend(...)`, `AvailableBackends()` |
| `tests/unit/jit_random_ir.hpp/.cpp` (new) | random verified IR blocks for backend-vs-backend tests |
| `tests/unit/test_jit_x64.cpp` (new) | x64-specific tests |

---

### Task 1: asmjit, the native backend interface and backend selection

**Files:**
- Create: `vendor/asmjit/**`, `vendor/asmjit/BRIMIR_VENDOR.md`, `src/jit/include/brimir/jit/backend.hpp`, `src/jit/src/backend.cpp`, `src/jit/src/x64/x64_backend.hpp`, `src/jit/src/x64/x64_backend.cpp`
- Create (tests): `tests/unit/jit_test_backend.hpp`, `tests/unit/jit_test_backend.cpp`, `tests/unit/test_jit_x64.cpp`
- Modify: `CMakeLists.txt`, `src/jit/CMakeLists.txt`, `src/jit/include/brimir/jit/block_cache.hpp`, `src/jit/src/block_cache.cpp`, `src/jit/include/brimir/jit/executor.hpp`, `src/jit/src/executor.cpp`, `include/brimir/core_wrapper.hpp`, `src/bridge/core_wrapper.cpp`, `tools/brimir_bench.cpp`, `tools/README.md`, `tests/CMakeLists.txt`, `tests/unit/test_jit_opcodes.cpp`, `tests/unit/test_jit_diff.cpp`, `tests/unit/test_jit_executor.cpp`, `tests/unit/test_jit_lockstep.cpp`, `design/sh2-jit-m2.md`

**Interfaces — Produces (`brimir/jit/backend.hpp`, namespace `brimir::jit`):**

```cpp
#pragma once
// Native code backends for the SH-2 JIT (design/sh2-jit-m2.md section 4). A block a native backend
// cannot compile runs on the IR interpreter (RunBlock), which is also the reference semantics.
#include <brimir/jit/interp_backend.hpp> // ExitInfo, kNoCycleTarget
#include <brimir/jit/ir.hpp>
#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace brimir::jit {

enum class BackendKind : uint8_t { Ir, X64 };

bool IsBackendAvailable(BackendKind kind); // Ir: always; X64: only in x86-64 builds
BackendKind DefaultBackend();              // X64 when available, else Ir
const char *BackendName(BackendKind kind); // "ir", "x64"
bool ParseBackend(std::string_view name, BackendKind &out);

struct NativeCode {
    const void *entry = nullptr; // nullptr: run the block with RunBlock
};

class INativeBackend {
public:
    virtual ~INativeBackend() = default;
    virtual BackendKind Kind() const = 0;
    // Compiles a verified block (guestInstrCount > 0) for the CPU whose state ctx points to.
    // Returns false, leaving out.entry == nullptr, if this backend cannot compile it.
    virtual bool Compile(const Block &block, const ymir::sh2::SH2JitContext &ctx, NativeCode &out) = 0;
    // Same contract as RunBlock. An exception thrown by a context callback is rethrown here after
    // the generated code has returned.
    virtual ExitInfo Run(const NativeCode &code, ymir::sh2::SH2JitContext &ctx, uint64_t target = kNoCycleTarget,
                         const bool *abortRequested = nullptr) = 0;
    // Frees all generated code. Never called while generated code runs.
    virtual void Reset() = 0;
    virtual size_t CodeBytes() const = 0; // bytes of generated code currently held
};

// nullptr for BackendKind::Ir or when the kind is unavailable in this build.
std::unique_ptr<INativeBackend> MakeNativeBackend(BackendKind kind);

} // namespace brimir::jit
```

**Executor and cache changes:**
- `explicit Executor(BackendKind kind = DefaultBackend());` plus `BackendKind Backend() const`. If the kind is unavailable it falls back to `Ir`. `Stats` gains `uint64_t nativeBlocksRun = 0; uint64_t compileFallbacks = 0;`. `blocksRun` keeps counting every compiled block run (native or IR).
- `BlockCache` takes `INativeBackend *native` (nullable) in its constructor. Each entry holds `std::unique_ptr<Block>` plus `NativeCode`. `Get` returns `const CachedBlock &` (`struct CachedBlock { Block block; NativeCode code; };` or equivalent).
  - On a miss with `guestInstrCount > 0` and a native backend, it calls `Compile`. If that fails, it increments a counter that the executor reports as `compileFallbacks`.
  - `Flush()` also calls `native->Reset()`.
- `Executor::Step` runs `native->Run(code, ...)` when `code.entry != nullptr`, otherwise `RunBlock`. Everything else (gates, `intrAllow = true`, `BlockScope`, deferred flush) is unchanged.

**x64 stub in this task:**
- `X64Backend::Compile` returns `false` for every block. `Run` is never reached. `Reset` and `CodeBytes` work: `Reset` destroys and recreates the `asmjit::JitRuntime`.
- Only `src/jit/src/x64/*` includes asmjit.

**asmjit vendoring and build:**
- Vendor asmjit at commit `0d7f01052f2d3f6d6b9bb6714ef32bd7b80177eb` (2026-09-22). Keep `asmjit/`, `CMakeLists.txt`, `LICENSE.md`; leave out `test/`, `tools/`, docs and CI files. `BRIMIR_VENDOR.md` records the URL, commit, date and the CMake options used.
- Root `CMakeLists.txt` defines `BRIMIR_JIT_X64` (option, default `ON`), effective only when the target is x86-64:
  - condition: `CMAKE_SIZEOF_VOID_P EQUAL 8`, `CMAKE_SYSTEM_PROCESSOR` matches `^(AMD64|amd64|x86_64|x64)$`, and `CMAKE_OSX_ARCHITECTURES` does not contain `arm64`;
  - when effective: set asmjit's cache options `ASMJIT_STATIC ON`, `ASMJIT_NO_AARCH64 ON`, `ASMJIT_NO_FOREIGN ON` (check the exact option names in the vendored `CMakeLists.txt`), `add_subdirectory(vendor/asmjit EXCLUDE_FROM_ALL)`, and suppress its warnings like the other vendors.
- `brimir-jit` adds `src/backend.cpp` always, and `src/x64/*.cpp` plus `PRIVATE asmjit::asmjit` only when x64 is effective. It gets the compile definition `PUBLIC BRIMIR_JIT_HAS_X64=1` or `=0`.

**Selection plumbing:**
- `CoreWrapper::SetSH2JitBackend(jit::BackendKind)` and `GetSH2JitBackend()`. Forward-declare `enum class BackendKind : uint8_t;` in `core_wrapper.hpp`. Changing the kind detaches and drops both executors; if the JIT is enabled, it recreates them with the new kind. The default is `DefaultBackend()`.
- `brimir_bench --jit-backend ir|x64`:
  - valid only with `--sh2-jit` or `--lockstep`, and rejected if the backend is unavailable;
  - with `--lockstep`, it applies to the JIT core;
  - output adds `SH2 JIT backend: <name>` and, per CPU, `nativeBlocksRun` and `compileFallbacks` next to the existing `blocksRun`/`interpreted`;
  - document it in `tools/README.md`.

**Tests:**
- `tests/unit/jit_test_backend.hpp` (namespace `sh2test`):
  - `brimir::jit::BackendKind TestBackend()` returns env `BRIMIR_JIT_BACKEND` (`ir`/`x64`) if set and available, else `DefaultBackend()`. It fails the test with a clear message if the variable names an unavailable backend.
  - `std::vector<brimir::jit::BackendKind> AvailableBackends()`.
  - `brimir::jit::ExitInfo RunOnBackend(BackendKind kind, const Block &, SH2JitContext &, uint64_t target = kNoCycleTarget, const bool *abort = nullptr)`. For `Ir` it calls `RunBlock`. Otherwise it `REQUIRE`s that `Compile` succeeds, then calls `Run`. It keeps the backend alive for the call.
- Every `brimir::jit::Executor exec;` in the test files becomes `brimir::jit::Executor exec{sh2test::TestBackend()};`. Lockstep tests call `SetSH2JitBackend(sh2test::TestBackend())` on the JIT core before enabling the JIT.
- `test_jit_executor.cpp`: the two flush-hook tests put `kData` on the rig's MMIO page (`0x22040000`), which always takes the callback, so they stay meaningful once Task 4 inlines RAM accesses.
- `test_jit_x64.cpp` (tags `[jit][x64]`; every test starts with `if (!IsBackendAvailable(BackendKind::X64)) SKIP("no x64 backend");`):
  - names, parsing and availability;
  - `MakeNativeBackend(Ir) == nullptr`;
  - an executor with `X64` and the stub still runs a 3-instruction program identically to the interpreter, with `compileFallbacks > 0` and `nativeBlocksRun == 0`.
- `tests/CMakeLists.txt`: when `BRIMIR_JIT_HAS_X64`, add a second CTest entry `brimir_tests_jit_ir` running `brimir_tests "[jit]"` with `ENVIRONMENT BRIMIR_JIT_BACKEND=ir`. The default entry then covers x64.

**Design doc:** add a "Plan 2B refinements" subsection to `design/sh2-jit-m2.md` §4 listing the six design decisions above, and set its status line to "Part A done; Part B in progress".

- [ ] **Step 1:** Write `test_jit_x64.cpp`, `jit_test_backend.*` and the test-file edits; build. Expect compile errors, because `backend.hpp` does not exist yet.
- [ ] **Step 2:** Vendor asmjit, wire CMake, add `backend.hpp/.cpp` and the x64 stub, and change the executor and cache. Build all targets on Windows.
- [ ] **Step 3:** Run `"[jit]"` with no env var (x64 stub → all IR fallbacks) and with `BRIMIR_JIT_BACKEND=ir`, then the full suite, `ctest` (both entries), and BIOS lockstep 1200 with `--jit-backend x64` and with `ir`. All pass.
- [ ] **Step 4:** Commit `feat(jit): add native backend interface and vendor asmjit`. Push the branch and confirm CI is green on all three platforms (macOS builds without asmjit).

---

### Task 2: x64 code generation without calls

**Files:**
- Create: `src/jit/src/x64/x64_emitter.hpp`, `src/jit/src/x64/x64_emitter.cpp`, `tests/unit/jit_random_ir.hpp`, `tests/unit/jit_random_ir.cpp`
- Modify: `src/jit/src/x64/x64_backend.cpp`, `src/jit/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/unit/test_jit_backend.cpp`, `tests/unit/test_jit_x64.cpp`

**Generated function and frame (internal to `src/jit/src/x64/`):**

```cpp
struct X64Frame {
    ymir::sh2::SH2JitContext *ctx;
    uint64_t limit;         // boundary when cycles >= limit: target >= entry ? target - entry : 0
    uint64_t entryCycles;   // *ctx->cyclesExecuted at entry
    const bool *abortRequested; // never null (points to a static false when the caller passed nullptr)
    uint8_t stop = 0;       // set by trampolines (Task 3)
    std::exception_ptr error; // set by trampolines (Task 3)
    ExitInfo out;           // written by generated code before it returns
};
using X64BlockFn = void (*)(X64Frame *frame);
```

- `Run` fills the frame, calls the function, rethrows `frame.error` if set (Task 3), and returns `frame.out`.
- Generated code keeps `cycles` (64-bit) in a virtual register. It loads `regs = ctx->R` at entry. Every other state field is accessed as `[regs + off]`, with `off = (intptr_t)ctx.<field> - (intptr_t)ctx.R` computed from the `ctx` passed to `Compile`. If an offset does not fit in `int32_t`, `Compile` returns false.
- Each IR `ValueId` maps to one 32-bit virtual register.
- asmjit errors (through an `ErrorHandler`) and unsupported ops make `Compile` return false.

**Ops lowered in this task** (each exactly as its `RunBlock` case):
- **State and ALU:** `Const, GetReg, SetReg, GetPR, SetPR, GetT, SetT, GetGBR, SetGBR, GetVBR, SetVBR, GetSR, GetMACH, GetMACL, SetMACH, SetMACL, ClearIntrAllow, SetIntrAllow, GetDelayTarget, Add, Sub, And, Or, Xor, Not, Shl, Shr, Sar, SExt8, SExt16, CmpEq, CmpGtU, CmpGeU, CmpGtS, CmpGeS, Mul, MulHiS, MulHiU, SetSRBits`.
- **Cycles and control:** `AddCycles, WbStall, SetWb, SyncCycles, CheckBoundary, ExitIf` (refill flag false only), `Exit, ExitDynamic`.
- **Not supported yet:** any other op, or `ExitIf` with refill. These make `Compile` return false; Task 3 adds them.

Notes on the trickier ops:
- `SetT`: `SR = (SR & ~1) | (a != 0)`.
- `MulHiS`/`MulHiU`: a 64-bit multiply of the sign- or zero-extended operands, keeping the high half.
- `WbStall`: `wb = byte [wbReg]`; if `wb <= 16 && ((imm >> wb) & 1)`, `cycles += 1`.
- `SyncCycles`: `*cyclesExecuted = entryCycles + cycles`.
- `CheckBoundary`:
  - if `cycles >= limit`, or (`*intrPending` and `*intrAllow`), take an out-of-line exit stub;
  - the stub writes `PC = imm`, `out.retired`, `out.boundary = true` and `out.cycles`, then returns.
- `ExitIf` (taken): `cycles += imm2`, `PC = imm`, `out.retired`, `out.cycles`, return.
- `Exit`: `PC = imm`, `out.retired`, `out.cycles`, return.
- `ExitDynamic`: `out.retired`, `out.cycles`, return.

**Tests:**
- `jit_random_ir.hpp` (namespace `sh2test`):
  - `struct RandomIrOptions { bool calls = false; bool memory = false; };` and `brimir::jit::Block RandomBlock(std::mt19937 &rng, uint32_t startPC, const RandomIrOptions &opt);`.
  - It builds a verified block (check `VerifyBlock`) of 20–200 ops over the allowed set. It keeps a pool of live values and reuses old values at random so many stay live, which forces spills.
  - It uses random immediates and register indices, with `CheckBoundary` markers at increasing `retired` counts and `AddCycles`/`WbStall`/`SetWb` between them.
  - It adds 0–3 `ExitIf` on random conditions and ends with `Exit` or `ExitDynamic`.
  - The ops allowed with `calls = false` are exactly Task 2's list.
- `test_jit_backend.cpp`: every `Fixture` test whose block uses only Task 2 ops runs once per backend (`GENERATE(from_range(AvailableBackends()))`) through `RunOnBackend`, with the same assertions.
- `test_jit_x64.cpp`:
  - **"x64 matches the IR interpreter on random blocks":** 2000 seeds. Two rigs get the same random state: random R0–R15, SR (`0x3F3` bits), GBR, VBR, PR, MACH/MACL, delay target, wbReg (0–16 or 0xFF), intrPending/intrAllow and cyclesExecuted. One runs `RunBlock`, the other the x64 code, each with a random target (sometimes `kNoCycleTarget`, sometimes within 0–40 cycles of the entry count). It REQUIREs that `Compile` succeeds, then compares every `ExitInfo` field and `DiffRigs(a, b)`. Failures print the seed and `PrintBlock`.
  - **"x64 spills":** a block of 300 `Const`/`Add` values that stay live until a final reduction into R0–R15 matches IR.
  - **"x64 boundary at every check":** a 32-instruction straight block, run with each target from entry to entry+40, gives the same `PC`, `retired`, `boundary` and `cycles` as IR. Repeat with `intrPending && intrAllow` set before each check.
  - **"x64 code is freed by Reset":** `CodeBytes() > 0` after compiling and 0 after `Reset()`.

- [ ] **Step 1:** Write the random generator and tests; build. The x64 tests fail because `Compile` returns false.
- [ ] **Step 2:** Implement the emitter and `Run`.
- [ ] **Step 3:** All `[jit]` tests pass with both backends, plus the full suite, `ctest`, and BIOS lockstep 1200 (`--jit-backend x64`). Record in the report how many compiles fell back during that lockstep (most blocks still have memory ops).
- [ ] **Step 4:** Commit `feat(jit): generate x64 code for ALU, state and exits`.

---

### Task 3: x64 calls — memory, delay slots, SR and helpers, aborts and exceptions

**Files:**
- Modify: `src/jit/src/x64/x64_backend.hpp/.cpp`, `src/jit/src/x64/x64_emitter.cpp`, `tests/unit/jit_random_ir.cpp`, `tests/unit/test_jit_backend.cpp`, `tests/unit/test_jit_x64.cpp`

**Trampolines** (in `x64_backend.cpp`, `noexcept`, first argument `X64Frame *`):

| Trampoline | Calls | Sets `frame->stop` |
|---|---|---|
| `uint32_t TrRead(f, addr, size, instrFetch)` | `ctx->read` | exception, or `*abortRequested` after the call |
| `void TrWrite(f, addr, size, value)` | `ctx->write` | exception, or `*abortRequested` |
| `void TrRefill(f, addr)` | `ctx->refillPipeline` | exception, or `*abortRequested` |
| `uint64_t TrAccessCycles(f, addr, size, write)` | `ctx->accessCycles` | exception only |
| `uint64_t TrAccessCyclesRMWByte(f, addr)` | `ctx->accessCyclesRMWByte` | exception only |
| `uint32_t TrBusWait(f, addr, size, write)` | `ctx->busWait` (returns 0/1) | exception only |
| `void TrSetupDelaySlot(f, target)`, `void TrEndDelaySlot(f)` | `ctx->setupDelaySlot` / `ctx->endDelaySlot` | exception only |
| `void TrSetSR(f, value, delaySlot)` | `ctx->setSR` | exception only |
| `uint32_t TrDiv1(f, rn, rm, rmIsRn)` | `Div1Step(rn, rm, rmIsRn, *ctx->SR)` | never |
| `void TrMacW(f, op1, op2)`, `void TrMacL(f, op1, op2)` | `MacWStep`/`MacLStep` exactly as `RunBlock` | never |

- Every trampoline loads the callback pointer from `ctx` at call time, so tests can swap `ctx.read`/`ctx.write`.
- On an exception it stores `std::current_exception()` in `frame->error`, sets `stop`, and returns 0.
- After every trampoline that can set `stop`, generated code tests `frame->stop`. If set, it writes `out.aborted = true` and `out.cycles` (`retired` stays 0, `PC` untouched) and returns. This is `RunBlock`'s abort exit; for exceptions, `Run` then rethrows.

**Ops added** (exactly as `RunBlock`): `Load, Store, AddAccessCycles, AddAccessCyclesRMWByte, ExitIfBusWait, Refill, SetupDelaySlot, EndDelaySlot, SetSR, Div1, MacW, MacL`, and `ExitIf` with refill (`cycles += imm2`, `TrRefill(imm)`, stop check, then `PC`/`retired`/return). After this task, `Compile` succeeds for every block the front end produces.

**Tests:**
- `jit_random_ir.cpp`: `calls = true` adds the Task 3 ops except memory.
  - `memory = true` adds `Load`/`Store`/`AddAccessCycles`/`ExitIfBusWait` on addresses drawn from: rig RAM cached (`0x06xxxxxx`), RAM cache-through (`0x26xxxxxx`), MMIO (`0x22xxxxxx`), and, with low probability, the I/O area (`0xFFFFFE10`–`0xFFFFFE1F`, FRT registers, a slow-path partition). It adds misaligned addresses with low probability.
  - Delay-slot ops come in valid pairs only (`SetupDelaySlot` then `EndDelaySlot`, as the front end emits them). Check against `frontend.cpp` and `VerifyBlock`.
- `test_jit_backend.cpp`: the remaining `Fixture` tests run on both backends.
- `test_jit_x64.cpp`:
  - **"x64 matches IR on random blocks with calls and memory":** 2000 seeds, compared as in Task 2, with `DiffRigs(a, b)` including the MMIO log and bus-wait counters (set `mmio.busWaitEvery` to 0, 2 or 3 at random on both rigs).
  - **"x64 values live across calls":** 40 values computed before a `Load` from MMIO and a `SetSR` are all used afterwards. It matches IR.
  - **"x64 aborts after each memory kind":** for `Load`, `Store`, `Refill` and `ExitIf`-with-refill, a hooked callback requests an abort on an MMIO address. Expect `aborted`, `PC` unchanged, and no later state change, all the same as IR.
  - **"x64 propagates callback exceptions":** a hooked `ctx.read` throws `std::runtime_error`. `Run` (and `Executor::Step`) rethrow the same exception type and message, state matches IR's after the same throw, and the executor flushes normally afterwards.
- With `BRIMIR_JIT_BACKEND` unset, run BIOS lockstep 1200 with `--jit-backend x64` and require `compileFallbacks == 0` for both CPUs. The bench prints it.

- [ ] **Step 1:** Extend the random generator and tests; the new x64 tests fail.
- [ ] **Step 2:** Implement trampolines and the remaining ops.
- [ ] **Step 3:** `[jit]` on both backends, full suite, `ctest`, and BIOS lockstep 1200 (`--jit-backend x64`, `compileFallbacks == 0`).
- [ ] **Step 4:** Commit `feat(jit): call helpers and memory callbacks from x64 code`.

---

### Task 4: Inline bus fast path

**Files:**
- Modify (fork): `src/core/include/ymir/sys/bus.hpp`, `src/core/include/ymir/hw/sh2/sh2_jit_iface.hpp`, `src/core/src/ymir/hw/sh2/sh2.cpp`, `src/core/BRIMIR_FORK.md`
- Create: `src/jit/include/brimir/jit/bus_fast_path.hpp`, `src/jit/src/bus_fast_path.cpp`
- Modify: `src/jit/CMakeLists.txt`, `src/jit/src/x64/x64_emitter.cpp`, `tests/unit/test_sh2_jit_iface.cpp`, `tests/unit/test_jit_x64.cpp`, `tests/unit/test_jit_helpers.cpp`

**Fork additions:**

```cpp
// bus.hpp, public section of Bus:
// Brimir: read-only page-table layout for the SH-2 JIT's inline fast path (src/core/BRIMIR_FORK.md).
struct PageTableLayout {
    const uint8 *pages;      // &m_pages[0]
    uint32 pageStride;       // sizeof(MemoryPage)
    uint32 pageShift;        // pageGranularityBits
    uint32 addressMask;      // kAddressMask
    uint32 arrayOffset;      // offsetof(MemoryPage, array)          (uint8 *)
    uint32 arrayWritableOffset; // offsetof(MemoryPage, arrayWritable) (bool)
    uint32 readCyclesOffset[3];  // readCycles8/16/32  (uint64)
    uint32 writeCyclesOffset[3]; // writeCycles8/16/32 (uint64)
};
PageTableLayout GetPageTableLayout() const;

// sh2_jit_iface.hpp: Brimir: same fields with the same meaning, filled in by SH2::InitJitContext.
struct SH2JitBusLayout { /* identical members */ };
// SH2JitContext gains:  SH2JitBusLayout bus;
```

Add a `static_assert(std::is_standard_layout_v<MemoryPage>)` next to the accessor.

**Fast path rules** (`bus_fast_path.hpp`). These are a C++ reference of exactly what generated code does, and Task 5 also uses them:

```cpp
// The page entry for a bus address (address & addressMask) >> pageShift.
const uint8_t *PageEntry(const ymir::sh2::SH2JitBusLayout &bus, uint32_t address);
// Partition 0b000/0b001/0b101 data access to an array page: returns the byte pointer for the
// size-aligned address and sets writable; nullptr when the access must take the callback.
uint8_t *FastArrayPointer(const ymir::sh2::SH2JitBusLayout &bus, uint32_t address, uint32_t size, bool &writable);
// SH2::AccessCycles<T, write, emulateCache = false> for every partition:
// 0b000 -> 1; 0b001/0b101 -> page read/write cycles for the size; 0b010/0b011/0b100/0b110 -> 1; 0b111 -> 4.
uint64_t FastAccessCycles(const ymir::sh2::SH2JitBusLayout &bus, uint32_t address, uint32_t size, bool write);
// SH2Bus::IsBusWait: false for array pages; otherwise unknown (returns false and sets needsCallback).
bool FastBusWait(const ymir::sh2::SH2JitBusLayout &bus, uint32_t address, bool &needsCallback);
// Side-effect-free 16-bit instruction peek for partitions 0b000/0b001/0b101 on array pages.
bool FastPeek16(const ymir::sh2::SH2JitBusLayout &bus, uint32_t address, uint16_t &out);
```

**Inline lowering** (replaces the trampoline call when the fast path applies; the trampoline remains the fallback inside the same op):
- **`Load`:** if partition ∈ {0, 1, 5} and the page array is non-null, load big-endian (byte, or `movbe`/`bswap` for 16/32-bit) from `array + (addr & ~(size-1) & 0xFFFF)` and zero-extend. Otherwise call `TrRead` and check `stop`.
- **`Store`:** the same test. On an array page, store only if `arrayWritable`, otherwise do nothing (`Bus::Write`). On other pages call `TrWrite` and check `stop`.
- **`AddAccessCycles`:** fully inline per `FastAccessCycles`; no call.
- **`ExitIfBusWait`:** array page → no exit; otherwise call `TrBusWait`.
- **`Refill` and `AddAccessCyclesRMWByte`** keep their trampolines.
- `SyncCycles` stays an unconditional store (cheap, and exact).

**Tests:**
- `test_sh2_jit_iface.cpp`: on a rig, `ctx.bus.pages` is non-null, `pageShift == 16` and `addressMask == 0x7FFFFFF`. Reading RAM through `FastArrayPointer` for `0x06001234` equals `rig.Read32`. A handler page (MMIO, `0x22000000`) returns nullptr.
- `test_jit_helpers.cpp`, **"Fast path matches the SH-2 callbacks"**:
  - **Addresses:** every partition (top 3 bits 0–7) × offsets {RAM `0x06001230`, MMIO `0x02000010`, a read-only ROM page mapped in the test with `rig.bus.MapArray(0x4000000, 0x400FFFF, rom, false)`, an unmapped page via `rig.bus.Unmap` of one page} × sizes {1, 2, 4} × read/write, plus misaligned variants.
  - **Checks:**
    - `FastAccessCycles` equals `ctx.accessCycles`;
    - when `FastArrayPointer` returns non-null, the value read through it equals `ctx.read`;
    - `FastBusWait` without `needsCallback` equals `ctx.busWait`;
    - `FastPeek16` (when it returns true) equals `ctx.peekInstruction`.
- `test_jit_x64.cpp`, **"x64 inline memory matches IR"**:
  - **Inputs:** the address matrix above × sizes × {Load, Store, AddAccessCycles, ExitIfBusWait}, inside a block on both backends, with the ROM and unmapped pages mapped on both rigs.
  - **Comparison:** `ExitInfo`, `DiffRigs` including the MMIO log, and the ROM contents (unchanged after stores).
  - Rerun the random-block test with `memory = true`.

- [ ] **Step 1:** Write the tests; they fail to compile, because `SH2JitContext::bus` and `bus_fast_path.hpp` do not exist yet.
- [ ] **Step 2:** Add the fork accessor and context field, and log them in `BRIMIR_FORK.md`. Implement `bus_fast_path.cpp`, then the inline lowering.
- [ ] **Step 3:** `[jit]` on both backends, full suite, `ctest`, and BIOS lockstep 1200 on each backend.
- [ ] **Step 4:** Commit `feat(jit): inline RAM accesses and access cycles in x64 code`.

---

### Task 5: Fast dispatch and the code-cache cap

**Files:**
- Modify: `src/jit/include/brimir/jit/block_cache.hpp`, `src/jit/src/block_cache.cpp`, `src/jit/src/executor.cpp`, `tests/unit/test_jit_executor.cpp`, `tests/unit/test_jit_x64.cpp`

**Changes:**
- **`BlockCache::IsCurrent`:** compares each stored opcode with `FastPeek16` and uses `ctx.peekInstruction` only when `FastPeek16` returns false. The result is identical because the peek path on array pages is `Bus::Peek` → the same array.
- **`Executor::Step`'s `PC & 2` gate:** uses `FastPeek16` with the same callback fallback.
- **Recent-lookup table in `BlockCache`:** `std::array<RecentSlot, 4096>` indexed by `(pc >> 1) & 4095`, holding `{uint32_t pc; CachedBlock *entry;}`. `Get` checks it before the map; the entry is still validated with `IsCurrent`. Slots are cleared on `Flush` and on invalidation of that PC.
- **Code-size cap:** `constexpr size_t kMaxNativeCodeBytes = 64 << 20;`. `Get` flushes when `native->CodeBytes() >= limit`, at the same point where it checks `kMaxCachedInsts`. A test-only constructor parameter sets a smaller limit.

**Tests:**
- `test_jit_executor.cpp`:
  - **"Modified code in RAM is recompiled":** run a block, overwrite one opcode through `rig.WriteCode`, step again. The new code runs and `Invalidations() == 1`. Run on both backends.
  - **"Code on a handler page is checked through the callback":** map code into MMIO (`0x22001000`; the rig's MMIO accepts writes to `mmio.data`). The block runs, and changing the MMIO bytes invalidates it.
  - **"The recent table never returns a stale block":** step A at PC X, flush, and write different code at X. Step runs the new code. Repeat with an invalidation instead of a flush.
  - **"The native code cap flushes the cache":** with limit 4096 bytes, run 50 distinct blocks. The cache flushes at least once, every result matches the interpreter, and `CodeBytes() <= limit + largest block`.
- The `[jit]` suite on both backends.

- [ ] **Step 1:** Write the tests; confirm the cap test fails.
- [ ] **Step 2:** Implement.
- [ ] **Step 3:** `[jit]` on both backends, full suite, `ctest`, BIOS lockstep 1200 on both backends.
- [ ] **Step 4:** Commit `perf(jit): fast block validation, lookup cache and code-size cap`.

---

### Task 6: Validation and measurement

**Files:**
- Modify: `tests/unit/test_jit_diff.cpp`, `design/sh2-validation.md`, `tools/README.md`
- Create: `design/sh2-x64-performance.md`

**Fuzz, backend against backend:** the random-program fuzz test ("JIT matches the interpreter on random programs") gets a third rig when x64 is available. The interpreter, an IR executor and an x64 executor run the same program. After every step, compare IR against the interpreter (as today) and x64 against IR with `DiffRigs(ir, x64)` and identical `ExitInfo`. Report the x64 coverage numbers (`nativeBlocksRun`, `compileFallbacks == 0`).

**Game lockstep with x64:** follow the 1D Task 8 procedure, using `--jit-backend x64` with `--lockstep 36000` for the BIOS menu and all six titles (BIOS paths, titles, scratch system dir and debugging procedure as in `design/plans/2026-10-01-sh2-jit-m1d-coverage-validation.md` Task 8):
- run them sequentially with `Start-Process`, output to `.superpowers\sdd\m2b-ls-<title>.out`;
- on a divergence, reduce it to a regression test in `test_jit_x64.cpp` or `test_jit_diff.cpp`, fix it with interpreter evidence, commit `fix(jit): ...`, and rerun that title;
- record every final lockstep line, with `nativeBlocksRun`/`compileFallbacks`, in `design/sh2-validation.md` under "Milestone 2B".

**Measurement:**
- In the Release+LTO `build-bench` directory, use the same scenes, warmup and frames as `design/sh2-baseline.md` on the same machine.
- Run each title three ways: the interpreter, `--sh2-jit --jit-backend ir`, and `--sh2-jit --jit-backend x64`.
- `design/sh2-x64-performance.md` has:
  - a per-title table of ms/frame avg/p95 and SH2 master/slave/total ms for each mode;
  - the ratio interpreter SH2 total / x64 SH2 total;
  - the method and machine;
  - a verdict line: **target met** if the ratio is ≥ 2.0 on all six titles, otherwise **target missed**, listing the titles below 2.0;
  - for a miss, profile data (Windows: Visual Studio profiler or `xperf`/WPA). Show where SH-2 time goes: generated code, dispatch (`Step`/`Get`/`IsCurrent`), slow-path callbacks, interpreter fallbacks.

- [ ] **Step 1:** Extend the fuzz test to three rigs; run it.
- [ ] **Step 2:** Run the seven lockstep runs; fix any divergence.
- [ ] **Step 3:** Measure and write the performance report.
- [ ] **Step 4:** Commit `test(jit): fuzz the x64 backend against the IR interpreter` and `docs(design): record milestone 2B validation and x64 performance`.

---

### Task 7: Rollout (only if Task 6 says "target met")

If the target was missed, skip this task. Instead, write `design/plans/<date>-sh2-jit-m2c-performance.md` from the profile in `design/sh2-x64-performance.md` (block linking per `sh2-jit-m2.md` §4.6, then whatever the profile shows), and merge 2B with the JIT still off by default.

**Files:**
- Modify: `src/libretro/options.cpp`, `src/libretro/libretro.cpp`, `README.md`, `CHANGELOG.md`, `design/sh2-jit.md`, `design/sh2-jit-m2.md`, `tests/unit/` (option default test, if one exists for core options)

**Changes:**
- **Defaults:** when `BRIMIR_JIT_HAS_X64`, `brimir_sh2_jit` defaults to `enabled` (both the option definition's default value and the `g_options.sh2_jit` initial value). It stays `disabled` otherwise.
- **Label and text:** the label keeps "(Experimental)". The description drops "about 2x SLOWER" and states that compiled code runs natively on x86-64, is validated to match the interpreter, and is not used while SH-2 cache emulation is active.
- **Docs:**
  - README: JIT on by default on x86-64;
  - CHANGELOG: the x64 backend, with the speedup range from the report;
  - `sh2-jit.md` status: milestone 2 done;
  - `sh2-jit-m2.md` status: done.

- [ ] **Step 1:** Change defaults and text; build. Run the full suite, `ctest`, and a 1800-frame interpreter-vs-JIT smoke run of one title with the libretro default.
- [ ] **Step 2:** Commit `feat(libretro): enable the SH-2 JIT by default on x86-64`.

---

## Self-review notes

- **Spec coverage:**

  | Spec item | Task |
  |---|---|
  | §4.1 asmjit, `JitRuntime` | 1 |
  | §4.2 interface | 1 |
  | §4.3 compiled block | 2 |
  | §4.4 fast path | 4 |
  | §4.5 boundaries, stalls, helpers, abort, exceptions | 2, 3 |
  | §4.6 linking | deferred to 2C, if the target is missed |
  | §4.7 code-cache cap | 5 |
  | §4.8 fallback | 1, 3 |
  | §5.1–5.2 tests on both backends | 1 |
  | §5.3 backend-vs-backend fuzz | 2, 3, 6 |
  | §5.4 x64 unit tests | 2–5 |
  | §5.5 lockstep 36,000 frames × 7 | 6 |
  | §6 measurement, report, rollout | 6, 7 |

- **Check-on-entry through the fast path** (§4.4, last paragraph) is Task 5.
- **CI:** macOS ARM64 builds without asmjit and runs the IR backend by default. Windows and Linux run x64 by default plus the IR `[jit]` entry.
