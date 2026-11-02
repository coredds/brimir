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
//
// N x sprite surface (GPU stage 3; docs/ymir-hw-renderer-notes/stage3-research.md sections 1.4 and 2): with SetNxScale
// (N > 1) the back half also owns the N x surface (Vdp1NxFbramBufferSize) and its N x unit counts, offsets and scan
// sums. Every op is then replayed at N x too, in the same Record and right after its native passes:
// - a draw batch: the generator's sub-batches (vdp1_nx_front.hpp; none for a continuation, several for a large batch at
//   4x), each through raster_nx, batch_range, bucket, bucket_sums_nx, bucket_sums_nx_add, scatter and resolve_nx (the
//   N x spans into the span buffer, the other per-batch buffers shared with the native passes);
// - an erase: erase_nx;
// - a CPU write run: fb_write_nx after fb_write_clear (it reads the run's final native words).
// RecordFbramUpload rebuilds the uploaded slices (rebuild_nx), and a new surface (SetNxScale) is rebuilt whole from the
// GPU FBRAM at the next Record (one-time work, as the zero fill: re-armed by FrameDropped). The op lists replayed after a
// dropped command buffer carry their N x passes with them.

#include "gpu/vdp1/vdp1_bindings.hpp"
#include "gpu/vdp1/vdp1_front.hpp"
#include "gpu/vdp1/vdp1_nx_front.hpp"
#include "gpu/vk_device.hpp"
#include "gpu/vk_util.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
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
    // Copies the slices in `mask` of the GPU FBRAM into `context`'s readback buffer (ReadbackData once cmd has
    // completed), after the work recorded before it. Each context has its own buffer, so a readback stays readable
    // until the context's next command buffer begins. False when the buffer cannot be allocated (nothing recorded).
    // The buffer's memory, by preference: host-cached and coherent, host-cached (ReadbackData invalidates the slice
    // before the CPU reads it), then host-visible and coherent (uncached); the first choice is logged once.
    bool RecordReadback(uint32_t context, uint32_t mask, VkCommandBuffer cmd);
    // Slice `slice` (kFbramSliceSize bytes) of `context`'s readback buffer, made visible to the CPU (invalidated in
    // non-coherent memory); null before the context's first RecordReadback, or when the invalidation fails.
    const uint8_t *ReadbackData(uint32_t context, uint32_t slice) const;
    enum class ReadbackMemory { None, CachedCoherent, CachedNonCoherent, Uncached };
    // The memory of `context`'s readback buffer (None before its first RecordReadback).
    ReadbackMemory ReadbackMemoryForTest(uint32_t context) const;
    // Test hook: ReadbackData invalidates even coherent memory (the non-coherent path under validation).
    void SetInvalidateReadbacksForTest(bool invalidate) { m_invalidateForTest = invalidate; }
    // Where the readback memory choice is logged; stderr when unset.
    void SetLog(std::function<void(const std::string &)> log) { m_log = std::move(log); }

    // The command buffer of the last BeginContext will not be submitted: the one-time work it carried (zeroing the
    // back half's buffers, an N x rebuild) is pending again. Its op lists are the caller's to replay.
    void FrameDropped();

    // The N x sprite surface (the top of this file). N > 1 (up to kMaxNxScale): frees the N x resources first, then
    // allocates them for N (the N x pipelines are created once, at the first N > 1); the next Record rebuilds the whole
    // surface from the GPU FBRAM. 0 or 1: frees them (Records are native only). The same N again keeps the surface.
    // Contract: no command buffer this back half recorded is pending or waiting to be submitted (the caller waited for
    // its fences; a dropped one was reported with FrameDropped). False when the resources cannot be allocated: the
    // N x path is then off (NxScale() 0; NxFailure() says why); the native path is unaffected.
    bool SetNxScale(uint32_t scale);
    VkResult NxFailure() const { return m_nxFailure; }
    // One-time work (zero fills, the rebuild of a new N x surface) waits for the next Record. RecordPendingWork records
    // only it (with the barriers of a Record), for a command buffer without ops; false when nothing was recorded.
    bool HasPendingWork() const { return m_pendingZero || m_nxPendingZero || m_nxPendingRebuild != 0; }
    bool RecordPendingWork(uint32_t context, VkCommandBuffer cmd);
    // The N x scale (0: no N x surface).
    uint32_t NxScale() const { return m_nxScale; }
    // The N x surface (VK_NULL_HANDLE without one): STORAGE | TRANSFER_SRC, the layout of vdp1_hw_params.hpp.
    VkBuffer NxSurfaceBuffer() const;
    // The device memory of the N x resources (0 without them).
    VkDeviceSize NxMemoryBytes() const;
    // Test hook: SetNxScale fails when its resources need more than `bytes` (0: no limit).
    void SetNxAllocationLimitForTest(VkDeviceSize bytes) { m_nxAllocationLimit = bytes; }
    // Test hook: the N x sub-batch capacities (Vdp1NxLimits).
    void SetNxLimitsForTest(const Vdp1NxLimits &limits) { m_nxArena.limits = limits; }

    // What the Records since the last BeginContext recorded.
    struct RecordStats {
        uint32_t batches = 0;
        uint32_t erases = 0;
        uint32_t fbWriteRuns = 0;
        uint32_t uploads = 0; // RecordFbramUpload calls
        VkDeviceSize uploadBytes = 0;
        uint32_t uploadBlocks = 0; // blocks the context holds
        uint32_t malformed = 0;    // ops and VRAM copies skipped as malformed (data out of range)
        // N x passes
        uint32_t nxSubBatches = 0;
        uint32_t nxMaxSubBatchesPerBatch = 0;
        uint64_t nxSpans = 0;
        uint64_t nxSlots = 0;
        uint32_t nxErases = 0;
        uint32_t nxFbWriteRuns = 0;
        uint32_t nxRebuilds = 0; // rebuild_nx dispatches (new surface, uploads)
        // N x spans whose slots alone exceed a sub-batch's slot capacity (Vdp1NxStats::oversizedSpans)
        uint32_t nxOversizedSpans = 0;
    };
    const RecordStats &StatsForTest() const { return m_stats; }
    // Ops, VRAM copies and (at N x) native spans Record skipped as malformed since creation (the front half never
    // produces one; the first is logged through SetLog).
    uint64_t MalformedTotal() const { return m_malformedTotal; }

    // Diagnostics (brimir_vkhost --probe): from now on every Record, RecordFbramUpload and RecordReadback is bracketed
    // by two timestamps (bottom of pipe: the span from the end of the earlier work to the end of its own), and each
    // BeginContext adds the context's completed spans to the GPU time TakeGpuSeconds returns (vkGetQueryPoolResults
    // without waiting; results that are not available are dropped). Each pair's difference is taken modulo
    // 2^timestampValidBits of the device's queue family, so a counter wrap inside a pair is harmless. Returns false (and
    // stays off) where that queue family has no timestamps (timestampValidBits 0) or the pool cannot be created.
    bool EnableTimestamps();
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

    // The N x pipelines (vdp1_bindings.hpp kNxPipelineBindings order); batch_range, bucket and scatter are the native
    // pipelines, bucket and scatter with N x descriptor sets.
    enum NxPass : uint32_t {
        kNxRaster, kNxBucketSums, kNxBucketSumsAdd, kNxResolve, kNxErase, kNxFbWrite, kNxRebuild, kNxPassCount
    };
    enum NxBuf : uint32_t { kNxUnitCounts, kNxUnitOffsets, kNxScanSums, kNxSurface, kNxBufCount };

    VkResult Init(uint32_t frameContexts);
    void Destroy();
    VkResult CreateNxPipelines();
    VkResult AllocateNx(uint32_t scale);
    void DestroyNxResources();
    void WriteSet(VkDescriptorSet set, std::span<const Binding> bindings, bool nx) const;
    bool Allocate(Context &context, VkDeviceSize size, Allocation &out);
    // Pending one-time work and the barrier from earlier work, at the start of every Record call.
    void Prologue(VkCommandBuffer cmd);
    void Epilogue(VkCommandBuffer cmd) const;
    void Barrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                 VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) const;
    void Dispatch(VkCommandBuffer cmd, Pass pass, const void *push, uint32_t x, uint32_t y = 1) const;
    void DispatchIndirect(VkCommandBuffer cmd, Pass pass, const void *push, uint32_t argsWord) const;
    void DispatchWith(VkCommandBuffer cmd, const Pipe &pipe, VkDescriptorSet set, const void *push, uint32_t x,
                      uint32_t y) const;
    void DispatchIndirectWith(VkCommandBuffer cmd, const Pipe &pipe, VkDescriptorSet set, const void *push,
                              uint32_t argsWord) const;
    void PassBarrier(VkCommandBuffer cmd) const;
    void RecordNxRebuild(VkCommandBuffer cmd, uint32_t sliceMask);

    VulkanDevice &m_device;
    VkResult m_result = VK_ERROR_INITIALIZATION_FAILED;
    VkBuffer m_fbram = VK_NULL_HANDLE;
    std::array<vk_util::Buffer, kBufCount> m_buffers{};
    std::array<Pipe, kPassCount> m_pipes{};
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    std::vector<Context> m_contexts;
    std::vector<vk_util::Buffer> m_readbacks; // per context
    std::vector<ReadbackMemory> m_readbackMemory; // per context
    bool m_readbackLogged = false;
    bool m_invalidateForTest = false;
    std::function<void(const std::string &)> m_log;
    bool m_pendingZero = true;  // the back half's buffers are zeroed by the next Record
    bool m_recordedZero = false; // ... and the command buffer of the last BeginContext carries it
    RecordStats m_stats{};
    uint64_t m_malformedTotal = 0;
    void Malformed(const char *what, uint32_t index);
    void Log(const std::string &text) const;
    // EnableTimestamps: kQueriesPerContext queries per context (the first kMaxTimedContexts), the queries the context's
    // current command buffer used, whether that command buffer reset them yet
    VkQueryPool m_queries = VK_NULL_HANDLE;
    std::array<uint32_t, kMaxTimedContexts> m_queryCount{};
    std::array<bool, kMaxTimedContexts> m_queriesReset{};
    int32_t m_lastContext = -1;
    double m_gpuSeconds = 0;
    uint32_t m_timestampBits = 0;
    bool m_timestampsUnavailable = false;

    // N x (SetNxScale)
    uint32_t m_nxScale = 0;
    std::array<Pipe, kNxPassCount> m_nxPipes{}; // created once; sets from m_nxPool
    VkDescriptorSet m_nxBucketSet = VK_NULL_HANDLE;  // the native bucket and scatter pipelines on the N x buffers
    VkDescriptorSet m_nxScatterSet = VK_NULL_HANDLE;
    VkDescriptorPool m_nxPool = VK_NULL_HANDLE;      // per allocation
    std::array<vk_util::Buffer, kNxBufCount> m_nxBuffers{};
    VkDeviceSize m_nxAllocationLimit = 0;
    VkResult m_nxFailure = VK_SUCCESS; // the last SetNxScale failure
    bool m_nxPendingZero = false;   // the N x unit counts (and offsets, sums) are zeroed by the next Record
    bool m_nxRecordedZero = false;  // ... and the command buffer of the last BeginContext carries it
    uint32_t m_nxPendingRebuild = 0;  // slices rebuilt from the GPU FBRAM by the next Record
    uint32_t m_nxRecordedRebuild = 0; // ... and the ones the command buffer of the last BeginContext carries
    Vdp1NxArena m_nxArena;
    struct NxDraw {
        uint32_t firstSubBatch = 0; // into m_nxArena.subBatches
        uint32_t subBatches = 0;
    };
    std::vector<NxDraw> m_nxDraws; // per op of the segment being recorded
};

} // namespace brimir::gpu::vdp1
