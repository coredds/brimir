#pragma once

#include <brimir/core/types.hpp>
#include <string>

namespace brimir {

// VDP profiling utilities for frontend integration
class VDPProfilingManager {
public:
    static void EnableProfiling(bool enable);
    static bool IsProfilingEnabled();
    
    // Set log file path (defaults to user's Documents folder)
    static void SetLogPath(const std::string& path);
    static std::string GetDefaultLogPath();
    
    // Get profiling statistics
    struct Stats {
        uint64 totalFrames;
        double avgFrameTimeMs;
        double minFrameTimeMs;
        double maxFrameTimeMs;
    };
    
    static Stats GetStatistics();
};

} // namespace brimir


