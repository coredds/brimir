#pragma once

// Diagnostics (brimir_vkhost --probe-passes, BRIMIR_GPU_PASS_PROFILE=1): the GPU time of each VDP1 and VDP2 pass kind.
// The back halves stamp a timestamp (bottom of pipe) after each pass they record; the time between a stamp and the one
// before it in the same command buffer goes to the later stamp's label (a Boundary stamp starts a new run and is not
// counted). Every pass is followed by a barrier, so each interval is that pass (and the copies before it, labelled on
// their own where a back half stamps them). Off unless enabled: the back halves then record nothing extra.
//
// Begin(context) starts a command buffer of a frame context (its previous one has completed): that one's stamps are read
// back (without waiting; unavailable results are dropped) and added to the totals Take returns.

#include "gpu/vk_device.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace brimir::gpu {

class GpuPassProfile {
public:
    enum Label : uint8_t {
        kBoundary,
        // VDP1 native
        kVdp1Copy, kVdp1Raster, kVdp1BatchRange, kVdp1Bucket, kVdp1BucketSums, kVdp1Scatter, kVdp1Resolve, kVdp1Erase,
        kVdp1FbWrite, kVdp1Other,
        // VDP1 N x
        kVdp1NxCopy, kVdp1NxRaster, kVdp1NxBatchRange, kVdp1NxBucket, kVdp1NxBucketSums, kVdp1NxScatter, kVdp1NxResolve,
        kVdp1NxErase, kVdp1NxFbWrite, kVdp1NxRebuild, kVdp1NxResolveFast, kVdp1NxFilter,
        // VDP2
        kVdp2Sprite, kVdp2SpriteNx, kVdp2Other,
        kLabelCount
    };
    static const char *Name(Label label);

    explicit GpuPassProfile(VulkanDevice &device);
    ~GpuPassProfile();
    GpuPassProfile(const GpuPassProfile &) = delete;
    GpuPassProfile &operator=(const GpuPassProfile &) = delete;

    // False where the queue family has no timestamps or the pool cannot be created (Stamp then does nothing).
    bool Ok() const { return m_pool != VK_NULL_HANDLE; }
    void Begin(uint32_t context);
    // The command buffer of the last Begin will not be submitted: its stamps are dropped.
    void Dropped();
    void Stamp(VkCommandBuffer cmd, Label label);
    // Microseconds per label collected since the last call, and the stamps that did not fit in a context.
    std::array<double, kLabelCount> Take();
    uint64_t TakeOverflow();

private:
    static constexpr uint32_t kQueriesPerContext = 8192;
    static constexpr uint32_t kMaxContexts = 8;
    void Collect(uint32_t context);

    VulkanDevice &m_device;
    VkQueryPool m_pool = VK_NULL_HANDLE;
    uint64_t m_mask = 0;
    int32_t m_current = -1;
    std::array<uint32_t, kMaxContexts> m_count{};
    std::array<bool, kMaxContexts> m_reset{};
    std::array<std::vector<Label>, kMaxContexts> m_labels;
    std::array<double, kLabelCount> m_micros{};
    uint64_t m_overflow = 0;
    std::vector<uint64_t> m_results;
};

} // namespace brimir::gpu
