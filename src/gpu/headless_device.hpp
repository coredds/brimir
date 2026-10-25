#pragma once

// A Vulkan device of Brimir's own, for tests and tools (brimir_tests, brimir_vkhost, brimir_bench). Loads the system
// Vulkan loader with volkInitialize(); the libretro core never calls this (it wraps the frontend's device).

#include "vk_device.hpp"

#include <memory>
#include <string>

namespace brimir::gpu {

struct HeadlessOptions {
    // Enable VK_LAYER_KHRONOS_validation (when installed) and count its validation error messages
    // (VulkanDevice::ValidationErrors). Warnings, errors and loader messages are printed to stderr. With the
    // environment variable BRIMIR_VK_SYNC_VALIDATION=1, synchronization validation is enabled too, including shader
    // accesses through descriptors when the layer has VK_EXT_layer_settings (hazards count as validation errors).
    bool validation = false;
    // Case-insensitive substring of the device name. When empty, the BRIMIR_VK_DEVICE environment variable is used;
    // when that is unset too, the first device meeting the baseline is picked.
    std::string preferredDeviceName;
};

// Creates an instance and a device with one compute-capable queue (graphics + compute preferred) on the first device
// meeting the baseline (gpu_version.hpp: API version, core features, device extensions). Returns nullptr and sets
// *reason (human-readable) when there is no loader, no instance, or no suitable device.
std::unique_ptr<VulkanDevice> CreateHeadlessDevice(const HeadlessOptions &options, std::string *reason = nullptr);

} // namespace brimir::gpu
