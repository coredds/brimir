#include "vdp2_vulkan_backend.hpp"

#include "gpu/shaders/generated/vdp2_compose.spv.h"
#include "gpu/shaders/generated/vdp2_nbg_fetch.spv.h"
#include "gpu/shaders/generated/vdp2_render_bgs.spv.h"
#include "gpu/shaders/generated/vdp2_render_sprite.spv.h"
#include "gpu/shaders/generated/vdp2_rescale.spv.h"
#include "gpu/vdp2/vdp2_bindings.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

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

// The layer images and the colour calculation window: VK_IMAGE_LAYOUT_GENERAL while in use; sampled and storage
// bindings share one view; TRANSFER_DST for the test-only poison clear.
constexpr VkImageUsageFlags kImageUsage =
    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

constexpr VkBufferUsageFlags kStorageDst = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
constexpr VkBufferUsageFlags kCompositeUsage = kStorageDst | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

vk_util::ImageDesc BGLayersDesc(uint32_t n) {
    return {.width = ScaledResH(n), .height = ScaledResV(n), .format = VK_FORMAT_R8G8B8A8_UINT, .usage = kImageUsage,
            .arrayLayers = kBGLayerImageLayers};
}

// VDP1 upscaling: the N x sprite layer and attribute images
vk_util::ImageDesc SpriteLayersNxDesc(uint32_t n) {
    return {.width = ScaledResH(n), .height = ScaledResV(n), .format = VK_FORMAT_R8G8B8A8_UINT, .usage = kImageUsage,
            .arrayLayers = kSpriteLayerImageLayers};
}
vk_util::ImageDesc SpriteAttrsNxDesc(uint32_t n) {
    return {.width = ScaledResH(n), .height = ScaledResV(n), .format = VK_FORMAT_R32_UINT, .usage = kImageUsage,
            .arrayLayers = kSpriteAttrImageLayers};
}

} // namespace

Vdp2VulkanBackend::Vdp2VulkanBackend(VulkanDevice &device, uint32_t frameContexts, uint32_t scale,
                                     VkDeviceSize scaleAllocationLimitForTest,
                                     std::function<void(const std::string &)> log, uint32_t fallbackScale)
    : m_device(device), m_log(std::move(log)), m_scaleAllocationLimit(scaleAllocationLimitForTest) {
    m_result = Init(frameContexts, std::clamp<uint32_t>(scale, 1, kMaxRenderScale), fallbackScale);
    if (m_result != VK_SUCCESS) {
        Destroy();
    }
}

Vdp2VulkanBackend::~Vdp2VulkanBackend() {
    Destroy();
}

VkResult Vdp2VulkanBackend::Init(uint32_t frameContexts, uint32_t scale, uint32_t fallbackScale) {
    if (frameContexts == 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    // Parameter snapshots are kParamSnapshotAlignment-aligned within the arena, the arena is placed at a multiple of
    // this, so every snapshot offset is a valid dynamic offset (minStorageBufferOffsetAlignment is at most 256).
    m_paramAlignment = std::max<VkDeviceSize>(kParamSnapshotAlignment,
                                              m_device.Properties().limits.minStorageBufferOffsetAlignment);

    // Buffers (the composite with the scaled resources below)
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
        // TRANSFER_SRC: the GPU FBRAM in GPU VDP1 mode is read back (SpriteFBBuffer)
        {&m_spriteFB, kSpriteFBBufferSize, kStorageDst | VK_BUFFER_USAGE_TRANSFER_SRC_BIT},
        {&m_nbgChars, kNBGCharBufferSize, kStorageDst}, // TRANSFER_DST for the test-only poison fill
    };
    for (const auto &b : buffers) {
        const VkResult result = vk_util::CreateBuffer(m_device, b.size, b.usage, kDeviceLocal, *b.buffer);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    // Native images
    const struct {
        vk_util::Image *image;
        vk_util::ImageDesc desc;
    } images[] = {
        {&m_spriteLayers, {.width = kMaxResH, .height = kMaxResV, .format = VK_FORMAT_R8G8B8A8_UINT,
                           .usage = kImageUsage, .arrayLayers = kSpriteLayerImageLayers}},
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
    // The background layer image and the composite at the requested internal resolution, else the fallback, else 1:
    // each candidate allocated directly.
    {
        std::vector<uint32_t> candidates{scale};
        if (fallbackScale > 1 && fallbackScale < scale) {
            candidates.push_back(fallbackScale);
        }
        if (scale > 1) {
            candidates.push_back(1);
        }
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        for (size_t i = 0; i < candidates.size(); ++i) {
            result = CreateScaledResources(candidates[i], m_bgLayers, m_composite);
            if (result == VK_SUCCESS) {
                m_scale = candidates[i];
                break;
            }
            if (i + 1 < candidates.size()) {
                Log("VDP2 internal resolution " + std::to_string(candidates[i]) + "x: allocating " +
                    std::to_string(ScaledResourceBytes(candidates[i]) >> 20) + " MiB failed (VkResult " +
                    std::to_string(static_cast<int>(result)) + "); trying " + std::to_string(candidates[i + 1]) + "x");
            }
        }
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    // The rescale pass: its own set (previous composite -> composite), written by SetScale.
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const Binding &b : kRescaleBindings) {
            bindings.push_back({b.binding, ToVk(b.kind), 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
        const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, kRescalePushConstantSize};
        const std::array<VkDescriptorPoolSize, 1> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}}};
        VkResult result = vk_util::CreateDescriptorSetLayout(m_device, bindings, m_rescaleSetLayout);
        if (result == VK_SUCCESS) {
            result = vk_util::CreatePipelineLayout(m_device, {&m_rescaleSetLayout, 1}, {&pushRange, 1},
                                                   m_rescalePipelineLayout);
        }
        if (result == VK_SUCCESS) {
            result = vk_util::CreateComputePipeline(m_device, shaders::kVdp2RescaleSpv, m_rescalePipelineLayout,
                                                    m_rescalePipeline);
        }
        if (result == VK_SUCCESS) {
            result = vk_util::CreateDescriptorPool(m_device, sizes, 1, m_rescalePool);
        }
        if (result == VK_SUCCESS) {
            result = vk_util::AllocateDescriptorSet(m_device, m_rescalePool, m_rescaleSetLayout, m_rescaleSet);
        }
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
        vk_util::DestroyBuffer(m_device, context.retiredComposite);
    }
    m_contexts.clear();
    vk_util::DestroyBuffer(m_device, m_rescaleSource.composite);
    vk.vkDestroyPipeline(dev, m_rescalePipeline, nullptr);
    vk.vkDestroyPipelineLayout(dev, m_rescalePipelineLayout, nullptr);
    vk.vkDestroyDescriptorPool(dev, m_rescalePool, nullptr); // frees the set
    vk.vkDestroyDescriptorSetLayout(dev, m_rescaleSetLayout, nullptr);
    m_rescalePipeline = VK_NULL_HANDLE;
    m_rescalePipelineLayout = VK_NULL_HANDLE;
    m_rescalePool = VK_NULL_HANDLE;
    m_rescaleSet = VK_NULL_HANDLE;
    m_rescaleSetLayout = VK_NULL_HANDLE;
    for (uint32_t p = 0; p < kPipelineCount; ++p) {
        vk.vkDestroyPipeline(dev, m_pipelines[p], nullptr);
        vk.vkDestroyPipelineLayout(dev, m_pipelineLayouts[p], nullptr);
        vk.vkDestroyDescriptorSetLayout(dev, m_setLayouts[p], nullptr);
        m_pipelines[p] = VK_NULL_HANDLE;
        m_pipelineLayouts[p] = VK_NULL_HANDLE;
        m_setLayouts[p] = VK_NULL_HANDLE;
    }
    for (vk_util::Image *image : {&m_bgLayers, &m_spriteLayers, &m_spriteAttrs, &m_rbgLineColor, &m_colorCalcWindow,
                                  &m_spriteLayersNx, &m_spriteAttrsNx}) {
        vk_util::DestroyImage(m_device, *image);
    }
    m_spriteSurfaceNx = VK_NULL_HANDLE;
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

void Vdp2VulkanBackend::Log(const std::string &text) const {
    if (m_log) {
        m_log(text);
    } else {
        std::fprintf(stderr, "[vulkan] %s\n", text.c_str());
    }
}

VkDeviceSize Vdp2VulkanBackend::ScaledResourceBytes(uint32_t n) {
    const VkDeviceSize layerBytes = VkDeviceSize{ScaledResH(n)} * ScaledResV(n) * 4 * kBGLayerImageLayers;
    return layerBytes + CompositeBufferSize(n);
}

VkResult Vdp2VulkanBackend::CreateScaledResources(uint32_t n, vk_util::Image &bgLayers, vk_util::Buffer &composite) {
    VkResult result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (m_scaleAllocationLimit == 0 || ScaledResourceBytes(n) <= m_scaleAllocationLimit) {
        result = vk_util::CreateImage2D(m_device, BGLayersDesc(n), bgLayers);
        if (result == VK_SUCCESS) {
            result = vk_util::CreateBuffer(m_device, CompositeBufferSize(n), kCompositeUsage,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, composite);
        }
    }
    if (result != VK_SUCCESS) {
        vk_util::DestroyImage(m_device, bgLayers);
        vk_util::DestroyBuffer(m_device, composite);
    }
    return result;
}

bool Vdp2VulkanBackend::SetScale(uint32_t n) {
    if (!Ok() || n < 1 || n > kMaxRenderScale) {
        return false;
    }
    if (n == m_scale) {
        return true;
    }
    // The N x sprite images belong to the old scale: freed first (the sets are rewritten below)
    DestroySpriteNx();
    // Nothing uses the old resources any more (the caller's guarantee). The old layers are freed first (the largest
    // allocation; a GPU short of memory can then still take a lower scale); the old composite stays for the rescale.
    vk_util::DestroyImage(m_device, m_bgLayers);
    vk_util::Image bgLayers;
    vk_util::Buffer composite;
    const VkResult result = CreateScaledResources(n, bgLayers, composite);
    if (result != VK_SUCCESS) {
        Log("VDP2 internal resolution " + std::to_string(n) + "x: allocating " +
            std::to_string(ScaledResourceBytes(n) >> 20) + " MiB failed (VkResult " +
            std::to_string(static_cast<int>(result)) + "); staying at " + std::to_string(m_scale) + "x");
        // The previous scale's layers again (they fitted before the attempt)
        if (const VkResult restored = vk_util::CreateImage2D(m_device, BGLayersDesc(m_scale), m_bgLayers);
            restored != VK_SUCCESS) {
            Log("VDP2 internal resolution " + std::to_string(m_scale) + "x: its layers could not be allocated again "
                "(VkResult " + std::to_string(static_cast<int>(restored)) + "); the back half is unusable");
            m_result = restored;
            return false;
        }
        WriteAllBlockSets();
        return false;
    }
    m_bgLayers = bgLayers;
    if (m_pending.rescale) {
        // The current composite was never written: keep rescaling from the composite of the last Record.
        vk_util::DestroyBuffer(m_device, m_composite);
    } else if (!m_pending.clearComposite && m_compositeWidth != 0 && m_compositeHeight != 0) {
        // The composite holds a frame (the other field when weaving): the next Record rescales it into the new one.
        m_rescaleSource = {m_composite, m_scale, m_compositeWidth, m_compositeHeight};
        m_pending.rescale = true;
    } else {
        // Nothing to keep: the new composite starts black, as a new back half's (a pending clear keeps its colour).
        vk_util::DestroyBuffer(m_device, m_composite);
        if (!m_pending.clearComposite) {
            m_pending.clearComposite = true;
            m_pending.clearColor = 0xFF000000u;
        }
    }
    m_composite = composite;
    m_scale = n;
    WriteAllBlockSets();
    if (m_pending.rescale) {
        const VkDescriptorBufferInfo infos[2] = {{m_rescaleSource.composite.buffer, 0, VK_WHOLE_SIZE},
                                                 {m_composite.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = m_rescaleSet;
            writes[i].dstBinding = kRescaleBindings[i].binding;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        m_device.Table().vkUpdateDescriptorSets(m_device.Device(), 2, writes, 0, nullptr);
    }
    return true;
}

void Vdp2VulkanBackend::WriteAllBlockSets() {
    for (FrameContext &context : m_contexts) {
        for (UploadBlock &block : context.blocks) {
            if (block.pool != VK_NULL_HANDLE) {
                WriteBlockSets(block);
            }
        }
    }
}

VkDeviceSize Vdp2VulkanBackend::SpriteNxImageBytes(uint32_t n) {
    return VkDeviceSize{ScaledResH(n)} * ScaledResV(n) * (4 * kSpriteLayerImageLayers + 4 * kSpriteAttrImageLayers);
}

void Vdp2VulkanBackend::DestroySpriteNx() {
    vk_util::DestroyImage(m_device, m_spriteLayersNx);
    vk_util::DestroyImage(m_device, m_spriteAttrsNx);
    m_spriteSurfaceNx = VK_NULL_HANDLE;
}

bool Vdp2VulkanBackend::SetSpriteSurfaceNx(VkBuffer surface) {
    if (!Ok()) {
        return false;
    }
    if (surface == m_spriteSurfaceNx) {
        return surface == VK_NULL_HANDLE || m_spriteLayersNx.image != VK_NULL_HANDLE;
    }
    // Free first: nothing uses the old images any more (the caller's guarantee)
    DestroySpriteNx();
    bool ok = surface == VK_NULL_HANDLE;
    if (surface != VK_NULL_HANDLE && m_scale > 1) {
        VkResult result = m_failSpriteNx ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
        if (result == VK_SUCCESS) {
            result = vk_util::CreateImage2D(m_device, SpriteLayersNxDesc(m_scale), m_spriteLayersNx);
        }
        if (result == VK_SUCCESS) {
            result = vk_util::CreateImage2D(m_device, SpriteAttrsNxDesc(m_scale), m_spriteAttrsNx);
        }
        if (result == VK_SUCCESS) {
            m_spriteSurfaceNx = surface;
            ok = true;
        } else {
            DestroySpriteNx();
            Log("VDP1 upscaling: the " + std::to_string(m_scale) + "x sprite layers (" +
                std::to_string(SpriteNxImageBytes(m_scale) >> 20) + " MiB) could not be allocated (VkResult " +
                std::to_string(static_cast<int>(result)) + ")");
        }
    }
    WriteAllBlockSets();
    return ok;
}

void Vdp2VulkanBackend::FrameDropped() {
    if (m_recorded.clearComposite && !m_pending.clearComposite) {
        m_pending.clearComposite = true;
        m_pending.clearColor = m_recorded.clearColor;
    }
    m_pending.zeroSpriteFB |= m_recorded.zeroSpriteFB && !m_spriteFBExternal;
    if (m_recorded.rescale && !m_pending.rescale && m_lastContext < m_contexts.size()) {
        // The rescale never ran: its source is still the previous composite (kept by the context until it records
        // again), and the descriptor set still points at it.
        FrameContext &context = m_contexts[m_lastContext];
        if (context.retiredComposite.buffer != VK_NULL_HANDLE) {
            m_rescaleSource.composite = context.retiredComposite;
            context.retiredComposite = {};
            m_pending.rescale = true;
        }
    }
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

VkDeviceSize Vdp2VulkanBackend::BlockSizeFor(VkDeviceSize bytes) const {
    return std::max<VkDeviceSize>(m_blockSize, AlignUp(bytes, 64 * 1024));
}

VkDeviceSize Vdp2VulkanBackend::BaseBlockSize(const FrameContext &context) const {
    return BlockSizeFor(context.highWater);
}

void Vdp2VulkanBackend::UpdateHighWater(FrameContext &context, VkDeviceSize bytes, bool overflowed) {
    if (overflowed) {
        // The next base block of this context holds a frame this big in one piece, up to the cap; after a high-water
        // block failed to allocate, never again.
        if (!m_highWaterFailed && bytes <= kMaxUploadBlockSize) {
            context.highWater = std::max(context.highWater, bytes);
        }
        context.smallRecords = 0;
        context.smallPeak = 0;
        return;
    }
    if (context.highWater == 0) {
        return;
    }
    const VkDeviceSize size = BlockSizeFor(bytes);
    if (size >= BaseBlockSize(context)) {
        context.smallRecords = 0;
        context.smallPeak = 0;
        return;
    }
    // Decay: after m_highWaterDecayRecords Records in a row that fit a smaller block, the largest of them sets it.
    context.smallPeak = std::max(context.smallPeak, bytes);
    if (++context.smallRecords >= m_highWaterDecayRecords) {
        context.highWater = context.smallPeak <= m_blockSize ? 0 : context.smallPeak;
        context.smallRecords = 0;
        context.smallPeak = 0;
    }
}

void Vdp2VulkanBackend::ResetContext(FrameContext &context) {
    // The context's previous frame has completed (the caller's fence): overflow blocks go, the base block is reused
    // unless its size no longer matches: the block size, or the context's high-water mark (the total of its last frame
    // that overflowed) when that is larger.
    vk_util::DestroyBuffer(m_device, context.retiredComposite);
    while (context.blocks.size() > 1) {
        DestroyBlock(context.blocks.back());
        context.blocks.pop_back();
    }
    const VkDeviceSize baseSize = BaseBlockSize(context);
    if (!context.blocks.empty() && context.blocks.front().buffer.size != baseSize) {
        DestroyBlock(context.blocks.front());
        context.blocks.clear();
    }
    if (!context.blocks.empty()) {
        context.blocks.front().used = 0;
    }
}

bool Vdp2VulkanBackend::AddBlock(FrameContext &context, VkDeviceSize size) {
    if (m_failHighWaterBlocks && context.blocks.empty() && size > m_blockSize) {
        return false;
    }
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
    if (context.blocks.empty() && !AddBlock(context, BaseBlockSize(context))) {
        // A high-water block that cannot be allocated falls back to the default size (and overflow blocks), for every
        // context from now on: logged once, never retried.
        if (BaseBlockSize(context) <= m_blockSize) {
            return false;
        }
        if (!m_highWaterFailed) {
            Log("VDP2 upload block of " + std::to_string(BaseBlockSize(context) >> 10) +
                " KiB could not be allocated; upload blocks stay at " + std::to_string(m_blockSize >> 10) +
                " KiB (larger frames chain overflow blocks)");
            m_highWaterFailed = true;
        }
        for (FrameContext &c : m_contexts) {
            c.highWater = 0;
            c.smallRecords = 0;
            c.smallPeak = 0;
        }
        if (!AddBlock(context, m_blockSize)) {
            return false;
        }
    }
    UploadBlock *block = &context.blocks.back();
    VkDeviceSize offset = AlignUp(block->used, alignment);
    if (offset + size > block->buffer.size) {
        // Overflow: chain a new block, never wait for the GPU. It is freed at the context's next Record.
        if (!AddBlock(context, std::max<VkDeviceSize>(m_blockSize, AlignUp(size, 64 * 1024)))) {
            return false;
        }
        ++m_overflowBlocks;
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
    // One set per pipeline, and the N x sprite pass's and compose's N x sets
    for (uint32_t p = 0; p < kPipelineCount; ++p) {
        for (const Binding &b : BindingTable(p)) {
            sizes[static_cast<size_t>(b.kind)].descriptorCount += (p == kSprite || p == kCompose) ? 2 : 1;
        }
    }
    static_assert(static_cast<size_t>(DescriptorKind::StorageBuffer) == 0 &&
                  static_cast<size_t>(DescriptorKind::StorageBufferDynamic) == 1 &&
                  static_cast<size_t>(DescriptorKind::SampledImage) == 2 &&
                  static_cast<size_t>(DescriptorKind::StorageImage) == 3);
    VkResult result = vk_util::CreateDescriptorPool(m_device, sizes, kPipelineCount + 2, block.pool);
    for (uint32_t p = 0; p < kPipelineCount && result == VK_SUCCESS; ++p) {
        result = vk_util::AllocateDescriptorSet(m_device, block.pool, m_setLayouts[p], block.sets[p]);
    }
    if (result == VK_SUCCESS) {
        result = vk_util::AllocateDescriptorSet(m_device, block.pool, m_setLayouts[kSprite], block.nxSpriteSet);
    }
    if (result == VK_SUCCESS) {
        result = vk_util::AllocateDescriptorSet(m_device, block.pool, m_setLayouts[kCompose], block.nxComposeSet);
    }
    if (result != VK_SUCCESS) {
        vk.vkDestroyDescriptorPool(dev, block.pool, nullptr);
        block.pool = VK_NULL_HANDLE;
        block.sets = {};
        block.nxSpriteSet = block.nxComposeSet = VK_NULL_HANDLE;
        return result;
    }
    WriteBlockSets(block);
    return VK_SUCCESS;
}

void Vdp2VulkanBackend::WriteBlockSets(UploadBlock &block) {
    const auto &vk = m_device.Table();
    const VkDevice dev = m_device.Device();
    // Sets 0 to kPipelineCount - 1: the pipelines'; then the N x sprite pass's and compose's N x sets, on the N x
    // sprite images and the N x surface (the native images and framebuffer while there are none).
    const bool haveNx = m_spriteSurfaceNx != VK_NULL_HANDLE;
    for (uint32_t s = 0; s < kPipelineCount + 2; ++s) {
        const bool nxSet = s >= kPipelineCount;
        const uint32_t p = s == kPipelineCount ? kSprite : s == kPipelineCount + 1 ? kCompose : s;
        const VkDescriptorSet set = s == kPipelineCount ? block.nxSpriteSet
                                    : nxSet             ? block.nxComposeSet
                                                        : block.sets[s];
        const bool nx = nxSet && haveNx;
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
            case kBindingSpriteFBNx: bufferInfo.buffer = nx ? m_spriteSurfaceNx : m_spriteFB.buffer; break;
            case kBindingSpriteAttrsIn:
            case kBindingSpriteAttrsOut: imageInfo.imageView = nx ? m_spriteAttrsNx.view : m_spriteAttrs.view; break;
            case kBindingLayerIn:
            case kBindingLayerOut: imageInfo.imageView = m_bgLayers.view; break;
            case kBindingSpriteLayerIn:
            case kBindingSpriteLayerOut: imageInfo.imageView = nx ? m_spriteLayersNx.view : m_spriteLayers.view; break;
            case kBindingRBGLineColorIn:
            case kBindingRBGLineColorOut: imageInfo.imageView = m_rbgLineColor.view; break;
            case kBindingColorCalcWindowIn:
            case kBindingColorCalcWindowOut: imageInfo.imageView = m_colorCalcWindow.view; break;
            default: break;
            }
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = set;
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
}

void Vdp2VulkanBackend::Barrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                                VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) const {
    vk_util::GlobalBarrier(m_device, cmd, srcStage, srcAccess, dstStage, dstAccess);
}

void Vdp2VulkanBackend::Dispatch(VkCommandBuffer cmd, Pipeline pipeline, const UploadBlock &block,
                                 uint32_t paramsOffset, const VDP2CommonRenderParams &common, uint32_t startY,
                                 uint32_t lines, uint32_t groupsX, uint32_t groupsZ, uint32_t outputSize,
                                 uint32_t scale, uint32_t spriteScale, VkDescriptorSet set) const {
    const auto &vk = m_device.Table();
    VDP2CommonRenderParams push = common;
    push.startY = startY;
    push.outputSize = outputSize;
    push.renderScale = scale;
    push.spriteScale = spriteScale;
    if (set == VK_NULL_HANDLE) {
        set = block.sets[pipeline];
    }
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelines[pipeline]);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayouts[pipeline], 0, 1, &set, 1,
                               &paramsOffset);
    vk.vkCmdPushConstants(cmd, m_pipelineLayouts[pipeline], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vk.vkCmdDispatch(cmd, groupsX, lines, groupsZ);
}

void Vdp2VulkanBackend::DispatchNBGFetch(VkCommandBuffer cmd, const UploadBlock &block, uint32_t paramsOffset,
                                         const VDP2CommonRenderParams &common, uint32_t startY, uint32_t lines,
                                         uint32_t outputSize) const {
    const auto &vk = m_device.Table();
    // The pass is native at every internal resolution: a scaled width would be clamped to its kMaxResH-pixel
    // groupshared rows by the shader without notice.
    assert((outputSize & 0xFFFFu) <= kMaxResH && (outputSize >> 16) <= kMaxResV);
    VDP2NBGFetchPushConstants push{};
    push.common = common;
    push.common.startY = startY;
    push.common.outputSize = outputSize;
    push.common.renderScale = 1;
    push.common.spriteScale = 0;
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
    constexpr VkDeviceSize kLinesBytes = kRotBasesBytes + kLnclBackBytes + kNBGLinesBytes;
    const VkDeviceSize arenaSize = std::max<VkDeviceSize>(frame.arena.size(), 4);
    m_overflowBlocks = 0;
    if (!Allocate(context, kLinesBytes, 16, lines) || !Allocate(context, arenaSize, m_paramAlignment, arena)) {
        return failed();
    }
    UpdateHighWater(context, AlignUp(kLinesBytes, m_paramAlignment) + arenaSize, m_overflowBlocks > 0);
    RecordStats stats{};
    stats.uploadBytes = kLinesBytes + frame.arena.size();
    stats.overflowBlocks = m_overflowBlocks;
    stats.baseBlockSize = context.blocks.front().buffer.size;
    stats.highWater = context.highWater;
    stats.scale = m_scale;
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
    if (m_profile != nullptr) {
        m_profile->Stamp(cmd, GpuPassProfile::kBoundary);
    }
    Barrier(cmd, kCompute | kTransfer, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, kTransfer,
            VK_ACCESS_TRANSFER_WRITE_BIT);

    // Pending one-time work: the composite clear, and zeroing the sprite framebuffer of a new back half so slots that
    // are never uploaded read zeros. Recorded here, it stays recorded unless the caller reports the command buffer
    // dropped (FrameDropped).
    m_recorded = m_pending;
    if (m_pending.rescale) {
        // First the previous composite into the new one (a woven field survives a scale change), so the clears below
        // (pending, or the frame's own) and the frame's compose writes come after it. The previous composite stays
        // alive until this context records again.
        const RescaleSource &source = m_rescaleSource;
        const VDP2RescalePushConstants push{source.width, source.height, source.scale, m_scale};
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_rescalePipeline);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_rescalePipelineLayout, 0, 1, &m_rescaleSet, 0,
                                   nullptr);
        vk.vkCmdPushConstants(cmd, m_rescalePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vk.vkCmdDispatch(cmd, (source.width * m_scale + kWorkgroupWidth - 1) / kWorkgroupWidth, source.height * m_scale,
                         1);
        Barrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute | kTransfer,
                kShaderRW | VK_ACCESS_TRANSFER_WRITE_BIT);
        context.retiredComposite = m_rescaleSource.composite;
        m_rescaleSource.composite = {};
        m_compositeWidth = source.width;
        m_compositeHeight = source.height;
    }
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
    // VDP1 upscaling: the N x sprite layers of this frame (none without an N x surface)
    const bool nxSprites = m_spriteSurfaceNx != VK_NULL_HANDLE && m_scale > 1;
    for (const vk_util::Image *image : {&m_bgLayers, &m_spriteLayers, &m_spriteAttrs, &m_rbgLineColor,
                                        &m_colorCalcWindow, &m_spriteLayersNx, &m_spriteAttrsNx}) {
        if (image->image == VK_NULL_HANDLE) {
            continue;
        }
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
    // first; later compose writes (WAW) and the frame's readers come after. A clear of `words` native words (whole
    // native rows, pitch HRes) is N^2 times that at internal resolution N (the same rows, pitch N * HRes).
    const uint32_t scale = m_scale;
    size_t nextClear = 0;
    const auto clearsBefore = [&](size_t batchIndex) {
        for (; nextClear < frame.clears.size() && frame.clears[nextClear].beforeBatch <= batchIndex; ++nextClear) {
            const Vdp2CompositeClear &clear = frame.clears[nextClear];
            const VkDeviceSize bytes =
                clear.words == 0 ? VK_WHOLE_SIZE
                                 : std::min<VkDeviceSize>(VkDeviceSize{clear.words} * scale * scale * sizeof(uint32_t),
                                                          m_composite.size);
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
        // outputSize is native at every internal resolution; bgs and compose cover N x N threads per native pixel.
        const uint32_t outputSize = MakeOutputSize(hres, vres);
        const uint32_t groupsX = (hres + kWorkgroupWidth - 1) / kWorkgroupWidth;
        const uint32_t scaledGroupsX = (hres * scale + kWorkgroupWidth - 1) / kWorkgroupWidth;
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
                (target == 0 ? stats.vramCopyBytes : stats.cramCopyBytes) += copy.size;
            }
            flushRegions();
            Barrier(cmd, kTransfer, VK_ACCESS_TRANSFER_WRITE_BIT, kCompute, VK_ACCESS_SHADER_READ_BIT);
        }
        if (batch.layerLines > 0) {
            const uint32_t layerOffset = static_cast<uint32_t>(arenaBase) + batch.layerParamsOffset;
            const uint32_t spriteLayers = batch.common.enhancements.transparentMeshes ? 2 : 1;
            if (m_profile != nullptr) {
                m_profile->Stamp(cmd, GpuPassProfile::kVdp2Other);
            }
            Dispatch(cmd, kSprite, arenaBlock, layerOffset, batch.common, batch.layerStartY, batch.layerLines,
                     groupsX, spriteLayers, outputSize, 1);
            if (nxSprites) {
                if (m_profile != nullptr) {
                    // Diagnostics only: the two sprite passes one after the other, each timed
                    Barrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute, kShaderRW);
                    m_profile->Stamp(cmd, GpuPassProfile::kVdp2Sprite);
                }
                // VDP1 upscaling: the N x sprite pass (the native one above still feeds the sprite windows)
                Dispatch(cmd, kSprite, arenaBlock, layerOffset, batch.common, batch.layerStartY,
                         batch.layerLines * scale, scaledGroupsX, spriteLayers, outputSize, scale, scale,
                         arenaBlock.nxSpriteSet);
                ++stats.nxSpritePasses;
            }
            Barrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute, kShaderRW);
            if (m_profile != nullptr) {
                m_profile->Stamp(cmd, nxSprites ? GpuPassProfile::kVdp2SpriteNx : GpuPassProfile::kVdp2Sprite);
            }
            if (batch.needsFetchPass) {
                // After the sprite pass (the sprite window), before bgs (the character table).
                DispatchNBGFetch(cmd, arenaBlock, layerOffset, batch.common, batch.layerStartY, batch.layerLines,
                                 outputSize);
                Barrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute, kShaderRW);
                ++stats.fetchPasses;
                stats.fetchOutputSize = outputSize;
            }
            // The dispatch rows: the y of every row is below 65536 (512 lines x 4).
            Dispatch(cmd, kBgs, arenaBlock, layerOffset, batch.common, batch.layerStartY, batch.layerLines * scale,
                     scaledGroupsX, kBgsDispatchZ, outputSize, scale);
            Barrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute, kShaderRW);
        }
        if (batch.composeLines > 0) {
            const uint32_t composeOffset = static_cast<uint32_t>(arenaBase) + batch.composeParamsOffset;
            Dispatch(cmd, kCompose, arenaBlock, composeOffset, batch.common, batch.composeStartY,
                     batch.composeLines * scale, scaledGroupsX, 1, outputSize, scale, nxSprites ? scale : 0,
                     nxSprites ? arenaBlock.nxComposeSet : VK_NULL_HANDLE);
            stats.nxComposes += nxSprites ? 1 : 0;
            Barrier(cmd, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute, kShaderRW);
            stats.composeOutputSize = outputSize;
        }
    }
    clearsBefore(frame.batches.size());
    if (!frame.batches.empty()) {
        m_compositeWidth = std::min(frame.batches.back().hres, kMaxResH);
        m_compositeHeight = std::min(frame.batches.back().vres, kMaxResV);
    }
    m_lastContext = contextIndex;
    if (!frame.clears.empty()) {
        // A dropped command buffer re-arms the frame's last clear (as a whole-composite clear at the next Record).
        m_recorded.clearComposite = true;
        m_recorded.clearColor = frame.clears.back().color;
    }

    // The composite is complete: readable by later compute (the presenter) and transfer (readbacks) commands.
    Barrier(cmd, kCompute | kTransfer, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, kCompute | kTransfer,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    if (m_profile != nullptr) {
        m_profile->Stamp(cmd, GpuPassProfile::kVdp2Other);
    }
    m_lastStats = stats;
    return true;
}

} // namespace brimir::gpu::vdp2
