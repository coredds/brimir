# Brimir Development Tools

Utilities for performance measurement, testing, and development.

---

## 📊 Benchmarking Tools

### benchmark_sh2

**Purpose**: Microbenchmark suite for SH-2 interpreter optimizations

**Location**: `tools/benchmark_sh2.cpp`

**What it measures**:
- Byte swap operations (16/32/64-bit)
- Bit extraction (4/8/12-bit fields)
- Instruction decode (full SH-2 decode)
- Memory access with endian conversion
- Combined real-world operations

**Usage**:

```powershell
# Build
cmake --build build --config Release --target benchmark_sh2

# Run
.\build\bin\Release\Release\benchmark_sh2.exe

# Save results
.\build\bin\Release\Release\benchmark_sh2.exe > benchmark_results.txt
```

**Expected Output**:
```
Byte Swap 32-bit                    0.32 ns/op
Bit Extract [4:7]                   0.51 ns/op
Instruction Decode (SH-2)           0.87 ns/op
Memory Read 32-bit                  0.38 ns/op
```

**Interpreting Results**:
- **< 1 ns/op**: Excellent (single-cycle or near-optimal)
- **1-2 ns/op**: Good (2-6 cycles)
- **2-5 ns/op**: Acceptable (hot path overhead)
- **> 5 ns/op**: Needs investigation

---

## 🔨 Building Tools

### Quick Build

```powershell
# All tools
cmake --build build --config Release

# Specific tool
cmake --build build --config Release --target benchmark_sh2
```

### CMake Configuration

Tools are automatically built when you configure the project:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

---

## 📈 Performance Tracking

### Baseline Results (December 2024)

**Platform**: Windows x64, MSVC 2022, Modern CPU with BMI2

| Operation | Time (ns/op) | Status |
|-----------|--------------|--------|
| Byte Swap 32-bit | 0.32 | ✅ Baseline |
| Bit Extract 4-bit | 0.51 | ✅ Baseline |
| Instruction Decode | 0.87 | ✅ Baseline |
| Memory Read 32-bit | 0.38 | ✅ Baseline |

**How to track**:
1. Run benchmark before making changes
2. Save results to file
3. Make your changes
4. Run benchmark again
5. Compare results

**Example**:
```powershell
# Before
.\build\bin\Release\Release\benchmark_sh2.exe > before.txt

# Make changes...

# After
cmake --build build --config Release --target benchmark_sh2
.\build\bin\Release\Release\benchmark_sh2.exe > after.txt

# Compare
Compare-Object (Get-Content before.txt) (Get-Content after.txt)
```

---

## 🧪 Future Tools (Planned)

### emulator_test (Planned)
- Full emulator validation suite
- Saturn Open SDK test generation
- 1190+ instruction tests
- Dual-execution validation (interpreter vs JIT)

### jit_benchmark (Planned)
- JIT compiler performance measurement
- Block compilation times
- Runtime overhead analysis
- Comparison vs interpreter

### profiler_helper (Planned)
- Integration with Visual Studio Profiler
- Hot path identification
- Call graph visualization
- Performance regression detection

---

## 📝 Adding New Tools

### Template

```cpp
/**
 * @file your_tool.cpp
 * @brief Brief description
 */

#include <brimir/core/types.hpp>
#include <iostream>

int main() {
    std::cout << "Tool output\n";
    return 0;
}
```

### CMakeLists.txt

```cmake
add_executable(your_tool your_tool.cpp)
target_link_libraries(your_tool PRIVATE brimir::brimir-core)
target_compile_features(your_tool PRIVATE cxx_std_20)

set_target_properties(your_tool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}"
)
```

---

## 🎯 Best Practices

### Benchmarking

1. **Always use Release builds** for meaningful results
2. **Run multiple times** to verify consistency
3. **Close background applications** to reduce noise
4. **Use fixed CPU frequency** if available (disable boost)
5. **Warm up** - First run may be slower

### Performance Testing

1. **Baseline first** - Always measure before optimizing
2. **Isolate changes** - One optimization at a time
3. **Document results** - Keep track of what works
4. **Regression test** - Don't break existing performance

---

## 📖 Documentation

For detailed analysis of current performance, see:
- `docs/BENCHMARK_RESULTS.md` - Current benchmark analysis
- `docs/OPTIMIZATION_RESULTS.md` - Optimization implementation
- `docs/INTERPRETER_OPTIMIZATION_OPPORTUNITIES.md` - Future improvements

---

## 🤝 Contributing Tools

When adding new tools:
1. Add to `tools/CMakeLists.txt`
2. Document in this README
3. Add usage examples
4. Include expected output samples
5. Update main project documentation

---

**Last Updated**: December 1, 2024

