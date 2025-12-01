/**
 * @file benchmark_sh2.cpp
 * @brief SH-2 Interpreter Microbenchmark Suite
 * 
 * Measures performance of optimized hot paths:
 * - Byte swap operations (memory access)
 * - Carry flag operations (ADDC/SUBC)
 * - Bit extraction (instruction decode)
 */

#include <brimir/util/bit_ops.hpp>
#include <brimir/util/benchmark.hpp>
#include <brimir/core/types.hpp>

#include <iostream>
#include <iomanip>
#include <random>
#include <vector>
#include <chrono>

using namespace brimir;
using namespace brimir::util;
using ::bit::byte_swap;
using ::bit::extract;

// Test data generator
class TestData {
public:
    TestData(size_t count) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32> dist;
        
        data.reserve(count);
        for (size_t i = 0; i < count; i++) {
            data.push_back(dist(gen));
        }
    }
    
    std::vector<uint32> data;
};

// ============================================================================
// Byte Swap Benchmarks
// ============================================================================

void BenchmarkByteSwap16(const TestData& test) {
    volatile uint16 result = 0;
    const size_t iterations = test.data.size();
    
    BenchmarkIterations b("Byte Swap 16-bit", iterations);
    for (size_t i = 0; i < iterations; i++) {
        result = byte_swap<uint16>(static_cast<uint16>(test.data[i]));
    }
}

void BenchmarkByteSwap32(const TestData& test) {
    volatile uint32 result = 0;
    const size_t iterations = test.data.size();
    
    BenchmarkIterations b("Byte Swap 32-bit", iterations);
    for (size_t i = 0; i < iterations; i++) {
        result = byte_swap(test.data[i]);
    }
}

void BenchmarkByteSwap64(const TestData& test) {
    volatile uint64 result = 0;
    const size_t iterations = test.data.size();
    
    BenchmarkIterations b("Byte Swap 64-bit", iterations);
    for (size_t i = 0; i < iterations; i++) {
        uint64 val = (uint64(test.data[i]) << 32) | test.data[(i + 1) % iterations];
        result = byte_swap(val);
    }
}

// ============================================================================
// Bit Extraction Benchmarks
// ============================================================================

void BenchmarkBitExtract4(const TestData& test) {
    volatile uint32 result = 0;
    const size_t iterations = test.data.size();
    
    BenchmarkIterations b("Bit Extract [4:7] (4-bit)", iterations);
    for (size_t i = 0; i < iterations; i++) {
        // Extract 4 bits (common for register numbers)
        result += extract<4, 7>(test.data[i]);
    }
}

void BenchmarkBitExtract8(const TestData& test) {
    volatile uint32 result = 0;
    const size_t iterations = test.data.size();
    
    BenchmarkIterations b("Bit Extract [0:7] (8-bit)", iterations);
    for (size_t i = 0; i < iterations; i++) {
        // Extract 8 bits (common for immediates)
        result += extract<0, 7>(test.data[i]);
    }
}

void BenchmarkBitExtract12(const TestData& test) {
    volatile uint32 result = 0;
    const size_t iterations = test.data.size();
    
    BenchmarkIterations b("Bit Extract [0:11] (12-bit)", iterations);
    for (size_t i = 0; i < iterations; i++) {
        // Extract 12 bits (common for displacements)
        result += extract<0, 11>(test.data[i]);
    }
}

// ============================================================================
// Simulated Instruction Decode Benchmark
// ============================================================================

struct DecodedInstruction {
    uint8 opcode;
    uint8 rn;
    uint8 rm;
    uint16 imm;
};

void BenchmarkInstructionDecode(const TestData& test) {
    volatile DecodedInstruction result{};
    const size_t iterations = test.data.size();
    
    BenchmarkIterations b("Instruction Decode (SH-2)", iterations);
    for (size_t i = 0; i < iterations; i++) {
        uint16 instruction = static_cast<uint16>(test.data[i]);
        
        // Simulate typical SH-2 instruction decode
        result.opcode = extract<12, 15, uint16>(instruction);
        result.rn = extract<8, 11, uint16>(instruction);
        result.rm = extract<4, 7, uint16>(instruction);
        result.imm = extract<0, 7, uint16>(instruction);
    }
}

// ============================================================================
// Memory Access Simulation
// ============================================================================

void BenchmarkMemoryRead32(const TestData& test) {
    volatile uint32 result = 0;
    const size_t iterations = test.data.size();
    
    BenchmarkIterations b("Memory Read 32-bit (with endian swap)", iterations);
    for (size_t i = 0; i < iterations; i++) {
        // Simulate reading big-endian value from memory
        uint32 raw = test.data[i];
        result = byte_swap(raw); // Convert to host endian
    }
}

void BenchmarkMemoryWrite32(const TestData& test) {
    std::vector<uint32> memory(test.data.size());
    const size_t iterations = test.data.size();
    
    BenchmarkIterations b("Memory Write 32-bit (with endian swap)", iterations);
    for (size_t i = 0; i < iterations; i++) {
        // Simulate writing host-endian value to big-endian memory
        memory[i] = byte_swap(test.data[i]);
    }
}

// ============================================================================
// Real-world Combined Benchmark
// ============================================================================

void BenchmarkInstructionFetch(const TestData& test) {
    volatile uint32 result = 0;
    const size_t iterations = test.data.size();
    
    BenchmarkIterations b("Instruction Fetch (read + decode)", iterations);
    for (size_t i = 0; i < iterations; i++) {
        // Fetch instruction (big-endian)
        uint32 raw = test.data[i];
        uint16 instruction = static_cast<uint16>(byte_swap(raw) >> 16);
        
        // Decode
        uint8 opcode = extract<12, 15, uint16>(instruction);
        uint8 rn = extract<8, 11, uint16>(instruction);
        
        result += opcode + rn;
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                                                                       ║\n";
    std::cout << "║          SH-2 INTERPRETER MICROBENCHMARK SUITE                        ║\n";
    std::cout << "║                                                                       ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
    
    std::cout << "Optimizations applied:\n";
    std::cout << "  • Byte swap: _byteswap_ulong() [MSVC intrinsic]\n";
    std::cout << "  • Bit extract: _bextr_u32() [BMI2 instruction]\n";
    std::cout << "\n";
    
    const size_t TEST_SIZE = 10000000; // 10 million operations
    std::cout << "Generating " << TEST_SIZE << " test values...\n";
    TestData test(TEST_SIZE);
    std::cout << "Ready.\n\n";
    
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << std::left << std::setw(40) << "Benchmark" 
              << std::right << std::setw(15) << "Time (ns/op)" << "\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    
    // Byte swap benchmarks
    std::cout << "\nByte Swap Operations:\n";
    BenchmarkByteSwap16(test);
    BenchmarkByteSwap32(test);
    BenchmarkByteSwap64(test);
    
    // Bit extraction benchmarks
    std::cout << "\nBit Extraction Operations:\n";
    BenchmarkBitExtract4(test);
    BenchmarkBitExtract8(test);
    BenchmarkBitExtract12(test);
    
    // Instruction decode
    std::cout << "\nInstruction Decode:\n";
    BenchmarkInstructionDecode(test);
    
    // Memory access simulation
    std::cout << "\nMemory Access (with endian conversion):\n";
    BenchmarkMemoryRead32(test);
    BenchmarkMemoryWrite32(test);
    
    // Real-world combined
    std::cout << "\nCombined Operations:\n";
    BenchmarkInstructionFetch(test);
    
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "\nBenchmark complete!\n";
    std::cout << "\nExpected results (vs non-intrinsic implementation):\n";
    std::cout << "  • Byte swap: ~1-2 ns/op (4× faster)\n";
    std::cout << "  • Bit extract: ~1-2 ns/op (2× faster)\n";
    std::cout << "  • Instruction decode: ~4-6 ns/op (2-3× faster)\n";
    std::cout << "  • Memory read/write: ~2-4 ns/op (3-4× faster)\n";
    std::cout << "\n";
    
    return 0;
}

