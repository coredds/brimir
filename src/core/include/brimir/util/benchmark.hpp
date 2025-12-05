#pragma once

/**
 * @file benchmark.hpp
 * @brief Simple benchmarking utilities for performance measurement
 */

#include <chrono>
#include <string>
#include <iostream>
#include <iomanip>

namespace brimir::util {

/// @brief Simple RAII benchmark timer
class Benchmark {
public:
    explicit Benchmark(const std::string& name)
        : name_(name)
        , start_(std::chrono::high_resolution_clock::now())
    {}
    
    ~Benchmark() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start_);
        std::cout << std::left << std::setw(40) << name_ 
                  << std::right << std::setw(12) << duration.count() << " µs\n";
    }
    
private:
    std::string name_;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};

/// @brief Benchmark a code block multiple times and report average
class BenchmarkIterations {
public:
    explicit BenchmarkIterations(const std::string& name, size_t iterations)
        : name_(name)
        , iterations_(iterations)
        , start_(std::chrono::high_resolution_clock::now())
    {}
    
    ~BenchmarkIterations() {
        auto end = std::chrono::high_resolution_clock::now();
        auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
        double avg_ns = (total_us * 1000.0) / iterations_;
        
        std::cout << std::left << std::setw(40) << name_ 
                  << std::right << std::setw(12) << std::fixed << std::setprecision(2) 
                  << avg_ns << " ns/op"
                  << " (" << iterations_ << " iterations)\n";
    }
    
private:
    std::string name_;
    size_t iterations_;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};

/// @brief Measure cycles per operation (statistical)
class CycleCounter {
public:
    explicit CycleCounter(const std::string& name, size_t operations)
        : name_(name)
        , operations_(operations)
        , start_(std::chrono::high_resolution_clock::now())
    {}
    
    ~CycleCounter() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();
        
        // Rough cycle estimate (assume 3 GHz CPU)
        double cycles_per_op = (duration * 3.0) / operations_;
        
        std::cout << std::left << std::setw(40) << name_
                  << std::right << std::setw(12) << std::fixed << std::setprecision(1)
                  << cycles_per_op << " cycles/op"
                  << " (" << operations_ << " ops)\n";
    }
    
private:
    std::string name_;
    size_t operations_;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};

// Usage examples:
//
// {
//     Benchmark b("Function execution");
//     expensive_function();
// }
//
// {
//     BenchmarkIterations b("MOV instruction", 1000000);
//     for (int i = 0; i < 1000000; i++) {
//         execute_mov();
//     }
// }
//
// {
//     CycleCounter c("Byte swap", 10000000);
//     for (int i = 0; i < 10000000; i++) {
//         result = byte_swap(value);
//     }
// }

} // namespace brimir::util













