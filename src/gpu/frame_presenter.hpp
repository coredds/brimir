#pragma once

// The GPU frame output pass: takes a native Ymir frame (XBGR8888, 0xAABBGGRR, pitch = width) and produces the
// frame the libretro layer presents, in an R8G8B8A8_UNORM image per sync slot. The transforms are the bridge's
// (CoreWrapper::OnFrameComplete): XBGR -> RGB, overscan crop, then rotation. At 1x the RGB channels equal the
// software path's XRGB8888 output byte for byte; alpha is written as 1.0 (the software path leaves 0x00).

#include "vk_device.hpp"
#include "vk_util.hpp"

#include <cstdint>
#include <vector>

namespace brimir::gpu {

struct OutputSettings {
    // Total overscan crop per axis (left/top get half, rounded down), as CoreWrapper::SetOverscanCrop.
    uint32_t cropH = 0, cropV = 0;
    // 0, 90 (clockwise), 180 or 270 (counter-clockwise); anything else is 0.
    uint32_t rotation = 0;
};

struct OutputSize {
    uint32_t width = 0, height = 0;
    bool operator==(const OutputSize &) const = default;
};

// The bridge's output size for a w x h native frame. The crop applies only when the frame exceeds it by more than
// 32 px on both axes; otherwise the frame passes through uncropped. 90/270 swap the axes.
OutputSize ComputeOutputSize(uint32_t w, uint32_t h, const OutputSettings &settings);

class FramePresenter {
public:
    // Largest native frame (VDP2 hi-res interlaced) and output image size (square so 90/270 fit).
    static constexpr uint32_t kMaxSourceWidth = 704;
    static constexpr uint32_t kMaxSourceHeight = 512;
    static constexpr uint32_t kMaxOutputSize = 704;

    // Creates slotCount output images and upload buffers. Check Ok() afterwards.
    FramePresenter(VulkanDevice &device, uint32_t slotCount);
    ~FramePresenter();
    FramePresenter(const FramePresenter &) = delete;
    FramePresenter &operator=(const FramePresenter &) = delete;

    bool Ok() const { return m_result == VK_SUCCESS; }
    // VK_SUCCESS, or the result of the creation step that failed.
    VkResult Result() const { return m_result; }
    uint32_t SlotCount() const { return static_cast<uint32_t>(m_slots.size()); }

    // Copies the frame into the slot's upload buffer now and records the upload copy, the conversion dispatch and
    // the barriers into cmd. The slot's previous frame must no longer be in use (GPU or frontend). Afterwards the
    // image is in VK_IMAGE_LAYOUT_GENERAL with the compute writes available to any later read; its top-left
    // ComputeOutputSize(w, h, settings) pixels hold the frame. Returns false and records nothing when the
    // presenter failed to initialize, slot is out of range, xbgr is null or w x h is zero or exceeds 704 x 512.
    bool Present(uint32_t slot, const uint32_t *xbgr, uint32_t w, uint32_t h, const OutputSettings &settings,
                 VkCommandBuffer cmd);

    VkImage OutputImage(uint32_t slot) const;
    VkImageView OutputView(uint32_t slot) const;

    // Test/vkhost helper: reads the top-left size pixels of the slot's image back as XRGB8888 (0x00RRGGBB, tightly
    // packed) in a one-shot submit. Call after the Present for the slot has completed; empty on failure.
    std::vector<uint32_t> ReadbackXRGB(uint32_t slot, OutputSize size);

private:
    struct Slot {
        vk_util::Buffer upload; // host-visible staging
        vk_util::Buffer source; // device-local, read by the shader
        vk_util::Image output;
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool written = false; // the image has left VK_IMAGE_LAYOUT_UNDEFINED
    };

    VkResult Init(uint32_t slotCount);
    void Destroy();

    VulkanDevice &m_device;
    VkResult m_result = VK_ERROR_INITIALIZATION_FAILED;
    std::vector<Slot> m_slots;
    vk_util::Buffer m_readback; // ReadbackXRGB only
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

} // namespace brimir::gpu
