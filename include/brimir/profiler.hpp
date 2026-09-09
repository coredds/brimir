#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace brimir {

/// @brief Simple performance profiler for identifying bottlenecks
class Profiler {
public:
    struct Timing {
        double totalMs = 0.0;
        size_t count = 0;
        double minMs = 1e9;
        double maxMs = 0.0;
        
        double avgMs() const { return count > 0 ? totalMs / count : 0.0; }
    };
    
    /// @brief State transitions clear samples and invalidate active scopes.
    /// Reapplying the current state is a no-op; enabling starts a fresh session.
    void SetEnabled(bool enabled) {
        std::lock_guard lock(m_mutex);
        const auto state = m_state.load(std::memory_order_relaxed);
        if (static_cast<bool>(state & 1) != enabled) {
            m_state.store(state + 1, std::memory_order_relaxed);
            m_timings.clear();
        }
    }
    
    /// @brief Get a snapshot, never a pointer into mutable aggregates.
    std::optional<Timing> GetTiming(const std::string& name) const {
        std::lock_guard lock(m_mutex);
        auto it = m_timings.find(name);
        return it != m_timings.end() ? std::optional<Timing>(it->second) : std::nullopt;
    }
    
    /// @brief Get a snapshot of all timings.
    std::unordered_map<std::string, Timing> GetAllTimings() const {
        std::lock_guard lock(m_mutex);
        return m_timings;
    }
    
    /// @brief Clear samples and invalidate active scopes, preserving enabled state.
    void Reset() {
        std::lock_guard lock(m_mutex);
        m_state.store(m_state.load(std::memory_order_relaxed) + 2, std::memory_order_relaxed);
        m_timings.clear();
    }
    
    /// @brief Get profiler report as string
    std::string GetReport() const {
        const auto timings = GetAllTimings();
        if (timings.empty()) {
            return {};
        }
        std::string report = "=== Performance Profile ===\n";
        for (const auto& [name, timing] : timings) {
            report += name + ": avg=" + std::to_string(timing.avgMs()) + "ms, " +
                     "min=" + std::to_string(timing.minMs) + "ms, " +
                     "max=" + std::to_string(timing.maxMs) + "ms, " +
                     "samples=" + std::to_string(timing.count) + "\n";
        }
        return report;
    }
    
private:
    friend class ScopedTimer;

    // Low bit enables collection; remaining bits identify the session. A single
    // atomic snapshot avoids mixing an old enabled flag with a new generation.
    // Aggregates are only accessed under the mutex, so relaxed ordering suffices.
    std::atomic<uint64_t> m_state{0};
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Timing> m_timings;
};

/// @brief RAII helper for automatic timing
class ScopedTimer {
public:
    ScopedTimer(Profiler& profiler, std::string_view name)
        : m_profiler(profiler), m_state(profiler.m_state.load(std::memory_order_relaxed)) {
        if (!(m_state & 1)) {
            return;
        }
        m_name = name;
        m_start = std::chrono::steady_clock::now();
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    
    ~ScopedTimer() {
        if (!(m_state & 1) || m_profiler.m_state.load(std::memory_order_relaxed) != m_state) {
            return;
        }
        const auto end = std::chrono::steady_clock::now();
        std::lock_guard lock(m_profiler.m_mutex);
        // Reset/toggle may have happened while this scope was running or waiting.
        if (m_profiler.m_state.load(std::memory_order_relaxed) != m_state) {
            return;
        }
        const double ms = std::chrono::duration<double, std::milli>(end - m_start).count();
        auto& timing = m_profiler.m_timings[m_name];
        timing.totalMs += ms;
        timing.count++;
        timing.minMs = std::min(timing.minMs, ms);
        timing.maxMs = std::max(timing.maxMs, ms);
    }
    
private:
    Profiler& m_profiler;
    uint64_t m_state;
    std::string m_name;
    std::chrono::steady_clock::time_point m_start;
};

} // namespace brimir
