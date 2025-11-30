# SH-2 JIT Implementation Roadmap

**Branch:** `feature/sh2-jit`  
**Start Date:** 2025-11-29  
**Target Release:** v0.3.0 (Q3 2026)  
**Status:** Phase 0 - Planning & Research

---

## Project Goals

1. **Primary**: Enable 60 FPS Saturn emulation on entry-level ARM handhelds (Trimui Smart Pro S)
2. **Secondary**: 3-5× performance improvement over interpreter
3. **Critical**: Maintain 100% accuracy via dual-execution validation
4. **Architecture**: Hybrid interpreter/JIT (never replace interpreter)

---

## Phase Checklist

### ✅ Phase 0: Planning & Research (Week 1-2)

**Status**: In Progress  
**Duration**: 2 weeks  
**Started**: 2025-11-29

**Tasks**:
- [x] Create evaluation document
- [x] Create branch `feature/sh2-jit`
- [x] Set up directory structure
- [ ] Clone and build Yabause
- [ ] Study Yabause SH-2 dynarec architecture
- [ ] Document reusable patterns
- [ ] Identify accuracy shortcuts in Yabause
- [ ] List all Yabause TODOs/FIXMEs
- [ ] Create architecture comparison doc

**Deliverables**:
- [x] `docs/SH2_JIT_EVALUATION.md`
- [ ] `docs/YABAUSE_ARCHITECTURE_NOTES.md`
- [ ] `docs/JIT_DESIGN_DECISIONS.md`

---

### ⏳ Phase 1: Test Infrastructure (Week 3-6)

**Status**: Not Started  
**Duration**: 4 weeks  
**Priority**: CRITICAL - Do this before any JIT code!

**Tasks**:
- [ ] Design dual-execution test framework
- [ ] Implement instruction-level test harness
- [ ] Create SH-2 state comparison utilities
- [ ] Build test case generator
- [ ] Implement Level 1: Instruction tests (500-800 tests)
  - [ ] Data movement instructions (MOV family)
  - [ ] Arithmetic instructions (ADD, SUB, MUL, DIV)
  - [ ] Logic instructions (AND, OR, XOR, NOT)
  - [ ] Shift/rotate instructions
  - [ ] Branch instructions (BT, BF, BRA, BSR, JMP, JSR, RTS)
  - [ ] Delay slot handling
  - [ ] MAC instructions
  - [ ] System instructions (SLEEP, RTE, TRAPA)
- [ ] Implement Level 2: Basic block tests (100-200 tests)
- [ ] Implement Level 3: Control flow tests (50-100 tests)
- [ ] Implement Level 4: Game regression framework
- [ ] Implement Level 5: Fuzzing infrastructure
- [ ] Add performance profiling tools
- [ ] Document test coverage metrics

**Deliverables**:
- [ ] `src/ymir/libs/ymir-jit/tests/test_jit_instructions.cpp`
- [ ] `src/ymir/libs/ymir-jit/tests/test_jit_blocks.cpp`
- [ ] `src/ymir/libs/ymir-jit/tests/test_jit_control_flow.cpp`
- [ ] `src/ymir/libs/ymir-jit/tests/test_jit_games.cpp`
- [ ] `src/ymir/libs/ymir-jit/src/validation/jit_validator.cpp`
- [ ] `src/ymir/libs/ymir-jit/src/validation/jit_fuzzer.cpp`
- [ ] Test coverage report (target: >90% instruction coverage)

**Success Criteria**:
- All 100+ SH-2 instructions have unit tests
- Dual-execution framework validates full CPU state
- Game regression tests can run 60-second comparisons
- Fuzzer can generate and validate random instruction sequences

---

### ⏳ Phase 2: IR Design & Compiler Skeleton (Week 7-8)

**Status**: Not Started  
**Duration**: 2 weeks  
**Depends On**: Phase 1 (test infrastructure)

**Tasks**:
- [ ] Design intermediate representation (IR)
  - [ ] IR instruction set definition
  - [ ] IR operand types
  - [ ] IR control flow primitives
  - [ ] IR metadata for debugging
- [ ] Implement SH-2 → IR translator
  - [ ] Reuse Ymir's decode tables
  - [ ] Handle delay slots in IR
  - [ ] Map SH-2 registers to IR
- [ ] Create IR validation/verification
- [ ] Implement IR pretty-printing
- [ ] Design code cache structure
  - [ ] Block lookup by PC
  - [ ] Cache invalidation strategy
  - [ ] Self-modifying code detection
- [ ] Add JIT enable/disable core option
- [ ] Implement hybrid SH2::Advance() dispatcher

**Deliverables**:
- [ ] `src/ymir/libs/ymir-jit/include/ymir/jit/jit_ir.hpp`
- [ ] `src/ymir/libs/ymir-jit/include/ymir/jit/jit_compiler.hpp`
- [ ] `src/ymir/libs/ymir-jit/include/ymir/jit/jit_cache.hpp`
- [ ] `src/ymir/libs/ymir-jit/src/jit_compiler.cpp`
- [ ] `src/ymir/libs/ymir-jit/src/jit_cache.cpp`
- [ ] Modified `src/ymir/libs/ymir-core/src/ymir/hw/sh2/sh2.cpp` (hybrid dispatcher)

**Success Criteria**:
- IR can represent all SH-2 instructions
- SH-2 → IR translation is lossless
- IR can be pretty-printed for debugging
- Code cache can store/retrieve blocks by PC
- JIT can be toggled via core option

---

### ⏳ Phase 3: x86-64 Code Generator (Week 9-14)

**Status**: Not Started  
**Duration**: 6 weeks  
**Depends On**: Phase 2 (IR design)

**Tasks**:
- [ ] Implement IR → x86-64 backend base
- [ ] Week 1: Simple instructions
  - [ ] MOV, ADD, SUB, AND, OR, XOR
  - [ ] Register allocation basics
  - [ ] Test against Level 1 tests
- [ ] Week 2: Memory access
  - [ ] Load/store instructions
  - [ ] Address calculation
  - [ ] Memory barrier handling
  - [ ] Test against Level 1 tests
- [ ] Week 3: Branches and delay slots
  - [ ] Conditional branches
  - [ ] Delay slot implementation
  - [ ] Block chaining
  - [ ] Test against Level 3 tests
- [ ] Week 4: Complex instructions
  - [ ] MAC instructions
  - [ ] Division
  - [ ] Multiply-accumulate
  - [ ] Test against Level 1 tests
- [ ] Week 5: Exception/interrupt handling
  - [ ] Exception detection
  - [ ] Interrupt checking
  - [ ] Fallback to interpreter
  - [ ] Test against all levels
- [ ] Week 6: Register allocation optimization
  - [ ] Live range analysis
  - [ ] Spill code generation
  - [ ] Full optimization pass
  - [ ] Test against all levels + games

**Deliverables**:
- [ ] `src/ymir/libs/ymir-jit/src/backends/jit_x64.cpp`
- [ ] `src/ymir/libs/ymir-jit/include/ymir/jit/jit_backend.hpp`
- [ ] All Level 1-3 tests passing at 100%
- [ ] Initial game compatibility testing

**Success Criteria**:
- All instruction tests pass
- All control flow tests pass
- Basic block tests pass
- At least 3 games run identically to interpreter
- Measurable speedup (>2×) on x86-64 desktop

---

### ⏳ Phase 4: ARM64 Code Generator (Week 15-18)

**Status**: Not Started  
**Duration**: 4 weeks  
**Depends On**: Phase 3 (x86-64 backend validated)

**Tasks**:
- [ ] Port x86-64 backend structure to ARM64
- [ ] Implement ARM64 instruction encoding
- [ ] ARM64 register allocation
- [ ] NEON optimization where applicable
- [ ] Cross-compile for ARM64
- [ ] Test on Trimui Smart Pro S (or similar)
- [ ] Profile performance on target hardware
- [ ] Optimize hot paths identified in profiling

**Deliverables**:
- [ ] `src/ymir/libs/ymir-jit/src/backends/jit_arm64.cpp`
- [ ] ARM64 build configuration
- [ ] Performance benchmarks on Trimui Smart Pro S

**Success Criteria**:
- All tests pass on ARM64
- 60 FPS on Trimui Smart Pro S for target games
- 3-5× speedup over interpreter on ARM
- Identical behavior to x86-64 JIT

---

### ⏳ Phase 5: Optimization & Hardening (Week 19-21)

**Status**: Not Started  
**Duration**: 3 weeks  
**Depends On**: Phase 4 (ARM64 working)

**Tasks**:
- [ ] Implement basic block chaining
- [ ] Advanced register allocation
- [ ] Constant propagation
- [ ] Dead code elimination
- [ ] Common subexpression elimination
- [ ] Fast paths for hot loops
- [ ] Extensive game testing (20-30 games)
- [ ] Bug fixing based on game tests
- [ ] Performance tuning
- [ ] Documentation updates

**Deliverables**:
- [ ] Optimized JIT with full test coverage
- [ ] Game compatibility list
- [ ] Performance comparison report
- [ ] User documentation

**Success Criteria**:
- All games tested run at 60+ FPS on target hardware
- Zero state divergence in 60-second dual-execution tests
- 3-5× minimum speedup achieved
- Per-game JIT blacklist for edge cases
- Production-ready code quality

---

### ⏳ Phase 6: Production Release (Week 22)

**Status**: Not Started  
**Duration**: 1 week  
**Depends On**: Phase 5 (optimization complete)

**Tasks**:
- [ ] Final testing sweep
- [ ] Update README.md
- [ ] Update ROADMAP.md
- [ ] Update CHANGELOG.md
- [ ] Create release notes
- [ ] Merge to master
- [ ] Tag v0.3.0
- [ ] Create GitHub release
- [ ] Build and upload artifacts

**Deliverables**:
- [ ] v0.3.0 release with SH-2 JIT
- [ ] Complete documentation
- [ ] Windows and Linux builds
- [ ] User guide for JIT options

**Success Criteria**:
- Release builds successfully
- Documentation is complete and accurate
- Community testing begins

---

## Current Status Summary

**Overall Progress**: 2% (Planning complete)

| Phase | Status | Progress |
|-------|--------|----------|
| Phase 0: Planning | 🟢 In Progress | 60% |
| Phase 1: Test Infrastructure | ⚪ Not Started | 0% |
| Phase 2: IR Design | ⚪ Not Started | 0% |
| Phase 3: x86-64 Backend | ⚪ Not Started | 0% |
| Phase 4: ARM64 Backend | ⚪ Not Started | 0% |
| Phase 5: Optimization | ⚪ Not Started | 0% |
| Phase 6: Release | ⚪ Not Started | 0% |

**Estimated Completion**: Q3 2026 (22 weeks from start)

---

## Key Decisions Log

### 2025-11-29: Hybrid Architecture Approved
- **Decision**: Keep interpreter as reference, JIT is optional enhancement
- **Rationale**: Safety, debuggability, per-game fallback
- **Impact**: Longer implementation, but safer and more maintainable

### 2025-11-29: Test-First Approach Approved
- **Decision**: Build test infrastructure BEFORE any JIT code
- **Rationale**: Cannot validate accuracy without tests
- **Impact**: Delays coding, but ensures correctness from day 1

### 2025-11-29: Yabause as Reference (Not Port)
- **Decision**: Study Yabause for patterns, don't copy-paste code
- **Rationale**: Known accuracy issues, different architecture
- **Impact**: Slower development, but higher quality result

---

## Resources

- [SH-2 JIT Evaluation](SH2_JIT_EVALUATION.md)
- [Yabause Repository](https://github.com/devmiyax/yabause)
- [SH-2 Programming Manual](https://www.st.com/resource/en/user_manual/cd00147165.pdf)
- [Ymir SH-2 Implementation](../src/ymir/libs/ymir-core/src/ymir/hw/sh2/)

---

## Next Steps

1. ✅ Create this roadmap
2. ⏭️ Clone and build Yabause
3. ⏭️ Study Yabause dynarec architecture
4. ⏭️ Complete Phase 0 documentation
5. ⏭️ Begin Phase 1: Test infrastructure

**Last Updated**: 2025-11-29

