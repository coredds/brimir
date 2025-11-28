#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

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
    
    /// @brief Start timing a named section
    void Begin(const std::string& name) {
        m_startTimes[name] = std::chrono::high_resolution_clock::now();
    }
    
    /// @brief End timing a named section
    void End(const std::string& name) {
        auto endTime = std::chrono::high_resolution_clock::now();
        auto it = m_startTimes.find(name);
        if (it != m_startTimes.end()) {
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - it->second);
            double ms = duration.count() / 1000.0;
            
            auto& timing = m_timings[name];
            timing.totalMs += ms;
            timing.count++;
            timing.minMs = std::min(timing.minMs, ms);
            timing.maxMs = std::max(timing.maxMs, ms);
            
            m_startTimes.erase(it);
        }
    }
    
    /// @brief Get timing data for a section
    const Timing* GetTiming(const std::string& name) const {
        auto it = m_timings.find(name);
        return it != m_timings.end() ? &it->second : nullptr;
    }
    
    /// @brief Get all timings
    const std::unordered_map<std::string, Timing>& GetAllTimings() const {
        return m_timings;
    }
    
    /// @brief Reset all timing data
    void Reset() {
        m_timings.clear();
        m_startTimes.clear();
    }
    
    /// @brief Get profiler report as string
    std::string GetReport() const {
        std::string report = "=== Performance Profile ===\n";
        for (const auto& [name, timing] : m_timings) {
            report += name + ": avg=" + std::to_string(timing.avgMs()) + "ms, " +
                     "min=" + std::to_string(timing.minMs) + "ms, " +
                     "max=" + std::to_string(timing.maxMs) + "ms, " +
                     "samples=" + std::to_string(timing.count) + "\n";
        }
        return report;
    }
    
private:
    std::unordered_map<std::string, std::chrono::high_resolution_clock::time_point> m_startTimes;
    std::unordered_map<std::string, Timing> m_timings;
};

/// @brief RAII helper for automatic timing
class ScopedTimer {
public:
    ScopedTimer(Profiler& profiler, const std::string& name)
        : m_profiler(profiler), m_name(name) {
        m_profiler.Begin(m_name);
    }
    
    ~ScopedTimer() {
        m_profiler.End(m_name);
    }
    
private:
    Profiler& m_profiler;
    std::string m_name;
};

} // namespace brimir







