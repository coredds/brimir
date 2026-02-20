#include <brimir/hw/vdp/vdp_profiler.hpp>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

namespace brimir::vdp {

std::string VDPResolutionMode::ToString() const {
    std::ostringstream oss;
    oss << width << "x" << height;
    if (interlaced) oss << "i";
    if (hiRes) oss << " HiRes";
    return oss.str();
}

VDPProfiler::VDPProfiler() = default;

VDPProfiler::~VDPProfiler() {
    if (m_enabled && m_logFile.is_open()) {
        WriteSummary();
        m_logFile.close();
    }
}

void VDPProfiler::Enable(bool enable) {
    if (enable && !m_enabled) {
        if (!m_logPath.empty() && !m_logFile.is_open()) {
            m_logFile.open(m_logPath, std::ios::out | std::ios::trunc);
            if (!m_logFile.is_open()) {
                m_logPath = "brimir_vdp_profile.txt";
                m_logFile.open(m_logPath, std::ios::out | std::ios::trunc);
            }
        }
    } else if (!enable && m_enabled) {
        if (m_logFile.is_open()) {
            WriteSummary();
            m_logFile.close();
        }
    }
    m_enabled = enable;
}

void VDPProfiler::SetLogPath(const std::string& path) {
    if (m_logFile.is_open()) {
        WriteSummary();
        m_logFile.close();
    }
    m_logPath = path;
    if (m_enabled && !m_logPath.empty()) {
        m_logFile.open(m_logPath, std::ios::out | std::ios::trunc);
    }
}

void VDPProfiler::BeginFrame(uint64 frameNumber, const VDPResolutionMode& mode) {
    if (!m_enabled) return;
    
    m_currentFrame = {};
    m_currentFrame.mode = mode;
    m_frameStartTime = std::chrono::high_resolution_clock::now();
    m_frameInProgress = true;
}

void VDPProfiler::EndFrame() {
    if (!m_enabled || !m_frameInProgress) return;
    
    auto endTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = endTime - m_frameStartTime;
    m_currentFrame.renderTimeMs = duration.count();
    
    AccumulateFrame(m_currentFrame);
    m_totalFrames++;
    
    // Write summary every 300 frames (~5 seconds at 60fps)
    if (m_totalFrames - m_lastWriteFrame >= 300) {
        WriteSummary();
        m_lastWriteFrame = m_totalFrames;
    }
    
    m_frameInProgress = false;
}

void VDPProfiler::BeginSection() {
    if (!m_enabled || !m_frameInProgress) return;
    m_sectionStartTime = std::chrono::high_resolution_clock::now();
}

double VDPProfiler::EndSection() {
    if (!m_enabled || !m_frameInProgress) return 0.0;
    auto endTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::micro> duration = endTime - m_sectionStartTime;
    return duration.count();
}

void VDPProfiler::AccumulateFrame(const FrameProfile& frame) {
    ModeStats& stats = m_modeStats[frame.mode];
    stats.frameCount++;
    stats.totalFrameTimeMs += frame.renderTimeMs;
    stats.minFrameTimeMs = std::min(stats.minFrameTimeMs, frame.renderTimeMs);
    stats.maxFrameTimeMs = std::max(stats.maxFrameTimeMs, frame.renderTimeMs);
    
    stats.spriteWindowCalcUs += frame.spriteWindowCalcUs;
    stats.spriteLayerUs += frame.spriteLayerUs;
    stats.bgWindowCalcUs += frame.bgWindowCalcUs;
    stats.rbg0Us += frame.rbg0Us;
    stats.rbg1Us += frame.rbg1Us;
    stats.nbg0Us += frame.nbg0Us;
    stats.nbg1Us += frame.nbg1Us;
    stats.nbg2Us += frame.nbg2Us;
    stats.nbg3Us += frame.nbg3Us;
    stats.composeUs += frame.composeUs;
    stats.bobDeinterlaceUs += frame.bobDeinterlaceUs;
    
    // Granular sub-operations
    stats.vramFetchUs += frame.vramFetchUs;
    stats.cramLookupUs += frame.cramLookupUs;
    stats.pixelWriteUs += frame.pixelWriteUs;
    stats.windowClipUs += frame.windowClipUs;
    stats.rotationCalcUs += frame.rotationCalcUs;
}

void VDPProfiler::WriteDiagnostic(const std::string& message) {
    if (m_logFile.is_open()) {
        m_logFile << message << std::endl;
        m_logFile.flush(); // Ensure it's written immediately
    }
}

void VDPProfiler::WriteSummary() {
    if (!m_logFile.is_open() || m_modeStats.empty()) return;
    
    // Clear file and write fresh summary
    m_logFile.seekp(0);
    
    m_logFile << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
    m_logFile << "║                    BRIMIR VDP PERFORMANCE PROFILER                           ║\n";
    m_logFile << "║                         Component Breakdown                                  ║\n";
    m_logFile << "╚══════════════════════════════════════════════════════════════════════════════╝\n\n";
    m_logFile << "Total frames analyzed: " << m_totalFrames << "\n";
    m_logFile << "Target frame time: 16.67ms (60 FPS)\n\n";
    
    for (const auto& [mode, stats] : m_modeStats) {
        double avgMs = stats.AvgFrameTimeMs();
        double totalUs = stats.TotalComponentUs();
        double avgTotalUs = stats.frameCount > 0 ? totalUs / stats.frameCount : 0;
        
        bool overBudget = avgMs > 16.67;
        
        m_logFile << "══════════════════════════════════════════════════════════════════════════════\n";
        m_logFile << "  Resolution: " << mode.ToString() << "\n";
        m_logFile << "══════════════════════════════════════════════════════════════════════════════\n";
        m_logFile << "  Frames: " << stats.frameCount << "\n";
        m_logFile << "  Frame Time: " << std::fixed << std::setprecision(2) 
                  << avgMs << " ms avg  |  " 
                  << stats.minFrameTimeMs << " ms min  |  " 
                  << stats.maxFrameTimeMs << " ms max";
        if (overBudget) {
            m_logFile << "  *** OVER BUDGET ***";
        }
        m_logFile << "\n\n";
        
        if (avgTotalUs > 0) {
            m_logFile << "  Component Breakdown (avg per frame):\n";
            m_logFile << "  ─────────────────────────────────────────────────────────────────────────\n";
            
            // Calculate percentages and find bottleneck
            struct Component {
                const char* name;
                double avgUs;
                double pct;
            };
            
            std::array<Component, 11> components = {{
                {"Sprite Window", stats.spriteWindowCalcUs / stats.frameCount, 0},
                {"Sprite Layer ", stats.spriteLayerUs / stats.frameCount, 0},
                {"BG Window    ", stats.bgWindowCalcUs / stats.frameCount, 0},
                {"RBG0         ", stats.rbg0Us / stats.frameCount, 0},
                {"RBG1         ", stats.rbg1Us / stats.frameCount, 0},
                {"NBG0         ", stats.nbg0Us / stats.frameCount, 0},
                {"NBG1         ", stats.nbg1Us / stats.frameCount, 0},
                {"NBG2         ", stats.nbg2Us / stats.frameCount, 0},
                {"NBG3         ", stats.nbg3Us / stats.frameCount, 0},
                {"Compose      ", stats.composeUs / stats.frameCount, 0},
                {"Bob Deinterl ", stats.bobDeinterlaceUs / stats.frameCount, 0},
            }};
            
            // Calculate percentages
            for (auto& c : components) {
                c.pct = avgTotalUs > 0 ? (c.avgUs / avgTotalUs) * 100.0 : 0;
            }
            
            // Find max for bottleneck detection
            double maxUs = 0;
            size_t maxIdx = 0;
            for (size_t i = 0; i < components.size(); i++) {
                if (components[i].avgUs > maxUs) {
                    maxUs = components[i].avgUs;
                    maxIdx = i;
                }
            }
            
            // Print components
            for (size_t i = 0; i < components.size(); i++) {
                const auto& c = components[i];
                if (c.avgUs < 1.0) continue; // Skip negligible components
                
                m_logFile << "    " << c.name << ": ";
                m_logFile << std::setw(8) << std::fixed << std::setprecision(1) << c.avgUs << " µs  ";
                m_logFile << "(" << std::setw(5) << std::setprecision(1) << c.pct << "%)";
                
                // Visual bar
                int barLen = static_cast<int>(c.pct / 2.5);
                m_logFile << "  ";
                for (int b = 0; b < barLen; b++) m_logFile << "█";
                
                if (i == maxIdx && c.pct > 25.0) {
                    m_logFile << "  <-- BOTTLENECK";
                }
                m_logFile << "\n";
            }
            
            m_logFile << "  ─────────────────────────────────────────────────────────────────────────\n";
            m_logFile << "    TOTAL:         " << std::setw(8) << std::setprecision(1) << avgTotalUs << " µs\n";
            
            // Granular sub-operation breakdown
            double vramAvg = stats.vramFetchUs / stats.frameCount;
            double cramAvg = stats.cramLookupUs / stats.frameCount;
            double pixelAvg = stats.pixelWriteUs / stats.frameCount;
            double windowAvg = stats.windowClipUs / stats.frameCount;
            double rotAvg = stats.rotationCalcUs / stats.frameCount;
            double granularTotal = vramAvg + cramAvg + pixelAvg + windowAvg + rotAvg;
            
            if (granularTotal > 1.0) {
                m_logFile << "\n  Granular Sub-Operations (within all layers):\n";
                m_logFile << "  ─────────────────────────────────────────────────────────────────────────\n";
                
                if (vramAvg > 1.0) {
                    m_logFile << "    VRAM Fetch   : " << std::setw(8) << std::fixed << std::setprecision(1) 
                              << vramAvg << " µs  (" << std::setw(5) << std::setprecision(1) 
                              << (vramAvg / avgTotalUs) * 100.0 << "%)\n";
                }
                if (cramAvg > 1.0) {
                    m_logFile << "    CRAM Lookup  : " << std::setw(8) << std::fixed << std::setprecision(1) 
                              << cramAvg << " µs  (" << std::setw(5) << std::setprecision(1) 
                              << (cramAvg / avgTotalUs) * 100.0 << "%)\n";
                }
                if (pixelAvg > 1.0) {
                    m_logFile << "    Pixel Write  : " << std::setw(8) << std::fixed << std::setprecision(1) 
                              << pixelAvg << " µs  (" << std::setw(5) << std::setprecision(1) 
                              << (pixelAvg / avgTotalUs) * 100.0 << "%)\n";
                }
                if (windowAvg > 1.0) {
                    m_logFile << "    Window Clip  : " << std::setw(8) << std::fixed << std::setprecision(1) 
                              << windowAvg << " µs  (" << std::setw(5) << std::setprecision(1) 
                              << (windowAvg / avgTotalUs) * 100.0 << "%)\n";
                }
                if (rotAvg > 1.0) {
                    m_logFile << "    Rotation Calc: " << std::setw(8) << std::fixed << std::setprecision(1) 
                              << rotAvg << " µs  (" << std::setw(5) << std::setprecision(1) 
                              << (rotAvg / avgTotalUs) * 100.0 << "%)\n";
                }
                m_logFile << "  ─────────────────────────────────────────────────────────────────────────\n";
                m_logFile << "    Sub-Total:     " << std::setw(8) << std::setprecision(1) << granularTotal << " µs\n";
            }
        } else {
            m_logFile << "  (No component timing data - profiling hooks may not be active)\n";
        }
        
        m_logFile << "\n";
    }
    
    m_logFile.flush();
}

} // namespace brimir::vdp
