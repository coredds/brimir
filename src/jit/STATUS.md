# JIT Status - June 2026

## Current State

✅ **JIT test framework restored** from git history  
✅ **API updated for Ymir 2026-06-04 sync** — wrapper uses new SH2 constructor and callbacks  
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

## API Changes (2026-06-04 Ymir Sync) ✅

### SH2 Constructor
**Updated from**:
```cpp
sh2_(scheduler_, bus_, true, features_)  // 4 params: Scheduler, Bus, master, SystemFeatures
```

**To current API**:
```cpp
sh2_(bus_, true)  // 2 params: (SH2Bus&, bool master)
sh2_.BindGlobalCycleCounter(cycleCounter_);
sh2_.BindEmulateCacheOption(useCache_);
```

### Namespace
**Updated from** `brimir::` wrappers to direct `ymir::` usage:
- `brimir::sh2::SH2` → `ymir::sh2::SH2`
- `brimir::sys::SH2Bus` → `ymir::sys::SH2Bus`
- `brimir::core::Scheduler` — removed (decoupled from SH2)
- `brimir::sys::SystemFeatures` — removed (replaced by direct bool members)

### Template Parameters
- `Step<debug, enableCache>()` → `Step<debug, emulateCache>()` (rename only)
- `Advance<debug, enableCache>(cycles)` → `Advance<debug, emulateCache>(cycles)` (rename only)

## Next Steps

1. Update `SystemFeatures` initialization — no longer needed, replaced by `BindEmulateCacheOption()`
2. Replace `std::vector<uint8_t> m_ram` with `std::array` or custom mapper
3. Test compilation and fix remaining API mismatches
4. Verify test runner against new SH2 cycle counts (WB/EX stalls may change timing)

## Build Commands

```bash
# Build WITHOUT JIT tests (default, working)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# Build WITH JIT tests (requires API updates)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_JIT_TESTS=ON
cmake --build build --config Release --target jit-tests
```

