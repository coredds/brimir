#pragma once

#include <brimir/core/types.hpp>
#include <chrono>
#include <string>
#include <fstream>
#include <array>
#include <map>

namespace brimir::vdp {

struct VDPResolutionMode {
    uint16 width;
    uint16 height;
    bool interlaced;
    bool hiRes;
    uint8 colorMode;
    
    bool operator==(const VDPResolutionMode& other) const {
        return width == other.width && height == other.height && 
               interlaced == other.interlaced && hiRes == other.hiRes && 
               colorMode == other.colorMode;
    }
    
    bool operator<(const VDPResolutionMode& other) const {
        if (width != other.width) return width < other.width;
        if (height != other.height) return height < other.height;
        if (interlaced != other.interlaced) return interlaced < other.interlaced;
        if (hiRes != other.hiRes) return hiRes < other.hiRes;
        return colorMode < other.colorMode;
    }
    
    std::string ToString() const;
};

// Per-mode aggregated statistics
struct ModeStats {
    uint64 frameCount = 0;
    double totalFrameTimeMs = 0;
    double minFrameTimeMs = 999999.0;
    double maxFrameTimeMs = 0;
    
    // Component totals (microseconds)
    double spriteWindowCalcUs = 0;
    double spriteLayerUs = 0;
    double bgWindowCalcUs = 0;
    double rbg0Us = 0;
    double rbg1Us = 0;
    double nbg0Us = 0;
    double nbg1Us = 0;
    double nbg2Us = 0;
    double nbg3Us = 0;
    double composeUs = 0;
    double bobDeinterlaceUs = 0;
    
    // Granular sub-operation totals (microseconds)
    double vramFetchUs = 0;
    double cramLookupUs = 0;
    double pixelWriteUs = 0;
    double windowClipUs = 0;
    double rotationCalcUs = 0;
    
    // Derived values
    double AvgFrameTimeMs() const { return frameCount > 0 ? totalFrameTimeMs / frameCount : 0; }
    double TotalComponentUs() const {
        return spriteWindowCalcUs + spriteLayerUs + bgWindowCalcUs + 
               rbg0Us + rbg1Us + nbg0Us + nbg1Us + nbg2Us + nbg3Us + 
               composeUs + bobDeinterlaceUs;
    }
};

struct FrameProfile {
    VDPResolutionMode mode;
    double renderTimeMs = 0;
    
    // Detailed VDP2 component timing (per frame totals in microseconds)
    double spriteWindowCalcUs = 0;
    double spriteLayerUs = 0;
    double bgWindowCalcUs = 0;
    double rbg0Us = 0;
    double rbg1Us = 0;
    double nbg0Us = 0;
    double nbg1Us = 0;
    double nbg2Us = 0;
    double nbg3Us = 0;
    double composeUs = 0;
    double bobDeinterlaceUs = 0;
    
    // Granular sub-operation timing (microseconds)
    double vramFetchUs = 0;         // VRAM pattern/character data reads
    double cramLookupUs = 0;        // CRAM color lookups
    double pixelWriteUs = 0;        // Writing pixels to layer buffers
    double windowClipUs = 0;        // Window clipping calculations
    double rotationCalcUs = 0;      // Rotation parameter calculations
};

class VDPProfiler {
public:
    VDPProfiler();
    ~VDPProfiler();
    
    void Enable(bool enable);
    bool IsEnabled() const { return m_enabled; }
    
    void SetLogPath(const std::string& path);
    std::string GetLogPath() const { return m_logPath; }
    
    // Called at the start of frame rendering
    void BeginFrame(uint64 frameNumber, const VDPResolutionMode& mode);
    
    // Called at the end of frame rendering
    void EndFrame();
    
    // Detailed VDP2 component timing
    void BeginSection();
    double EndSection(); // Returns elapsed microseconds
    void AddSpriteWindowCalc(double us) { if (m_frameInProgress) m_currentFrame.spriteWindowCalcUs += us; }
    void AddSpriteLayer(double us) { if (m_frameInProgress) m_currentFrame.spriteLayerUs += us; }
    void AddBGWindowCalc(double us) { if (m_frameInProgress) m_currentFrame.bgWindowCalcUs += us; }
    void AddRBG0(double us) { if (m_frameInProgress) m_currentFrame.rbg0Us += us; }
    void AddRBG1(double us) { if (m_frameInProgress) m_currentFrame.rbg1Us += us; }
    void AddNBG0(double us) { if (m_frameInProgress) m_currentFrame.nbg0Us += us; }
    void AddNBG1(double us) { if (m_frameInProgress) m_currentFrame.nbg1Us += us; }
    void AddNBG2(double us) { if (m_frameInProgress) m_currentFrame.nbg2Us += us; }
    void AddNBG3(double us) { if (m_frameInProgress) m_currentFrame.nbg3Us += us; }
    void AddCompose(double us) { if (m_frameInProgress) m_currentFrame.composeUs += us; }
    void AddBobDeinterlace(double us) { if (m_frameInProgress) m_currentFrame.bobDeinterlaceUs += us; }
    
    // Granular sub-operation timing
    void AddVRAMFetch(double us) { if (m_frameInProgress) m_currentFrame.vramFetchUs += us; }
    void AddCRAMLookup(double us) { if (m_frameInProgress) m_currentFrame.cramLookupUs += us; }
    void AddPixelWrite(double us) { if (m_frameInProgress) m_currentFrame.pixelWriteUs += us; }
    void AddWindowClip(double us) { if (m_frameInProgress) m_currentFrame.windowClipUs += us; }
    void AddRotationCalc(double us) { if (m_frameInProgress) m_currentFrame.rotationCalcUs += us; }
    
    // Write diagnostic info directly to log (for debugging)
    void WriteDiagnostic(const std::string& message);

private:
    bool m_enabled = false;
    std::string m_logPath;
    std::ofstream m_logFile;
    
    // Current frame being profiled
    FrameProfile m_currentFrame;
    bool m_frameInProgress = false;
    std::chrono::high_resolution_clock::time_point m_frameStartTime;
    std::chrono::high_resolution_clock::time_point m_sectionStartTime;
    
    // Per-mode statistics
    std::map<VDPResolutionMode, ModeStats> m_modeStats;
    uint64 m_totalFrames = 0;
    uint64 m_lastWriteFrame = 0;
    
    void AccumulateFrame(const FrameProfile& frame);
    void WriteSummary();
};

} // namespace brimir::vdp
