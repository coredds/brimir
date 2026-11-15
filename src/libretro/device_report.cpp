#include "device_report.hpp"

namespace brimir::libretro {

std::string FormatDeviceReport(const DeviceReport &report) {
    // VK_API_VERSION_MAJOR / MINOR / PATCH (no Vulkan headers here: the line is logged in every build)
    std::string api = "none";
    if (report.vulkanApiVersion != 0) {
        const uint32_t v = report.vulkanApiVersion;
        api = std::to_string((v >> 22) & 0x7Fu) + "." + std::to_string((v >> 12) & 0x3FFu) + "." +
              std::to_string(v & 0xFFFu);
    }
    return std::string("Device report: renderer ") + (report.vulkanRenderer ? "vulkan" : "software") + "; GPU \"" +
           (report.gpuName.empty() ? "none" : report.gpuName) + "\"; Vulkan API " + api +
           " (renderer needs 1.2); host threads " + std::to_string(report.hostThreads) + "; fallback \"" +
           (report.fallbackReason.empty() ? "none" : report.fallbackReason) + "\"";
}

} // namespace brimir::libretro
