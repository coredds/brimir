#pragma once

// The Vulkan baseline the GPU renderer requires (docs/superpowers/specs/2026-10-24-vulkan-gpu-renderer-design.md,
// "Vulkan baseline"). Device selection and the libretro negotiation check exactly this list (API version,
// extensions, core features, limits); a device that misses any entry falls back to the software renderer with a
// logged reason.

#include "gpu/vdp2/vdp2_bindings.hpp"

#include <volk.h>

#include <array>
#include <cstdint>
#include <string>

namespace brimir::gpu {

inline constexpr uint32_t kRequiredApiVersion = VK_API_VERSION_1_2;

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

// A VkPhysicalDeviceLimits minimum the renderer needs beyond (or at) what Vulkan guarantees.
struct RequiredLimit {
    const char *name;
    uint32_t VkPhysicalDeviceLimits::*member;
    uint32_t minimum;
};

// Per-stage descriptor counts of the VDP2 pipelines (src/gpu/vdp2/vdp2_bindings.hpp). The background shader's seven
// storage buffers exceed the guaranteed maxPerStageDescriptorStorageBuffers (4).
inline constexpr uint32_t kRequiredStageStorageBuffers = 7;
inline constexpr uint32_t kRequiredStageStorageImages = 3;
inline constexpr uint32_t kRequiredStageSampledImages = 4;
inline constexpr uint32_t kRequiredStageResources = 11;
static_assert(kRequiredStageStorageBuffers == vdp2::kMaxStageUsage.storageBuffers &&
                  kRequiredStageStorageImages == vdp2::kMaxStageUsage.storageImages &&
                  kRequiredStageSampledImages == vdp2::kMaxStageUsage.sampledImages &&
                  kRequiredStageResources == vdp2::kMaxStageUsage.resources,
              "the required limits must match the VDP2 binding table");

inline constexpr std::array<RequiredLimit, 7> kRequiredLimits{{
    {"maxPerStageDescriptorStorageBuffers", &VkPhysicalDeviceLimits::maxPerStageDescriptorStorageBuffers,
     kRequiredStageStorageBuffers},
    {"maxPerStageDescriptorStorageImages", &VkPhysicalDeviceLimits::maxPerStageDescriptorStorageImages,
     kRequiredStageStorageImages},
    {"maxPerStageDescriptorSampledImages", &VkPhysicalDeviceLimits::maxPerStageDescriptorSampledImages,
     kRequiredStageSampledImages},
    {"maxPerStageResources", &VkPhysicalDeviceLimits::maxPerStageResources, kRequiredStageResources},
    {"maxComputeWorkGroupInvocations", &VkPhysicalDeviceLimits::maxComputeWorkGroupInvocations,
     vdp2::kMaxWorkgroupInvocations},
    {"maxPushConstantsSize", &VkPhysicalDeviceLimits::maxPushConstantsSize, vdp2::kMaxPushConstantSize},
    // The NBG fetch pass's groupshared arrays (within the guaranteed 16384)
    {"maxComputeSharedMemorySize", &VkPhysicalDeviceLimits::maxComputeSharedMemorySize,
     vdp2::kMaxComputeSharedMemorySize},
}};

// Empty when `limits` meet kRequiredLimits, else the first unmet one as "<name> <value> < <minimum>".
inline std::string UnmetLimit(const VkPhysicalDeviceLimits &limits) {
    for (const RequiredLimit &limit : kRequiredLimits) {
        const uint32_t value = limits.*limit.member;
        if (value < limit.minimum) {
            return std::string(limit.name) + " " + std::to_string(value) + " < " + std::to_string(limit.minimum);
        }
    }
    return {};
}

} // namespace brimir::gpu
