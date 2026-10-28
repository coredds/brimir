#pragma once

// The Vulkan baseline the GPU renderer requires (docs/superpowers/specs/2026-10-24-vulkan-gpu-renderer-design.md,
// "Vulkan baseline"). Device selection and the libretro negotiation check exactly this list (API version,
// extensions, core features, limits); a device that misses any entry falls back to the software renderer with a
// logged reason.

#include "gpu/vdp1/vdp1_bindings.hpp"
#include "gpu/vdp2/vdp2_bindings.hpp"

#include <volk.h>

#include <algorithm>
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
// storage buffers exceed the guaranteed maxPerStageDescriptorStorageBuffers (4). The VDP1 pipelines
// (src/gpu/vdp1/vdp1_bindings.hpp) use at most five storage buffers and nothing else, so VDP2 sets every maximum.
inline constexpr uint32_t kRequiredStageStorageBuffers = 7;
inline constexpr uint32_t kRequiredStageStorageImages = 3;
inline constexpr uint32_t kRequiredStageSampledImages = 5;
inline constexpr uint32_t kRequiredStageResources = 11;
static_assert(kRequiredStageStorageBuffers ==
                      std::max(vdp2::kMaxStageUsage.storageBuffers, vdp1::kMaxStageUsage.storageBuffers) &&
                  kRequiredStageStorageImages ==
                      std::max(vdp2::kMaxStageUsage.storageImages, vdp1::kMaxStageUsage.storageImages) &&
                  kRequiredStageSampledImages ==
                      std::max(vdp2::kMaxStageUsage.sampledImages, vdp1::kMaxStageUsage.sampledImages) &&
                  kRequiredStageResources == std::max(vdp2::kMaxStageUsage.resources, vdp1::kMaxStageUsage.resources),
              "the required limits must match the VDP2 and VDP1 binding tables");

// The largest of VDP2's and VDP1's workgroups, push-constant blocks and groupshared memory.
inline constexpr uint32_t kRequiredWorkgroupInvocations =
    std::max(vdp2::kMaxWorkgroupInvocations, vdp1::kWorkgroupSize);
inline constexpr uint32_t kRequiredPushConstantSize =
    std::max(vdp2::kMaxPushConstantSize, vdp1::kMaxPushConstantSize);
inline constexpr uint32_t kRequiredSharedMemorySize =
    std::max(vdp2::kMaxComputeSharedMemorySize, vdp1::kMaxComputeSharedMemorySize);
// One storage buffer binding's range: VDP1's fragment buffer (32 MiB), within the guaranteed 2^27.
inline constexpr uint32_t kRequiredStorageBufferRange = vdp1::kMaxStorageBufferRange;
static_assert(kRequiredStorageBufferRange >= vdp2::CompositeBufferSize(vdp2::kMaxRenderScale) &&
              kRequiredStorageBufferRange <= (1u << 27));

inline constexpr std::array<RequiredLimit, 8> kRequiredLimits{{
    {"maxPerStageDescriptorStorageBuffers", &VkPhysicalDeviceLimits::maxPerStageDescriptorStorageBuffers,
     kRequiredStageStorageBuffers},
    {"maxPerStageDescriptorStorageImages", &VkPhysicalDeviceLimits::maxPerStageDescriptorStorageImages,
     kRequiredStageStorageImages},
    {"maxPerStageDescriptorSampledImages", &VkPhysicalDeviceLimits::maxPerStageDescriptorSampledImages,
     kRequiredStageSampledImages},
    {"maxPerStageResources", &VkPhysicalDeviceLimits::maxPerStageResources, kRequiredStageResources},
    {"maxComputeWorkGroupInvocations", &VkPhysicalDeviceLimits::maxComputeWorkGroupInvocations,
     kRequiredWorkgroupInvocations},
    {"maxPushConstantsSize", &VkPhysicalDeviceLimits::maxPushConstantsSize, kRequiredPushConstantSize},
    // The NBG fetch pass's groupshared arrays (within the guaranteed 16384)
    {"maxComputeSharedMemorySize", &VkPhysicalDeviceLimits::maxComputeSharedMemorySize, kRequiredSharedMemorySize},
    {"maxStorageBufferRange", &VkPhysicalDeviceLimits::maxStorageBufferRange, kRequiredStorageBufferRange},
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
