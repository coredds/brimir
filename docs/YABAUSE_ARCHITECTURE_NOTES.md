# Yabause SH-2 Dynarec Architecture Analysis

**Date:** 2025-11-29  
**Repository**: https://github.com/devmiyax/yabause  
**Analyzed Version**: Latest (depth-1 clone)  
**Author**: Ari64 (original dynarec author, also did PCSX-ReARMed)

---

## Executive Summary

Yabause's SH-2 dynarec is a **mature, battle-tested** JIT implementation originally by Ari64 (known for PSX dynarec). It uses a **register allocation-heavy** approach with extensive optimizations, but makes **accuracy trade-offs** for performance.

### Key Findings

| Aspect | Assessment | Notes |
|--------|------------|-------|
| **Code Quality** | 🟢 Mature | Production-proven in Yaba Sanshiro |
| **Architecture** | 🟡 Complex | 8,000+ lines, heavily optimized |
| **Accuracy** | ⚠️ Approximat

e | Cycle counting is approximate, not cycle-perfect |
| **Portability** | ✅ Good | x86, x86-64, ARM32 backends |
| **Maintainability** | ⚠️ Poor | Monolithic, lacks comments, hard to understand |
| **Reusability** | 🟡 Moderate | Patterns are useful, but code is tightly coupled |

**Recommendation**: Use as **architectural reference** for patterns, but **do NOT copy-paste**. Build our own cleaner implementation inspired by Yabause's approach.

---

## File Structure

```
sh2_dynarec/
├── sh2_dynarec.h           # Public API (minimal)
├── sh2_dynarec.c           # Core compiler (8,456 lines!)
│                             - IR generation
│                             - Register allocation
│                             - Block compilation
│                             - Cache management
│                             - Code invalidation
│
├── assem_x86.c/h           # x86-32 code generator
├── assem_x64.c/h           # x86-64 code generator
├── assem_arm.c/h           # ARM32 code generator
│
└── linkage_*.s             # Assembly linkage/trampolines
    ├── linkage_x86.s       # x86-32 entry/exit stubs
    ├── linkage_x64.s       # x86-64 entry/exit stubs
    └── linkage_arm.s       # ARM32 entry/exit stubs
```

**Key Observation**: **Everything is in one 8,456-line file** (`sh2_dynarec.c`). This is a red flag for maintainability but proves the approach works.

---

## Core Architecture

### 1. Compilation Unit: Basic Block

**Definition**: Sequence of SH-2 instructions ending at a branch

```c
#define MAXBLOCK 4096  // Max instructions per block

// Block metadata arrays (global!)
char insn[MAXBLOCK][10];        // Instruction mnemonics (debug)
unsigned char itype[MAXBLOCK];  // Instruction type (LOAD, STORE, ALU, etc.)
unsigned char opcode[MAXBLOCK]; // Primary opcode
signed char rs1[MAXBLOCK];      // Source register 1
signed char rt1[MAXBLOCK];      // Target register 1
int imm[MAXBLOCK];              // Immediate values
char is_ds[MAXBLOCK];           // Is delay slot?
// ... many more arrays
```

**Approach**: **Structure of Arrays** (SoA) instead of Array of Structures (AoS)  
**Reason**: Cache-friendly for analysis passes

### 2. Register Allocation

**Strategy**: **Linear scan** with spilling

```c
struct regstat {
    signed char regmap_entry[HOST_REGS];  // Register mapping at block entry
    signed char regmap[HOST_REGS];        // Current register mapping
    u32 wasdirty;                         // Dirty register tracking
    u32 dirty;                            // Current dirty registers
    u64 u;                                // Register usage tracking
    u32 isconst;                          // Constant propagation flags
    u32 constmap[SH2_REGS];               // Constant values
};

struct regstat regs[MAXBLOCK];           // Per-instruction register state
```

**Key Insight**: **Track register state at every instruction**, not just block boundaries. This enables aggressive optimizations but increases complexity.

### 3. Code Cache

**Size**: 16-32 MB depending on platform

```c
// x86-64
#define BASE_ADDR 0x70000000
#define TARGET_SIZE_2 25  // 2^25 = 32 MB

// ARM32
#define BASE_ADDR ((u32)&sh2_dynarec_target)
#define TARGET_SIZE_2 24  // 2^24 = 16 MB
```

**Hash Table Lookup**:

```c
ALIGNED(16) u32 hash_table[65536][4];  // 4-way set-associative cache

// Fast path lookup:
u32 *ht_bin = hash_table[((vaddr>>16)^vaddr)&0xFFFF];
if (ht_bin[0] == vaddr) return ht_bin[1];  // Hit!
if (ht_bin[2] == vaddr) return ht_bin[3];  // Second entry
```

**Cache Invalidation**:

```c
// Shadow RAM for tracking modified code
ALIGNED(16) char shadow[2097152];  // 2 MB shadow RAM
char cached_code[0x20000];         // 128 KB granularity tracking

// Invalidation on write
void invalidate_addr(u32 addr) {
    // Mark page as dirty
    cached_code[addr>>15] &= ~(1<<((addr>>12)&7));
    // Flush blocks in affected range
    remove_hash(addr);
}
```

### 4. Instruction Types (IR Categories)

```c
#define NOP 0       // No operation
#define LOAD 1      // Load from memory
#define STORE 2     // Store to memory
#define RMW 3       // Read-Modify-Write (atomic)
#define PCREL 4     // PC-relative Load
#define MOV 5       // Move
#define ALU 6       // Arithmetic/logic
#define MULTDIV 7   // Multiply/divide
#define SHIFTIMM 8  // Shift by immediate
#define IMM8 9      // 8-bit immediate
#define EXT 10      // Sign/Zero Extension
#define FLAGS 11    // SETT/CLRT/MOVT
#define UJUMP 12    // Unconditional jump
#define RJUMP 13    // Jump to register
#define CJUMP 14    // Conditional branch (BT/BF)
#define SJUMP 15    // Conditional with delay slot
#define COMPLEX 16  // Complex (call function)
#define SYSTEM 17   // Halt/Trap/Exception
#define SYSCALL 18  // TRAPA
#define NI 19       // Not implemented
#define DATA 20     // Constant pool
#define BIOS 21     // BIOS emulation
```

**Observation**: **Flat IR encoding** (just an enum), not a rich IR structure. Simple but limits optimization passes.

---

## Compilation Pipeline

### Phase 1: Disassembly & Analysis

```c
// 1. Fetch instructions until branch
for (i = 0; i < MAXBLOCK; i++) {
    u16 instr = source[i];
    
    // Decode instruction type
    itype[i] = decode_type(instr);
    
    // Extract operands
    rs1[i] = extract_rs1(instr);
    rt1[i] = extract_rt1(instr);
    imm[i] = extract_immediate(instr);
    
    // Detect delay slots
    if (is_branch(itype[i])) {
        is_ds[i+1] = 1;  // Next instruction is in delay slot
        slen = i + 2;     // Block length includes delay slot
        break;
    }
}
```

### Phase 2: Register Liveness Analysis

```c
// Backward pass to determine register usage
for (i = slen-1; i >= 0; i--) {
    // Calculate which registers are needed in future
    unneeded_reg[i] = calculate_unneeded(i);
    
    // Determine which registers will be dirty
    will_dirty[i] = calculate_dirty_propagation(i);
}
```

**Purpose**: Avoid loading/storing unused registers

### Phase 3: Register Allocation

```c
// Forward pass to allocate host registers
for (i = 0; i < slen; i++) {
    // Allocate registers for this instruction
    alloc_reg(i, rs1[i]);  // Source operand
    alloc_reg(i, rt1[i]);  // Dest operand
    
    // Track constant propagation
    if (is_constant_load(itype[i])) {
        regs[i].isconst |= (1 << rt1[i]);
        regs[i].constmap[rt1[i]] = imm[i];
    }
}
```

### Phase 4: Code Generation

```c
// Generate native code for each instruction
for (i = 0; i < slen; i++) {
    switch (itype[i]) {
        case ALU:
            emit_alu_instruction(i);
            break;
        case LOAD:
            emit_load(i);
            break;
        case STORE:
            emit_store(i);
            break;
        // ... etc
    }
}
```

### Phase 5: Block Linking

```c
// Link blocks together for fast transitions
void add_to_linker(int addr, int target, int ext) {
    link_addr[linkcount][0] = addr;    // Jump source
    link_addr[linkcount][1] = target;  // Jump target
    link_addr[linkcount][2] = ext;     // External jump flag
    linkcount++;
}
```

**Optimization**: Direct jumps between blocks avoid hash table lookup

---

## Platform Backends

### x86-64 Backend

**Register Mapping**:
```c
// SH-2 registers → x86-64 host registers
#define HOST_REGS 8  // RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI

// Calling convention (System V AMD64 ABI):
// func(rdi, rsi, rdx, rcx, r8, r9) {return rax;}
// Callee-save: %rbp %rbx %r12-%r15

#define ARG1_REG 7  /* RDI */
#define ARG2_REG 6  /* RSI */
```

**Code Size**: `assem_x64.c` is ~3,000 lines of x86-64 emitter functions

### ARM32 Backend

**Register Mapping**:
```c
#define HOST_REGS 13  // r0-r12

// ARM calling convention:
// r0-r3, r12: caller-save
// r4-r11: callee-save

#define ARG1_REG 0
#define ARG2_REG 1
```

**Special**: Uses FP (r11) as base pointer to `dynarec_local` (global state)

**Code Size**: `assem_arm.c` is ~4,000 lines

---

## Accuracy Trade-offs

### 1. Approximate Cycle Counting

```c
#define CLOCK_DIVIDER 1

// Cycles are accumulated, not precise per-instruction
int cycles[MAXBLOCK];  // Estimated cycles for block
int ccadj[MAXBLOCK];   // Cycle count adjustments
```

**Impact**: **Not cycle-perfect**. Games that rely on precise timing may break.

### 2. Memory Access Simplifications

```c
// Yabause uses "stubs" for memory access instead of inline checks
#define LOADB_STUB 3
#define LOADW_STUB 4
#define LOADL_STUB 5

// Memory access goes through function call (slower but simpler)
void *get_addr(u32 vaddr) {
    // Lookup in hash table
    // If not cached, call interpreter stub
}
```

**Impact**: Simpler code generation, but loses some performance

### 3. Cache Invalidation Granularity

```c
// 128 KB granularity for cached code tracking
char cached_code[0x20000];

// Page-level invalidation (4 KB pages)
cached_code[vaddr>>15] |= 1<<((vaddr>>12)&7);
```

**Impact**: May invalidate more code than necessary

---

## Strengths

### ✅ 1. Proven Track Record

- Used in Yaba Sanshiro (millions of downloads)
- Based on Ari64's PSX dynarec (highly regarded)
- Years of bug fixes and optimizations

### ✅ 2. Aggressive Optimizations

- **Constant propagation**: Tracks known constant values
- **Register allocation**: Linear scan with smart spilling
- **Block chaining**: Direct jumps between blocks
- **Dead code elimination**: Doesn't load/store unused registers

### ✅ 3. Multi-Platform

- x86, x86-64, ARM32 backends
- Architecture-specific optimizations
- Portable memory management

---

## Weaknesses

### ⚠️ 1. Monolithic Design

- **8,456 lines** in a single file
- **Global variables everywhere**
- **Hard to understand** without extensive study
- **Difficult to debug** when things go wrong

### ⚠️ 2. Accuracy Shortcuts

- **Approximate cycle counting** (not cycle-perfect)
- **No cache emulation** by default (performance vs accuracy)
- **Memory access stubs** instead of inline checks
- **Per-game hacks** needed for some titles

### ⚠️ 3. Poor Documentation

- **Minimal comments** in the code
- **No architecture document**
- **Cryptic variable names** (`ht_bin`, `ccadj`, etc.)
- **Magic numbers** everywhere

### ⚠️ 4. Tight Coupling

- Yabause-specific memory_map integration
- BIOS emulation hooks embedded in dynarec
- Hard to extract and reuse elsewhere

---

## What We Can Learn

### ✅ Use These Patterns

1. **Hash Table Lookup**
   - 4-way set-associative cache for fast block lookup
   - Good balance of speed and memory

2. **Structure of Arrays (SoA)**
   - Cache-friendly for compiler passes
   - Better than AoS for analysis phases

3. **Delay Slot Tracking**
   - Explicit `is_ds[]` flag for each instruction
   - Critical for correct SH-2 semantics

4. **Register Liveness Analysis**
   - Don't load/store registers that won't be used
   - Significant performance win

5. **Block Linking**
   - Direct jumps between blocks
   - Avoid hash table lookup on known jumps

### ⚠️ Avoid These Mistakes

1. **Monolithic Design**
   - Split into modules: IR, codegen, cache, validation
   - Use proper abstraction layers

2. **Approximate Cycles**
   - **Must track accurate cycles** for Brimir
   - Ymir's interpreter is cycle-perfect; maintain that

3. **Global State**
   - Use proper class/struct encapsulation
   - Make state explicit and testable

4. **Lack of Testing**
   - Yabause has no visible test suite
   - **We must build comprehensive tests first**

5. **Poor Documentation**
   - Document design decisions
   - Use meaningful variable names
   - Comment non-obvious code

---

## Ymir Integration Strategy

### What to Reuse

```
Yabause Concept          →  Brimir Implementation
────────────────────────────────────────────────
Hash table lookup        →  JITCache class
4-way set-associative    →  Reuse algorithm
Structure of Arrays      →  Use for IR metadata
Delay slot tracking      →  Copy approach
Register liveness        →  Adapt to our IR
Block linking            →  Implement carefully
```

### What to Redesign

```
Yabause Problem          →  Brimir Solution
────────────────────────────────────────────────
Monolithic file          →  Modular architecture
Approximate cycles       →  Precise cycle tracking
Global state             →  Encapsulated classes
Memory stubs             →  Inline checks (initially)
No tests                 →  Test-driven development
Poor docs                →  Comprehensive docs
```

---

## Comparison with Ymir's Interpreter

| Aspect | Ymir Interpreter | Yabause Dynarec | Brimir JIT (Planned) |
|--------|------------------|-----------------|----------------------|
| **Cycle Accuracy** | ✅ Perfect | ⚠️ Approximate | ✅ Perfect |
| **Code Size** | ~5,000 lines | ~15,000 lines | ~8,000 lines (est) |
| **Architecture** | Clean, modular | Monolithic | Modular |
| **Testability** | ✅ Excellent | ⚠️ Poor | ✅ Excellent (test-first) |
| **Performance** | 1× baseline | ~5× speedup | 3-5× speedup (target) |
| **Maintainability** | ✅ High | ⚠️ Low | ✅ High |
| **Platforms** | All | x86, ARM32 | x86-64, ARM64 |

**Key Difference**: **Ymir prioritizes accuracy and maintainability**, Yabause prioritizes raw performance. **Brimir JIT will balance both**.

---

## Recommended Approach

### Phase 0: Study (DONE ✅)

- [x] Clone Yabause
- [x] Analyze dynarec structure
- [x] Document patterns and anti-patterns

### Phase 1: Test Infrastructure

**Before writing ANY JIT code**:

- [ ] Build dual-execution test framework
- [ ] Create instruction-level tests
- [ ] Implement state comparison utilities
- [ ] Target: 800+ tests covering all SH-2 instructions

### Phase 2: Clean IR Design

**Learn from Yabause's mistakes**:

```c
// ❌ Yabause: Flat enum
unsigned char itype[MAXBLOCK];

// ✅ Brimir: Rich IR structure
struct IRInstruction {
    IROpcode opcode;
    IROperand src1, src2, dest;
    uint32_t flags;
    uint8_t cycles;  // Precise!
};

std::vector<IRInstruction> ir_block;
```

### Phase 3: Modular Backend

**Split responsibilities**:

```
src/jit/
├── jit_compiler.cpp       # SH-2 → IR translation
├── jit_cache.cpp          # Block cache (adapt Yabause hash table)
├── backends/
│   ├── jit_backend.hpp    # Abstract backend interface
│   ├── jit_x64.cpp        # x86-64 codegen (learn from assem_x64.c)
│   └── jit_arm64.cpp      # ARM64 codegen (NOT ARM32!)
└── validation/
    ├── jit_validator.cpp  # Dual-execution testing
    └── jit_fuzzer.cpp     # Random testing
```

### Phase 4: Incremental Implementation

1. Start with **simple instructions** (MOV, ADD, SUB)
2. **Test immediately** against interpreter
3. Add **complex instructions** incrementally
4. **Re-test constantly**
5. **Never merge failing code**

---

## Key Takeaways

### ✅ What Yabause Got Right

1. **Hash table block lookup** - Fast and effective
2. **Register liveness analysis** - Significant optimization
3. **Delay slot handling** - Critical for SH-2 semantics
4. **Block linking** - Good performance win
5. **Multi-platform** - Proves portability is possible

### ⚠️ What Yabause Got Wrong (for our needs)

1. **Monolithic design** - Unmaintainable
2. **Approximate cycles** - Breaks accuracy
3. **No testing** - Risky
4. **Poor documentation** - Hard to understand
5. **Global state** - Not encapsulated

### 🎯 Brimir's Approach

**"Learn from Yabause's patterns, avoid its mistakes"**

1. ✅ **Use hash table lookup** (proven)
2. ✅ **Use liveness analysis** (optimization)
3. ✅ **Modular architecture** (maintainability)
4. ✅ **Precise cycles** (accuracy)
5. ✅ **Test-driven** (correctness)
6. ✅ **Well-documented** (clarity)

---

## Next Steps

- [x] Complete Yabause analysis ✅
- [ ] Begin Phase 1: Test infrastructure
- [ ] Design clean IR (Phase 2)
- [ ] Implement x86-64 backend (Phase 3)
- [ ] Port to ARM64 (Phase 4)

**Estimated Start Date**: 2025-12-01 (Phase 1)  
**Target Completion**: Q3 2026

---

## References

- [Yabause Repository](https://github.com/devmiyax/yabause)
- [Yaba Sanshiro (Fork)](https://www.uoyabause.org/)
- [Ari64's PCSX-ReARMed](https://github.com/notaz/pcsx_rearmed) (Similar dynarec)
- [Ymir SH-2 Interpreter](../src/ymir/libs/ymir-core/src/ymir/hw/sh2/sh2.cpp)

---

**Analysis Complete**: We now have a comprehensive understanding of Yabause's approach and can proceed with confidence to Phase 1 (test infrastructure).

**Recommendation**: **Proceed with hybrid JIT implementation**, using Yabause as inspiration but NOT as a copy-paste source. Build our own clean, tested, documented version.

