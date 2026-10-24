# Brimir Test Suite

Unit and integration tests for the Brimir libretro core and the Ymir emulator
it wraps. Tests use the Catch2 v3 amalgamated build (`catch_amalgamated.hpp/.cpp`).

## Layout

```
tests/
├── CMakeLists.txt            # brimir_tests executable (single CTest entry)
├── main.cpp                  # Catch2 runner
├── catch_amalgamated.*       # Catch2 v3 single-file distribution
├── fixtures/                 # Test data (audio samples, optional BIOS images)
└── unit/
    ├── test_basic.cpp               # CoreWrapper smoke tests
    ├── test_core_wrapper.cpp        # Core API: lifecycle, save states, IPL, audio volume,
    │                                #   rotation, overscan crop, region mapping
    ├── test_options.cpp             # libretro core option metadata
    ├── test_bitmask_enum.cpp        # Bitmask enum helpers
    ├── test_cdblock_hle.cpp         # CD block HLE
    ├── test_game_db.cpp             # Game database lookups
    ├── test_hw_backports.cpp        # Ymir hardware-layer backport regressions (SMPC, VDP1, VDP2)
    ├── test_vdp_color_calc.cpp      # VDP2 color calculation
    ├── test_vdp_composite.cpp       # VDP2 layer compositing
    ├── test_profiler.cpp            # Profiler
    ├── test_audio_ring_buffer.cpp   # Audio ring buffer
    ├── test_media_loader.cpp        # CUE/CCD loading, compressed audio tracks, sector alignment
    ├── test_sram_persistence.cpp    # SRAM buffer handling
    ├── test_memory_components.cpp   # IPL ROM / Work RAM behavior, IPL hash, save state restore
    ├── test_cd_operations.cpp       # CD tray state and save state restore
    ├── test_system_integration.cpp  # Config persistence across reset, save state round trips
    ├── test_bios.cpp                # IPL loading edge cases and LoadIPLFromFile
    ├── test_bios_integration.cpp    # Real BIOS boot / save state tests (skip without BIOS)
    ├── test_dev_log.cpp             # devlog robustness (never throws)
    │
    │   SH-2 fork and JIT ([jit]):
    ├── test_sh2_jit_iface.cpp       # SH-2 fork hooks: executor attach, flushes, JIT context callbacks
    ├── test_sh2_state_diff.cpp      # SH-2 state diff and test-rig bus log
    ├── test_jit_ir.cpp              # IR builder, verifier, printer
    ├── test_jit_ir_opt.cpp          # IR timing-bookkeeping pass (OptimizeBlock)
    ├── test_jit_helpers.cpp         # shared helpers against the interpreter's instructions
    ├── test_jit_diff.cpp            # JIT vs interpreter, per instruction and random programs
    ├── test_jit_opcodes.cpp         # table-driven JIT vs interpreter tests (jit_opcode_specs)
    ├── test_jit_backend.cpp         # IR interpreter and native backends
    ├── test_jit_x64.cpp             # x64 backend: native code, chaining, caps, reuse, resume
    ├── test_jit_executor.cpp        # executor and block cache: flushes, tiers, reuse, IR cap, resume
    ├── test_jit_worker.cpp          # compile worker and background compiles
    ├── test_jit_integration.cpp     # JIT through CoreWrapper (options, save states, unload)
    ├── test_jit_lockstep.cpp        # whole-system lockstep: JIT core vs interpreter core
    │
    │   Shared test helpers:
    ├── sh2_test_rig.*               # isolated SH-2 on a synthetic bus
    ├── jit_test_backend.*           # backend selection (BRIMIR_JIT_BACKEND)
    ├── jit_opcode_specs.*           # table of compiled opcodes
    ├── jit_fuzz_programs.*          # random SH-2 programs
    ├── jit_random_ir.*              # random verified IR blocks
    └── jit_worker_helpers.hpp       # latches and blockers for compile-worker tests
```

## Building and running

```bash
cmake -B build -DBRIMIR_BUILD_TESTS=ON
cmake --build build --target brimir_tests

# Via CTest
ctest --test-dir build --output-on-failure

# Or directly (binary lands in build/bin/)
./build/bin/brimir_tests
./build/bin/brimir_tests "[savestate]"       # by tag
./build/bin/brimir_tests "~[integration]"    # exclude a tag
./build/bin/brimir_tests --list-tests
./build/bin/brimir_tests --list-tags

# SH-2 JIT tests on each backend (x64 only exists in x86-64 builds)
./build/bin/brimir_tests "[jit]"                          # default backend
BRIMIR_JIT_BACKEND=ir ./build/bin/brimir_tests "[jit]"    # IR interpreter backend
```

Build with asserts on (a Debug or RelWithDebInfo build without `NDEBUG`) to exercise the JIT's internal checks; the whole suite passes there. CI also runs `[jit]` under ThreadSanitizer (see the `test-linux-tsan` job in `.github/workflows/ci.yml` and `tests/tsan_suppressions.txt`).

## BIOS fixtures

`test_bios_integration.cpp` looks for real Saturn BIOS images in
`tests/fixtures/` (see `fixtures/README.md` for accepted file names). When no
BIOS is present (e.g. on CI) those test cases `SKIP()` and the suite still
passes. All other tests use synthetic IPL data or Ymir's built-in null IPL
program and need no external files.

## Writing tests

- Include `"catch_amalgamated.hpp"` and `<brimir/core_wrapper.hpp>` (or Ymir headers directly).
- Assert observable behavior; avoid placeholders such as `REQUIRE(true)` or
  tautologies like `REQUIRE(&ref != nullptr)`.
- Never write into the source tree or the current working directory; use
  `std::filesystem::temp_directory_path()` for scratch files and clean up.
- Tests that need copyrighted assets must `SKIP()` cleanly when they are absent.
- Add new files to the `brimir_tests` source list in `tests/CMakeLists.txt`.
