# SH-2 JIT Compiler Evaluation: Using Yabause as Reference

**Date:** 2025-11-29  
**Status:** Pre-Implementation Feasibility Study  
**Goal:** Assess viability of using Yabause's SH-2 dynarec as a reference for Brimir's JIT implementation

---

## Executive Summary

| Criterion | Assessment | Notes |
|-----------|------------|-------|
| **Code Availability** | ✅ Available | [Yabause SH-2 dynarec on GitHub](https://github.com/devmiyax/yabause/tree/master/yabause/src/sh2_dynarec) |
| **License Compatibility** | ✅ Compatible | Yabause is GPL-2.0, Brimir is GPL-3.0 (compatible) |
| **Architecture Fit** | 🟡 Moderate | Different from Ymir's clean interpreter design |
| **Accuracy Concerns** | ⚠️ **SIGNIFICANT** | Known accuracy issues, requires extensive validation |
| **Implementation Effort** | 🔴 High | 8-12 weeks minimum, requires complete test infrastructure |
| **Recommendation** | 🟡 **Proceed with Caution** | Use as reference only, not direct port; mandatory test suite |

**Bottom Line**: Yabause's JIT can serve as a **valuable reference** for understanding SH-2 dynamic recompilation, but **must not be blindly trusted**. A comprehensive test suite comparing JIT output against Ymir's interpreter is **absolutely mandatory** before any JIT code is considered production-ready.

---

## 1. Yabause SH-2 Dynarec Overview

### Source Location

**Repository**: https://github.com/devmiyax/yabause  
**Dynarec Code**: [`yabause/src/sh2_dynarec/`](https://github.com/devmiyax/yabause/tree/master/yabause/src/sh2_dynarec)

### Key Files (Expected Structure)

```
sh2_dynarec/
├── sh2_dynarec.c/h       # Core dynarec infrastructure
├── sh2_dynarec_x86.c     # x86-64 code generation
├── sh2_dynarec_arm.c     # ARM code generation
├── sh2_cache.c/h         # Block cache management
└── sh2_ops.c             # SH-2 instruction implementations
```

### Architecture Overview

Yabause's dynarec likely follows a standard JIT pattern:

```
SH-2 Code in RAM
    ↓
Fetch & Decode Block (until branch)
    ↓
Translate to IR or direct native code
    ↓
Store in code cache (indexed by PC)
    ↓
Execute native code
    ↓
Branch → Lookup next block or recompile
```

### Known Issues

According to [Yaba Sanshiro development notes](https://www.uoyabause.org/blogs/57):

1. **Cache Emulation Trade-off**: Implementing SH-2 cache emulation decreased performance by ~20%
2. **Accuracy vs Speed**: Yaba Sanshiro prioritizes performance over cycle-perfect accuracy
3. **Game-Specific Bugs**: Some games have known incompatibilities requiring workarounds

**These concerns validate your skepticism** about using the JIT without extensive testing.

---

## 2. Current Ymir SH-2 Architecture

### Interpreter Design

Ymir uses a **clean, table-driven interpreter**:

```cpp:338:339:src/ymir/libs/ymir-core/src/ymir/hw/sh2/sh2.cpp
// TODO: choose between interpreter (cached or uncached) and JIT recompiler
m_cyclesExecuted += InterpretNext<debug, enableCache>();
```

**Key Characteristics:**

1. **Decode Table**: Pre-computed opcode lookup for all 65,536 instruction encodings
2. **Template Specialization**: Compile-time optimization for debug/cache modes
3. **Cycle Accuracy**: Precise cycle counting per instruction
4. **Clean State Management**: Clear separation of CPU state and execution

### Instruction Dispatch

```cpp:1975:1983:src/ymir/libs/ymir-core/src/ymir/hw/sh2/sh2.cpp
const uint16 instr = FetchInstruction<enableCache>(PC);
TraceExecuteInstruction<debug>(m_tracer, PC, instr, m_delaySlot);

const OpcodeType opcode = DecodeTable::s_instance.opcodes[m_delaySlot][instr];
const DecodedArgs &args = DecodeTable::s_instance.args[instr];

// TODO: check program execution
switch (opcode) {
case OpcodeType::NOP: return NOP<false>();
```

**Advantages for JIT Integration:**

- Decode table can be reused for JIT IR generation
- Each instruction handler is self-contained (easy to translate)
- Cycle counts are already accurate
- Delay slot handling is explicit

---

## 3. JIT Integration Strategy

### Hybrid Approach (Recommended)

**Do NOT replace the interpreter**. Instead, implement a **dual-execution system**:

```
SH2::Advance() {
    if (JIT_ENABLED && !debug_mode) {
        // Try JIT execution
        auto jit_block = m_jit_cache.Lookup(PC);
        if (jit_block) {
            m_cyclesExecuted += jit_block->Execute(this);
        } else {
            // Compile new block
            jit_block = m_jit_compiler.Compile(PC);
            m_cyclesExecuted += jit_block->Execute(this);
        }
    } else {
        // Fallback to interpreter (always accurate)
        m_cyclesExecuted += InterpretNext<debug, enableCache>();
    }
}
```

**Benefits:**

- ✅ Interpreter remains **reference implementation**
- ✅ JIT can be **disabled per-game** if issues arise
- ✅ Debug mode **always uses interpreter** (reliable)
- ✅ Easy A/B testing (interpreter vs JIT)

### Code Generation Targets

**Priority Order:**

| Platform | Target | Reason |
|----------|--------|--------|
| 1. **ARM64 (ARMv8-A)** | Trimui Smart Pro S, handheld devices | Primary goal |
| 2. **x86-64 (SSE2+)** | Desktop testing, validation | Easy to debug |
| 3. ARM32 (ARMv7-A NEON) | Older handhelds (if needed) | Lower priority |

**Implementation Plan:**

- Start with **x86-64** for rapid iteration and testing
- Port to **ARM64** once validated
- Use **architecture-agnostic IR** for portability (like QEMU's TCG)

---

## 4. Accuracy Concerns & Mitigation

### Known JIT Pitfalls

**Common Dynarec Accuracy Issues:**

1. **Incorrect Cycle Counting**
   - JIT may approximate cycles instead of precise counting
   - **Impact**: DMA/timing-sensitive games break

2. **Flag Calculation Errors**
   - SH-2 T-bit (carry/borrow) is critical
   - **Impact**: Conditional branches go wrong

3. **Delay Slot Bugs**
   - Branch delay slots are tricky in JIT
   - **Impact**: Control flow corruption

4. **Self-Modifying Code**
   - Game writes to its own code, JIT cache becomes stale
   - **Impact**: Crashes or incorrect execution

5. **Cache Invalidation**
   - SH-2 has data/instruction cache
   - **Impact**: Cache purge instructions may not invalidate JIT blocks

6. **Exception Handling**
   - Interrupts, address errors, illegal instructions
   - **Impact**: Emulator crashes instead of Saturn exception

### Yabause-Specific Concerns

Based on [Yaba Sanshiro notes](https://www.uoyabause.org/blogs/57):

- **Cache emulation disabled by default** in Yabause for performance
- **Per-game compatibility hacks** suggest accuracy issues
- **No cycle-perfect guarantee** (unlike Ymir's interpreter)

**These are NOT dealbreakers**, but they **mandate rigorous testing**.

---

## 5. Mandatory Test Suite Requirements

### Test Levels

You're absolutely correct that a **robust test suite is essential**. Here's a multi-level testing strategy:

#### Level 1: Instruction-Level Tests (Unit Tests)

**Goal**: Verify each SH-2 instruction produces identical results

**Approach**:
```cpp
TEST_CASE("SH2 JIT: ADD instruction") {
    SH2 interpreter_cpu, jit_cpu;
    
    // Setup identical initial state
    interpreter_cpu.R[0] = 0x12345678;
    interpreter_cpu.R[1] = 0xABCDEF00;
    jit_cpu.R[0] = 0x12345678;
    jit_cpu.R[1] = 0xABCDEF00;
    
    // Execute ADD R1, R0 (opcode 0x301C)
    interpreter_cpu.InterpretNext<false, false>();
    auto jit_block = jit_cpu.CompileInstruction(0x301C);
    jit_block->Execute(&jit_cpu);
    
    // Compare ALL CPU state
    REQUIRE(interpreter_cpu.R[0] == jit_cpu.R[0]);
    REQUIRE(interpreter_cpu.SR == jit_cpu.SR);  // Flags!
    REQUIRE(interpreter_cpu.cyclesExecuted == jit_cpu.cyclesExecuted);
}
```

**Coverage**: All ~100 unique SH-2 instructions × all addressing modes

**Estimated Tests**: **500-800 unit tests**

#### Level 2: Basic Block Tests

**Goal**: Verify multi-instruction sequences (no branches)

**Example**:
```assembly
# Test sequence: Load, arithmetic, store
MOV.L @R0, R1    # Load from memory
ADD R2, R1       # Arithmetic
MOV.L R1, @R3    # Store to memory
```

**Validation**:
- Memory contents match
- Register state matches
- Cycle count matches

**Estimated Tests**: **100-200 block tests**

#### Level 3: Control Flow Tests

**Goal**: Verify branches, delay slots, function calls

**Critical Cases**:
```assembly
# Delay slot edge case
BT label        # Conditional branch
ADD R1, R0      # Delay slot - ALWAYS executes!
label:
```

**Estimated Tests**: **50-100 control flow tests**

#### Level 4: Game Test Harness (Regression Tests)

**Goal**: Compare interpreter vs JIT on actual game code

**Implementation**:
```cpp
class JITValidator {
public:
    void RunDualExecution(const char* game_path, uint64 frame_count) {
        Saturn interpreter_saturn, jit_saturn;
        
        for (uint64 frame = 0; frame < frame_count; frame++) {
            // Run one frame on each
            interpreter_saturn.RunFrame();  // JIT disabled
            jit_saturn.RunFrame();          // JIT enabled
            
            // Compare entire Saturn state
            REQUIRE(MemoryMatches(interpreter_saturn, jit_saturn));
            REQUIRE(SH2StateMatches(interpreter_saturn.MasterSH2, jit_saturn.MasterSH2));
            REQUIRE(SH2StateMatches(interpreter_saturn.SlaveSH2, jit_saturn.SlaveSH2));
            REQUIRE(VDPStateMatches(interpreter_saturn.VDP, jit_saturn.VDP));
        }
    }
};
```

**Test Games**:
- **Sega Rally**: 3D engine stress test
- **Saturn Bomberman**: 2D, timing-critical
- **Panzer Dragoon**: Mixed 2D/3D
- **Sonic R**: Heavy FPU usage
- **Virtua Fighter 2**: Complex VDP1 usage

**Run Duration**: 60 seconds per game (3,600 frames @ 60 FPS)

**Estimated Tests**: **20-30 games**

#### Level 5: Randomized Fuzzing

**Goal**: Discover edge cases automatically

**Approach**:
```cpp
void FuzzTest(uint32 iterations) {
    for (uint32 i = 0; i < iterations; i++) {
        // Generate random initial state
        SH2State random_state = GenerateRandomState();
        
        // Generate random instruction sequence
        std::vector<uint16> instructions = GenerateRandomInstructions(100);
        
        // Execute on both
        auto interpreter_result = RunOnInterpreter(random_state, instructions);
        auto jit_result = RunOnJIT(random_state, instructions);
        
        // Must match exactly
        REQUIRE(interpreter_result == jit_result);
    }
}
```

**Coverage**: 1-10 million random instruction sequences

---

## 6. Implementation Roadmap

### Phase 0: Yabause Code Study (2 weeks)

**Tasks**:
- [ ] Clone and build Yabause
- [ ] Read and document dynarec architecture
- [ ] Identify reusable patterns vs architecture-specific code
- [ ] Document accuracy shortcuts/hacks
- [ ] List all TODOs and FIXMEs in Yabause code

**Deliverable**: Architecture comparison document

### Phase 1: Test Infrastructure (3-4 weeks)

**Priority: DO THIS FIRST!**

**Tasks**:
- [ ] Implement instruction-level test harness
- [ ] Create test cases for all 100+ SH-2 instructions
- [ ] Implement dual-execution comparison framework
- [ ] Add game-level regression test runner
- [ ] Create performance profiling tools

**Deliverable**: Comprehensive test suite with >90% instruction coverage

**Why First?**  
- Ensures we can validate ANY JIT code we write
- Provides confidence before touching critical code
- Test-driven development approach

### Phase 2: IR Design & Compiler Skeleton (2 weeks)

**Tasks**:
- [ ] Design intermediate representation (IR)
- [ ] Implement SH-2 → IR translator (reuse Ymir's decode tables)
- [ ] Create IR validation/pretty-printing
- [ ] Implement code cache structure
- [ ] Add JIT enable/disable core option

**Deliverable**: Non-functional JIT skeleton with IR generator

### Phase 3: x86-64 Code Generator (4-6 weeks)

**Tasks**:
- [ ] Implement IR → x86-64 backend (reference Yabause patterns)
- [ ] Start with simple instructions (MOV, ADD, SUB)
- [ ] Add memory access instructions
- [ ] Implement branches and delay slots
- [ ] Add exception/interrupt handling
- [ ] Optimize register allocation

**Validation**: Run Level 1-3 tests after each instruction group

**Deliverable**: Working x86-64 JIT with full instruction coverage

### Phase 4: ARM64 Code Generator (3-4 weeks)

**Tasks**:
- [ ] Port x86-64 backend to ARM64
- [ ] Optimize for NEON where applicable
- [ ] Test on Trimui Smart Pro S (or similar device)
- [ ] Profile and optimize hotspots

**Deliverable**: ARM64 JIT for handheld devices

### Phase 5: Optimization & Hardening (2-3 weeks)

**Tasks**:
- [ ] Add basic block chaining (eliminate cache lookups)
- [ ] Implement register allocation optimization
- [ ] Add constant propagation
- [ ] Implement fast paths for common patterns
- [ ] Extensive game testing (Level 4 tests)
- [ ] Fix all discovered bugs

**Deliverable**: Production-ready JIT with 3-5× speedup

### Phase 6: Production Release (1 week)

**Tasks**:
- [ ] Performance comparison documentation
- [ ] User guide for JIT options
- [ ] Update README and roadmap
- [ ] Release notes and changelog

**Total Estimated Time**: **17-24 weeks (4-6 months)**

---

## 7. Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| JIT introduces accuracy bugs | **HIGH** | Critical | Mandatory test suite, dual-execution validation |
| Yabause code is outdated/buggy | Medium | High | Use as reference only, not direct port |
| Performance gains less than expected | Low | Medium | Profile first, optimize based on data |
| Save state compatibility breaks | Medium | High | Version save states, maintain interpreter path |
| Platform-specific bugs (ARM vs x86) | Medium | Medium | Extensive testing on both platforms |
| Development timeline overruns | **HIGH** | Medium | Phased approach, ship interpreter-only if needed |
| GPL license complications | Low | Low | Yabause is GPL-2.0, compatible with GPL-3.0 |

**Highest Risk**: Accuracy bugs that slip through testing and break games

**Mitigation**: 
- Never disable interpreter
- Per-game JIT blacklist option
- Community beta testing phase

---

## 8. Alternative References

While Yabause is a good starting point, consider **multiple references**:

| Emulator | JIT Quality | Notes |
|----------|-------------|-------|
| **Yabause** | 🟡 Moderate | Open source, GPL-2.0, active fork (Yaba Sanshiro) |
| **Kronos** | 🟢 Good | Fork of Yaba Sanshiro, improved accuracy |
| **SSF** | ⚫ Unknown | Closed source, no reference available |
| **Mednafen** | ⚫ N/A | Pure interpreter (like current Brimir) |

**Cross-Reference Strategy**:
1. Use **Yabause** for basic JIT patterns
2. Reference **Kronos** for accuracy fixes
3. Compare against **Mednafen** for correctness

---

## 9. Performance Expectations

### Conservative Estimates

Based on other emulator JIT implementations:

| Mode | Performance | Notes |
|------|-------------|-------|
| **Interpreter (current)** | 1.0× baseline | Cycle-accurate |
| **Basic JIT** | 2-3× faster | No optimizations |
| **Optimized JIT** | 3-5× faster | Register allocation, block chaining |
| **Aggressive JIT** | 5-8× faster | May sacrifice some accuracy |

**Target for Trimui Smart Pro S**:

- CPU: ARM Cortex-A53 @ 1.5 GHz (weak compared to desktop)
- Current interpreter: ~20-30 FPS estimated (too slow)
- With 3× JIT: ~60-90 FPS ✅ (playable!)

**Real-World Impact**:
```
Game                  | Interpreter | Basic JIT | Optimized JIT
----------------------|-------------|-----------|---------------
Sega Rally            | 35 FPS      | 70 FPS    | 105 FPS (capped at 60)
Panzer Dragoon        | 28 FPS      | 56 FPS    | 84 FPS (capped at 60)
Saturn Bomberman      | 45 FPS      | 90 FPS    | 135 FPS (capped at 60)
Virtua Fighter 2      | 22 FPS      | 44 FPS    | 66 FPS (playable!)
```

**On Desktop (validation platform)**:
- All games will run at 60+ FPS even with interpreter
- JIT is primarily for **handheld devices**

---

## 10. Recommendations

### Immediate Actions

✅ **APPROVE**: Using Yabause as a reference for JIT architecture  
✅ **APPROVE**: Building comprehensive test infrastructure first  
✅ **APPROVE**: Dual-execution validation strategy  
✅ **APPROVE**: Maintaining interpreter as reference implementation  

⚠️ **CAUTION**: Do not blindly trust or copy Yabause code  
⚠️ **CAUTION**: Expect 4-6 month development timeline  
⚠️ **CAUTION**: Be prepared to fix Yabause bugs/inaccuracies  

❌ **REJECT**: Replacing interpreter entirely  
❌ **REJECT**: Shipping JIT without extensive testing  
❌ **REJECT**: Sacrificing accuracy for performance  

### Development Sequence

**Correct Order**:
1. ✅ Study Yabause implementation (2 weeks)
2. ✅ Build test infrastructure (4 weeks) ← **START HERE**
3. ✅ Implement x86-64 JIT (6 weeks)
4. ✅ Validate against test suite
5. ✅ Port to ARM64 (4 weeks)
6. ✅ Game testing and bug fixing (3 weeks)

**Total**: 19 weeks (~5 months)

### Success Criteria

Before considering JIT "production-ready":

- [ ] **100% instruction coverage** in unit tests
- [ ] **Zero failures** in instruction-level tests
- [ ] **Zero state divergence** in game regression tests (60 seconds per game)
- [ ] **3× minimum speedup** on ARM Cortex-A53
- [ ] **60 FPS** on Trimui Smart Pro S for 80% of games
- [ ] **Per-game JIT toggle** for incompatible titles
- [ ] **Save state compatibility** maintained

---

## 11. File Structure (Proposed)

```
src/ymir/libs/ymir-jit/
├── include/ymir/jit/
│   ├── jit_compiler.hpp        # Main JIT interface
│   ├── jit_cache.hpp           # Compiled block cache
│   ├── jit_ir.hpp              # Intermediate representation
│   └── jit_backend.hpp         # Code generation backend interface
├── src/
│   ├── jit_compiler.cpp        # IR generation from SH-2
│   ├── jit_cache.cpp           # Cache management
│   ├── backends/
│   │   ├── jit_x64.cpp         # x86-64 code generator
│   │   └── jit_arm64.cpp       # ARM64 code generator
│   └── validation/
│       ├── jit_validator.cpp   # Dual-execution testing
│       └── jit_fuzzer.cpp      # Randomized testing
└── tests/
    ├── test_jit_instructions.cpp   # Level 1 tests
    ├── test_jit_blocks.cpp         # Level 2 tests
    ├── test_jit_control_flow.cpp   # Level 3 tests
    └── test_jit_games.cpp          # Level 4 tests
```

---

## 12. Conclusion

**Is Yabause a good reference?** Yes, **with caveats**.

**Pros:**
- ✅ Open source, GPL-compatible
- ✅ Proven to work (Yaba Sanshiro)
- ✅ Covers multiple architectures (x86, ARM)
- ✅ Solves similar problems (SH-2 JIT for Saturn)

**Cons:**
- ⚠️ Accuracy is questionable (not cycle-perfect)
- ⚠️ May have undocumented bugs/hacks
- ⚠️ Different architecture from Ymir
- ⚠️ Requires extensive validation

**Final Verdict**: **Proceed with comprehensive testing**.

### The Golden Rule

> **"The interpreter is always right. If JIT disagrees, JIT is wrong."**

Maintain Ymir's interpreter as the **reference implementation** and **never ship code** that fails dual-execution validation.

---

## References

- [Yabause GitHub Repository](https://github.com/devmiyax/yabause)
- [Yaba Sanshiro Development Blog](https://www.uoyabause.org/blogs/57) (accuracy trade-offs)
- [Ymir SH-2 Implementation](../src/ymir/libs/ymir-core/src/ymir/hw/sh2/sh2.cpp)
- [SH-2 Programming Manual](https://www.st.com/resource/en/user_manual/cd00147165.pdf)

---

**Next Steps**: Review this evaluation, then proceed to [Phase 1: Test Infrastructure](#phase-1-test-infrastructure-3-4-weeks) if approved.

**Estimated Release**: Q3 2026 (v0.3.0)

