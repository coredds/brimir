#include "vdp1_vulkan_backend.hpp"

#include "gpu/shaders/generated/vdp1_batch_range.spv.h"
#include "gpu/shaders/generated/vdp1_bucket.spv.h"
#include "gpu/shaders/generated/vdp1_bucket_sums.spv.h"
#include "gpu/shaders/generated/vdp1_erase.spv.h"
#include "gpu/shaders/generated/vdp1_fb_write.spv.h"
#include "gpu/shaders/generated/vdp1_fb_write_clear.spv.h"
#include "gpu/shaders/generated/vdp1_fb_write_mark.spv.h"
#include "gpu/shaders/generated/vdp1_raster.spv.h"
#include "gpu/shaders/generated/vdp1_resolve.spv.h"
#include "gpu/shaders/generated/vdp1_scatter.spv.h"
// N x (GPU stage 3)
#include "gpu/shaders/generated/vdp1_bucket_sums_nx.spv.h"
#include "gpu/shaders/generated/vdp1_bucket_sums_nx_add.spv.h"
#include "gpu/shaders/generated/vdp1_erase_nx.spv.h"
#include "gpu/shaders/generated/vdp1_fb_write_nx.spv.h"
#include "gpu/shaders/generated/vdp1_filter_nx.spv.h"
#include "gpu/shaders/generated/vdp1_raster_nx.spv.h"
#include "gpu/shaders/generated/vdp1_rebuild_nx.spv.h"
#include "gpu/shaders/generated/vdp1_resolve_fast_nx.spv.h"
#include "gpu/shaders/generated/vdp1_resolve_nx.spv.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

namespace brimir::gpu::vdp1 {

namespace {

constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags kTransfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
constexpr VkAccessFlags kWrites = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
constexpr VkAccessFlags kAll = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                               VK_ACCESS_TRANSFER_WRITE_BIT;
constexpr VkDeviceSize kUploadAlignment = 16;

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

uint32_t Groups(uint32_t count, uint32_t perGroup) {
    return (count + perGroup - 1) / perGroup;
}

// The binding of each native buffer (Vdp1VulkanBackend::Buf order)
constexpr std::array<uint32_t, 11> kBufBindings{
    kBindingCommands,  kBindingSpans,    kBindingVRAM,        kBindingFbWrites,   kBindingFragments, kBindingUnitCounts,
    kBindingBucket,    kBindingScanSums, kBindingUnitOffsets, kBindingBatchState, kBindingFbWinners};

} // namespace

Vdp1VulkanBackend::Vdp1VulkanBackend(VulkanDevice &device, uint32_t frameContexts, VkBuffer fbram)
    : m_device(device), m_fbram(fbram) {
    m_result = Init(frameContexts);
    if (m_result != VK_SUCCESS) {
        Destroy();
    }
}

Vdp1VulkanBackend::~Vdp1VulkanBackend() {
    Destroy();
}

VkResult Vdp1VulkanBackend::Init(uint32_t frameContexts) {
    if (frameContexts == 0 || m_fbram == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    static_assert(kBufBindings.size() == kBufCount);
    static constexpr std::array<uint32_t, kBufCount> kSizes{
        kCommandBufferSize,    kSpanBufferSize,        kVRAMBufferSize,       kFbWriteBufferSize,
        kFragmentBufferSize,   kUnitCountsBufferSize,  kBucketBufferSize,     kScanSumsBufferSize,
        kUnitOffsetsBufferSize, kBatchStateBufferSize, kFbWinnersBufferSize};
    for (uint32_t b = 0; b < kBufCount; ++b) {
        // The batch state is also the bucket and resolve passes' indirect dispatch arguments; TRANSFER_DST for the
        // uploads and the zero fill at first use (every buffer starts zeroed: the bindings contract)
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         (b == kBatchState ? VkBufferUsageFlags{VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT} : 0u);
        const VkResult result =
            vk_util::CreateBuffer(m_device, kSizes[b], usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_buffers[b]);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    struct PassDesc {
        std::span<const uint32_t> spirv;
        std::span<const Binding> bindings;
        uint32_t pushSize;
    };
    static_assert(kPassCount == kPipelineBindings.size(), "kPipelineBindings must list every pass, in Pass order");
    const std::array<PassDesc, kPassCount> passes{{
        {shaders::kVdp1RasterSpv, kPipelineBindings[kRaster], sizeof(Vdp1BatchPushConstants)},
        {shaders::kVdp1BatchRangeSpv, kPipelineBindings[kBatchRange], 0},
        {shaders::kVdp1BucketSpv, kPipelineBindings[kBucket], 0},
        {shaders::kVdp1BucketSumsSpv, kPipelineBindings[kBucketSums], 0},
        {shaders::kVdp1ScatterSpv, kPipelineBindings[kScatter], sizeof(Vdp1BatchPushConstants)},
        {shaders::kVdp1ResolveSpv, kPipelineBindings[kResolve], sizeof(Vdp1BatchPushConstants)},
        {shaders::kVdp1EraseSpv, kPipelineBindings[kErase], sizeof(Vdp1ErasePushConstants)},
        {shaders::kVdp1FbWriteMarkSpv, kPipelineBindings[kFbWriteMark], sizeof(Vdp1FbWritePushConstants)},
        {shaders::kVdp1FbWriteSpv, kPipelineBindings[kFbWrite], sizeof(Vdp1FbWritePushConstants)},
        {shaders::kVdp1FbWriteClearSpv, kPipelineBindings[kFbWriteClear], sizeof(Vdp1FbWritePushConstants)},
    }};
    uint32_t descriptors = 0;
    for (const PassDesc &p : passes) {
        descriptors += static_cast<uint32_t>(p.bindings.size());
    }
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descriptors};
    if (const VkResult result = vk_util::CreateDescriptorPool(m_device, {&poolSize, 1}, kPassCount, m_pool);
        result != VK_SUCCESS) {
        return result;
    }
    for (uint32_t i = 0; i < kPassCount; ++i) {
        const PassDesc &p = passes[i];
        Pipe &pipe = m_pipes[i];
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const Binding &b : p.bindings) {
            bindings.push_back({b.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, p.pushSize};
        pipe.pushSize = p.pushSize;
        VkResult result = vk_util::CreateDescriptorSetLayout(m_device, bindings, pipe.setLayout);
        if (result == VK_SUCCESS) {
            result = vk_util::CreatePipelineLayout(
                m_device, {&pipe.setLayout, 1},
                p.pushSize != 0 ? std::span(&range, 1) : std::span<const VkPushConstantRange>(), pipe.layout);
        }
        if (result == VK_SUCCESS) {
            result = vk_util::CreateComputePipeline(m_device, p.spirv, pipe.layout, pipe.pipeline);
        }
        if (result == VK_SUCCESS) {
            result = vk_util::AllocateDescriptorSet(m_device, m_pool, pipe.setLayout, pipe.set);
        }
        if (result != VK_SUCCESS) {
            return result;
        }
        WriteSet(pipe.set, p.bindings, false);
    }
    m_contexts.resize(frameContexts);
    m_readbacks.resize(frameContexts);
    m_readbackMemory.resize(frameContexts, ReadbackMemory::None);
    return VK_SUCCESS;
}

void Vdp1VulkanBackend::WriteSet(VkDescriptorSet set, std::span<const Binding> bindings, bool nx) const {
    for (const Binding &b : bindings) {
        VkDescriptorBufferInfo info{VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
        if (b.binding == kBindingFbram) {
            info.buffer = m_fbram;
        } else if (b.binding == kBindingFbramNx) {
            info.buffer = m_nxBuffers[kNxSurface].buffer;
        } else if (nx && b.binding == kBindingUnitCounts) {
            info.buffer = m_nxBuffers[kNxUnitCounts].buffer;
        } else if (nx && b.binding == kBindingUnitOffsets) {
            info.buffer = m_nxBuffers[kNxUnitOffsets].buffer;
        } else if (nx && b.binding == kBindingScanSums) {
            info.buffer = m_nxBuffers[kNxScanSums].buffer;
        } else {
            for (uint32_t k = 0; k < kBufCount; ++k) {
                if (kBufBindings[k] == b.binding) {
                    info.buffer = m_buffers[k].buffer;
                }
            }
        }
        assert(info.buffer != VK_NULL_HANDLE);
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = b.binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &info;
        m_device.Table().vkUpdateDescriptorSets(m_device.Device(), 1, &write, 0, nullptr);
    }
}

VkResult Vdp1VulkanBackend::CreateNxPipelines() {
    if (m_nxPipes[0].pipeline != VK_NULL_HANDLE) {
        return VK_SUCCESS;
    }
    struct PassDesc {
        std::span<const uint32_t> spirv;
        uint32_t pushSize;
    };
    static_assert(kNxPassCount == kNxPipelineBindings.size(), "kNxPipelineBindings must list every N x pass, in order");
    const std::array<PassDesc, kNxPassCount> passes{{
        {shaders::kVdp1RasterNxSpv, sizeof(Vdp1BatchPushConstants)},
        {shaders::kVdp1BucketSumsNxSpv, sizeof(Vdp1BatchPushConstants)},
        {shaders::kVdp1BucketSumsNxAddSpv, sizeof(Vdp1BatchPushConstants)},
        {shaders::kVdp1ResolveNxSpv, sizeof(Vdp1BatchPushConstants)},
        {shaders::kVdp1EraseNxSpv, sizeof(Vdp1EraseNxPushConstants)},
        {shaders::kVdp1FbWriteNxSpv, sizeof(Vdp1FbWriteNxPushConstants)},
        {shaders::kVdp1RebuildNxSpv, sizeof(Vdp1RebuildNxPushConstants)},
        {shaders::kVdp1ResolveFastNxSpv, sizeof(Vdp1BatchPushConstants)},
        {shaders::kVdp1FilterNxSpv, sizeof(Vdp1BatchPushConstants)},
    }};
    for (uint32_t i = 0; i < kNxPassCount; ++i) {
        Pipe &pipe = m_nxPipes[i];
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const Binding &b : kNxPipelineBindings[i]) {
            bindings.push_back({b.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, passes[i].pushSize};
        pipe.pushSize = passes[i].pushSize;
        VkResult result = vk_util::CreateDescriptorSetLayout(m_device, bindings, pipe.setLayout);
        if (result == VK_SUCCESS) {
            result = vk_util::CreatePipelineLayout(m_device, {&pipe.setLayout, 1}, std::span(&range, 1), pipe.layout);
        }
        if (result == VK_SUCCESS) {
            result = vk_util::CreateComputePipeline(m_device, passes[i].spirv, pipe.layout, pipe.pipeline);
        }
        if (result != VK_SUCCESS) {
            const auto &vk = m_device.Table();
            for (Pipe &p : m_nxPipes) {
                vk.vkDestroyPipeline(m_device.Device(), p.pipeline, nullptr);
                vk.vkDestroyPipelineLayout(m_device.Device(), p.layout, nullptr);
                vk.vkDestroyDescriptorSetLayout(m_device.Device(), p.setLayout, nullptr);
                p = Pipe{};
            }
            return result;
        }
    }
    return VK_SUCCESS;
}

VkResult Vdp1VulkanBackend::AllocateNx(uint32_t scale) {
    const std::array<VkDeviceSize, kNxBufCount> sizes{Vdp1NxUnitCountsBufferSize(scale),
                                                      Vdp1NxUnitOffsetsBufferSize(scale),
                                                      Vdp1NxScanSumsBufferSize(scale), Vdp1NxFbramBufferSize(scale)};
    VkDeviceSize total = 0;
    for (const VkDeviceSize s : sizes) {
        total += s;
    }
    if (m_nxAllocationLimit != 0 && total > m_nxAllocationLimit) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (const VkResult result = CreateNxPipelines(); result != VK_SUCCESS) {
        return result;
    }
    for (uint32_t b = 0; b < kNxBufCount; ++b) {
        // TRANSFER_DST for the zero fill; the surface is also read by copies (tests, readbacks of the N x image)
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         (b == kNxSurface ? VkBufferUsageFlags{VK_BUFFER_USAGE_TRANSFER_SRC_BIT} : 0u);
        if (const VkResult result =
                vk_util::CreateBuffer(m_device, sizes[b], usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_nxBuffers[b]);
            result != VK_SUCCESS) {
            return result;
        }
    }
    // One set per N x pipeline plus the N x sets of the native bucket and scatter pipelines
    uint32_t descriptors = static_cast<uint32_t>(kBucketBindings.size() + kScatterBindings.size());
    for (const std::span<const Binding> bindings : kNxPipelineBindings) {
        descriptors += static_cast<uint32_t>(bindings.size());
    }
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descriptors};
    if (const VkResult result = vk_util::CreateDescriptorPool(m_device, {&poolSize, 1}, kNxPassCount + 2, m_nxPool);
        result != VK_SUCCESS) {
        return result;
    }
    for (uint32_t i = 0; i < kNxPassCount; ++i) {
        if (const VkResult result =
                vk_util::AllocateDescriptorSet(m_device, m_nxPool, m_nxPipes[i].setLayout, m_nxPipes[i].set);
            result != VK_SUCCESS) {
            return result;
        }
        WriteSet(m_nxPipes[i].set, kNxPipelineBindings[i], true);
    }
    for (const auto &[pass, set] : {std::pair{kBucket, &m_nxBucketSet}, std::pair{kScatter, &m_nxScatterSet}}) {
        if (const VkResult result = vk_util::AllocateDescriptorSet(m_device, m_nxPool, m_pipes[pass].setLayout, *set);
            result != VK_SUCCESS) {
            return result;
        }
        WriteSet(*set, kPipelineBindings[pass], true);
    }
    return VK_SUCCESS;
}

void Vdp1VulkanBackend::DestroyNxResources() {
    m_device.Table().vkDestroyDescriptorPool(m_device.Device(), m_nxPool, nullptr); // frees the sets
    m_nxPool = VK_NULL_HANDLE;
    for (Pipe &p : m_nxPipes) {
        p.set = VK_NULL_HANDLE;
    }
    m_nxBucketSet = VK_NULL_HANDLE;
    m_nxScatterSet = VK_NULL_HANDLE;
    for (vk_util::Buffer &b : m_nxBuffers) {
        vk_util::DestroyBuffer(m_device, b);
    }
    m_nxScale = 0;
    m_nxPendingZero = m_nxRecordedZero = false;
    m_nxPendingRebuild = m_nxRecordedRebuild = 0;
    m_nxOffsetsDirty = m_nxOddWinnersDirty = false; // new buffers are zeroed before their first use
}

uint8_t Vdp1VulkanBackend::OverwriteKey(const Vdp1CommandParams &c) {
    // Every fragment of the command replaces the bits it writes whatever they held (ApplyFragment without a read of the
    // destination): no MSB-on, 16 bpp replace or half-luminance (8 bpp ignores the colour calculation), no write to
    // the mesh framebuffer (transparent meshes with a mesh command). Commands with one key write (and clear) the same
    // bits of a unit, so its last fragment decides it.
    const uint32_t calc = Vdp1CmdColorCalc(c.drawFlags);
    const bool pixel8 = Vdp1RasPixel8(c.rasterFlags);
    const bool transparentMeshes = Vdp1RasTransparentMeshes(c.rasterFlags);
    if (Vdp1CmdMsbOn(c.drawFlags) || (transparentMeshes && Vdp1CmdMesh(c.drawFlags)) ||
        (!pixel8 && calc != 0 && calc != 2)) {
        return 0;
    }
    return static_cast<uint8_t>(kKeyOverwrite | (pixel8 ? kKeyPixel8 : 0u) | (transparentMeshes ? kKeyMeshes : 0u));
}

Vdp1VulkanBackend::NxPath Vdp1VulkanBackend::ClassifyNx(std::span<const Vdp1CommandParams> commands) {
    // One path for all of a draw's sub-batches. (Cutting sub-batches where the commands' key changes was measured
    // slower: smaller raster dispatches and more resolves.)
    if (commands.empty()) {
        return NxPath::Ordered;
    }
    const uint8_t first = OverwriteKey(commands.front());
    bool same = first != 0;
    bool anyOverwrite = false;
    // Hybrid: one pixel depth and transparent-meshes setting, so an overwrite-class fragment replaces every bit any
    // fragment of the batch writes at its word (8 bpp: its byte): the main word's, and with transparent meshes on also
    // the mesh word's (it clears them; mesh framebuffer writes, possible only then, count as fragments that read)
    bool hybrid = true;
    const bool pixel8 = Vdp1RasPixel8(commands.front().rasterFlags);
    const bool transparentMeshes = Vdp1RasTransparentMeshes(commands.front().rasterFlags);
    for (const Vdp1CommandParams &c : commands) {
        const uint8_t key = OverwriteKey(c);
        same = same && key == first;
        anyOverwrite = anyOverwrite || key != 0;
        hybrid = hybrid && Vdp1RasPixel8(c.rasterFlags) == pixel8 &&
                 Vdp1RasTransparentMeshes(c.rasterFlags) == transparentMeshes;
    }
    if (same) {
        return (first & kKeyPixel8) != 0 ? NxPath::OverwriteOddBytes : NxPath::Overwrite;
    }
    if (hybrid && anyOverwrite) {
        return pixel8 ? NxPath::HybridPixel8 : NxPath::Hybrid;
    }
    return NxPath::Ordered;
}

bool Vdp1VulkanBackend::SetNxScale(uint32_t scale) {
    if (scale <= 1) {
        scale = 0;
    }
    if (!Ok() || scale > kMaxNxScale) {
        return false;
    }
    if (scale == m_nxScale) {
        return true;
    }
    // Free first, allocate second: the contents are regenerable from the GPU FBRAM, so the peak is the larger of the two
    DestroyNxResources();
    if (scale == 0) {
        return true;
    }
    if (const VkResult result = AllocateNx(scale); result != VK_SUCCESS) {
        DestroyNxResources();
        m_nxFailure = result;
        return false;
    }
    m_nxScale = scale;
    m_nxPendingZero = true;
    m_nxPendingRebuild = (1u << kFbramSliceCount) - 1;
    return true;
}

bool Vdp1VulkanBackend::RecordPendingWork(uint32_t contextIndex, VkCommandBuffer cmd) {
    if (!Ok() || contextIndex >= m_contexts.size() || cmd == VK_NULL_HANDLE || !HasPendingWork()) {
        return false;
    }
    StampBegin(contextIndex, cmd);
    ProfileStamp(cmd, GpuPassProfile::kBoundary);
    Prologue(cmd);
    Epilogue(cmd);
    ProfileStamp(cmd, GpuPassProfile::kVdp1Other);
    StampEnd(contextIndex, cmd);
    return true;
}

VkBuffer Vdp1VulkanBackend::NxSurfaceBuffer() const {
    return m_nxBuffers[kNxSurface].buffer;
}

VkDeviceSize Vdp1VulkanBackend::NxMemoryBytes() const {
    VkDeviceSize total = 0;
    for (const vk_util::Buffer &b : m_nxBuffers) {
        total += b.buffer != VK_NULL_HANDLE ? b.size : 0;
    }
    return total;
}

void Vdp1VulkanBackend::Destroy() {
    const auto &vk = m_device.Table();
    const VkDevice dev = m_device.Device();
    for (Context &context : m_contexts) {
        for (vk_util::Buffer &block : context.blocks) {
            vk_util::DestroyBuffer(m_device, block);
        }
    }
    m_contexts.clear();
    DestroyNxResources();
    for (Pipe &p : m_nxPipes) {
        vk.vkDestroyPipeline(dev, p.pipeline, nullptr);
        vk.vkDestroyPipelineLayout(dev, p.layout, nullptr);
        vk.vkDestroyDescriptorSetLayout(dev, p.setLayout, nullptr);
        p = Pipe{};
    }
    for (Pipe &p : m_pipes) {
        vk.vkDestroyPipeline(dev, p.pipeline, nullptr);
        vk.vkDestroyPipelineLayout(dev, p.layout, nullptr);
        vk.vkDestroyDescriptorSetLayout(dev, p.setLayout, nullptr);
        p = Pipe{};
    }
    vk.vkDestroyDescriptorPool(dev, m_pool, nullptr); // frees the sets
    m_pool = VK_NULL_HANDLE;
    for (vk_util::Buffer &b : m_buffers) {
        vk_util::DestroyBuffer(m_device, b);
    }
    for (vk_util::Buffer &readback : m_readbacks) {
        vk_util::DestroyBuffer(m_device, readback);
    }
    m_readbacks.clear();
    m_readbackMemory.clear();
    if (m_queries != VK_NULL_HANDLE) {
        vk.vkDestroyQueryPool(dev, m_queries, nullptr);
        m_queries = VK_NULL_HANDLE;
    }
}

bool Vdp1VulkanBackend::EnableTimestamps() {
    if (m_queries != VK_NULL_HANDLE) {
        return true;
    }
    if (!Ok() || m_timestampsUnavailable) {
        return false;
    }
    // The queue family's timestampValidBits (0: no timestamps on this queue); a pair's difference is taken modulo
    // 2^validBits, so a counter wrap between the two stamps of a pair is harmless.
    uint32_t families = 0;
    volk::vkGetPhysicalDeviceQueueFamilyProperties(m_device.Physical(), &families, nullptr);
    std::vector<VkQueueFamilyProperties> props(families);
    volk::vkGetPhysicalDeviceQueueFamilyProperties(m_device.Physical(), &families, props.data());
    m_timestampBits = m_device.QueueFamily() < families ? props[m_device.QueueFamily()].timestampValidBits : 0;
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kQueriesPerContext * kMaxTimedContexts;
    if (m_timestampBits == 0 ||
        m_device.Table().vkCreateQueryPool(m_device.Device(), &info, nullptr, &m_queries) != VK_SUCCESS) {
        m_queries = VK_NULL_HANDLE;
        m_timestampsUnavailable = true;
        return false;
    }
    m_queryCount.fill(0);
    m_queriesReset.fill(false);
    return true;
}

double Vdp1VulkanBackend::TakeGpuSeconds() {
    const double seconds = m_gpuSeconds;
    m_gpuSeconds = 0;
    return seconds;
}

void Vdp1VulkanBackend::StampBegin(uint32_t contextIndex, VkCommandBuffer cmd) {
    if (m_queries == VK_NULL_HANDLE || contextIndex >= kMaxTimedContexts) {
        return;
    }
    const auto &vk = m_device.Table();
    const uint32_t base = contextIndex * kQueriesPerContext;
    if (!m_queriesReset[contextIndex]) {
        vk.vkCmdResetQueryPool(cmd, m_queries, base, kQueriesPerContext);
        m_queriesReset[contextIndex] = true;
    }
    if (m_queryCount[contextIndex] + 2 <= kQueriesPerContext) {
        vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queries, base + m_queryCount[contextIndex]);
    }
}

void Vdp1VulkanBackend::StampEnd(uint32_t contextIndex, VkCommandBuffer cmd) {
    if (m_queries == VK_NULL_HANDLE || contextIndex >= kMaxTimedContexts ||
        m_queryCount[contextIndex] + 2 > kQueriesPerContext) {
        return;
    }
    const uint32_t base = contextIndex * kQueriesPerContext;
    m_device.Table().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queries,
                                         base + m_queryCount[contextIndex] + 1);
    m_queryCount[contextIndex] += 2;
}

void Vdp1VulkanBackend::CollectTimestamps(uint32_t contextIndex) {
    if (m_queries == VK_NULL_HANDLE || contextIndex >= kMaxTimedContexts) {
        return;
    }
    const uint32_t count = m_queryCount[contextIndex];
    m_queryCount[contextIndex] = 0;
    m_queriesReset[contextIndex] = false;
    if (count == 0) {
        return;
    }
    std::array<uint64_t, kQueriesPerContext> stamps{};
    if (m_device.Table().vkGetQueryPoolResults(m_device.Device(), m_queries, contextIndex * kQueriesPerContext, count,
                                               sizeof(stamps), stamps.data(), sizeof(uint64_t),
                                               VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return;
    }
    const uint64_t mask = m_timestampBits >= 64 ? ~uint64_t{0} : (uint64_t{1} << m_timestampBits) - 1;
    uint64_t ticks = 0;
    for (uint32_t i = 0; i + 1 < count; i += 2) {
        ticks += (stamps[i + 1] - stamps[i]) & mask;
    }
    m_gpuSeconds += static_cast<double>(ticks) * m_device.Properties().limits.timestampPeriod * 1e-9;
}

void Vdp1VulkanBackend::EnsureContexts(uint32_t count) {
    if (count > m_contexts.size()) {
        m_contexts.resize(count);
        m_readbacks.resize(count);
        m_readbackMemory.resize(count, ReadbackMemory::None);
    }
}

bool Vdp1VulkanBackend::BeginContext(uint32_t contextIndex) {
    if (!Ok() || contextIndex >= m_contexts.size()) {
        return false;
    }
    Context &context = m_contexts[contextIndex];
    CollectTimestamps(contextIndex);
    m_lastContext = static_cast<int32_t>(contextIndex);
    // The context's previous command buffer has completed: its blocks are free. Chained blocks are merged into one of
    // the high-water size at the next allocation.
    context.highWater = std::max(context.highWater, context.total);
    if (context.blocks.size() > 1) {
        for (vk_util::Buffer &block : context.blocks) {
            vk_util::DestroyBuffer(m_device, block);
        }
        context.blocks.clear();
    }
    context.used = 0;
    context.total = 0;
    m_recordedZero = false;
    m_nxRecordedZero = false;
    m_nxRecordedRebuild = 0;
    m_stats = {};
    m_stats.uploadBlocks = static_cast<uint32_t>(context.blocks.size());
    return true;
}

bool Vdp1VulkanBackend::Allocate(Context &context, VkDeviceSize size, Allocation &out) {
    size = AlignUp(std::max<VkDeviceSize>(size, kUploadAlignment), kUploadAlignment);
    const auto addBlock = [&](VkDeviceSize bytes) {
        vk_util::Buffer block;
        const VkResult result = vk_util::CreateBuffer(
            m_device, AlignUp(bytes, 64 * 1024), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, block);
        if (result != VK_SUCCESS || block.mapped == nullptr) {
            vk_util::DestroyBuffer(m_device, block);
            return false;
        }
        context.blocks.push_back(block);
        context.used = 0;
        return true;
    };
    if (context.blocks.empty() &&
        !addBlock(std::max({kDefaultUploadBlockSize, context.highWater, size}))) {
        return false;
    }
    VkDeviceSize offset = AlignUp(context.used, kUploadAlignment);
    if (offset + size > context.blocks.back().size) {
        // Never wait for the GPU: chain a block, merged at the context's next BeginContext.
        if (!addBlock(std::max(kDefaultUploadBlockSize, size))) {
            return false;
        }
        offset = 0;
    }
    const vk_util::Buffer &block = context.blocks.back();
    context.used = offset + size;
    context.total += size;
    out = {block.buffer, offset, static_cast<uint8_t *>(block.mapped) + offset};
    m_stats.uploadBytes += size;
    m_stats.uploadBlocks = static_cast<uint32_t>(context.blocks.size());
    return true;
}

void Vdp1VulkanBackend::Barrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                                VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) const {
    vk_util::GlobalBarrier(m_device, cmd, srcStage, srcAccess, dstStage, dstAccess);
}

void Vdp1VulkanBackend::Prologue(VkCommandBuffer cmd) {
    // Earlier work of this queue (an earlier VDP1 op, the VDP2 sprite pass reading FBRAM, earlier copies) comes first.
    Barrier(cmd, kCompute | kTransfer, kWrites, kCompute | kTransfer, kAll);
    if (m_pendingZero) {
        const auto &vk = m_device.Table();
        for (const vk_util::Buffer &b : m_buffers) {
            vk.vkCmdFillBuffer(cmd, b.buffer, 0, VK_WHOLE_SIZE, 0);
        }
        Barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kCompute | kTransfer, kAll);
        m_pendingZero = false;
        m_recordedZero = true;
    }
    if (m_nxScale == 0) {
        return;
    }
    if (m_nxPendingZero) {
        // The N x unit counts start zeroed (the raster counts into them, the resolve clears what it consumed); the
        // offsets and sums are written before they are read, zeroed so nothing reads uninitialised memory
        const auto &vk = m_device.Table();
        for (const NxBuf b : {kNxUnitCounts, kNxUnitOffsets, kNxScanSums}) {
            vk.vkCmdFillBuffer(cmd, m_nxBuffers[b].buffer, 0, VK_WHOLE_SIZE, 0);
        }
        Barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kCompute | kTransfer, kAll);
        m_nxPendingZero = false;
        m_nxRecordedZero = true;
    }
    if (m_nxPendingRebuild != 0) {
        // A new surface: rebuilt from the GPU FBRAM as the work before this command buffer left it
        RecordNxRebuild(cmd, m_nxPendingRebuild);
        m_nxRecordedRebuild |= m_nxPendingRebuild;
        m_nxPendingRebuild = 0;
        // The rebuild reads the GPU FBRAM and writes the surface: both before what this Record does next (an FBRAM
        // upload's copy overwrites what it read, its rebuild_nx and the N x passes write the surface again)
        Barrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute | kTransfer, kAll);
    }
}

void Vdp1VulkanBackend::RecordNxRebuild(VkCommandBuffer cmd, uint32_t sliceMask) {
    const Vdp1RebuildNxPushConstants pc{sliceMask, m_nxScale};
    DispatchWith(cmd, m_nxPipes[kNxRebuild], m_nxPipes[kNxRebuild].set, &pc, Vdp1RebuildNxGroups(m_nxScale),
                 kFbramSliceCount);
    ++m_stats.nxRebuilds;
}

void Vdp1VulkanBackend::Epilogue(VkCommandBuffer cmd) const {
    // FBRAM (and everything else) is readable and writable by later compute and transfer work: the VDP2 sprite pass,
    // readbacks, the next Record.
    Barrier(cmd, kCompute | kTransfer, kWrites, kCompute | kTransfer, kAll);
}

void Vdp1VulkanBackend::Log(const std::string &text) const {
    if (m_log) {
        m_log(text);
    } else {
        std::fprintf(stderr, "[vulkan] %s\n", text.c_str());
    }
}

void Vdp1VulkanBackend::Malformed(const char *what, uint32_t index) {
    ++m_stats.malformed;
    if (m_malformedTotal++ == 0) {
        Log(std::string("VDP1: skipped a malformed op (") + what + " " + std::to_string(index) +
            ": data out of range); later ones are only counted");
    }
}

void Vdp1VulkanBackend::PassBarrier(VkCommandBuffer cmd) const {
    // A pass's writes before the next pass's shader accesses and indirect argument reads
    Barrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
}

void Vdp1VulkanBackend::Dispatch(VkCommandBuffer cmd, Pass pass, const void *push, uint32_t x, uint32_t y) const {
    DispatchWith(cmd, m_pipes[pass], m_pipes[pass].set, push, x, y);
}

void Vdp1VulkanBackend::DispatchIndirect(VkCommandBuffer cmd, Pass pass, const void *push, uint32_t argsWord) const {
    DispatchIndirectWith(cmd, m_pipes[pass], m_pipes[pass].set, push, argsWord);
}

void Vdp1VulkanBackend::DispatchWith(VkCommandBuffer cmd, const Pipe &p, VkDescriptorSet set, const void *push,
                                     uint32_t x, uint32_t y) const {
    const auto &vk = m_device.Table();
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
    if (p.pushSize != 0) {
        vk.vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, p.pushSize, push);
    }
    vk.vkCmdDispatch(cmd, x, y, 1);
    PassBarrier(cmd);
    ProfileStamp(cmd, ProfileLabel(p));
}

void Vdp1VulkanBackend::ProfileStamp(VkCommandBuffer cmd, GpuPassProfile::Label label) const {
    if (m_profile != nullptr) {
        m_profile->Stamp(cmd, label);
    }
}

GpuPassProfile::Label Vdp1VulkanBackend::ProfileLabel(const Pipe &p) const {
    using P = GpuPassProfile;
    const bool nx = m_profileNx;
    for (uint32_t i = 0; i < kPassCount; ++i) {
        if (&p == &m_pipes[i]) {
            switch (i) {
            case kRaster: return nx ? P::kVdp1NxRaster : P::kVdp1Raster;
            case kBatchRange: return nx ? P::kVdp1NxBatchRange : P::kVdp1BatchRange;
            case kBucket: return nx ? P::kVdp1NxBucket : P::kVdp1Bucket;
            case kBucketSums: return nx ? P::kVdp1NxBucketSums : P::kVdp1BucketSums;
            case kScatter: return nx ? P::kVdp1NxScatter : P::kVdp1Scatter;
            case kResolve: return nx ? P::kVdp1NxResolve : P::kVdp1Resolve;
            case kErase: return P::kVdp1Erase;
            default: return P::kVdp1FbWrite;
            }
        }
    }
    for (uint32_t i = 0; i < kNxPassCount; ++i) {
        if (&p == &m_nxPipes[i]) {
            switch (i) {
            case kNxRaster: return P::kVdp1NxRaster;
            case kNxBucketSums:
            case kNxBucketSumsAdd: return P::kVdp1NxBucketSums;
            case kNxResolve: return P::kVdp1NxResolve;
            case kNxErase: return P::kVdp1NxErase;
            case kNxFbWrite: return P::kVdp1NxFbWrite;
            case kNxResolveFast: return P::kVdp1NxResolveFast;
            case kNxFilter: return P::kVdp1NxFilter;
            default: return P::kVdp1NxRebuild;
            }
        }
    }
    return P::kVdp1Other;
}

void Vdp1VulkanBackend::DispatchIndirectWith(VkCommandBuffer cmd, const Pipe &p, VkDescriptorSet set, const void *push,
                                             uint32_t argsWord) const {
    const auto &vk = m_device.Table();
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
    if (p.pushSize != 0) {
        vk.vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, p.pushSize, push);
    }
    vk.vkCmdDispatchIndirect(cmd, m_buffers[kBatchState].buffer, argsWord * sizeof(uint32_t));
    PassBarrier(cmd);
    ProfileStamp(cmd, ProfileLabel(p));
}

bool Vdp1VulkanBackend::Record(const Vdp1OpList &ops, const Vdp1Segment &segment, uint32_t contextIndex,
                               VkCommandBuffer cmd) {
    if (!Ok() || contextIndex >= m_contexts.size() || cmd == VK_NULL_HANDLE ||
        uint64_t{segment.firstOp} + segment.opCount > ops.ops.size()) {
        return false;
    }
    if (segment.opCount == 0) {
        return true;
    }
    // Upload space for the whole segment first (all or nothing); ops whose data is out of range are skipped.
    const auto batchOk = [&](const Vdp1DrawBatch &b) {
        return uint64_t{b.cmdOffset} + b.cmdCount <= ops.commands.size() && b.cmdCount <= kMaxCommandsPerBatch &&
               uint64_t{b.spanOffset} + b.spanCount <= ops.spans.size() && b.spanCount <= kMaxSpansPerBatch &&
               b.fragmentCount <= kMaxFragmentsPerBatch && uint64_t{b.firstCopy} + b.copyCount <= ops.vramCopies.size();
    };
    const auto copyOk = [&](const Vdp1VramCopy &c) {
        return uint64_t{c.arenaOffset} + c.size <= ops.arena.size() && uint64_t{c.vramOffset} + c.size <= kVRAMBufferSize;
    };
    const auto runOk = [&](const Vdp1FbWritePushConstants &r) {
        return uint64_t{r.offset} + r.count <= ops.fbWrites.size() && r.count <= kMaxFbWritesPerBatch;
    };
    // At N x: the N x sub-batches of every draw first (their spans are uploaded with the rest)
    const uint32_t nx = m_nxScale;
    const uint64_t malformedSpansBefore = m_nxArena.stats.malformedSpans;
    const uint64_t oversizedSpansBefore = m_nxArena.stats.oversizedSpans;
    if (nx != 0) {
        m_nxArena.Clear();
        m_nxDraws.assign(segment.opCount, NxDraw{});
    }
    VkDeviceSize bytes = 0;
    for (uint32_t i = segment.firstOp; i < segment.firstOp + segment.opCount; ++i) {
        const Vdp1Op &op = ops.ops[i];
        if (op.kind == Vdp1OpKind::Draw && op.index < ops.batches.size() && batchOk(ops.batches[op.index])) {
            const Vdp1DrawBatch &b = ops.batches[op.index];
            for (uint32_t k = 0; k < b.copyCount; ++k) {
                bytes += AlignUp(ops.vramCopies[b.firstCopy + k].size, kUploadAlignment);
            }
            bytes += AlignUp(VkDeviceSize{b.cmdCount} * sizeof(Vdp1CommandParams), kUploadAlignment);
            bytes += AlignUp(VkDeviceSize{b.spanCount} * sizeof(Vdp1Span), kUploadAlignment);
            if (nx != 0) {
                const std::span<const Vdp1NxSubBatch> subs = GenerateNx(ops, op.index, nx, m_nxArena);
                NxDraw &draw = m_nxDraws[i - segment.firstOp];
                draw.firstSubBatch = static_cast<uint32_t>(m_nxArena.subBatches.size() - subs.size());
                draw.subBatches = static_cast<uint32_t>(subs.size());
                draw.path = m_nxOverwritePath ? ClassifyNx(std::span(ops.commands).subspan(b.cmdOffset, b.cmdCount))
                                              : NxPath::Ordered;
                for (uint32_t s = 0; s < draw.subBatches; ++s) {
                    const Vdp1NxSubBatch &sb = m_nxArena.subBatches[draw.firstSubBatch + s];
                    bytes += AlignUp(VkDeviceSize{sb.spanCount} * sizeof(Vdp1NxSpan), kUploadAlignment);
                }
            }
        } else if (op.kind == Vdp1OpKind::FbWrites && op.index < ops.fbWriteRuns.size() &&
                   runOk(ops.fbWriteRuns[op.index])) {
            bytes += AlignUp(VkDeviceSize{ops.fbWriteRuns[op.index].count} * sizeof(Vdp1FbWrite), kUploadAlignment);
        }
    }
    Context &context = m_contexts[contextIndex];
    Allocation upload{};
    if (bytes != 0 && !Allocate(context, bytes, upload)) {
        return false;
    }
    // The generator's skipped native spans count as malformed data (the first logged); a lone N x span above a
    // sub-batch's slot capacity (unreachable with 13-bit coordinates) is counted in the stats
    if (const uint64_t skipped = m_nxArena.stats.malformedSpans - malformedSpansBefore; skipped != 0) {
        m_stats.malformed += static_cast<uint32_t>(skipped);
        if (m_malformedTotal == 0) {
            Log("VDP1: skipped " + std::to_string(skipped) +
                " malformed native span(s) at N x (data out of range); later ones are only counted");
        }
        m_malformedTotal += skipped;
    }
    m_stats.nxOversizedSpans += static_cast<uint32_t>(m_nxArena.stats.oversizedSpans - oversizedSpansBefore);

    const auto &vk = m_device.Table();
    VkDeviceSize cursor = 0;
    const auto put = [&](const void *data, VkDeviceSize size) {
        const VkDeviceSize offset = cursor;
        if (size != 0) {
            std::memcpy(upload.mapped + offset, data, size);
        }
        cursor += AlignUp(size, kUploadAlignment);
        return upload.offset + offset;
    };
    // Earlier reads of the back half's buffers (the previous batch's passes) before the copies overwrite them.
    const auto beforeCopies = [&] { Barrier(cmd, kCompute | kTransfer, kWrites, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT); };
    const auto afterCopies = [&] {
        Barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kCompute, VK_ACCESS_SHADER_READ_BIT);
        ProfileStamp(cmd, m_profileNx ? GpuPassProfile::kVdp1NxCopy : GpuPassProfile::kVdp1Copy);
    };

    StampBegin(contextIndex, cmd);
    ProfileStamp(cmd, GpuPassProfile::kBoundary);
    Prologue(cmd);
    std::vector<VkBufferCopy> vramRegions;
    for (uint32_t i = segment.firstOp; i < segment.firstOp + segment.opCount; ++i) {
        const Vdp1Op &op = ops.ops[i];
        switch (op.kind) {
        case Vdp1OpKind::Draw: {
            if (op.index >= ops.batches.size() || !batchOk(ops.batches[op.index])) {
                Malformed("draw batch", op.index);
                break;
            }
            const Vdp1DrawBatch &b = ops.batches[op.index];
            vramRegions.clear();
            for (uint32_t k = 0; k < b.copyCount; ++k) {
                const Vdp1VramCopy &c = ops.vramCopies[b.firstCopy + k];
                if (c.size == 0) {
                    continue;
                }
                if (!copyOk(c)) {
                    Malformed("VRAM copy", b.firstCopy + k);
                    continue;
                }
                vramRegions.push_back({put(ops.arena.data() + c.arenaOffset, c.size), c.vramOffset, c.size});
            }
            const VkDeviceSize cmdBytes = VkDeviceSize{b.cmdCount} * sizeof(Vdp1CommandParams);
            const VkDeviceSize spanBytes = VkDeviceSize{b.spanCount} * sizeof(Vdp1Span);
            const VkBufferCopy cmdCopy{put(ops.commands.data() + b.cmdOffset, cmdBytes), 0, cmdBytes};
            const VkBufferCopy spanCopy{put(ops.spans.data() + b.spanOffset, spanBytes), 0, spanBytes};
            beforeCopies();
            if (!vramRegions.empty()) {
                // The chunks of one batch are disjoint (whole dirty 256-byte chunks, each once)
                vk.vkCmdCopyBuffer(cmd, upload.buffer, m_buffers[kVram].buffer, static_cast<uint32_t>(vramRegions.size()),
                                   vramRegions.data());
            }
            if (cmdBytes != 0) {
                vk.vkCmdCopyBuffer(cmd, upload.buffer, m_buffers[kCommands].buffer, 1, &cmdCopy);
            }
            if (spanBytes != 0) {
                vk.vkCmdCopyBuffer(cmd, upload.buffer, m_buffers[kSpans].buffer, 1, &spanCopy);
            }
            afterCopies();

            Vdp1BatchPushConstants pc = Vdp1BatchConstants(b);
            assert(pc.scale == kRasterScale && pc.scale == 1); // the native scatter reads scale > 1 as N x units
            pc.cmdOffset = 0;
            pc.spanOffset = 0;
            if (const uint32_t groups = Groups(pc.spanCount, kWorkgroupSize); groups != 0) {
                Dispatch(cmd, kRaster, &pc, groups);
            }
            Dispatch(cmd, kBatchRange, nullptr, 1);
            DispatchIndirect(cmd, kBucket, nullptr, kBatchStateBucketArgs);
            Dispatch(cmd, kBucketSums, nullptr, 1);
            if (const uint32_t groups = Groups(pc.fragmentCount, kScatterSlotsPerGroup); groups != 0) {
                Dispatch(cmd, kScatter, &pc, groups);
            }
            DispatchIndirect(cmd, kResolve, &pc, kBatchStateResolveArgs);
            ++m_stats.batches;
            if (nx != 0) {
                // The same batch at N x: its sub-batches in order (each resolves on its own; a continuation has none)
                const NxDraw &draw = m_nxDraws[i - segment.firstOp];
                m_profileNx = true;
                for (uint32_t s = 0; s < draw.subBatches; ++s) {
                    const Vdp1NxSubBatch &sb = m_nxArena.subBatches[draw.firstSubBatch + s];
                    // The overwrite-class fast path (research section 1.6): a unit's last fragment is its result
                    const bool overwrite = draw.path == NxPath::Overwrite || draw.path == NxPath::OverwriteOddBytes;
                    const bool oddBytes = draw.path == NxPath::OverwriteOddBytes;
                    const bool hybrid = draw.path == NxPath::Hybrid || draw.path == NxPath::HybridPixel8;
                    const bool oddWinners = oddBytes || draw.path == NxPath::HybridPixel8;
                    const VkDeviceSize nxSpanBytes = VkDeviceSize{sb.spanCount} * sizeof(Vdp1NxSpan);
                    const VkBufferCopy nxSpanCopy{put(m_nxArena.spans.data() + sb.spanOffset, nxSpanBytes), 0,
                                                  nxSpanBytes};
                    beforeCopies();
                    if (nxSpanBytes != 0) {
                        vk.vkCmdCopyBuffer(cmd, upload.buffer, m_buffers[kSpans].buffer, 1, &nxSpanCopy);
                    }
                    // The winners in the unit offsets start from zero (the copies' barrier orders the fills after the
                    // earlier passes' accesses; the raster's atomics come after them)
                    const VkDeviceSize half = Vdp1NxUnitCountsBufferSize(nx);
                    const bool fillFirst = hybrid && m_nxOffsetsDirty;
                    const bool fillSecond = oddWinners && m_nxOddWinnersDirty;
                    if (fillFirst) {
                        vk.vkCmdFillBuffer(cmd, m_nxBuffers[kNxUnitOffsets].buffer, 0, half, 0);
                        m_nxOffsetsDirty = false;
                    }
                    if (fillSecond) {
                        vk.vkCmdFillBuffer(cmd, m_nxBuffers[kNxUnitOffsets].buffer, half, half, 0);
                        m_nxOddWinnersDirty = false;
                    }
                    if (fillFirst || fillSecond) {
                        Barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kCompute,
                                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
                    }
                    afterCopies();
                    const uint32_t drawFB = (b.drawFB & kBatchDrawFbMask) | (overwrite ? kBatchOverwrite : 0u) |
                                            (oddBytes ? kBatchOverwriteOddBytes : 0u) | (hybrid ? kBatchHybrid : 0u);
                    const Vdp1BatchPushConstants npc{0, sb.spanCount, 0, sb.slotCount, drawFB, nx};
                    if (const uint32_t groups = Groups(sb.spanCount, kWorkgroupSize); groups != 0) {
                        DispatchWith(cmd, m_nxPipes[kNxRaster], m_nxPipes[kNxRaster].set, &npc, groups, 1);
                    }
                    Dispatch(cmd, kBatchRange, nullptr, 1);
                    ++m_stats.nxSubBatches;
                    m_stats.nxSpans += sb.spanCount;
                    m_stats.nxSlots += sb.slotCount;
                    if (overwrite) {
                        DispatchIndirectWith(cmd, m_nxPipes[kNxResolveFast], m_nxPipes[kNxResolveFast].set, &npc,
                                             kBatchStateResolveArgs);
                        ++m_stats.nxOverwriteSubBatches;
                        m_stats.nxOverwriteOddBytes += oddBytes ? 1 : 0;
                        ++m_nxPathTotals.overwrite;
                        m_nxPathTotals.overwriteOddBytes += oddBytes ? 1 : 0;
                        continue;
                    }
                    if (hybrid) {
                        // Only each unit's last overwrite-class fragment and the later ones are counted
                        if (const uint32_t groups = Groups(sb.slotCount, kScatterSlotsPerGroup); groups != 0) {
                            DispatchWith(cmd, m_nxPipes[kNxFilter], m_nxPipes[kNxFilter].set, &npc, groups, 1);
                        }
                        ++m_stats.nxHybridSubBatches;
                        ++m_nxPathTotals.hybrid;
                        // filter_nx leaves the odd bytes' winners
                        m_nxOddWinnersDirty = m_nxOddWinnersDirty || oddWinners;
                    } else {
                        ++m_nxPathTotals.ordered;
                    }
                    m_nxOffsetsDirty = true; // the bucket pass writes the range's offsets
                    DispatchIndirectWith(cmd, m_pipes[kBucket], m_nxBucketSet, nullptr, kBatchStateBucketArgs);
                    DispatchWith(cmd, m_nxPipes[kNxBucketSums], m_nxPipes[kNxBucketSums].set, &npc, nx * nx, 1);
                    DispatchWith(cmd, m_nxPipes[kNxBucketSumsAdd], m_nxPipes[kNxBucketSumsAdd].set, &npc, nx * nx, 1);
                    if (const uint32_t groups = Groups(sb.slotCount, kScatterSlotsPerGroup); groups != 0) {
                        DispatchWith(cmd, m_pipes[kScatter], m_nxScatterSet, &npc, groups, 1);
                    }
                    DispatchIndirectWith(cmd, m_nxPipes[kNxResolve], m_nxPipes[kNxResolve].set, &npc,
                                         kBatchStateResolveArgs);
                }
                m_profileNx = false;
                m_stats.nxMaxSubBatchesPerBatch = std::max(m_stats.nxMaxSubBatchesPerBatch, draw.subBatches);
            }
            break;
        }
        case Vdp1OpKind::Erase: {
            if (op.index >= ops.erases.size()) {
                Malformed("erase", op.index);
                break;
            }
            const Vdp1ErasePushConstants &e = ops.erases[op.index];
            if (e.x3 > e.x1 && e.y3 >= e.y1) {
                const uint32_t gx = Groups(e.x3 - e.x1, kEraseGroupSize), gy = Groups(e.y3 - e.y1 + 1, kEraseGroupSize);
                Dispatch(cmd, kErase, &e, gx, gy);
                if (nx != 0) {
                    // The same region, value and write limit on the N x blocks
                    const Vdp1EraseNxPushConstants npc{e, nx};
                    DispatchWith(cmd, m_nxPipes[kNxErase], m_nxPipes[kNxErase].set, &npc, gx, gy);
                    ++m_stats.nxErases;
                }
            }
            ++m_stats.erases;
            break;
        }
        case Vdp1OpKind::FbWrites: {
            if (op.index >= ops.fbWriteRuns.size() || !runOk(ops.fbWriteRuns[op.index])) {
                Malformed("framebuffer write run", op.index);
                break;
            }
            const Vdp1FbWritePushConstants &run = ops.fbWriteRuns[op.index];
            if (run.count == 0) {
                break;
            }
            const VkDeviceSize recordBytes = VkDeviceSize{run.count} * sizeof(Vdp1FbWrite);
            const VkBufferCopy copy{put(ops.fbWrites.data() + run.offset, recordBytes), 0, recordBytes};
            beforeCopies();
            vk.vkCmdCopyBuffer(cmd, upload.buffer, m_buffers[kFbWrites].buffer, 1, &copy);
            afterCopies();
            const Vdp1FbWritePushConstants pc{0, run.count};
            const uint32_t groups = Groups(run.count, kWorkgroupSize);
            Dispatch(cmd, kFbWriteMark, &pc, groups);
            Dispatch(cmd, kFbWrite, &pc, groups);
            Dispatch(cmd, kFbWriteClear, &pc, groups);
            if (nx != 0) {
                // After the native run: each record's N x blocks take its mask's bits of the final native word
                const Vdp1FbWriteNxPushConstants npc{pc, nx};
                DispatchWith(cmd, m_nxPipes[kNxFbWrite], m_nxPipes[kNxFbWrite].set, &npc, groups, 1);
                ++m_stats.nxFbWriteRuns;
            }
            ++m_stats.fbWriteRuns;
            break;
        }
        }
    }
    Epilogue(cmd);
    StampEnd(contextIndex, cmd);
    return true;
}

bool Vdp1VulkanBackend::RecordFbramUpload(std::span<const uint8_t> slices, uint32_t mask, uint32_t contextIndex,
                                          VkCommandBuffer cmd) {
    if (!Ok() || contextIndex >= m_contexts.size() || cmd == VK_NULL_HANDLE || slices.size() < kFbramBufferSize) {
        return false;
    }
    mask &= (1u << kFbramSliceCount) - 1;
    if (mask == 0) {
        return true;
    }
    Allocation upload{};
    if (!Allocate(m_contexts[contextIndex], VkDeviceSize{static_cast<uint32_t>(std::popcount(mask))} * kFbramSliceSize,
                  upload)) {
        return false;
    }
    std::array<VkBufferCopy, kFbramSliceCount> regions{};
    uint32_t count = 0;
    for (uint32_t slice = 0; slice < kFbramSliceCount; ++slice) {
        if ((mask >> slice) & 1u) {
            const VkDeviceSize offset = VkDeviceSize{count} * kFbramSliceSize;
            std::memcpy(upload.mapped + offset, slices.data() + VkDeviceSize{slice} * kFbramSliceSize, kFbramSliceSize);
            regions[count++] = {upload.offset + offset, VkDeviceSize{slice} * kFbramSliceSize, kFbramSliceSize};
        }
    }
    StampBegin(contextIndex, cmd);
    ProfileStamp(cmd, GpuPassProfile::kBoundary);
    Prologue(cmd);
    m_device.Table().vkCmdCopyBuffer(cmd, upload.buffer, m_fbram, count, regions.data());
    ProfileStamp(cmd, GpuPassProfile::kVdp1Copy);
    if (m_nxScale != 0) {
        // The uploaded slices' N x blocks become their native words
        Barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kCompute, VK_ACCESS_SHADER_READ_BIT);
        RecordNxRebuild(cmd, mask);
    }
    Epilogue(cmd);
    StampEnd(contextIndex, cmd);
    ++m_stats.uploads;
    return true;
}

bool Vdp1VulkanBackend::RecordReadback(uint32_t contextIndex, uint32_t mask, VkCommandBuffer cmd) {
    mask &= (1u << kFbramSliceCount) - 1;
    if (!Ok() || cmd == VK_NULL_HANDLE || contextIndex >= m_readbacks.size()) {
        return false;
    }
    if (mask == 0) {
        return true;
    }
    vk_util::Buffer &readback = m_readbacks[contextIndex];
    if (readback.buffer == VK_NULL_HANDLE) {
        // The CPU reads all of it at every merge: cached memory first, coherent or not (non-coherent memory is
        // invalidated before each read, ReadbackData), then uncached.
        constexpr VkMemoryPropertyFlags kVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        constexpr VkMemoryPropertyFlags kCached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        constexpr VkMemoryPropertyFlags kCoherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VkMemoryPropertyFlags chosen = 0;
        for (const VkMemoryPropertyFlags flags :
             {kVisible | kCached | kCoherent, kVisible | kCached, kVisible | kCoherent}) {
            if (vk_util::CreateBuffer(m_device, kFbramBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, flags, readback) ==
                VK_SUCCESS) {
                chosen = flags;
                break;
            }
        }
        if (chosen == 0) {
            return false;
        }
        if (readback.mapped == nullptr) {
            vk_util::DestroyBuffer(m_device, readback);
            return false;
        }
        // The type CreateBuffer allocated from (the first with the flags): a cached request may get coherent memory
        VkMemoryRequirements requirements{};
        m_device.Table().vkGetBufferMemoryRequirements(m_device.Device(), readback.buffer, &requirements);
        const std::optional<uint32_t> type = vk_util::FindMemoryType(m_device, requirements.memoryTypeBits, chosen);
        const VkMemoryPropertyFlags actual =
            type ? m_device.MemoryProperties().memoryTypes[*type].propertyFlags : chosen;
        const bool cached = (actual & kCached) != 0, coherent = (actual & kCoherent) != 0;
        ReadbackMemory &memory = m_readbackMemory[contextIndex];
        memory = !coherent ? ReadbackMemory::CachedNonCoherent
                           : (cached ? ReadbackMemory::CachedCoherent : ReadbackMemory::Uncached);
        if (!m_readbackLogged) {
            m_readbackLogged = true;
            const char *kind = memory == ReadbackMemory::CachedCoherent      ? "host-cached coherent"
                               : memory == ReadbackMemory::CachedNonCoherent ? "host-cached non-coherent (invalidated)"
                                                                             : "uncached host-visible coherent";
            const std::string text = std::string("VDP1: framebuffer readbacks in ") + kind + " memory (type " +
                                     (type ? std::to_string(*type) : std::string("?")) + ")";
            Log(text);
        }
    }
    std::array<VkBufferCopy, kFbramSliceCount> regions{};
    uint32_t count = 0;
    for (uint32_t slice = 0; slice < kFbramSliceCount; ++slice) {
        if ((mask >> slice) & 1u) {
            const VkDeviceSize offset = VkDeviceSize{slice} * kFbramSliceSize;
            regions[count++] = {offset, offset, kFbramSliceSize};
        }
    }
    StampBegin(contextIndex, cmd);
    ProfileStamp(cmd, GpuPassProfile::kBoundary);
    Barrier(cmd, kCompute | kTransfer, kWrites, kTransfer, VK_ACCESS_TRANSFER_READ_BIT);
    m_device.Table().vkCmdCopyBuffer(cmd, m_fbram, readback.buffer, count, regions.data());
    Barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    ProfileStamp(cmd, GpuPassProfile::kVdp1Other);
    StampEnd(contextIndex, cmd);
    return true;
}

const uint8_t *Vdp1VulkanBackend::ReadbackData(uint32_t contextIndex, uint32_t slice) const {
    if (contextIndex >= m_readbacks.size() || slice >= kFbramSliceCount) {
        return nullptr;
    }
    const vk_util::Buffer &readback = m_readbacks[contextIndex];
    if (readback.mapped == nullptr) {
        return nullptr;
    }
    const VkDeviceSize offset = VkDeviceSize{slice} * kFbramSliceSize;
    if (m_readbackMemory[contextIndex] == ReadbackMemory::CachedNonCoherent || m_invalidateForTest) {
        // The range must be a multiple of nonCoherentAtomSize (a slice is, for every atom size up to 256 KiB)
        const VkDeviceSize atom = std::max<VkDeviceSize>(m_device.Properties().limits.nonCoherentAtomSize, 1);
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = readback.memory;
        range.offset = kFbramSliceSize % atom == 0 ? offset : 0;
        range.size = kFbramSliceSize % atom == 0 ? kFbramSliceSize : VK_WHOLE_SIZE;
        if (m_device.Table().vkInvalidateMappedMemoryRanges(m_device.Device(), 1, &range) != VK_SUCCESS) {
            return nullptr;
        }
    }
    return static_cast<const uint8_t *>(readback.mapped) + offset;
}

Vdp1VulkanBackend::ReadbackMemory Vdp1VulkanBackend::ReadbackMemoryForTest(uint32_t contextIndex) const {
    return contextIndex < m_readbackMemory.size() ? m_readbackMemory[contextIndex] : ReadbackMemory::None;
}

void Vdp1VulkanBackend::FrameDropped() {
    if (m_lastContext >= 0 && static_cast<uint32_t>(m_lastContext) < kMaxTimedContexts) {
        // Its timestamps are never written: drop them (the slots may hold an older command buffer's values)
        m_queryCount[m_lastContext] = 0;
        m_queriesReset[m_lastContext] = false;
    }
    if (m_recordedZero) {
        m_pendingZero = true;
        m_recordedZero = false;
    }
    if (m_nxRecordedZero) {
        m_nxPendingZero = true;
        m_nxRecordedZero = false;
    }
    m_nxPendingRebuild |= m_nxRecordedRebuild;
    m_nxRecordedRebuild = 0;
    m_nxOffsetsDirty = m_nxOddWinnersDirty = true; // a zero fill it carried never ran
}

} // namespace brimir::gpu::vdp1
