# Brimir Optimization Guide

**Test Suite Status:** ✅ Ready (77% Coverage, 310 Tests)  
**Safe to Optimize:** Yes  
**Last Updated:** December 15, 2025

---

## Quick Start

```bash
# 1. Pick an optimization target from below
# 2. Run baseline tests (if Catch2 available)
# 3. Make your changes
# 4. Run tests again
# 5. If tests pass → Ship it! ✅
```

---

## Optimization Targets by Priority

### 🔥 Priority 1: High Impact, Well Tested

#### 1. VDP Rendering Pipeline
**Why:** Major performance bottleneck, 11 tests cover it  
**Test File:** `tests/unit/test_vdp_components.cpp`  
**What to Optimize:**
- Pixel format conversion (RGB565/XRGB8888)
- SIMD usage in framebuffer operations
- Deinterlacing algorithms
- Resolution switching overhead

**Protected By:**
- Frame generation tests (11 tests)
- Framebuffer consistency tests
- Resolution reporting tests
- Integration with video standards

**Risk Level:** 🟢 Low - Well tested

---

#### 2. SH-2 Instruction Execution
**Why:** CPU is the heart of emulation, 17 tests cover it  
**Test File:** `tests/unit/test_sh2_cpu.cpp`  
**What to Optimize:**
- Interpreter loop optimization
- Cache behavior improvements
- Branch prediction
- JIT compiler enhancements

**Protected By:**
- Master/Slave CPU tests (17 tests)
- Cache operation tests
- Execution stability tests
- Extended run tests (300 frames)

**Risk Level:** 🟢 Low - Well tested

---

#### 3. Memory Access Patterns
**Why:** Memory bandwidth is critical, 17 tests cover it  
**Test File:** `tests/unit/test_memory_components.cpp`  
**What to Optimize:**
- Work RAM access patterns
- Cache line utilization
- Memory alignment
- Bus timing simulation

**Protected By:**
- Memory read/write tests (17 tests)
- Boundary condition tests
- Alignment verification tests
- State persistence tests

**Risk Level:** 🟢 Low - Well tested

---

#### 4. Audio Processing (SCSP)
**Why:** Audio generation can be optimized, 11 tests cover it  
**Test File:** `tests/unit/test_scsp_audio.cpp`  
**What to Optimize:**
- DSP operations
- Sample interpolation
- Buffer management
- Ring buffer efficiency

**Protected By:**
- Audio generation tests (11 tests)
- Buffer safety tests
- Interpolation mode tests
- Continuous generation tests

**Risk Level:** 🟢 Low - Well tested

---

### 🎯 Priority 2: Good Impact, Adequate Tests

#### 5. System Configuration & State Management
**Why:** Save/load and mode switching can be faster, 15 tests cover it  
**Test File:** `tests/unit/test_system_integration.cpp`  
**What to Optimize:**
- State serialization/deserialization
- Video standard switching
- Reset handling efficiency
- Configuration changes

**Protected By:**
- Integration tests (15 tests)
- Save/load cycle tests
- Configuration persistence tests
- Reset behavior tests

**Risk Level:** 🟡 Medium - Adequate testing

---

#### 6. CD-ROM Operations
**Why:** Disc access can be optimized, 10 tests cover it  
**Test File:** `tests/unit/test_cd_operations.cpp`  
**What to Optimize:**
- Read speed multiplier
- Buffer management
- Seek optimization
- Cache prefetching

**Protected By:**
- CD operation tests (10 tests)
- Tray state tests
- Component access tests
- DRAM tests

**Risk Level:** 🟡 Medium - Adequate testing

---

### ⚠️ Priority 3: Consider Adding Tests First

#### 7. SCU DMA Operations
**Why:** DMA is powerful but only 5 tests cover it  
**Test File:** `tests/unit/test_scu_components.cpp`  
**What to Optimize:**
- DMA transfer efficiency
- Channel scheduling
- Memory copy operations

**Protected By:**
- Basic DMA state tests (5 tests)

**Risk Level:** 🟠 Medium-High - Add more tests first

**Recommendation:** Add specific DMA tests before major optimization

---

## Optimization Workflow

### Step-by-Step Process

```bash
# 1. Establish Baseline
git checkout -b optimize-vdp-rendering
cmake -B build -DBRIMIR_BUILD_TESTS=ON
cmake --build build --target brimir_tests
./build/brimir_tests --benchmark  # If available

# 2. Profile Current Performance
# Use your profiler of choice to identify hotspots

# 3. Make Optimization Changes
# Edit source files

# 4. Rebuild and Test
cmake --build build --target brimir_libretro
./build/brimir_tests

# 5. Compare Performance
# Run benchmarks again

# 6. If Tests Pass + Performance Improves
git add -A
git commit -m "Optimize: [description]"
git push
```

---

## Common Optimization Scenarios

### Scenario 1: Optimizing Framebuffer Operations

**Goal:** Reduce framebuffer conversion overhead  
**Files to Edit:** `src/core/src/brimir/hw/vdp/*`  
**Tests to Run:** `test_vdp_components.cpp`

**Example Optimizations:**
- Use SIMD for pixel format conversion
- Batch framebuffer operations
- Optimize deinterlacing algorithm

**Validation:**
```bash
./build/brimir_tests "[vdp]"
```

---

### Scenario 2: Optimizing SH-2 Interpreter

**Goal:** Speed up instruction execution  
**Files to Edit:** `src/core/src/brimir/hw/sh2/*`  
**Tests to Run:** `test_sh2_cpu.cpp`, `test_system_integration.cpp`

**Example Optimizations:**
- Optimize hot path instructions
- Improve cache behavior
- Reduce branch mispredictions

**Validation:**
```bash
./build/brimir_tests "[sh2]"
./build/brimir_tests "[system][execution]"
```

---

### Scenario 3: Optimizing Memory Access

**Goal:** Reduce memory access overhead  
**Files to Edit:** `src/core/src/brimir/sys/memory.cpp`  
**Tests to Run:** `test_memory_components.cpp`, `test_system_integration.cpp`

**Example Optimizations:**
- Improve cache line utilization
- Optimize alignment
- Batch memory operations

**Validation:**
```bash
./build/brimir_tests "[memory]"
./build/brimir_tests "[system][integration]"
```

---

### Scenario 4: Optimizing Audio Generation

**Goal:** Reduce audio processing overhead  
**Files to Edit:** `src/core/src/brimir/hw/scsp/*`  
**Tests to Run:** `test_scsp_audio.cpp`

**Example Optimizations:**
- SIMD for interpolation
- Optimize ring buffer
- Batch sample generation

**Validation:**
```bash
./build/brimir_tests "[scsp]"
```

---

## Performance Measurement

### Built-in Profiling

The CoreWrapper has built-in profiling:

```cpp
CoreWrapper core;
core.Initialize();

// Run workload
for (int i = 0; i < 1000; ++i) {
    core.RunFrame();
}

// Get report
std::string report = core.GetProfilingReport();
std::cout << report << std::endl;

// Reset for next run
core.ResetProfiling();
```

### External Tools

**Recommended Profilers:**
- **Windows:** Visual Studio Profiler, Intel VTune
- **Linux:** perf, Valgrind (cachegrind)
- **Cross-platform:** Tracy Profiler

---

## Optimization Checklist

Before optimizing:
- [ ] Identify hotspot with profiler
- [ ] Check test coverage for that area
- [ ] Establish performance baseline
- [ ] Create optimization branch

During optimization:
- [ ] Make incremental changes
- [ ] Test after each change
- [ ] Measure performance impact
- [ ] Document optimization rationale

After optimization:
- [ ] All tests pass
- [ ] Performance improved (measured)
- [ ] No regressions in other areas
- [ ] Code remains readable

---

## Red Flags 🚩

**Stop and add tests if:**
- You're changing code with <50% test coverage
- Tests don't exist for the feature you're optimizing
- You're unsure if tests cover your changes
- You've broken tests and are tempted to "fix" them

**Good practice:**
- Add tests → Optimize → Validate
- Don't "fix" tests to match new behavior without investigation

---

## Example Optimizations

### Example 1: SIMD Pixel Conversion

```cpp
// Before:
for (size_t i = 0; i < pixels; ++i) {
    output[i] = ConvertXRGB8888toRGB565(input[i]);
}

// After:
#ifdef __SSE2__
for (size_t i = 0; i < pixels; i += 4) {
    __m128i xrgb = _mm_loadu_si128((__m128i*)&input[i]);
    __m128i rgb565 = ConvertXRGB8888toRGB565_SIMD(xrgb);
    _mm_storeu_si128((__m128i*)&output[i], rgb565);
}
#endif
```

**Tests to validate:** `test_vdp_components.cpp` - framebuffer tests

---

### Example 2: Cache Line Optimization

```cpp
// Before: Poor cache locality
struct Data {
    uint32_t field1;  // Hot
    uint32_t field2;  // Hot
    uint32_t field3;  // Cold
    uint32_t field4;  // Cold
};

// After: Group hot data
struct Data {
    // Hot path (1 cache line)
    uint32_t field1;
    uint32_t field2;
    
    // Cold path (separate cache line)
    uint32_t field3;
    uint32_t field4;
} __attribute__((aligned(64)));
```

**Tests to validate:** `test_memory_components.cpp` - alignment tests

---

## Saturn-Specific Optimization Tips

### From Official Documentation

**Memory Access (Source: Saturn Hardware Manual)**
- Work RAM High is faster than Work RAM Low
- Cache line size: 16 bytes (4 longwords)
- Burst mode: 4-longword aligned access is optimal

**VDP Rendering (Source: VDP1/VDP2 Manuals)**
- VDP1 VRAM: 512KB, 16-bit bus
- VDP2 VRAM: 512KB, 32-bit bus
- Framebuffer: Double-buffered, swap on VBLANK

**SH-2 Cache (Source: SH-2 Programming Manual)**
- 4KB 4-way set-associative cache
- 16-byte cache lines
- Write-back policy (when cache enabled)

**Use `query_saturn.py` for detailed specs:**
```bash
python query_saturn.py "VDP1 framebuffer timing"
python query_saturn.py "SH-2 cache behavior"
python query_saturn.py "Saturn memory map"
```

---

## Getting Help

**If you're unsure:**
1. Check test coverage: `tests/TEST_SUITE_STATUS.md`
2. Run relevant tests before/after changes
3. Profile to verify improvement
4. Commit incrementally

**If tests fail:**
1. Don't immediately "fix" the test
2. Understand why it's failing
3. Is it a regression or a test issue?
4. Ask: "Is my optimization changing behavior?"

---

## Summary

**You have excellent test coverage (77%) and can optimize confidently!**

**Best Starting Points:**
1. VDP rendering (11 tests) 🔥
2. SH-2 execution (17 tests) 🔥
3. Memory access (17 tests) 🔥
4. Audio processing (11 tests) 🔥

**Remember:**
- Tests are your safety net
- Profile first, optimize second
- Measure improvements
- Keep changes incremental

**Now go make Brimir fast! 🚀**

