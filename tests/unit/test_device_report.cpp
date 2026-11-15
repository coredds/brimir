// The device report line logged once per content load (src/libretro/device_report.hpp).

#include "catch_amalgamated.hpp"

#include "device_report.hpp"

#include <cstdint>

using namespace brimir::libretro;

// VK_MAKE_API_VERSION without the Vulkan headers (brimir_tests is also built without BRIMIR_VULKAN)
static constexpr uint32_t MakeApiVersion(uint32_t major, uint32_t minor, uint32_t patch) {
    return (major << 22) | (minor << 12) | patch;
}

TEST_CASE("device report: the Vulkan renderer on a 1.3 GPU", "[device-report]") {
    const DeviceReport report{true, "Test GPU", MakeApiVersion(1, 3, 128), 8, ""};
    CHECK(FormatDeviceReport(report) ==
          "Device report: renderer vulkan; GPU \"Test GPU\"; Vulkan API 1.3.128 (renderer needs 1.2); host threads 8; "
          "fallback \"none\"");
}

TEST_CASE("device report: a 1.1 GPU falls back to software with the reason", "[device-report]") {
    const DeviceReport report{false, "Old GPU", MakeApiVersion(1, 1, 177), 4, "Vulkan 1.1 < 1.2"};
    CHECK(FormatDeviceReport(report) ==
          "Device report: renderer software; GPU \"Old GPU\"; Vulkan API 1.1.177 (renderer needs 1.2); host threads 4; "
          "fallback \"Vulkan 1.1 < 1.2\"");
}

TEST_CASE("device report: no Vulkan device seen", "[device-report]") {
    DeviceReport report;
    report.hostThreads = 2;
    CHECK(FormatDeviceReport(report) ==
          "Device report: renderer software; GPU \"none\"; Vulkan API none (renderer needs 1.2); host threads 2; "
          "fallback \"none\"");
}
