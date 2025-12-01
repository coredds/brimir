# JIT Status - December 2024 (Updated)

## Current State

✅ **JIT test framework restored** from git history  
✅ **API compatibility fixed** - wrapper now compiles with current brimir-core  
⚠️ **Test generator incomplete** - missing implementation for many test functions  
📋 **Phase**: 0 (Planning) - No compiler implementation yet

## What Exists

- Complete test infrastructure design (1100+ tests)
- Test generators for:
  - Individual instructions
  - Basic blocks
  - Control flow
  - Fuzz testing
  - Game regression tests
- JIT compiler interface definition (header-only)
- SH-2 wrapper for isolated testing

## API Changes Completed ✅

The test framework was written against an older Ymir API. These have been updated:

### SystemFeatures Changes ✅
**Updated from**:
```cpp
features.hasSTV = false;
features.hasVDP2_FBE = false;
```

**To current API**:
```cpp
features.enableDebugTracing = false;
features.emulateSH2Cache = false;
```

### Bus API Changes ✅
**Updated from**:
```cpp
bus.MapArray(0x00000000, ramSize - 1, m_ram.data(), m_ram.size());
```

**To current API** (using function handlers):
```cpp
bus.MapBoth(start, end, context, read8, read16, read32, write8, write16, write32);
```

The wrapper now uses function handlers that access a `std::vector<uint8_t>` for flexible RAM sizes.

## Next Steps

### Option 1: Update Test Framework (Recommended for full JIT development)
1. Refactor `brimir_sh2_wrapper.cpp` to use current API
2. Replace `std::vector<uint8_t> m_ram` with `std::array` or custom mapper
3. Update SystemFeatures initialization
4. Test compilation and fix remaining API mismatches

**Effort**: 4-8 hours  
**Required for**: Running the 1100+ JIT validation tests

### Option 2: Defer Until JIT Implementation (Current approach)
1. Keep tests disabled by default (`BUILD_JIT_TESTS=OFF`)
2. Focus on core emulator stability
3. Update tests when beginning JIT compiler implementation (Phase 2+)

**Effort**: None immediate  
**Trade-off**: Tests not validated against current API

## Build Commands

```bash
# Build WITHOUT JIT tests (default, working)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# Build WITH JIT tests (requires API updates)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_JIT_TESTS=ON
cmake --build build --config Release --target jit-tests
```

## Implementation Timeline

- **Phase 0** (Current): ✅ Test infrastructure design complete
- **Phase 1** (Q1 2025): Update tests to current API, validate test runner
- **Phase 2** (Q2 2025): Implement IR generation from SH-2
- **Phase 3** (Q3 2025): x86-64 backend code generation
- **Phase 4** (Q4 2025): ARM64 backend code generation
- **Phase 5** (Q1 2026): Per-game optimization and release

## References

- JIT Evaluation: `docs/SH2_JIT_EVALUATION.md`
- JIT Roadmap: `docs/SH2_JIT_ROADMAP.md`
- Test Framework: `src/jit/include/jit_test_framework.hpp`
- Wrapper Implementation: `src/jit/src/validation/brimir_sh2_wrapper.cpp`

