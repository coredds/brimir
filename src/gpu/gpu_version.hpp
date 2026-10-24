#pragma once

// The Vulkan baseline the GPU renderer requires (docs/superpowers/specs/2026-10-24-vulkan-gpu-renderer-design.md,
// "Vulkan baseline"). Device selection and the libretro negotiation check exactly this list; a device that misses
// any entry falls back to the software renderer with a logged reason.

#include <volk.h>

#include <array>
#include <cstdint>

namespace brimir::gpu {

inline constexpr uint32_t kRequiredApiVersion = VK_API_VERSION_1_2;

// Compute workgroups never exceed this many invocations (the guaranteed maxComputeWorkGroupInvocations is 128).
inline constexpr uint32_t kMaxWorkgroupInvocations = 128;

// Device extensions beyond core 1.2. None so far.
inline constexpr std::array<const char *, 0> kRequiredDeviceExtensions{};

// A VkPhysicalDeviceFeatures (Vulkan 1.0) feature the renderer needs.
struct CoreFeature {
    const char *name;
    VkBool32 VkPhysicalDeviceFeatures::*member;
};

// Vulkan 1.0 features beyond the guaranteed ones. None so far: storage buffers and rgba8/r32ui storage images with a
// declared format need no optional feature. Features from 1.1/1.2 are added here as per-feature structs (for example
// VkPhysicalDeviceTimelineSemaphoreFeatures), never as VkPhysicalDeviceVulkan11/12Features.
inline constexpr std::array<CoreFeature, 0> kRequiredCoreFeatures{};

} // namespace brimir::gpu
