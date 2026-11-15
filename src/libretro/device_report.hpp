#pragma once

// One log line per content load that says what the core runs on: the renderer drawing, the frontend's GPU and its
// Vulkan version, the host's threads and why the Vulkan renderer is not drawing (if it was selected). For bug reports
// from devices nobody here has (phones).

#include <cstdint>
#include <string>

namespace brimir::libretro {

struct DeviceReport {
    bool vulkanRenderer = false;   // the Vulkan renderer is the one drawing
    std::string gpuName;           // empty: no Vulkan device seen
    uint32_t vulkanApiVersion = 0; // VK_MAKE_API_VERSION value; 0: none
    unsigned hostThreads = 0;      // std::thread::hardware_concurrency()
    std::string fallbackReason;    // empty: none
};

// Device report: renderer <software|vulkan>; GPU "<name>"; Vulkan API <major>.<minor>.<patch> (renderer needs 1.2);
// host threads <N>; fallback "<reason>"   (GPU "none", Vulkan API none and fallback "none" when empty or 0)
std::string FormatDeviceReport(const DeviceReport &report);

} // namespace brimir::libretro
