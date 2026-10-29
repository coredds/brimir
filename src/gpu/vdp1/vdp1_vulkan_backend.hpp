#pragma once

// Vulkan back half of the GPU VDP1 renderer (GPU stage 2; docs/ymir-hw-renderer-notes/stage2-vdp1-research.md
// sections 2.3 and 4.3): replays the front half's op lists (vdp1_front.hpp) into a command buffer with the VDP1
// compute passes (vdp1_bindings.hpp), in op order:
// - a draw batch: its VDP1 VRAM uploads, commands and spans are copied into the back half's buffers, then raster,
//   batch_range, bucket (indirect), bucket_sums, scatter and resolve (indirect);
// - an erase: the erase pass;
// - a CPU framebuffer write run: its records are copied in, then fb_write_mark, fb_write and fb_write_clear.
// Every pass is followed by a compute-to-compute (and indirect-argument) barrier; copies are separated from the passes
// by transfer barriers.
//
// FBRAM is not owned here: it is the VDP2 back half's sprite framebuffer (Vdp2VulkanBackend::SpriteFBBuffer, the
// slice layout of vdp1_hw_params.hpp), which the VDP2 sprite pass reads at the display framebuffer. A Record leaves its
// writes visible to later compute and transfer work in the same queue, and waits (an execution dependency) for earlier
// compute and transfer work before it writes, so it can go before and after a VDP2 frame in one command buffer.
//
// Uploads go through frame contexts, as the VDP2 back half's: a context owns host-visible upload blocks that the
// command buffers it records read from. BeginContext starts a command buffer of a context (its previous one has
// completed); the Records into that command buffer then append to the context's blocks (chaining another block when
// one is full; the next BeginContext merges them into one block of the high-water size). Record never waits.

#include "gpu/vdp1/vdp1_bindings.hpp"
#include "gpu/vdp1/vdp1_front.hpp"
#include "gpu/vk_device.hpp"
#include "gpu/vk_util.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace brimir::gpu::vdp1 {

class Vdp1VulkanBackend {
public:
    static constexpr VkDeviceSize kDefaultUploadBlockSize = 4 * 1024 * 1024;

    // `fbram`: the GPU FBRAM (kFbramBufferSize bytes; STORAGE | TRANSFER_SRC | TRANSFER_DST), owned by the caller and
    // alive as long as this back half. Check Ok() afterwards.
    Vdp1VulkanBackend(VulkanDevice &device, uint32_t frameContexts, VkBuffer fbram);
    // Destroys everything immediately: the GPU must be done with all of it.
    ~Vdp1VulkanBackend();
    Vdp1VulkanBackend(const Vdp1VulkanBackend &) = delete;
    Vdp1VulkanBackend &operator=(const Vdp1VulkanBackend &) = delete;

    bool Ok() const { return m_result == VK_SUCCESS; }
    VkResult Result() const { return m_result; }
    uint32_t FrameContexts() const { return static_cast<uint32_t>(m_contexts.size()); }
    // Grows the number of frame contexts to at least `count`.
    void EnsureContexts(uint32_t count);

    // A new command buffer of `context` begins: the context's previous command buffer has completed (or was never
    // submitted). False for a context out of range.
    bool BeginContext(uint32_t context);
    // Records ops[segment.firstOp, + segment.opCount) of `ops` into cmd, after BeginContext(context) for this command
    // buffer. All or nothing: returns false and records nothing when the upload space cannot be allocated.
    bool Record(const Vdp1OpList &ops, const Vdp1Segment &segment, uint32_t context, VkCommandBuffer cmd);
    // Copies the slices in `mask` (bit i: slice i of vdp1_hw_params.hpp) from `slices` (kFbramBufferSize bytes, slice
    // i at i * kFbramSliceSize) into the GPU FBRAM, ordered with the Records around it. All or nothing, as Record.
    bool RecordFbramUpload(std::span<const uint8_t> slices, uint32_t mask, uint32_t context, VkCommandBuffer cmd);
    // Copies the slices in `mask` of the GPU FBRAM into `context`'s readback buffer (host-cached; ReadbackData once cmd
    // has completed), after the work recorded before it. Each context has its own buffer, so a readback stays readable
    // until the context's next command buffer begins. False when the buffer cannot be allocated (nothing recorded).
    bool RecordReadback(uint32_t context, uint32_t mask, VkCommandBuffer cmd);
    // `context`'s readback buffer (kFbramBufferSize bytes, slice i at i * kFbramSliceSize); null before its first
    // RecordReadback.
    const uint8_t *ReadbackData(uint32_t context) const;

    // The command buffer of the last BeginContext will not be submitted: the one-time work it carried (zeroing the
    // back half's buffers) is pending again. Its op lists are the caller's to replay.
    void FrameDropped();

    // What the Records since the last BeginContext recorded.
    struct RecordStats {
        uint32_t batches = 0;
        uint32_t erases = 0;
        uint32_t fbWriteRuns = 0;
        uint32_t uploads = 0; // RecordFbramUpload calls
        VkDeviceSize uploadBytes = 0;
        uint32_t uploadBlocks = 0; // blocks the context holds
    };
    const RecordStats &StatsForTest() const { return m_stats; }

    // Diagnostics (brimir_vkhost --probe): from now on every Record, RecordFbramUpload and RecordReadback is bracketed
    // by two timestamps (bottom of pipe: the span from the end of the earlier work to the end of its own), and each
    // BeginContext adds the context's completed spans to the GPU time TakeGpuSeconds returns (vkGetQueryPoolResults
    // without waiting; results that are not available are dropped). No-op where the device has no timestamps on its
    // graphics and compute queues (timestampComputeAndGraphics) or the pool cannot be created.
    void EnableTimestamps();
    // The GPU time of the command buffers whose results were collected since the last call, in seconds.
    double TakeGpuSeconds();

private:
    static constexpr uint32_t kQueriesPerContext = 64;
    static constexpr uint32_t kMaxTimedContexts = 16;
    void StampBegin(uint32_t contextIndex, VkCommandBuffer cmd);
    void StampEnd(uint32_t contextIndex, VkCommandBuffer cmd);
    void CollectTimestamps(uint32_t contextIndex);

    enum Pass : uint32_t {
        kRaster, kBatchRange, kBucket, kBucketSums, kScatter, kResolve, kErase, kFbWriteMark, kFbWrite, kFbWriteClear,
        kPassCount
    };
    enum Buf : uint32_t {
        kCommands, kSpans, kVram, kFbWrites, kFragments, kUnitCounts, kBucketBuf, kScanSums, kUnitOffsets, kBatchState,
        kFbWinners, kBufCount
    };
    struct Pipe {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t pushSize = 0;
    };
    struct Context {
        std::vector<vk_util::Buffer> blocks; // host-visible, coherent, mapped
        VkDeviceSize used = 0;               // in blocks.back()
        VkDeviceSize total = 0;              // bytes allocated since BeginContext
        VkDeviceSize highWater = 0;
    };
    struct Allocation {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        uint8_t *mapped = nullptr;
    };

    VkResult Init(uint32_t frameContexts);
    void Destroy();
    bool Allocate(Context &context, VkDeviceSize size, Allocation &out);
    // Pending one-time work and the barrier from earlier work, at the start of every Record call.
    void Prologue(VkCommandBuffer cmd);
    void Epilogue(VkCommandBuffer cmd) const;
    void Barrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                 VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) const;
    void Dispatch(VkCommandBuffer cmd, Pass pass, const void *push, uint32_t x, uint32_t y = 1) const;
    void DispatchIndirect(VkCommandBuffer cmd, Pass pass, const void *push, uint32_t argsWord) const;
    void PassBarrier(VkCommandBuffer cmd) const;

    VulkanDevice &m_device;
    VkResult m_result = VK_ERROR_INITIALIZATION_FAILED;
    VkBuffer m_fbram = VK_NULL_HANDLE;
    std::array<vk_util::Buffer, kBufCount> m_buffers{};
    std::array<Pipe, kPassCount> m_pipes{};
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    std::vector<Context> m_contexts;
    std::vector<vk_util::Buffer> m_readbacks; // per context
    bool m_pendingZero = true;  // the back half's buffers are zeroed by the next Record
    bool m_recordedZero = false; // ... and the command buffer of the last BeginContext carries it
    RecordStats m_stats{};
    // EnableTimestamps: kQueriesPerContext queries per context (the first kMaxTimedContexts), the queries the context's
    // current command buffer used, whether that command buffer reset them yet
    VkQueryPool m_queries = VK_NULL_HANDLE;
    std::array<uint32_t, kMaxTimedContexts> m_queryCount{};
    std::array<bool, kMaxTimedContexts> m_queriesReset{};
    int32_t m_lastContext = -1;
    double m_gpuSeconds = 0;
};

} // namespace brimir::gpu::vdp1
