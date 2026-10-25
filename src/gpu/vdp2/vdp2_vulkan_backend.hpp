#pragma once

// Vulkan back half of the GPU VDP2 renderer: owns the VDP2 resources on the device (stage1b-port-inventory.md section
// 2.1) and replays a Vdp2FrameRecord from the front half (vdp2_front.hpp) into a command buffer with the three VDP2
// compute pipelines (section 2.4). The frame lands in the persistent composite buffer (packed XBGR, pitch HRes), which
// FramePresenter::PresentDevice reads.
//
// Uploads go through frame contexts: each context owns a host-visible upload block (4 MiB) plus overflow blocks chained
// on demand; all of a context's blocks are reused (overflow blocks freed) the next time the context records. The caller
// guarantees that the GPU work of the context's previous Record has finished by then (its slot fence). Record never
// waits.

#include "gpu/vdp2/vdp2_front.hpp"
#include "gpu/vk_device.hpp"
#include "gpu/vk_util.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace brimir::gpu::vdp2 {

class Vdp2VulkanBackend {
public:
    static constexpr size_t kDefaultUploadBlockSize = 4 * 1024 * 1024;

    // Creates every resource and pipeline. Check Ok() afterwards.
    Vdp2VulkanBackend(VulkanDevice &device, uint32_t frameContexts);
    // Destroys everything immediately: the GPU must be done with all of it (e.g. after vkDeviceWaitIdle).
    ~Vdp2VulkanBackend();
    Vdp2VulkanBackend(const Vdp2VulkanBackend &) = delete;
    Vdp2VulkanBackend &operator=(const Vdp2VulkanBackend &) = delete;

    bool Ok() const { return m_result == VK_SUCCESS; }
    VkResult Result() const { return m_result; }
    uint32_t FrameContexts() const { return static_cast<uint32_t>(m_contexts.size()); }

    // Copies the frame's snapshots into the context's upload blocks and records its uploads, dispatches and barriers
    // into cmd. Afterwards the composite buffer holds the frame (HRes x VRes, pitch HRes) and its writes are visible to
    // compute shader and transfer reads recorded later. The context's previous Record must have completed on the GPU.
    // Returns false and records nothing when the backend failed to initialize, context is out of range or an upload
    // block cannot be allocated (the caller then re-uploads everything with the next frame).
    bool Record(const Vdp2FrameRecord &frame, uint32_t context, VkCommandBuffer cmd);

    // The persistent composite: uint[704 * 512] of packed XBGR (0xAABBGGRR), STORAGE | TRANSFER_SRC | TRANSFER_DST.
    VkBuffer CompositeBuffer() const { return m_composite.buffer; }

    // Size of the base upload block of every context (reallocated at each context's next Record).
    void ForceUploadBlockSizeForTest(size_t bytes);
    // Upload blocks the context used for its last Record (1 = no overflow).
    uint32_t UploadBlocksForTest(uint32_t context) const;

private:
    enum Pipeline : uint32_t { kSprite, kBgs, kCompose, kPipelineCount };

    struct UploadBlock {
        vk_util::Buffer buffer; // host-visible, coherent, persistently mapped
        VkDeviceSize used = 0;
        VkDescriptorPool pool = VK_NULL_HANDLE;              // created with the sets, on the first arena placement
        std::array<VkDescriptorSet, kPipelineCount> sets{}; // parameter bindings point into this block
    };
    struct FrameContext {
        std::vector<UploadBlock> blocks;
    };
    struct Allocation {
        size_t block = 0; // index into FrameContext::blocks
        VkDeviceSize offset = 0;
    };

    VkResult Init(uint32_t frameContexts);
    void Destroy();
    void DestroyBlock(UploadBlock &block);
    void ResetContext(FrameContext &context);
    bool AddBlock(FrameContext &context, VkDeviceSize size);
    bool Allocate(FrameContext &context, VkDeviceSize size, VkDeviceSize alignment, Allocation &out);
    VkResult CreateBlockSets(UploadBlock &block);
    void Barrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                 VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) const;
    void Dispatch(VkCommandBuffer cmd, Pipeline pipeline, const UploadBlock &block, uint32_t paramsOffset,
                  const VDP2CommonRenderParams &common, uint32_t startY, uint32_t lines, uint32_t groupsX,
                  uint32_t groupsZ, uint32_t outputSize) const;

    VulkanDevice &m_device;
    VkResult m_result = VK_ERROR_INITIALIZATION_FAILED;
    VkDeviceSize m_paramAlignment = 256;
    size_t m_blockSize = kDefaultUploadBlockSize;

    // Device-local resources (bindings in vdp2_bindings.hpp)
    vk_util::Buffer m_vram;
    vk_util::Buffer m_cramColors;
    vk_util::Buffer m_cramCoeffs;
    vk_util::Buffer m_rotBases;
    vk_util::Buffer m_lnclBack;
    vk_util::Buffer m_nbgLines;
    vk_util::Buffer m_spriteFB;
    vk_util::Buffer m_composite;
    vk_util::Image m_layers;        // R8G8B8A8_UINT 704x512, 8 layers
    vk_util::Image m_spriteAttrs;   // R32_UINT 704x512, 2 layers
    vk_util::Image m_rbgLineColor;  // R8G8B8A8_UINT 352x512, 2 layers
    vk_util::Image m_colorCalcWindow; // R32_UINT 704x512
    bool m_compositeInitialized = false;

    std::array<VkDescriptorSetLayout, kPipelineCount> m_setLayouts{};
    std::array<VkPipelineLayout, kPipelineCount> m_pipelineLayouts{};
    std::array<VkPipeline, kPipelineCount> m_pipelines{};

    std::vector<FrameContext> m_contexts;
};

} // namespace brimir::gpu::vdp2
