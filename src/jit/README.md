# Brimir JIT - SH-2 Just-In-Time Compiler

**Status**: 🚧 Under Development (Phase 0)  
**Branch**: `feature/sh2-jit`  
**Target**: v0.7.0+ (see ROADMAP)

---

## Overview

This module implements a **hybrid interpreter/JIT** dynamic recompiler for the Hitachi SH-2 CPU used in the Sega Saturn. The goal is to achieve 3-5× performance improvement over the pure interpreter, enabling full-speed emulation on entry-level ARM handheld devices like the Trimui Smart Pro S.

**Critical Design Principle**: The JIT **supplements, not replaces** the Ymir interpreter. The interpreter remains the reference implementation and source of truth.

---

## Directory Structure

```
jit/
├── include/
│   ├── jit_compiler.hpp        # Main JIT interface
│   ├── jit_cache.hpp           # Compiled block cache
│   ├── jit_ir.hpp              # Intermediate representation
│   └── jit_backend.hpp         # Code generation backend interface
│
├── src/
│   ├── jit_compiler.cpp        # IR generation from SH-2
│   ├── jit_cache.cpp           # Cache management & invalidation
│   │
│   ├── backends/
│   │   ├── jit_x64.cpp         # x86-64 code generator (Phase 3)
│   │   └── jit_arm64.cpp       # ARM64 code generator (Phase 4)
│   │
│   └── validation/
│       ├── jit_validator.cpp   # Dual-execution testing
│       └── jit_fuzzer.cpp      # Randomized testing
│
└── tests/
    ├── test_jit_instructions.cpp   # Level 1: Per-instruction tests
    ├── test_jit_blocks.cpp         # Level 2: Basic block tests
    ├── test_jit_control_flow.cpp   # Level 3: Branches & delay slots
    └── test_jit_games.cpp          # Level 4: Full game regression
```

---

## Integration with Ymir

The JIT integrates with Ymir's SH-2 implementation via a hybrid dispatch:

```cpp
// In SH2::Advance() - future integration point
if (g_jit_enabled && !debug_mode) {
    // Try JIT execution
    auto block = jit_cache.Lookup(PC);
    if (block) {
        m_cyclesExecuted += block->Execute(this);
    } else {
        block = jit_compiler.Compile(PC);
        m_cyclesExecuted += block->Execute(this);
    }
} else {
    // Fallback to Ymir interpreter (always accurate)
    m_cyclesExecuted += InterpretNext<debug, emulateCache>();
}
```

---

## Testing Strategy

### The Golden Rule

> **"The interpreter is always right. If JIT disagrees, JIT is wrong."**

### Acceptance Criteria

- [ ] 100% instruction coverage in tests
- [ ] Zero failures in instruction-level tests  
- [ ] Zero state divergence in 60-second game tests
- [ ] 3× minimum speedup on ARM Cortex-A53
- [ ] 60 FPS on Trimui Smart Pro S for 80%+ of games

---

## References

- [SH-2 JIT Evaluation](../../docs/SH2_JIT_EVALUATION.md)
- [SH-2 JIT Roadmap](../../docs/SH2_JIT_ROADMAP.md)
- [Yabause SH-2 Dynarec](https://github.com/devmiyax/yabause/tree/master/yabause/src/sh2_dynarec)
- [Ymir SH-2 Interpreter](../ymir/libs/ymir-core/src/ymir/hw/sh2/)

---

**Next Milestone**: Complete Phase 1 test infrastructure  
**Target Completion**: TBD

