# Brimir Regression Test Framework

## Overview

The regression test framework ensures that fixed bugs don't return and that optimizations don't break existing functionality. Every time a bug is fixed, a regression test should be added.

## Philosophy

**"If it broke once, it will break again."**

Regression tests serve as:
1. **Documentation** of known issues and their fixes
2. **Prevention** of bugs returning
3. **Validation** that optimizations don't break functionality
4. **Safety net** for refactoring

## Regression Test Structure

### File Location
`tests/unit/test_regressions.cpp`

### Test Template
```cpp
TEST_CASE("Regression: [Issue Description]", "[regression][component]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("[Specific behavior]") {
        // Issue: [Description of original bug]
        // Fix: [Description of fix]
        // Date: [When fixed]
        // Files: [Files modified]
        // Source: [Documentation reference if applicable]
        
        // Test implementation
        // Verify bug is fixed and doesn't recur
        REQUIRE(condition);
    }
}
```

### Required Information
Each regression test MUST document:
1. **Issue**: What was broken
2. **Fix**: How it was fixed
3. **Date**: When it was fixed
4. **Files**: What files were modified
5. **Source**: Documentation reference (if hardware issue)

## Current Regression Tests

### VDP2 Bitmap Rendering
- **Issue**: Interlaced field selection was incorrect
- **Fix**: Corrected odd/even field logic
- **Impact**: Bitmap rendering in interlaced modes
- **Tags**: `[regression][vdp2][video]`

### Cache Coherency
- **Issue**: CPU saw stale data after DMA writes
- **Fix**: Proper cache invalidation
- **Impact**: All DMA operations to cached memory
- **Tags**: `[regression][cache][dma]`

### Memory Access
- **Work RAM-L DMA**: Enforced hardware restriction
- **VDP2 DMA Read**: Enforced hardware restriction
- **A-bus DMA Write**: Enforced hardware restriction

### Register Access
- **VDP1 Byte Access**: Enforced word-only writes
- **VDP2 Register Buffer**: Proper V-BLANK timing

### Initialization
- **VDP2 Reset Values**: Registers cleared to 0
- **Cache Configuration**: Proper boot-time setup

### Timing
- **Frame Consistency**: Stable frame timing
- **V-BLANK Timing**: Correct interval

### Save States
- **Size Consistency**: Stable state format
- **Round-Trip**: No corruption

### Performance
- **Baseline**: Minimum performance threshold
- **No Degradation**: Performance monitoring

## Adding New Regression Tests

### When to Add
Add a regression test when:
1. You fix a bug
2. You discover incorrect behavior
3. You implement a hardware restriction
4. An optimization breaks something
5. A user reports an issue

### Process
1. **Document the bug** in the test
2. **Write a failing test** that reproduces the bug
3. **Fix the bug** in the code
4. **Verify the test passes** with the fix
5. **Commit both** fix and test together

### Example Workflow
```bash
# 1. Write failing test
git add tests/unit/test_regressions.cpp
git commit -m "Add regression test for VDP2 interlace bug"

# 2. Fix the bug
git add src/core/src/hw/vdp2/vdp2_render.cpp
git commit -m "Fix VDP2 interlaced field selection"

# 3. Verify test now passes
./brimir_tests "[regression]"

# Or commit together:
git add tests/unit/test_regressions.cpp src/core/src/hw/vdp2/vdp2_render.cpp
git commit -m "Fix VDP2 interlaced field selection + regression test"
```

## Running Regression Tests

### Run All Regression Tests
```bash
./brimir_tests "[regression]"
```

### Run Specific Component
```bash
./brimir_tests "[regression][vdp2]"
./brimir_tests "[regression][cache]"
./brimir_tests "[regression][dma]"
```

### Before Committing
```bash
# Run all tests
./brimir_tests

# Run regression tests specifically
./brimir_tests "[regression]"

# Run with verbose output
./brimir_tests "[regression]" -v
```

### In CI Pipeline
```bash
# Part of CI pipeline
ctest --output-on-failure
./brimir_tests "[regression]" --reporter junit --out regression_results.xml
```

## Test Categories

### By Component
- `[vdp1]` - VDP1 graphics processor
- `[vdp2]` - VDP2 backgrounds
- `[sh2]` - SH-2 CPU
- `[cache]` - Cache system
- `[dma]` - DMA operations
- `[scu]` - System Control Unit
- `[memory]` - Memory system
- `[registers]` - Hardware registers
- `[audio]` - Audio system
- `[input]` - Input handling
- `[savestate]` - Save states
- `[init]` - Initialization
- `[timing]` - Timing behavior
- `[performance]` - Performance

### By Impact
- `[critical]` - Breaks emulation
- `[major]` - Visible issues
- `[minor]` - Edge cases
- `[performance]` - Speed issues

## Best Practices

### DO:
✅ Add regression tests for every bug fix
✅ Document the issue thoroughly
✅ Test the specific failing case
✅ Test edge cases around the bug
✅ Keep tests focused and simple
✅ Use descriptive test names
✅ Tag appropriately for filtering

### DON'T:
❌ Skip regression tests ("I'll add it later")
❌ Write vague test descriptions
❌ Test multiple issues in one test
❌ Make tests dependent on each other
❌ Forget to document the fix
❌ Leave tests as placeholders forever

## Maintenance

### Regular Tasks
1. **Review regression tests** - Ensure all are implemented
2. **Update documentation** - Keep issue descriptions current
3. **Run full suite** - Before releases
4. **Monitor CI** - Watch for new failures
5. **Add tags** - As needed for organization

### When a Regression Occurs
1. **Identify the commit** that broke it
2. **Review the regression test** - why didn't it catch it?
3. **Improve the test** - make it more comprehensive
4. **Fix the bug** again
5. **Verify the fix** with improved test

## Integration with Development

### Pre-Commit Hook (Recommended)
```bash
#!/bin/bash
# .git/hooks/pre-commit

echo "Running regression tests..."
./build/brimir_tests "[regression]" --reporter compact

if [ $? -ne 0 ]; then
    echo "Regression tests failed!"
    echo "Fix the issues or use 'git commit --no-verify' to skip"
    exit 1
fi
```

### Code Review Checklist
- [ ] Bug fix includes regression test
- [ ] Test documents the issue clearly
- [ ] Test would have caught the original bug
- [ ] Test passes with the fix
- [ ] Test is tagged appropriately

## Metrics and Reporting

### Coverage
Track regression test coverage:
- Total bugs fixed: X
- Regression tests added: Y
- Coverage: Y/X * 100%

**Goal:** 100% regression test coverage

### Trends
Monitor:
- Regression test count over time
- Pass/fail rate
- Execution time
- Most common bug categories

### Reporting
```bash
# Generate regression report
./brimir_tests "[regression]" --reporter junit --out regression_report.xml

# Count regression tests
./brimir_tests "[regression]" --list-tests | wc -l

# Show test status
./brimir_tests "[regression]" --reporter compact
```

## Examples

### Example 1: Visual Bug
```cpp
TEST_CASE("Regression: Sprite flicker on hi-res mode", "[regression][vdp1][video]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Sprites render consistently in hi-res") {
        // Issue: Sprites flickered in 640x448 mode
        // Fix: Corrected sprite coordinate scaling
        // Date: January 2025
        // Files: src/core/src/hw/vdp1/vdp1_render.cpp
        
        // TODO: Test hi-res sprite rendering
        // Verify no flicker over multiple frames
        REQUIRE(true);
    }
}
```

### Example 2: Performance Bug
```cpp
TEST_CASE("Regression: Frame rate drop in audio CD mode", "[regression][performance][audio]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Audio CD playback maintains 60fps") {
        // Issue: Frame rate dropped to 30fps during CD playback
        // Fix: Optimized CD-ROM emulation loop
        // Date: January 2025
        // Files: src/core/src/hw/cdblock/cd_drive.cpp
        
        // TODO: Benchmark CD playback
        // Should maintain 60fps minimum
        REQUIRE(true);
    }
}
```

### Example 3: Hardware Accuracy Bug
```cpp
TEST_CASE("Regression: Incorrect cache line size", "[regression][cache][sh2]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Cache invalidation affects 16 bytes") {
        // Issue: Cache invalidation only cleared 4 bytes
        // Fix: Corrected to 16-byte cache line
        // Date: January 2025
        // Files: src/core/src/hw/sh2/sh2_cache.cpp
        // Source: ST-202-R1-120994.pdf
        
        // TODO: Test cache line invalidation
        // Verify 16-byte granularity
        REQUIRE(true);
    }
}
```

## Future Enhancements

### Planned Features
1. **Automated test generation** from bug reports
2. **Visual regression testing** (screenshot comparison)
3. **Performance regression tracking** (automatic benchmarks)
4. **Regression test priority** (critical vs minor)
5. **Historical bug database** integration

### Integration Goals
1. **CI/CD pipeline** - automated testing
2. **Issue tracker** - link tests to bug reports
3. **Code coverage** - ensure tests exercise fix code
4. **Notification system** - alert on new regressions

## Resources

- **Test File**: `tests/unit/test_regressions.cpp`
- **Documentation**: This file
- **Examples**: See existing regression tests
- **Catch2 Docs**: https://github.com/catchorg/Catch2

## Summary

The regression test framework is critical for maintaining emulation quality. Every bug fix should include a regression test. The framework makes it easy to add tests and ensures bugs don't return.

**Key Principle**: If it broke once, test for it forever.

---

**Status**: Framework established
**Tests**: 20+ regression test cases documented
**Coverage Goal**: 100% of fixed bugs
**Next**: Implement placeholder tests

