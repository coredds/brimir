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

#include <algorithm>
#include <bit>
#include <cstring>

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
    const auto &vk = m_device.Table();
    static constexpr std::array<uint32_t, kBufCount> kBindings{
        kBindingCommands,  kBindingSpans,    kBindingVRAM,        kBindingFbWrites,   kBindingFragments, kBindingUnitCounts,
        kBindingBucket,    kBindingScanSums, kBindingUnitOffsets, kBindingBatchState, kBindingFbWinners};
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
    const std::array<PassDesc, kPassCount> passes{{
        {shaders::kVdp1RasterSpv, kRasterBindings, sizeof(Vdp1BatchPushConstants)},
        {shaders::kVdp1BatchRangeSpv, kBatchRangeBindings, 0},
        {shaders::kVdp1BucketSpv, kBucketBindings, 0},
        {shaders::kVdp1BucketSumsSpv, kBucketSumsBindings, 0},
        {shaders::kVdp1ScatterSpv, kScatterBindings, sizeof(Vdp1BatchPushConstants)},
        {shaders::kVdp1ResolveSpv, kResolveBindings, sizeof(Vdp1BatchPushConstants)},
        {shaders::kVdp1EraseSpv, kEraseBindings, sizeof(Vdp1ErasePushConstants)},
        {shaders::kVdp1FbWriteMarkSpv, kFbWriteMarkBindings, sizeof(Vdp1FbWritePushConstants)},
        {shaders::kVdp1FbWriteSpv, kFbWriteBindings, sizeof(Vdp1FbWritePushConstants)},
        {shaders::kVdp1FbWriteClearSpv, kFbWriteClearBindings, sizeof(Vdp1FbWritePushConstants)},
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
        for (const Binding &b : p.bindings) {
            VkDescriptorBufferInfo info{VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
            if (b.binding == kBindingFbram) {
                info.buffer = m_fbram;
            } else {
                for (uint32_t k = 0; k < kBufCount; ++k) {
                    if (kBindings[k] == b.binding) {
                        info.buffer = m_buffers[k].buffer;
                    }
                }
            }
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = pipe.set;
            write.dstBinding = b.binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &info;
            vk.vkUpdateDescriptorSets(m_device.Device(), 1, &write, 0, nullptr);
        }
    }
    m_contexts.resize(frameContexts);
    return VK_SUCCESS;
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
    vk_util::DestroyBuffer(m_device, m_readback);
}

void Vdp1VulkanBackend::EnsureContexts(uint32_t count) {
    if (count > m_contexts.size()) {
        m_contexts.resize(count);
    }
}

bool Vdp1VulkanBackend::BeginContext(uint32_t contextIndex) {
    if (!Ok() || contextIndex >= m_contexts.size()) {
        return false;
    }
    Context &context = m_contexts[contextIndex];
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
}

void Vdp1VulkanBackend::Epilogue(VkCommandBuffer cmd) const {
    // FBRAM (and everything else) is readable and writable by later compute and transfer work: the VDP2 sprite pass,
    // readbacks, the next Record.
    Barrier(cmd, kCompute | kTransfer, kWrites, kCompute | kTransfer, kAll);
}

void Vdp1VulkanBackend::PassBarrier(VkCommandBuffer cmd) const {
    // A pass's writes before the next pass's shader accesses and indirect argument reads
    Barrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
}

void Vdp1VulkanBackend::Dispatch(VkCommandBuffer cmd, Pass pass, const void *push, uint32_t x, uint32_t y) const {
    const auto &vk = m_device.Table();
    const Pipe &p = m_pipes[pass];
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &p.set, 0, nullptr);
    if (p.pushSize != 0) {
        vk.vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, p.pushSize, push);
    }
    vk.vkCmdDispatch(cmd, x, y, 1);
    PassBarrier(cmd);
}

void Vdp1VulkanBackend::DispatchIndirect(VkCommandBuffer cmd, Pass pass, const void *push, uint32_t argsWord) const {
    const auto &vk = m_device.Table();
    const Pipe &p = m_pipes[pass];
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &p.set, 0, nullptr);
    if (p.pushSize != 0) {
        vk.vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, p.pushSize, push);
    }
    vk.vkCmdDispatchIndirect(cmd, m_buffers[kBatchState].buffer, argsWord * sizeof(uint32_t));
    PassBarrier(cmd);
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
    const auto afterCopies = [&] { Barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kCompute, VK_ACCESS_SHADER_READ_BIT); };

    Prologue(cmd);
    std::vector<VkBufferCopy> vramRegions;
    for (uint32_t i = segment.firstOp; i < segment.firstOp + segment.opCount; ++i) {
        const Vdp1Op &op = ops.ops[i];
        switch (op.kind) {
        case Vdp1OpKind::Draw: {
            if (op.index >= ops.batches.size() || !batchOk(ops.batches[op.index])) {
                break;
            }
            const Vdp1DrawBatch &b = ops.batches[op.index];
            vramRegions.clear();
            for (uint32_t k = 0; k < b.copyCount; ++k) {
                const Vdp1VramCopy &c = ops.vramCopies[b.firstCopy + k];
                if (c.size == 0 || !copyOk(c)) {
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
            break;
        }
        case Vdp1OpKind::Erase: {
            if (op.index >= ops.erases.size()) {
                break;
            }
            const Vdp1ErasePushConstants &e = ops.erases[op.index];
            if (e.x3 > e.x1 && e.y3 >= e.y1) {
                Dispatch(cmd, kErase, &e, Groups(e.x3 - e.x1, kEraseGroupSize), Groups(e.y3 - e.y1 + 1, kEraseGroupSize));
            }
            ++m_stats.erases;
            break;
        }
        case Vdp1OpKind::FbWrites: {
            if (op.index >= ops.fbWriteRuns.size() || !runOk(ops.fbWriteRuns[op.index])) {
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
            ++m_stats.fbWriteRuns;
            break;
        }
        }
    }
    Epilogue(cmd);
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
    Prologue(cmd);
    m_device.Table().vkCmdCopyBuffer(cmd, upload.buffer, m_fbram, count, regions.data());
    Epilogue(cmd);
    ++m_stats.uploads;
    return true;
}

bool Vdp1VulkanBackend::RecordReadback(VkCommandBuffer cmd) {
    if (!Ok() || cmd == VK_NULL_HANDLE) {
        return false;
    }
    if (m_readback.buffer == VK_NULL_HANDLE) {
        constexpr VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (vk_util::CreateBuffer(m_device, kFbramBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  kHost | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, m_readback) != VK_SUCCESS &&
            vk_util::CreateBuffer(m_device, kFbramBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, kHost, m_readback) !=
                VK_SUCCESS) {
            return false;
        }
        if (m_readback.mapped == nullptr) {
            vk_util::DestroyBuffer(m_device, m_readback);
            return false;
        }
    }
    Barrier(cmd, kCompute | kTransfer, kWrites, kTransfer, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferCopy region{0, 0, kFbramBufferSize};
    m_device.Table().vkCmdCopyBuffer(cmd, m_fbram, m_readback.buffer, 1, &region);
    Barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    return true;
}

const uint8_t *Vdp1VulkanBackend::ReadbackData() const {
    return static_cast<const uint8_t *>(m_readback.mapped);
}

void Vdp1VulkanBackend::FrameDropped() {
    if (m_recordedZero) {
        m_pendingZero = true;
        m_recordedZero = false;
    }
}

} // namespace brimir::gpu::vdp1
