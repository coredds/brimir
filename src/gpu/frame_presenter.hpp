#pragma once

// The GPU frame output pass: takes a native Ymir frame (XBGR8888, 0xAABBGGRR, pitch = width) and produces the
// frame the libretro layer presents, in an R8G8B8A8_UNORM image per sync slot whose extent is exactly the output size
// (a frontend samples the whole image it gets from set_image as the frame video_cb reports). The transforms are the
// bridge's (CoreWrapper::OnFrameComplete): XBGR -> RGB, overscan crop, then rotation. At 1x the RGB channels equal the
// software path's XRGB8888 output byte for byte; alpha is 0xFF (the software path leaves 0x00). The shader stores
// the packed words through an R32_UINT view of each image, so no float conversion is involved. At internal resolution
// N (device frames only) the same transforms apply to the N x frame: crop decided natively, offsets N * (crop / 2).

#include "vk_device.hpp"
#include "vk_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace brimir::gpu {

struct OutputSettings {
    // Total overscan crop per axis (left/top get half, rounded down), as CoreWrapper::SetOverscanCrop. The bridge's
    // crops are int: values above INT_MAX never apply (like any crop the frame is too small for).
    uint32_t cropH = 0, cropV = 0;
    // 0, 90 (clockwise), 180 or 270 (counter-clockwise); anything else is 0.
    uint32_t rotation = 0;
    // Internal resolution N of the source (PresentDevice: an N x frame of the native w x h, pitch N * w): the output
    // is N x the native output. The crop guard is decided on the native size and the crop offsets are N * (crop / 2).
    // 0 is 1.
    uint32_t scale = 1;
};

struct OutputSize {
    uint32_t width = 0, height = 0;
    bool operator==(const OutputSize &) const = default;
};

// The bridge's output size for a w x h native frame, times settings.scale. The crop applies only when the native frame
// exceeds it by more than 32 px on both axes; otherwise the frame passes through uncropped. 90/270 swap the axes.
OutputSize ComputeOutputSize(uint32_t w, uint32_t h, const OutputSettings &settings);

// The size-dependent half of the frame output: one R8G8B8A8_UNORM image per sync slot of exactly `extent` (with the
// R32_UINT view the shader writes through), for frames at internal resolution `scale`. Written by a PresenterCore of
// the same slot count; any number of them can share one core (the libretro output keeps a few extents alive while it
// changes size).
class OutputImages {
public:
    // Creates slotCount images of exactly `extent`. extent must be non-zero and at most 704 * scale per axis, scale
    // 1 to 4. Check Ok() afterwards.
    OutputImages(VulkanDevice &device, uint32_t slotCount, OutputSize extent, uint32_t scale = 1);
    // Destroys the images immediately: the GPU and the frontend must be done with them.
    ~OutputImages();
    OutputImages(const OutputImages &) = delete;
    OutputImages &operator=(const OutputImages &) = delete;

    bool Ok() const { return m_result == VK_SUCCESS; }
    // VK_SUCCESS, or the result of the creation step that failed.
    VkResult Result() const { return m_result; }
    uint32_t SlotCount() const { return static_cast<uint32_t>(m_slots.size()); }
    // The internal resolution of the frames the images are sized for.
    uint32_t Scale() const { return m_scale; }
    OutputSize Extent() const { return m_extent; }
    // The images' pixel bytes (4 per pixel, every slot): what keeping them alive costs, roughly.
    uint64_t Bytes() const;

    // The slot's image and its R8G8B8A8_UNORM view (for retro_vulkan_image); null when slot is out of range.
    VkImage Image(uint32_t slot) const;
    VkImageView View(uint32_t slot) const;

    // Test helper: reads the top-left size pixels (up to Extent()) of the slot's image back as XRGB8888 (0x00RRGGBB,
    // tightly packed) in a one-shot submit. Call after the Present for the slot has completed; empty on failure.
    std::vector<uint32_t> ReadbackXRGB(uint32_t slot, OutputSize size);

private:
    friend class PresenterCore;
    struct Slot {
        vk_util::Image output; // output.view: R8G8B8A8_UNORM, for the frontend
        VkImageView storageView = VK_NULL_HANDLE; // R32_UINT, written by the shader
        bool presented = false; // a Present was recorded (ReadbackXRGB's precondition only)
    };

    VkResult Init(uint32_t slotCount);
    void Destroy();

    VulkanDevice &m_device;
    VkResult m_result = VK_ERROR_INITIALIZATION_FAILED;
    uint32_t m_scale = 1;
    OutputSize m_extent{};
    // Never reused (unlike a destroyed view's handle value): names these images in the cores' descriptor sets.
    uint64_t m_serial = 0;
    std::vector<Slot> m_slots;
    vk_util::Buffer m_readback; // ReadbackXRGB only
};

// The size-independent half of the frame output: the conversion pipeline, its layouts and descriptor pool, and per
// sync slot the upload buffers (sized for the largest native frame) and two descriptor sets. The output image binding
// of a slot's set is written when the slot is recorded with other images than last time, so one core serves output
// images of any extent (a size change creates images only, never a pipeline).
class PresenterCore {
public:
    // Creates the pipeline and slotCount slots. Check Ok() afterwards.
    PresenterCore(VulkanDevice &device, uint32_t slotCount);
    // Destroys everything immediately: the GPU must be done with every slot first.
    ~PresenterCore();
    PresenterCore(const PresenterCore &) = delete;
    PresenterCore &operator=(const PresenterCore &) = delete;

    bool Ok() const { return m_result == VK_SUCCESS; }
    VkResult Result() const { return m_result; }
    uint32_t SlotCount() const { return static_cast<uint32_t>(m_slots.size()); }

    // FramePresenter::Present into `images` (same slot count; Extent() the frame's output size, Scale() 1). The slot's
    // previous frame must no longer be in use by the GPU (its descriptor sets may be rewritten), and `images`' slot
    // image no longer by the frontend.
    bool Present(uint32_t slot, OutputImages &images, const uint32_t *xbgr, uint32_t w, uint32_t h,
                 const OutputSettings &settings, VkCommandBuffer cmd);
    // FramePresenter::PresentDevice into `images` (settings.scale must be images.Scale()), with the same preconditions.
    bool PresentDevice(uint32_t slot, OutputImages &images, VkBuffer source, uint32_t w, uint32_t h,
                       const OutputSettings &settings, VkCommandBuffer cmd);

private:
    struct Slot {
        vk_util::Buffer upload; // host-visible staging
        vk_util::Buffer source; // device-local, read by the shader
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSet deviceSet = VK_NULL_HANDLE; // PresentDevice: binding 0 rewritten on every call
        // The OutputImages (serial) whose slot image each set's output binding holds; 0: none yet
        uint64_t setImages = 0;
        uint64_t deviceSetImages = 0;
    };

    VkResult Init(uint32_t slotCount);
    void Destroy();
    // Whether `images` fit the frame (slot count, extent, scale) and the slot is in range.
    bool Accepts(uint32_t slot, const OutputImages &images, uint32_t w, uint32_t h,
                 const OutputSettings &settings) const;
    // Points the set's output binding at the slot's image of `images` (when it does not already).
    void BindOutput(VkDescriptorSet set, uint64_t &bound, OutputImages &images, uint32_t slot);
    // The image transition, conversion dispatch and final barrier shared by Present and PresentDevice.
    void RecordConversion(OutputImages::Slot &image, VkDescriptorSet set, uint32_t w, uint32_t h,
                          const OutputSettings &settings, VkCommandBuffer cmd);

    VulkanDevice &m_device;
    VkResult m_result = VK_ERROR_INITIALIZATION_FAILED;
    std::vector<Slot> m_slots;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

// A PresenterCore with one set of OutputImages: the frame output for one extent.
class FramePresenter {
public:
    // Largest native frame (VDP2 hi-res interlaced) and largest native output size per axis (704 either way after a
    // 90/270 rotation).
    static constexpr uint32_t kMaxSourceWidth = 704;
    static constexpr uint32_t kMaxSourceHeight = 512;
    static constexpr uint32_t kMaxOutputSize = 704;
    // Largest internal resolution: outputs up to 704 * 4 = 2816 px per axis.
    static constexpr uint32_t kMaxScale = 4;

    // Creates slotCount output images of exactly `extent` and upload buffers: the presenter presents frames whose
    // output size (ComputeOutputSize) is `extent`, at internal resolution `scale` (uploads, Present, are native: scale
    // 1). extent must be non-zero and at most 704 * scale per axis. Check Ok() afterwards.
    FramePresenter(VulkanDevice &device, uint32_t slotCount, OutputSize extent, uint32_t scale = 1);
    // Destroys everything immediately: the GPU (and the frontend) must be done with every slot first, e.g. after
    // vkDeviceWaitIdle in context_destroy.
    ~FramePresenter() = default;
    FramePresenter(const FramePresenter &) = delete;
    FramePresenter &operator=(const FramePresenter &) = delete;

    bool Ok() const { return m_core.Ok() && m_images.Ok(); }
    // VK_SUCCESS, or the result of the creation step that failed.
    VkResult Result() const { return m_core.Ok() ? m_images.Result() : m_core.Result(); }
    uint32_t SlotCount() const { return Ok() ? m_core.SlotCount() : 0; }
    // The internal resolution of the frames the images are sized for.
    uint32_t Scale() const { return m_images.Scale(); }
    // The output images' extent.
    OutputSize Extent() const { return m_images.Extent(); }

    // Copies the frame into the slot's upload buffer now and records the upload copy, the conversion dispatch and
    // the barriers into cmd. The slot's previous frame must no longer be in use (GPU or frontend). Afterwards the
    // image is in VK_IMAGE_LAYOUT_GENERAL with the compute writes available to any later read, holding the frame.
    // Returns false and records nothing when the presenter failed to initialize, slot is out of range, xbgr is null,
    // w x h is zero or exceeds 704 x 512, settings.scale is not 1 (uploads are native), or the output size
    // ComputeOutputSize(w, h, settings) is not Extent().
    bool Present(uint32_t slot, const uint32_t *xbgr, uint32_t w, uint32_t h, const OutputSettings &settings,
                 VkCommandBuffer cmd) {
        return Ok() && m_core.Present(slot, m_images, xbgr, w, h, settings, cmd);
    }

    // As Present, but the frame is already on the device: the shader reads `source` (a storage buffer, e.g. the VDP2
    // composite: XBGR8888, the N x frame of the native w x h at N = settings.scale, pitch N * w) through the slot's
    // second descriptor set, with no upload. Writes to `source` recorded earlier in cmd or in earlier submissions are
    // made visible here. Also false when settings.scale is not Scale() (the output size must be Extent()).
    bool PresentDevice(uint32_t slot, VkBuffer source, uint32_t w, uint32_t h, const OutputSettings &settings,
                       VkCommandBuffer cmd) {
        return Ok() && m_core.PresentDevice(slot, m_images, source, w, h, settings, cmd);
    }

    // The slot's image and its R8G8B8A8_UNORM view (for retro_vulkan_image); null when slot is out of range.
    VkImage OutputImage(uint32_t slot) const { return m_images.Image(slot); }
    VkImageView OutputView(uint32_t slot) const { return m_images.View(slot); }

    // Test helper: OutputImages::ReadbackXRGB.
    std::vector<uint32_t> ReadbackXRGB(uint32_t slot, OutputSize size) {
        return Ok() ? m_images.ReadbackXRGB(slot, size) : std::vector<uint32_t>{};
    }

private:
    PresenterCore m_core;
    OutputImages m_images;
};

// Retry backoff for output images that could not be created: after a failure of (extent, scale) the identical request
// is refused without trying for 1, 2, 4 ... kMaxDelay frames (doubling per failure), until another extent is requested
// at that scale or a creation succeeds. Each scale keeps its own last failure, so a failed size change and the
// fallback to lower scales after it (each failing too) are all backed off, instead of each failure making the others
// fresh again every frame. Frames are the caller's counter (retro_run calls).
class OutputRetryBackoff {
public:
    static constexpr uint64_t kMaxDelay = 120;
    static constexpr uint32_t kScales = 8; // scales 1 .. kScales - 1 have their own entry; larger ones share the last

    // Whether creating images of (extent, scale) may be tried at `frame` (counted as backed off when not).
    bool MayTry(OutputSize extent, uint32_t scale, uint64_t frame);
    // The same without counting (a caller's check before work that only pays off with a try).
    bool WouldTry(OutputSize extent, uint32_t scale, uint64_t frame) const;
    // A creation of (extent, scale) failed at `frame`. True when it is a new failure (another request than the last
    // failed one at that scale): the caller logs it, once.
    bool Failed(OutputSize extent, uint32_t scale, uint64_t frame);
    // A creation succeeded: the backoff is forgotten (at every scale).
    void Succeeded() { m_entries = {}; }

    uint64_t Failures() const { return m_failures; }
    uint64_t BackedOff() const { return m_backedOff; }

private:
    struct Entry {
        bool active = false;
        OutputSize extent{};
        uint64_t delay = 0;
        uint64_t retryAt = 0;
    };
    Entry &At(uint32_t scale) { return m_entries[std::min(scale, kScales - 1)]; }
    const Entry &At(uint32_t scale) const { return m_entries[std::min(scale, kScales - 1)]; }
    std::array<Entry, kScales> m_entries{};
    uint64_t m_failures = 0;
    uint64_t m_backedOff = 0;
};

// When to try the requested internal resolution again after the output ran below it (a fallback on a failed size
// change, a sync mask growth or an attach): on the next native size change, or kFirstDelay frames after the downgrade
// (then double that after each attempt), at most kMaxAttempts times until the request changes (Reset).
class ScaleRestoreSchedule {
public:
    static constexpr uint64_t kFirstDelay = 600;
    static constexpr uint32_t kMaxAttempts = 3;

    // Call every frame with whether the output is below the request: starts the schedule at a downgrade, ends it when
    // the request is met. True when an attempt should be made now (counted).
    bool Update(bool degraded, bool sizeChanged, uint64_t frame);
    // The request changed (an option change): attempts start over.
    void Reset();

    bool Degraded() const { return m_degraded; }
    uint32_t AttemptsThisRequest() const { return m_attempts; }
    uint64_t Attempts() const { return m_totalAttempts; }

private:
    bool m_degraded = false;
    uint32_t m_attempts = 0;
    uint64_t m_nextAt = 0;
    uint64_t m_totalAttempts = 0;
};

} // namespace brimir::gpu
