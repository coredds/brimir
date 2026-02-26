#include <brimir/util/cpu_features.hpp>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    #define BRIMIR_X86_FAMILY 1
#else
    #define BRIMIR_X86_FAMILY 0
#endif

#if BRIMIR_X86_FAMILY
    #ifdef _MSC_VER
        #include <intrin.h>
    #elif defined(__GNUC__)
        #include <cpuid.h>
    #endif
#endif

namespace brimir::util {

const CPUFeatures& CPUFeatures::get() {
    static CPUFeatures features;
    return features;
}

CPUFeatures::CPUFeatures() {
    detect();
}

void CPUFeatures::detect() {
#if !BRIMIR_X86_FAMILY
    // Non-x86 architectures: set all features to false
    sse2 = false;
    sse3 = false;
    ssse3 = false;
    sse4_1 = false;
    sse4_2 = false;
    avx = false;
    avx2 = false;
    bmi1 = false;
    bmi2 = false;
    popcnt = false;
    lzcnt = false;
#elif defined(_MSC_VER)
    int cpuInfo[4] = {0};
    
    // Get vendor string and max function
    __cpuid(cpuInfo, 0);
    int maxFunc = cpuInfo[0];
    
    if (maxFunc >= 1) {
        // Basic features (CPUID function 1)
        __cpuid(cpuInfo, 1);
        
        // EDX features
        sse2 = (cpuInfo[3] & (1 << 26)) != 0;
        
        // ECX features
        sse3 = (cpuInfo[2] & (1 << 0)) != 0;
        ssse3 = (cpuInfo[2] & (1 << 9)) != 0;
        sse4_1 = (cpuInfo[2] & (1 << 19)) != 0;
        sse4_2 = (cpuInfo[2] & (1 << 20)) != 0;
        popcnt = (cpuInfo[2] & (1 << 23)) != 0;
        avx = (cpuInfo[2] & (1 << 28)) != 0;
    }
    
    if (maxFunc >= 7) {
        // Extended features (CPUID function 7, sub-leaf 0)
        __cpuidex(cpuInfo, 7, 0);
        
        // EBX features
        bmi1 = (cpuInfo[1] & (1 << 3)) != 0;
        avx2 = (cpuInfo[1] & (1 << 5)) != 0;
        bmi2 = (cpuInfo[1] & (1 << 8)) != 0;
    }
    
    // Extended function (CPUID 0x80000001)
    __cpuid(cpuInfo, 0x80000000);
    int maxExtFunc = cpuInfo[0];
    
    if (maxExtFunc >= 0x80000001) {
        __cpuid(cpuInfo, 0x80000001);
        // ECX features
        lzcnt = (cpuInfo[2] & (1 << 5)) != 0;
    }
    
#elif defined(__GNUC__)
    unsigned int eax, ebx, ecx, edx;
    unsigned int maxFunc;
    
    // Get max function
    __get_cpuid(0, &maxFunc, &ebx, &ecx, &edx);
    
    if (maxFunc >= 1) {
        __get_cpuid(1, &eax, &ebx, &ecx, &edx);
        
        sse2 = (edx & (1 << 26)) != 0;
        sse3 = (ecx & (1 << 0)) != 0;
        ssse3 = (ecx & (1 << 9)) != 0;
        sse4_1 = (ecx & (1 << 19)) != 0;
        sse4_2 = (ecx & (1 << 20)) != 0;
        popcnt = (ecx & (1 << 23)) != 0;
        avx = (ecx & (1 << 28)) != 0;
    }
    
    if (maxFunc >= 7) {
        __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
        
        bmi1 = (ebx & (1 << 3)) != 0;
        avx2 = (ebx & (1 << 5)) != 0;
        bmi2 = (ebx & (1 << 8)) != 0;
    }
    
    unsigned int maxExtFunc;
    __get_cpuid(0x80000000, &maxExtFunc, &ebx, &ecx, &edx);
    
    if (maxExtFunc >= 0x80000001) {
        __get_cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
        lzcnt = (ecx & (1 << 5)) != 0;
    }
    
#else
    // Conservative defaults for unknown compiler on x86
    sse2 = true;  // All x64 has SSE2
    sse3 = false;
    ssse3 = false;
    sse4_1 = false;
    sse4_2 = false;
    avx = false;
    avx2 = false;
    bmi1 = false;
    bmi2 = false;
    popcnt = false;
    lzcnt = false;
#endif
}

} // namespace brimir::util













