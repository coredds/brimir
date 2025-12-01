# Building and Running JIT Tests

**Quick Start**: Build JIT validation tests and verify SH-2 JIT infrastructure

---

## Prerequisites

- CMake 3.20+
- C++20 compiler (MSVC 2022 recommended on Windows)
- Brimir dependencies installed (vcpkg or manual)

---

## Option 1: Quick Build (Recommended)

### Step 1: Configure with JIT Testing

```powershell
# Windows (PowerShell)
cd build
cmake .. -DBRIMIR_ENABLE_JIT_TESTING=ON -DCMAKE_BUILD_TYPE=Release

# Or use the build script
.\build-all.ps1 -EnableJitTesting
```

```bash
# Linux/macOS
cd build
cmake .. -DBRIMIR_ENABLE_JIT_TESTING=ON -DCMAKE_BUILD_TYPE=Release
```

### Step 2: Build Core with JIT Interface

```powershell
# Build the core library with test accessors
cmake --build . --config Release --target brimir-core
```

### Step 3: Build Test Runner

```powershell
# Build JIT test runner (if you added it to CMakeLists)
cmake --build . --config Release --target jit-test-runner
```

### Step 4: Run Tests

```powershell
# Run the test executable
.\Release\jit-test-runner.exe

# Or from project root
.\build\Release\jit-test-runner.exe
```

---

## Option 2: Manual Integration Test

If the test runner isn't building yet, you can manually test the integration:

### Create Simple Test File

**File**: `test_jit_manual.cpp`

```cpp
#include <iostream>
#include "src/jit/include/jit_validator.hpp"
#include "src/jit/include/sh2_spec.hpp"

int main() {
    using namespace brimir::jit;
    
    std::cout << "JIT Test Framework\n";
    std::cout << "==================\n\n";
    
    // Get spec stats
    auto stats = SH2SpecDatabase::GetStats();
    std::cout << "Spec Database:\n";
    std::cout << "  Instructions: " << stats.total_instructions << "\n";
    std::cout << "  Implemented: " << stats.implemented << "\n\n";
    
    // Get all specs
    const auto& specs = SH2SpecDatabase::GetAllInstructions();
    std::cout << "Available Instructions:\n";
    for (const auto& spec : specs) {
        std::cout << "  " << spec.mnemonic << " - " << spec.syntax << "\n";
    }
    
    std::cout << "\nIntegration: OK\n";
    return 0;
}
```

### Build and Run

```powershell
# Compile test
cl /std:c++20 /EHsc /I"src/core/include" /I"src/jit/include" ^
   test_jit_manual.cpp ^
   src/jit/src/sh2_spec.cpp ^
   /link /OUT:test_jit.exe

# Run
.\test_jit.exe
```

---

## Option 3: Integration with Existing Build

### Modify src/jit/CMakeLists.txt

Add test runner target:

```cmake
if(BRIMIR_ENABLE_JIT_TESTING)
    # JIT Test Runner
    add_executable(jit-test-runner
        test_runner.cpp
        src/jit_integration.cpp
        src/jit_test_generator.cpp
        src/jit_validator.cpp
        src/sh2_spec.cpp
        src/jit_x64_codebuffer.cpp
        src/jit_x64_codegen.cpp
        src/jit_x64_register_alloc.cpp
    )
    
    target_include_directories(jit-test-runner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    )
    
    target_link_libraries(jit-test-runner PRIVATE
        brimir::brimir-core
    )
    
    message(STATUS "JIT test runner configured")
endif()
```

---

## Expected Output

### Success

```
╔════════════════════════════════════════════════════════════════╗
║                                                                ║
║              BRIMIR SH-2 JIT TEST RUNNER                       ║
║                                                                ║
╚════════════════════════════════════════════════════════════════╝

Specification Database:
  Total Instructions: 18
  Implemented: 0 (0.0%)
  Tested: 0 (0.0%)

Generating test suites...
Generated 6 test suites

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Test Suite: Data Transfer - MOV
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Testing: MOV - MOV R2, R3 - normal case
  ✅ PASS
Testing: MOV - MOV R1, R3 - move zero
  ✅ PASS
...

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Results: 180/180 passed ✅
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

### Failure Example

```
Testing: ADD - ADD R1, R2 - overflow
  ❌ FAIL
  Register mismatch:
    R1: 0x00000000 vs 0xFFFFFFFF
  T-bit: 0 vs 1
```

---

## Troubleshooting

### Error: "BRIMIR_ENABLE_JIT_TESTING not defined"

**Solution**: Make sure you built with the flag:

```powershell
cmake .. -DBRIMIR_ENABLE_JIT_TESTING=ON
```

### Error: "Cannot find brimir-core"

**Solution**: Build core first:

```powershell
cmake --build . --target brimir-core
```

### Error: "JIT_GetR not found"

**Solution**: The flag wasn't applied. Clean and rebuild:

```powershell
rm -r build/*
cmake .. -DBRIMIR_ENABLE_JIT_TESTING=ON
cmake --build .
```

### Linker Errors

**Solution**: Make sure all JIT source files are included in CMakeLists.txt

---

## What the Tests Validate

### 1. Specification Accuracy ✅
- Binary encoding patterns correct?
- Operand extraction working?
- Cycle counts accurate?

### 2. Test Generator Quality ✅
- Tests cover normal cases?
- Edge cases included?
- Flag effects tested?

### 3. Integration Correctness ✅
- State management working?
- Register access functional?
- Single instruction execution correct?

### 4. Interpreter Validation ✅
- Does interpreter match spec?
- Are there any bugs?
- Foundation solid for JIT?

---

## Next Steps After Tests Pass

### 1. Expand Coverage
```bash
# Add more instructions to spec database
# Aim for all 133 SH-2 instructions
```

### 2. Implement JIT Compilation
```bash
# Complete x86-64 backend
# Wire IR → native code
# Run dual-execution validation
```

### 3. Optimize
```bash
# Profile JIT performance
# Add optimization passes
# Tune register allocation
```

---

## CI/CD Integration

### GitHub Actions Example

```yaml
name: JIT Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v3
      - name: Configure
        run: cmake -B build -DBRIMIR_ENABLE_JIT_TESTING=ON
      - name: Build
        run: cmake --build build --config Release
      - name: Test
        run: ./build/Release/jit-test-runner.exe
```

---

## Performance Expectations

- **Test Generation**: < 1 second
- **Single Test Execution**: < 1ms
- **Full Suite (180 tests)**: < 1 second
- **Memory Usage**: < 100 MB

---

## Debugging Failed Tests

### Enable Verbose Output

Modify `jit_integration.cpp`:

```cpp
bool RunSingleTest(const InstructionTest& test) {
    std::cout << "Testing: " << test.mnemonic << " - " << test.description << "\n";
    std::cout << "  Instruction: 0x" << std::hex << test.instruction << std::dec << "\n";
    std::cout << "  Initial PC: 0x" << std::hex << test.initial_state.PC << std::dec << "\n";
    // ... more debug output
}
```

### Use Debugger

```powershell
# Visual Studio
devenv build\Brimir.sln
# Set breakpoint in jit_integration.cpp:RunSingleTest
# F5 to debug
```

---

**Ready to validate your JIT infrastructure!** 🚀

