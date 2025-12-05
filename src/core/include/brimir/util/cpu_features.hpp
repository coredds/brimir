#pragma once

#include <cstdint>

namespace brimir::util {

/// @brief CPU feature detection for x64 hardware optimizations
struct CPUFeatures {
    // SSE family
    bool sse2;      // All x64 has this
    bool sse3;      // Core 2 (2006+)
    bool ssse3;     // Core 2 (2006+)
    bool sse4_1;    // Core 2 45nm (2008+)
    bool sse4_2;    // Core i7 (2008+)
    
    // AVX family
    bool avx;       // Sandy Bridge (2011+)
    bool avx2;      // Haswell (2013+)
    
    // Bit manipulation
    bool bmi1;      // Haswell (2013+) - ANDN, BLSI, etc.
    bool bmi2;      // Haswell (2013+) - PEXT, PDEP, BZHI, etc.
    
    // Other useful features
    bool popcnt;    // Core i7 (2008+) - Population count
    bool lzcnt;     // Haswell (2013+) - Leading zero count
    
    /// @brief Get singleton instance with detected features
    static const CPUFeatures& get();
    
    /// @brief Check if BMI2 is available (for bit extraction)
    static bool has_bmi2() { return get().bmi2; }
    
    /// @brief Check if POPCNT is available
    static bool has_popcnt() { return get().popcnt; }
    
private:
    CPUFeatures();
    void detect();
};

// Compiler-specific force inline
#ifdef _MSC_VER
    #define BRIMIR_FORCE_INLINE __forceinline
#elif defined(__GNUC__)
    #define BRIMIR_FORCE_INLINE __attribute__((always_inline)) inline
#else
    #define BRIMIR_FORCE_INLINE inline
#endif

// Branch prediction hints (MSVC doesn't support these well, so no-op)
#ifdef __GNUC__
    #define BRIMIR_LIKELY(x) __builtin_expect(!!(x), 1)
    #define BRIMIR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define BRIMIR_LIKELY(x) (x)
    #define BRIMIR_UNLIKELY(x) (x)
#endif

} // namespace brimir::util













