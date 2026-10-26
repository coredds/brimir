#include "vdp2_vulkan_backend.hpp"

#include "gpu/shaders/generated/vdp2_compose.spv.h"
#include "gpu/shaders/generated/vdp2_nbg_fetch.spv.h"
#include "gpu/shaders/generated/vdp2_render_bgs.spv.h"
#include "gpu/shaders/generated/vdp2_render_sprite.spv.h"
#include "gpu/vdp2/vdp2_bindings.hpp"

#include <algorithm>
#include <cstring>
#include <span>

namespace brimir::gpu::vdp2 {

namespace {

constexpr VkDeviceSize kRotBasesBytes = sizeof(Vdp2FrameRecord::rotBases);
constexpr VkDeviceSize kLnclBackBytes = sizeof(Vdp2FrameRecord::lnclBack);
constexpr VkDeviceSize kNBGLinesBytes = sizeof(Vdp2FrameRecord::nbgLines);
static_assert(kRotBasesBytes == kRotParamBaseCount * sizeof(VDP2RotParamBase));
static_assert(kLnclBackBytes == kLnclBackCount * sizeof(uint32_t));
static_assert(kNBGLinesBytes == kNBGLineCount * sizeof(VDP2NBGLine));

// Start of each sprite slice in the framebuffer buffer (Ymir's layout: main, alt, mesh0, mesh1, each display FB 0/1).
constexpr std::array<VkDeviceSize, 4> kSpriteSliceBase{0, 2 * kSpriteSliceSize, 4 * kSpriteSliceSize,
                                                       6 * kSpriteSliceSize};
static_assert(8 * kSpriteSliceSize == kSpriteFBBufferSize);

constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags kTransfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
constexpr VkAccessFlags kShaderRW = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

VkDescriptorType ToVk(DescriptorKind kind) {
    switch (kind) {
    case DescriptorKind::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case DescriptorKind::StorageBufferDynamic: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    case DescriptorKind::SampledImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case DescriptorKind::StorageImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

std::span<const Binding> BindingTable(uint32_t pipeline) {
    switch (pipeline) {
    case 0: return kRenderSpriteBindings;
    case 1: return kRenderBgsBindings;
    case 2: return kComposeBindings;
    default: return kNBGFetchBindings;
    }
}

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

Vdp2VulkanBackend::Vdp2VulkanBackend(VulkanDevice &device, uint32_t frameContexts) : m_device(device) {
    m_result = Init(frameContexts);
    if (m_result != VK_SUCCESS) {
        Destroy();
    }
}

Vdp2VulkanBackend::~Vdp2VulkanBackend() {
    Destroy();
}

VkResult Vdp2VulkanBackend::Init(uint32_t frameContexts) {
    if (frameContexts == 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    // Parameter snapshots are kParamSnapshotAlignment-aligned within the arena, the arena is placed at a multiple of
    // this, so every snapshot offset is a valid dynamic offset (minStorageBufferOffsetAlignment is at most 256).
    m_paramAlignment = std::max<VkDeviceSize>(kParamSnapshotAlignment,
                                              m_device.Properties().limits.minStorageBufferOffsetAlignment);

    // Buffers
    constexpr VkBufferUsageFlags kStorageDst = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    constexpr VkMemoryPropertyFlags kDeviceLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const struct {
        vk_util::Buffer *buffer;
        VkDeviceSize size;
        VkBufferUsageFlags usage;
    } buffers[] = {
        {&m_vram, kVRAMBufferSize, kStorageDst},
        {&m_cramColors, kVDP2CRAMColorBufferSize, kStorageDst},
        {&m_cramCoeffs, kVDP2CRAMRotCoeffBufferSize, kStorageDst},
        {&m_rotBases, kRotBasesBytes, kStorageDst},
        {&m_lnclBack, kLnclBackBytes, kStorageDst},
        {&m_nbgLines, kNBGLinesBytes, kStorageDst},
        {&m_spriteFB, kSpriteFBBufferSize, kStorageDst},
        {&m_composite, kCompositeBufferSize, kStorageDst | VK_BUFFER_USAGE_TRANSFER_SRC_BIT},
        {&m_nbgChars, kNBGCharBufferSize, kStorageDst}, // TRANSFER_DST for the test-only poison fill
    };
    for (const auto &b : buffers) {
        const VkResult result = vk_util::CreateBuffer(m_device, b.size, b.usage, kDeviceLocal, *b.buffer);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    // Images (VK_IMAGE_LAYOUT_GENERAL while in use; sampled and storage bindings share one view)
    // TRANSFER_DST for the test-only poison clear.
    constexpr VkImageUsageFlags kImageUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const struct {
        vk_util::Image *image;
        vk_util::ImageDesc desc;
    } images[] = {
        {&m_layers, {.width = kMaxResH, .height = kMaxResV, .format = VK_FORMAT_R8G8B8A8_UINT, .usage = kImageUsage,
                     .arrayLayers = kLayerImageLayers}},
        {&m_spriteAttrs, {.width = kMaxResH, .height = kMaxResV, .format = VK_FORMAT_R32_UINT, .usage = kImageUsage,
                          .arrayLayers = kSpriteAttrImageLayers}},
        {&m_rbgLineColor, {.width = kRBGLineColorWidth, .height = kMaxResV, .format = VK_FORMAT_R8G8B8A8_UINT,
                           .usage = kImageUsage, .arrayLayers = kRBGLineColorLayers}},
        {&m_colorCalcWindow,
         {.width = kMaxResH, .height = kMaxResV, .format = VK_FORMAT_R32_UINT, .usage = kImageUsage}},
    };
    for (const auto &i : images) {
        const VkResult result = vk_util::CreateImage2D(m_device, i.desc, *i.image);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    // Pipelines: one set (vdp2_bindings.hpp) and the push-constant block each
    const std::array<std::span<const uint32_t>, kPipelineCount> spirv{
        shaders::kVdp2RenderSpriteSpv, shaders::kVdp2RenderBgsSpv, shaders::kVdp2ComposeSpv,
        shaders::kVdp2NbgFetchSpv};
    for (uint32_t p = 0; p < kPipelineCount; ++p) {
        const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                            p == kNBGFetch ? kNBGFetchPushConstantSize : kPushConstantSize};
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const Binding &b : BindingTable(p)) {
            bindings.push_back({b.binding, ToVk(b.kind), 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
        VkResult result = vk_util::CreateDescriptorSetLayout(m_device, bindings, m_setLayouts[p]);
        if (result == VK_SUCCESS) {
            result = vk_util::CreatePipelineLayout(m_device, {&m_setLayouts[p], 1}, {&pushRange, 1},
                                                   m_pipelineLayouts[p]);
        }
        if (result == VK_SUCCESS) {
            result = vk_util::CreateComputePipeline(m_device, spirv[p], m_pipelineLayouts[p], m_pipelines[p]);
        }
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    m_contexts.resize(frameContexts);
    return VK_SUCCESS;
}

void Vdp2VulkanBackend::Destroy() {
    const auto &vk = m_device.Table();
    const VkDevice dev = m_device.Device();
    for (FrameContext &context : m_contexts) {
        for (UploadBlock &block : context.blocks) {
            DestroyBlock(block);
        }
    }
    m_contexts.clear();
    for (uint32_t p = 0; p < kPipelineCount; ++p) {
        vk.vkDestroyPipeline(dev, m_pipelines[p], nullptr);
        vk.vkDestroyPipelineLayout(dev, m_pipelineLayouts[p], nullptr);
        vk.vkDestroyDescriptorSetLayout(dev, m_setLayouts[p], nullptr);
        m_pipelines[p] = VK_NULL_HANDLE;
        m_pipelineLayouts[p] = VK_NULL_HANDLE;
        m_setLayouts[p] = VK_NULL_HANDLE;
    }
    for (vk_util::Image *image : {&m_layers, &m_spriteAttrs, &m_rbgLineColor, &m_colorCalcWindow}) {
        vk_util::DestroyImage(m_device, *image);
    }
    for (vk_util::Buffer *buffer :
         {&m_vram, &m_cramColors, &m_cramCoeffs, &m_rotBases, &m_lnclBack, &m_nbgLines, &m_spriteFB, &m_composite,
          &m_nbgChars}) {
        vk_util::DestroyBuffer(m_device, *buffer);
    }
}

void Vdp2VulkanBackend::DestroyBlock(UploadBlock &block) {
    m_device.Table().vkDestroyDescriptorPool(m_device.Device(), block.pool, nullptr); // frees the sets
    vk_util::DestroyBuffer(m_device, block.buffer);
    block = UploadBlock{};
}

void Vdp2VulkanBackend::FrameDropped() {
    if (m_recorded.clearComposite && !m_pending.clearComposite) {
        m_pending.clearComposite = true;
        m_pending.clearColor = m_recorded.clearColor;
    }
    m_pending.zeroSpriteFB |= m_recorded.zeroSpriteFB;
    m_recorded = {};
}

void Vdp2VulkanBackend::EnsureContexts(uint32_t count) {
    if (count > m_contexts.size()) {
        m_contexts.resize(count);
    }
}

void Vdp2VulkanBackend::ForceUploadBlockSizeForTest(size_t bytes) {
    m_blockSize = std::max<size_t>(bytes, 4096);
}

uint32_t Vdp2VulkanBackend::UploadBlocksForTest(uint32_t context) const {
    return context < m_contexts.size() ? static_cast<uint32_t>(m_contexts[context].blocks.size()) : 0;
}

void Vdp2VulkanBackend::ResetContext(FrameContext &context) {
    // The context's previous frame has completed (the caller's fence): overflow blocks go, the base block is reused
    // unless its size no longer matches.
    while (context.blocks.size() > 1) {
        DestroyBlock(context.blocks.back());
        context.blocks.pop_back();
    }
    if (!context.blocks.empty() && context.blocks.front().buffer.size != m_blockSize) {
        DestroyBlock(context.blocks.front());
        context.blocks.clear();
    }
    if (!context.blocks.empty()) {
        context.blocks.front().used = 0;
    }
}

bool Vdp2VulkanBackend::AddBlock(FrameContext &context, VkDeviceSize size) {
    UploadBlock block;
    const VkResult result = vk_util::CreateBuffer(
        m_device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, block.buffer);
    if (result != VK_SUCCESS || block.buffer.mapped == nullptr) {
        vk_util::DestroyBuffer(m_device, block.buffer);
        return false;
    }
    context.blocks.push_back(block);
    return true;
}

bool Vdp2VulkanBackend::Allocate(FrameContext &context, VkDeviceSize size, VkDeviceSize alignment, Allocation &out) {
    if (context.blocks.empty() && !AddBlock(context, m_blockSize)) {
        return false;
    }
    UploadBlock *block = &context.blocks.back();
    VkDeviceSize offset = AlignUp(block->used, alignment);
    if (offset + size > block->buffer.size) {
        // Overflow: chain a new block, never wait for the GPU. It is freed at the context's next Record.
        if (!AddBlock(context, std::max<VkDeviceSize>(m_blockSize, AlignUp(size, 64 * 1024)))) {
            return false;
        }
        block = &context.blocks.back();
        offset = 0;
    }
    block->used = offset + size;
    out = {context.blocks.size() - 1, offset};
    return true;
}

VkResult Vdp2VulkanBackend::CreateBlockSets(UploadBlock &block) {
    const auto &vk = m_device.Table();
    const VkDevice dev = m_device.Device();

    std::array<VkDescriptorPoolSize, 4> sizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 0},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 0},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0},
    }};
    for (uint32_t p = 0; p < kPipelineCount; ++p) {
        for (const Binding &b : BindingTable(p)) {
            ++sizes[static_cast<size_t>(b.kind)].descriptorCount;
        }
    }
    static_assert(static_cast<size_t>(DescriptorKind::StorageBuffer) == 0 &&
                  static_cast<size_t>(DescriptorKind::StorageBufferDynamic) == 1 &&
                  static_cast<size_t>(DescriptorKind::SampledImage) == 2 &&
                  static_cast<size_t>(DescriptorKind::StorageImage) == 3);
    VkResult result = vk_util::CreateDescriptorPool(m_device, sizes, kPipelineCount, block.pool);
    for (uint32_t p = 0; p < kPipelineCount && result == VK_SUCCESS; ++p) {
        result = vk_util::AllocateDescriptorSet(m_device, block.pool, m_setLayouts[p], block.sets[p]);
    }
    if (result != VK_SUCCESS) {
        vk.vkDestroyDescriptorPool(dev, block.pool, nullptr);
        block.pool = VK_NULL_HANDLE;
        block.sets = {};
        return result;
    }

    for (uint32_t p = 0; p < kPipelineCount; ++p) {
        for (const Binding &b : BindingTable(p)) {
            VkDescriptorBufferInfo bufferInfo{VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
            VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
            switch (b.binding) {
            case kBindingLayerParams: bufferInfo = {block.buffer.buffer, 0, sizeof(VDP2LayerRenderParams)}; break;
            case kBindingComposeParams: bufferInfo = {block.buffer.buffer, 0, sizeof(VDP2ComposeParams)}; break;
            case kBindingVRAM: bufferInfo.buffer = m_vram.buffer; break;
            case kBindingCRAMColor: bufferInfo.buffer = m_cramColors.buffer; break;
            case kBindingCRAMRotCoeff: bufferInfo.buffer = m_cramCoeffs.buffer; break;
            case kBindingRotParamBases: bufferInfo.buffer = m_rotBases.buffer; break;
            case kBindingSpriteFB: bufferInfo.buffer = m_spriteFB.buffer; break;
            case kBindingNBGLines: bufferInfo.buffer = m_nbgLines.buffer; break;
            case kBindingLnclBack: bufferInfo.buffer = m_lnclBack.buffer; break;
            case kBindingCompositeOut: bufferInfo.buffer = m_composite.buffer; break;
            case kBindingNBGChars:
            case kBindingNBGCharsOut: bufferInfo.buffer = m_nbgChars.buffer; break;
            case kBindingSpriteAttrsIn:
            case kBindingSpriteAttrsOut: imageInfo.imageView = m_spriteAttrs.view; break;
            case kBindingLayerIn:
            case kBindingLayerOut: imageInfo.imageView = m_layers.view; break;
            case kBindingRBGLineColorIn:
            case kBindingRBGLineColorOut: imageInfo.imageView = m_rbgLineColor.view; break;
            case kBindingColorCalcWindowIn:
            case kBindingColorCalcWindowOut: imageInfo.imageView = m_colorCalcWindow.view; break;
            default: break;
            }
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = block.sets[p];
            write.dstBinding = b.binding;
            write.descriptorCount = 1;
            write.descriptorType = ToVk(b.kind);
            if (b.kind == DescriptorKind::SampledImage || b.kind == DescriptorKind::StorageImage) {
                write.pImageInfo = &imageInfo;
            } else {
                write.pBufferInfo = &bufferInfo;
            }
            vk.vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
        }
    }
    return VK_SUCCESS;
}

void Vdp2VulkanBackend::Barrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                                VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) const {
    vk_util::GlobalBarrier(m_device, cmd, srcStage, srcAccess, dstStage, dstAccess);
}

void Vdp2VulkanBackend::Dispatch(VkCommandBuffer cmd, Pipeline pipeline, const UploadBlock &block,
                                 uint32_t paramsOffset, const VDP2CommonRenderParams &common, uint32_t startY,
                                 uint32_t lines, uint32_t groupsX, uint32_t groupsZ, uint32_t outputSize) const {
    const auto &vk = m_device.Table();
    VDP2CommonRenderParams push = common;
    push.startY = startY;
    push.outputSize = outputSize;
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelines[pipeline]);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayouts[pipeline], 0, 1,
                               &block.sets[pipeline], 1, &paramsOffset);
    vk.vkCmdPushConstants(cmd, m_pipelineLayouts[pipeline], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vk.vkCmdDispatch(cmd, groupsX, lines, groupsZ);
}

void Vdp2VulkanBackend::DispatchNBGFetch(VkCommandBuffer cmd, const UploadBlock &block, uint32_t paramsOffset,
                                         const VDP2CommonRenderParams &common, uint32_t startY, uint32_t lines,
                                         uint32_t outputSize) const {
    const auto &vk = m_device.Table();
    VDP2NBGFetchPushConstants push{};
    push.common = common;
    push.common.startY = startY;
    push.common.outputSize = outputSize;
    push.rows = lines;
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelines[kNBGFetch]);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayouts[kNBGFetch], 0, 1,
                               &block.sets[kNBGFetch], 1, &paramsOffset);
    vk.vkCmdPushConstants(cmd, m_pipelineLayouts[kNBGFetch], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vk.vkCmdDispatch(cmd, lines, 1, kNBGFetchDispatchZ);
}

bool Vdp2VulkanBackend::Record(const Vdp2FrameRecord &frame, uint32_t contextIndex, VkCommandBuffer cmd) {
    if (!Ok() || contextIndex >= m_contexts.size() || cmd == VK_NULL_HANDLE) {
        return false;
    }
    // The frame's clear request becomes pending first, so a failed Record keeps it for the next frame (a newer clear
    // replaces an older one, as in the software framebuffer).
    if (frame.clearComposite) {
        m_pending.clearComposite = true;
        m_pending.clearColor = frame.clearColor;
    }
    // ... and so does the last clear inside it (as a whole-composite clear).
    const auto failed = [&] {
        if (!frame.clears.empty()) {
            m_pending.clearComposite = true;
            m_pending.clearColor = frame.clears.back().color;
        }
        return false;
    };
    const auto &vk = m_device.Table();
    FrameContext &context = m_contexts[contextIndex];
    ResetContext(context);

    // Upload: the per-line arrays, then the arena (parameter snapshots, VRAM/CRAM snapshots, sprite slices) at an
    // offset where every snapshot is a valid dynamic offset.
    Allocation lines, arena;
    const VkDeviceSize arenaSize = std::max<VkDeviceSize>(frame.arena.size(), 4);
    if (!Allocate(context, kRotBasesBytes + kLnclBackBytes + kNBGLinesBytes, 16, lines) ||
        !Allocate(context, arenaSize, m_paramAlignment, arena)) {
        return failed();
    }
    UploadBlock &linesBlock = context.blocks[lines.block];
    UploadBlock &arenaBlock = context.blocks[arena.block];
    if (arenaBlock.pool == VK_NULL_HANDLE && CreateBlockSets(arenaBlock) != VK_SUCCESS) {
        return failed();
    }
    {
        auto *dst = static_cast<uint8_t *>(linesBlock.buffer.mapped) + lines.offset;
        std::memcpy(dst, frame.rotBases.data(), kRotBasesBytes);
        std::memcpy(dst + kRotBasesBytes, frame.lnclBack.data(), kLnclBackBytes);
        std::memcpy(dst + kRotBasesBytes + kLnclBackBytes, frame.nbgLines.data(), kNBGLinesBytes);
        if (!frame.arena.empty()) {
            std::memcpy(static_cast<uint8_t *>(arenaBlock.buffer.mapped) + arena.offset, frame.arena.data(),
                        frame.arena.size());
        }
    }
    const VkBuffer arenaBuffer = arenaBlock.buffer.buffer;
    const VkDeviceSize arenaBase = arena.offset;
    const auto arenaRangeOk = [&](uint64_t offset, uint64_t size) { return offset + size <= frame.arena.size(); };

    // Earlier frames' accesses to everything below (shader reads and writes, copies, the presenter's composite read)
    // finish before this frame's uploads overwrite it.
    Barrier(cmd, kCompute | kTransfer, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, kTransfer,
            VK_ACCESS_TRANSFER_WRITE_BIT);

    // Pending one-time work: the composite clear, and zeroing the sprite framebuffer of a new back half so slots that
    // are never uploaded read zeros. Recorded here, it stays recorded unless the caller reports the command buffer
    // dropped (FrameDropped).
    m_recorded = m_pending;
    if (m_pending.clearComposite) {
        vk.vkCmdFillBuffer(cmd, m_composite.buffer, 0, VK_WHOLE_SIZE, m_pending.clearColor);
    }
    if (m_pending.zeroSpriteFB) {
        vk.vkCmdFillBuffer(cmd, m_spriteFB.buffer, 0, VK_WHOLE_SIZE, 0);
        // The slice copies below overwrite parts of it (WAW).
        Barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    m_pending = {};

    const VkBuffer linesBuffer = linesBlock.buffer.buffer;
    const VkBufferCopy rotCopy{lines.offset, 0, kRotBasesBytes};
    const VkBufferCopy lnclCopy{lines.offset + kRotBasesBytes, 0, kLnclBackBytes};
    const VkBufferCopy nbgCopy{lines.offset + kRotBasesBytes + kLnclBackBytes, 0, kNBGLinesBytes};
    vk.vkCmdCopyBuffer(cmd, linesBuffer, m_rotBases.buffer, 1, &rotCopy);
    vk.vkCmdCopyBuffer(cmd, linesBuffer, m_lnclBack.buffer, 1, &lnclCopy);
    vk.vkCmdCopyBuffer(cmd, linesBuffer, m_nbgLines.buffer, 1, &nbgCopy);

    // Sprite slices of the display framebuffer, at their place in Ymir's slice layout.
    std::array<VkBufferCopy, 4> sliceCopies;
    uint32_t sliceCount = 0;
    for (uint32_t i = 0; i < frame.spriteSliceOffset.size(); ++i) {
        const uint32_t offset = frame.spriteSliceOffset[i];
        if (offset == UINT32_MAX || !arenaRangeOk(offset, kSpriteSliceSize)) {
            continue;
        }
        sliceCopies[sliceCount++] = {arenaBase + offset,
                                     kSpriteSliceBase[i] + (frame.spriteDisplayFB & 1u) * kSpriteSliceSize,
                                     kSpriteSliceSize};
    }
    if (sliceCount > 0) {
        vk.vkCmdCopyBuffer(cmd, arenaBuffer, m_spriteFB.buffer, sliceCount, sliceCopies.data());
    }

    // The layer images only hold rows of the frame being rendered (every row composed this frame is rendered this
    // frame), so they start from UNDEFINED each frame, independent of what earlier command buffers did.
    for (const vk_util::Image *image : {&m_layers, &m_spriteAttrs, &m_rbgLineColor, &m_colorCalcWindow}) {
        vk_util::ImageBarrier(m_device, cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                              kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute | kTransfer,
                              kShaderRW | VK_ACCESS_TRANSFER_WRITE_BIT);
        if (m_poisonImages) {
            // Test only: any read of a texel this frame did not write sees the poison value.
            VkClearColorValue poison{};
            poison.uint32[0] = poison.uint32[1] = poison.uint32[2] = poison.uint32[3] = 0xA5A5A5A5u;
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
            vk.vkCmdClearColorImage(cmd, image->image, VK_IMAGE_LAYOUT_GENERAL, &poison, 1, &range);
        }
    }
    if (m_poisonImages) {
        vk.vkCmdFillBuffer(cmd, m_nbgChars.buffer, 0, VK_WHOLE_SIZE, 0xA5A5A5A5u);
    }
    Barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kCompute, kShaderRW);

    // Composite clears inside the frame, in order between the batches: the compose writes of earlier batches (WAW) come
    // first; later compose writes (WAW) and the frame's readers come after.
    size_t nextClear = 0;
    const auto clearsBefore = [&](size_t batchIndex) {
        for (; nextClear < frame.clears.size() && frame.clears[nextClear].beforeBatch <= batchIndex; ++nextClear) {
            const Vdp2CompositeClear &clear = frame.clears[nextClear];
            const VkDeviceSize bytes = clear.words == 0
                                           ? VK_WHOLE_SIZE
                                           : std::min<VkDeviceSize>(VkDeviceSize{clear.words} * sizeof(uint32_t),
                                                                    kCompositeBufferSize);
            Barrier(cmd, kCompute | kTransfer, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, kTransfer,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
            vk.vkCmdFillBuffer(cmd, m_composite.buffer, 0, bytes, clear.color);
            Barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kCompute, kShaderRW);
        }
    };

    // Batches in record order: clears, copies, then sprite and backgrounds over the layer rows, then compose, each
    // with the resolution its lines were rendered at.
    std::vector<VkBufferCopy> regions[3];
    for (size_t batchIndex = 0; batchIndex < frame.batches.size(); ++batchIndex) {
        const Vdp2Batch &batch = frame.batches[batchIndex];
        clearsBefore(batchIndex);
        const uint32_t hres = std::min(batch.hres, kMaxResH);
        const uint32_t vres = std::min(batch.vres, kMaxResV);
        const uint32_t outputSize = MakeOutputSize(hres, vres);
        const uint32_t groupsX = (hres + kWorkgroupWidth - 1) / kWorkgroupWidth;
        if (batch.copyCount > 0) {
            // Earlier dispatches' reads of these buffers (WAR) and earlier copies (WAW) come first.
            Barrier(cmd, kCompute | kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT);
            const VkBuffer targets[3] = {m_vram.buffer, m_cramColors.buffer, m_cramCoeffs.buffer};
            const auto flushRegions = [&] {
                for (size_t t = 0; t < 3; ++t) {
                    if (!regions[t].empty()) {
                        vk.vkCmdCopyBuffer(cmd, arenaBuffer, targets[t], static_cast<uint32_t>(regions[t].size()),
                                           regions[t].data());
                        regions[t].clear();
                    }
                }
            };
            for (auto &r : regions) {
                r.clear();
            }
            for (uint32_t i = 0; i < batch.copyCount && batch.firstCopy + i < frame.copies.size(); ++i) {
                const Vdp2Copy &copy = frame.copies[batch.firstCopy + i];
                VkDeviceSize limit = 0;
                size_t target = 0;
                switch (copy.target) {
                case Vdp2Copy::Target::VRAM: limit = kVRAMBufferSize, target = 0; break;
                case Vdp2Copy::Target::CRAMColors: limit = kVDP2CRAMColorBufferSize, target = 1; break;
                case Vdp2Copy::Target::CRAMCoeffs: limit = kVDP2CRAMRotCoeffBufferSize, target = 2; break;
                }
                if (copy.size == 0 || !arenaRangeOk(copy.arenaOffset, copy.size) ||
                    uint64_t{copy.dstOffset} + copy.size > limit) {
                    continue;
                }
                // A batch can carry several snapshots of the same range (e.g. CRAM written on lines that only split
                // compose); they apply in order, so an overlapping one waits for the copies before it.
                const auto overlaps = [&](const VkBufferCopy &r) {
                    return r.dstOffset < uint64_t{copy.dstOffset} + copy.size && copy.dstOffset < r.dstOffset + r.size;
                };
                if (std::any_of(regions[target].begin(), regions[target].end(), overlaps)) {
                    flushRegions();
                    Barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT);
                }
                regions[target].push_back({arenaBase + copy.arenaOffset, copy.dstOffset, copy.size});
            }
            flushRegions();
            Barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kCompute, VK_ACCESS_SHADER_READ_BIT);
        }
        if (batch.layerLines > 0) {
            const uint32_t layerOffset = static_cast<uint32_t>(arenaBase) + batch.layerParamsOffset;
            const uint32_t spriteLayers = batch.common.enhancements.transparentMeshes ? 2 : 1;
            Dispatch(cmd, kSprite, arenaBlock, layerOffset, batch.common, batch.layerStartY, batch.layerLines,
                     groupsX, spriteLayers, outputSize);
            Barrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute, kShaderRW);
            if (batch.needsFetchPass) {
                // After the sprite pass (the sprite window), before bgs (the character table).
                DispatchNBGFetch(cmd, arenaBlock, layerOffset, batch.common, batch.layerStartY, batch.layerLines,
                                 outputSize);
                Barrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute, kShaderRW);
            }
            Dispatch(cmd, kBgs, arenaBlock, layerOffset, batch.common, batch.layerStartY, batch.layerLines, groupsX,
                     kBgsDispatchZ, outputSize);
            Barrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute, kShaderRW);
        }
        if (batch.composeLines > 0) {
            const uint32_t composeOffset = static_cast<uint32_t>(arenaBase) + batch.composeParamsOffset;
            Dispatch(cmd, kCompose, arenaBlock, composeOffset, batch.common, batch.composeStartY,
                     batch.composeLines, groupsX, 1, outputSize);
            Barrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute, kShaderRW);
        }
    }
    clearsBefore(frame.batches.size());
    if (!frame.clears.empty()) {
        // A dropped command buffer re-arms the frame's last clear (as a whole-composite clear at the next Record).
        m_recorded.clearComposite = true;
        m_recorded.clearColor = frame.clears.back().color;
    }

    // The composite is complete: readable by later compute (the presenter) and transfer (readbacks) commands.
    Barrier(cmd, kCompute | kTransfer, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, kCompute | kTransfer,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    return true;
}

} // namespace brimir::gpu::vdp2
