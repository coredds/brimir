# Brimir Test Suite

## Overview

The Brimir test suite ensures emulation accuracy and prevents regressions during optimization. Tests are based on official Sega Saturn hardware documentation.

## Test Structure

```
tests/
├── TEST_PLAN.md              # Comprehensive test strategy
├── TEST_COVERAGE.md          # Coverage tracking and gaps
├── README.md                 # This file
├── QUICK_START.md            # Quick reference guide
├── REGRESSION_FRAMEWORK.md   # Regression testing guide
├── CMakeLists.txt            # Test build configuration
├── main.cpp                  # Catch2 test runner
├── fixtures/                 # Test data (BIOS files, etc.)
└── unit/                     # Unit tests
    ├── test_core_wrapper.cpp          # Core API tests
    ├── test_video_output.cpp          # Video system tests
    ├── test_audio_output.cpp          # Audio system tests
    ├── test_input_handling.cpp        # Input system tests
    ├── test_bios.cpp                  # BIOS loading tests
    ├── test_bios_integration.cpp      # BIOS integration tests
    ├── test_bios_boot_sequence.cpp    # Boot sequence validation (NEW)
    ├── test_game_loading.cpp          # Game loading tests
    ├── test_save_states.cpp           # Save state tests
    ├── test_cartridge_support.cpp     # Cartridge tests
    ├── test_sram_persistence.cpp      # SRAM tests
    ├── test_sh2_cache.cpp             # SH-2 cache tests (NEW)
    ├── test_vdp_registers.cpp         # VDP register tests (NEW)
    ├── test_memory_map.cpp            # Memory map tests (NEW)
    ├── test_scu_dma.cpp               # SCU DMA tests (NEW)
    └── test_regressions.cpp           # Regression tests (NEW)
```

## Test Categories

### 1. Wrapper Layer Tests (Implemented ✅)
Tests for the libretro wrapper and core API:
- Initialization and shutdown
- Game loading
- Video/audio output
- Input handling
- Save states

### 2. Hardware Accuracy Tests (New 🆕)
Tests based on official Saturn documentation:
- SH-2 cache behavior
- VDP1/VDP2 registers
- Memory addressing
- BIOS boot sequence

### 3. Integration Tests (Partial 🟡)
End-to-end functionality tests:
- BIOS + game loading
- Save state persistence
- Multi-frame consistency

### 4. Regression Tests (Planned 📋)
Tests for known issues:
- Bitmap rendering fixes
- Interlaced field selection
- Layer priority bugs

## Running Tests

### Build Tests
```bash
# Configure with tests enabled
cmake -B build -DBUILD_TESTING=ON

# Build tests
cmake --build build --target brimir_tests

# Run all tests
cd build
ctest --output-on-failure

# Or run directly
./brimir_tests
```

### Run Specific Tests
```bash
# Run tests matching a tag
./brimir_tests "[bios]"
./brimir_tests "[hardware]"
./brimir_tests "[cache]"

# Run specific test case
./brimir_tests "SH-2 Cache Configuration"

# List all tests
./brimir_tests --list-tests

# List all tags
./brimir_tests --list-tags
```

### Test Tags

- `[unit]` - Unit tests
- `[integration]` - Integration tests
- `[hardware]` - Hardware accuracy tests
- `[bios]` - BIOS-related tests
- `[sh2]` - SH-2 CPU tests
- `[cache]` - Cache behavior tests
- `[vdp]`, `[vdp1]`, `[vdp2]` - Video processor tests
- `[memory]` - Memory system tests
- `[video]` - Video output tests
- `[audio]` - Audio output tests
- `[input]` - Input handling tests
- `[save]` - Save state tests

## Documentation-Based Testing

### Philosophy
All hardware tests are based on official Sega Saturn documentation:
- ST-121-041594: Boot ROM initialization
- ST-058-R2-060194.pdf: VDP2 User's Manual
- ST-013-SP1-052794.pdf: VDP1 User's Manual
- ST-097-R5-072694.pdf: SCU User's Manual
- ST-202-R1-120994.pdf: CPU Communication
- ST-TECH.pdf: Technical bulletins

### Source Citations
Each test includes source citations:
```cpp
SECTION("Cache purge via CCR register") {
    // Source: ST-TECH.pdf
    // "Cache purge (full initialization): write 1 to the CP bit..."
    
    // Test implementation
}
```

### Query Documentation
Use the Saturn documentation query tool:
```bash
python query_saturn.py "VDP1 framebuffer structure"
python query_saturn.py "SH-2 cache behavior"
```

## Test Fixtures

### BIOS Files
Place BIOS files in `tests/fixtures/`:
- `sega_101.bin` - Japanese v1.01 (512KB)
- `sega_100.bin` - Japanese v1.00 (512KB)
- `mpr-17933.bin` - US BIOS (512KB)
- European BIOS variants

**Note:** BIOS files are copyrighted and not included in the repository.

### Test ROMs
For integration testing:
- Homebrew test ROMs
- Known-good game dumps
- Synthetic test programs

## Current Status

### Implemented Tests: 145+ test cases
- ✅ Core wrapper (90% coverage)
- ✅ BIOS loading (85% coverage)
- ✅ Video output (75% coverage)
- ✅ Audio output (70% coverage)
- ✅ Input handling (80% coverage)
- ✅ Save states (85% coverage)
- ✅ Game loading (75% coverage)

### New Tests: 255+ test cases (structure only)
- 🟡 SH-2 cache (10% coverage) - 60+ tests
- 🟡 VDP registers (15% coverage) - 40+ tests
- 🟡 Memory map (5% coverage) - 50+ tests
- 🟡 BIOS boot sequence (20% coverage) - 40+ tests
- 🟡 SCU DMA (5% coverage) - 45+ tests
- 🟡 Regression tests (10% coverage) - 20+ tests

### Missing: High priority
- ❌ Interrupt handling tests
- ❌ Timing accuracy tests
- ❌ Performance benchmarks

## API Requirements

The new hardware tests require additional APIs:

### Cache Control
```cpp
CacheMode GetMasterSH2CacheMode();
void PurgeCache();
void InvalidateCacheLine(uint32_t address);
uint32_t ReadMemoryCacheThrough(uint32_t address);
```

### Register Access
```cpp
uint16_t ReadVDP2Register(uint32_t offset);
void WriteVDP2Register(uint32_t offset, uint16_t value);
uint16_t GetRegisterResetValue(RegisterId reg);
```

### Memory Access
```cpp
uint8_t ReadMemory(uint32_t address);
void WriteMemory(uint32_t address, uint8_t value);
MemoryRegion GetMemoryRegion(uint32_t address);
```

See `TEST_COVERAGE.md` for complete API requirements.

## Contributing Tests

### Adding New Tests
1. Identify hardware behavior to test
2. Query Saturn documentation for specs
3. Create test file in `tests/unit/`
4. Add source citations
5. Update `CMakeLists.txt`
6. Update `TEST_COVERAGE.md`

### Test Template
```cpp
// Brimir [Component] Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0
//
// Based on official Saturn documentation:
// - [Document]: [Description]

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>

using namespace brimir;

TEST_CASE("[Component] [Feature]", "[tag1][tag2]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("[Specific behavior]") {
        // Source: [Document]
        // "[Quote from documentation]"
        
        // Test implementation
        REQUIRE(condition);
    }
}
```

### Best Practices
1. **Document sources**: Always cite official documentation
2. **Test hardware behavior**: Not just API contracts
3. **Use descriptive names**: Clear test and section names
4. **Add appropriate tags**: For filtering and organization
5. **Test edge cases**: Boundaries, errors, timing
6. **Avoid flaky tests**: Deterministic, repeatable
7. **Keep tests focused**: One concept per test case

## Continuous Integration

### Pre-commit Checks
```bash
# Run all tests
ctest --output-on-failure

# Check for regressions
./run_regression_tests.sh

# Performance benchmarks
./run_benchmarks.sh
```

### CI Pipeline (Planned)
1. Build on Windows/Linux/macOS
2. Run all unit tests
3. Run integration tests
4. Check code coverage
5. Run regression tests
6. Performance benchmarks

## Troubleshooting

### Tests Won't Build
- Ensure Catch2 is installed: `vcpkg install catch2`
- Check CMake configuration: `-DBUILD_TESTING=ON`
- Verify all test files are in `CMakeLists.txt`

### Tests Fail
- Check BIOS files in `fixtures/`
- Verify test fixtures are copied to build directory
- Check for missing APIs (new tests may be placeholders)

### Slow Tests
- Run specific test tags: `./brimir_tests "[unit]"`
- Skip integration tests: `./brimir_tests "~[integration]"`
- Use parallel execution: `ctest -j8`

## Resources

- **Test Plan**: See `TEST_PLAN.md` for comprehensive strategy
- **Coverage Report**: See `TEST_COVERAGE.md` for gaps and priorities
- **Saturn Docs**: Use `python query_saturn.py "query"`
- **Catch2 Docs**: https://github.com/catchorg/Catch2

## Next Steps

1. ⏳ Implement cache control API
2. ⏳ Implement register access API
3. ⏳ Implement memory access API
4. ⏳ Complete P0 critical tests
5. ⏳ Add regression test framework
6. ⏳ Create performance baselines

## License

Tests are licensed under GPL-3.0, same as the Brimir project.

