// Headless Vulkan device for [gpu] tests: validation on when the Khronos layer is installed, SKIP when there is no
// usable device (FAIL instead with BRIMIR_REQUIRE_VULKAN=1, the gpu-lavapipe CI job).
#pragma once

#include "catch_amalgamated.hpp"

#include <gpu/headless_device.hpp>
#include <gpu/vk_device.hpp>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace brimir::test {

inline bool RequireVulkan() {
    const char *env = std::getenv("BRIMIR_REQUIRE_VULKAN");
    return env && std::strcmp(env, "1") == 0;
}

inline std::unique_ptr<gpu::VulkanDevice> OpenDevice(gpu::HeadlessOptions options = {.validation = true}) {
    std::string reason;
    auto device = gpu::CreateHeadlessDevice(options, &reason);
    if (!device) {
        if (RequireVulkan()) {
            FAIL("no Vulkan device: " << reason);
        }
        SKIP("no Vulkan device: " << reason);
    }
    if (RequireVulkan() && options.validation && !device->ValidationEnabled()) {
        FAIL("VK_LAYER_KHRONOS_validation is not installed");
    }
    return device;
}

// Destroys the device (validation reports leaks then) and checks that no validation error was reported at all.
inline void CloseAndCheckValidation(std::unique_ptr<gpu::VulkanDevice> &device) {
    const bool validation = device->ValidationEnabled();
    auto errors = device->ValidationErrors();
    device.reset();
    if (validation) {
        CHECK(errors->load() == 0);
    }
}

} // namespace brimir::test
