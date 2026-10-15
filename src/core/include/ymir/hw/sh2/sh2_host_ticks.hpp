#pragma once

// Brimir: cheap host-time ticks for the SH-2 host-time accounting (see src/core/BRIMIR_FORK.md).
//
// SH2::Advance is called tens of millions of times per 1,800 frames, so reading std::chrono::steady_clock twice per
// call (about 38 ns) costs more than the work it measures in short calls. HostTicks() is a few cycles: the time-stamp
// counter on x86-64 and the virtual counter on AArch64. Ticks are converted to nanoseconds only when consumed.

#include <ymir/core/types.hpp>

#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
    #if defined(_MSC_VER)
        #include <intrin.h>
    #else
        #include <x86intrin.h>
    #endif
#endif

namespace ymir::sh2 {

/// @brief Reads `steady_clock` as nanoseconds since its epoch. Slow (about 20 ns): only for calibration.
inline uint64 HostSteadyNs() noexcept {
    return static_cast<uint64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// @brief Reads a monotonic host tick counter with a constant rate. The rate is unspecified: convert with
/// HostTicksToNs() and a calibration of ticks against nanoseconds.
///
/// x86-64: the time-stamp counter, which is constant-rate and synchronized between cores on every CPU made since about
/// 2008. AArch64 (GCC/Clang): `cntvct_el0`. Elsewhere: `steady_clock` nanoseconds.
inline uint64 HostTicks() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return __rdtsc();
#elif defined(__aarch64__)
    uint64 value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
    return HostSteadyNs();
#endif
}

/// @brief Converts `ticks` to nanoseconds with the ratio `calibNs / calibTicks` (nanoseconds elapsed over the ticks
/// elapsed in a calibration interval), computing `ticks * calibNs / calibTicks` without overflow. Returns 0 if
/// `calibTicks` is zero (no calibration interval yet).
inline uint64 HostTicksToNs(uint64 ticks, uint64 calibTicks, uint64 calibNs) noexcept {
    if (calibTicks == 0) {
        return 0;
    }
#if defined(__SIZEOF_INT128__)
    return static_cast<uint64>(static_cast<unsigned __int128>(ticks) * calibNs / calibTicks);
#else
    // ticks = q * calibTicks + r, with r < calibTicks: the product r * calibNs is exact when it fits, otherwise the
    // remainder's share (less than calibNs) is computed in floating point, which is accurate to well under a nanosecond.
    const uint64 q = ticks / calibTicks;
    const uint64 r = ticks % calibTicks;
    uint64 rest;
    if (calibNs == 0 || r <= UINT64_MAX / calibNs) {
        rest = r * calibNs / calibTicks;
    } else {
        rest = static_cast<uint64>(static_cast<double>(r) / static_cast<double>(calibTicks) *
                                   static_cast<double>(calibNs));
    }
    return q * calibNs + rest;
#endif
}

} // namespace ymir::sh2
