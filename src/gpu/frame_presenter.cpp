#include "frame_presenter.hpp"

#include "shaders/generated/cs_frame_output.spv.h"
#include "vdp2/vdp2_bindings.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace brimir::gpu {

namespace {

constexpr VkFormat kOutputFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kStorageViewFormat = VK_FORMAT_R32_UINT;
constexpr uint32_t kSourceBinding = 0;  // t0
constexpr uint32_t kOutputBinding = 16; // u0
constexpr uint32_t kGroupSize = vdp2::kFrameOutputGroupSize; // cs_frame_output.hlsl numthreads
constexpr VkDeviceSize kSourceBytes =
    VkDeviceSize{FramePresenter::kMaxSourceWidth} * FramePresenter::kMaxSourceHeight * sizeof(uint32_t);

// cs_frame_output.hlsl Params.
struct PushConstants {
    uint32_t srcWidth;
    uint32_t cropLeft;
    uint32_t cropTop;
    uint32_t cropWidth;
    uint32_t outWidth;
    uint32_t outHeight;
    uint32_t rotation;
};

uint32_t EffectiveRotation(uint32_t rotation) {
    return (rotation == 90 || rotation == 180 || rotation == 270) ? rotation : 0;
}

// The bridge's crop guard: crop only when the frame exceeds the crop by more than 32 px on both axes. The bridge's
// crops are int; larger values never apply.
bool CropApplies(uint32_t w, uint32_t h, const OutputSettings &settings) {
    if (settings.cropH == 0 && settings.cropV == 0) {
        return false;
    }
    if (settings.cropH > uint32_t{INT32_MAX} || settings.cropV > uint32_t{INT32_MAX}) {
        return false;
    }
    return uint64_t{w} > uint64_t{settings.cropH} + 32 && uint64_t{h} > uint64_t{settings.cropV} + 32;
}

} // namespace

OutputSize ComputeOutputSize(uint32_t w, uint32_t h, const OutputSettings &settings) {
    OutputSize size{w, h};
    if (CropApplies(w, h, settings)) {
        size = {w - settings.cropH, h - settings.cropV};
    }
    const uint32_t rotation = EffectiveRotation(settings.rotation);
    if (rotation == 90 || rotation == 270) {
        size = {size.height, size.width};
    }
    const uint32_t scale = std::max<uint32_t>(settings.scale, 1);
    return {size.width * scale, size.height * scale};
}


// ---------------------------------------------------------------------------------------------------------------
// OutputImages

namespace {
std::atomic<uint64_t> g_nextImagesSerial{1};
} // namespace

OutputImages::OutputImages(VulkanDevice &device, uint32_t slotCount, OutputSize extent, uint32_t scale)
    : m_device(device), m_scale(scale), m_extent(extent), m_serial(g_nextImagesSerial.fetch_add(1)) {
    const uint32_t maxOutput = FramePresenter::kMaxOutputSize;
    const bool valid = scale >= 1 && scale <= FramePresenter::kMaxScale && extent.width != 0 && extent.height != 0 &&
                       extent.width <= maxOutput * scale && extent.height <= maxOutput * scale;
    m_result = valid ? Init(slotCount) : VK_ERROR_INITIALIZATION_FAILED;
    if (m_result != VK_SUCCESS) {
        Destroy();
    }
}

OutputImages::~OutputImages() {
    Destroy();
}

uint64_t OutputImages::Bytes() const {
    return uint64_t{m_extent.width} * m_extent.height * sizeof(uint32_t) * m_slots.size();
}

VkResult OutputImages::Init(uint32_t slotCount) {
    if (slotCount == 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    // libretro output images: MUTABLE_FORMAT, SAMPLED | TRANSFER_SRC | STORAGE (stage1-libretro-vulkan.md), plus
    // TRANSFER_DST for the light gun crosshair copied over the converted frame (libretro_vulkan.cpp).
    const vk_util::ImageDesc imageDesc{
        .width = m_extent.width,
        .height = m_extent.height,
        .format = kOutputFormat,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
    };
    const auto &vk = m_device.Table();
    m_slots.resize(slotCount);
    for (Slot &slot : m_slots) {
        VkResult result = vk_util::CreateImage2D(m_device, imageDesc, slot.output);
        if (result == VK_SUCCESS) {
            // The shader writes packed words through an R32_UINT view (same 32-bit compatibility class; R32_UINT
            // storage images are mandatory), so the bytes are stored exactly.
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = slot.output.image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = kStorageViewFormat;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            result = vk.vkCreateImageView(m_device.Device(), &viewInfo, nullptr, &slot.storageView);
        }
        if (result != VK_SUCCESS) {
            return result;
        }
    }
    return VK_SUCCESS;
}

void OutputImages::Destroy() {
    const auto &vk = m_device.Table();
    for (Slot &slot : m_slots) {
        vk.vkDestroyImageView(m_device.Device(), slot.storageView, nullptr);
        vk_util::DestroyImage(m_device, slot.output);
    }
    m_slots.clear();
    vk_util::DestroyBuffer(m_device, m_readback);
}

VkImage OutputImages::Image(uint32_t slot) const {
    return slot < m_slots.size() ? m_slots[slot].output.image : VK_NULL_HANDLE;
}

VkImageView OutputImages::View(uint32_t slot) const {
    return slot < m_slots.size() ? m_slots[slot].output.view : VK_NULL_HANDLE;
}

std::vector<uint32_t> OutputImages::ReadbackXRGB(uint32_t slotIndex, OutputSize size) {
    std::vector<uint32_t> pixels;
    if (!Ok() || slotIndex >= m_slots.size() || !m_slots[slotIndex].presented || size.width == 0 || size.height == 0 ||
        size.width > m_extent.width || size.height > m_extent.height) {
        return pixels;
    }
    const auto &vk = m_device.Table();
    const Slot &slot = m_slots[slotIndex];

    // Created on first use (only tests read back), sized for the whole image. Host-cached when the device has such
    // memory: CPU reads from uncached (write-combined) memory are very slow.
    vk_util::Buffer &readback = m_readback;
    if (readback.buffer == VK_NULL_HANDLE) {
        const VkDeviceSize maxBytes = VkDeviceSize{m_extent.width} * m_extent.height * sizeof(uint32_t);
        constexpr VkMemoryPropertyFlags coherent =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (vk_util::CreateBuffer(m_device, maxBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, readback) != VK_SUCCESS &&
            vk_util::CreateBuffer(m_device, maxBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, coherent, readback) !=
                VK_SUCCESS) {
            return pixels;
        }
    }
    const VkResult result = vk_util::SubmitOneShot(m_device, [&](VkCommandBuffer cmd) {
        vk_util::ImageBarrier(m_device, cmd, slot.output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferImageCopy region{
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {size.width, size.height, 1},
        };
        vk.vkCmdCopyImageToBuffer(cmd, slot.output.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &region);
        vk_util::GlobalBarrier(m_device, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    });
    if (result == VK_SUCCESS && readback.mapped) {
        // Bytes R, G, B, A -> 0x00RRGGBB.
        const auto *rgba = static_cast<const uint8_t *>(readback.mapped);
        const size_t count = static_cast<size_t>(size.width) * size.height;
        pixels.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t *px = rgba + i * 4;
            pixels[i] = (uint32_t{px[0]} << 16) | (uint32_t{px[1]} << 8) | px[2];
        }
    }
    return pixels;
}

// ---------------------------------------------------------------------------------------------------------------
// PresenterCore

PresenterCore::PresenterCore(VulkanDevice &device, uint32_t slotCount) : m_device(device) {
    m_result = Init(slotCount);
    if (m_result != VK_SUCCESS) {
        Destroy();
    }
}

PresenterCore::~PresenterCore() {
    Destroy();
}

VkResult PresenterCore::Init(uint32_t slotCount) {
    if (slotCount == 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
        {kSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {kOutputBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    VkResult result = vk_util::CreateDescriptorSetLayout(m_device, bindings, m_setLayout);
    if (result != VK_SUCCESS) {
        return result;
    }
    const std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * slotCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * slotCount},
    }};
    // Two sets per slot: the upload source (Present) and a device buffer (PresentDevice).
    result = vk_util::CreateDescriptorPool(m_device, poolSizes, 2 * slotCount, m_pool);
    if (result != VK_SUCCESS) {
        return result;
    }
    const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    result = vk_util::CreatePipelineLayout(m_device, {&m_setLayout, 1}, {&pushRange, 1}, m_pipelineLayout);
    if (result != VK_SUCCESS) {
        return result;
    }
    result = vk_util::CreateComputePipeline(m_device, shaders::kCsFrameOutputSpv, m_pipelineLayout, m_pipeline);
    if (result != VK_SUCCESS) {
        return result;
    }

    m_slots.resize(slotCount);
    for (Slot &slot : m_slots) {
        result = vk_util::CreateBuffer(m_device, kSourceBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                       slot.upload);
        if (result == VK_SUCCESS && slot.upload.mapped == nullptr) {
            result = VK_ERROR_MEMORY_MAP_FAILED;
        }
        if (result == VK_SUCCESS) {
            result = vk_util::CreateBuffer(m_device, kSourceBytes,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, slot.source);
        }
        if (result == VK_SUCCESS) {
            result = vk_util::AllocateDescriptorSet(m_device, m_pool, m_setLayout, slot.set);
        }
        if (result == VK_SUCCESS) {
            result = vk_util::AllocateDescriptorSet(m_device, m_pool, m_setLayout, slot.deviceSet);
        }
        if (result != VK_SUCCESS) {
            return result;
        }
        vk_util::WriteStorageBuffer(m_device, slot.set, kSourceBinding, slot.source);
    }
    return VK_SUCCESS;
}

void PresenterCore::Destroy() {
    const auto &vk = m_device.Table();
    const VkDevice dev = m_device.Device();
    for (Slot &slot : m_slots) {
        vk_util::DestroyBuffer(m_device, slot.source);
        vk_util::DestroyBuffer(m_device, slot.upload);
    }
    m_slots.clear();
    vk.vkDestroyPipeline(dev, m_pipeline, nullptr);
    vk.vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    vk.vkDestroyDescriptorPool(dev, m_pool, nullptr); // frees the sets
    vk.vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
}

bool PresenterCore::Accepts(uint32_t slot, const OutputImages &images, uint32_t w, uint32_t h,
                            const OutputSettings &settings) const {
    return Ok() && images.Ok() && slot < m_slots.size() && images.SlotCount() == m_slots.size() && w != 0 && h != 0 &&
           w <= FramePresenter::kMaxSourceWidth && h <= FramePresenter::kMaxSourceHeight &&
           std::max<uint32_t>(settings.scale, 1) == images.Scale() &&
           ComputeOutputSize(w, h, settings) == images.Extent();
}

void PresenterCore::BindOutput(VkDescriptorSet set, uint64_t &bound, OutputImages &images, uint32_t slot) {
    // The slot's previous frame is done (precondition), so the set is not in use by pending work.
    if (bound != images.m_serial) {
        vk_util::WriteStorageImage(m_device, set, kOutputBinding, images.m_slots[slot].storageView);
        bound = images.m_serial;
    }
}

bool PresenterCore::Present(uint32_t slotIndex, OutputImages &images, const uint32_t *xbgr, uint32_t w, uint32_t h,
                            const OutputSettings &settings, VkCommandBuffer cmd) {
    if (xbgr == nullptr || images.Scale() != 1 || !Accepts(slotIndex, images, w, h, settings)) {
        return false;
    }
    const auto &vk = m_device.Table();
    Slot &slot = m_slots[slotIndex];
    BindOutput(slot.set, slot.setImages, images, slotIndex);

    // Host-coherent: the write is visible to the device at vkQueueSubmit, no flush or host barrier needed.
    const VkDeviceSize bytes = VkDeviceSize{w} * h * sizeof(uint32_t);
    std::memcpy(slot.upload.mapped, xbgr, static_cast<size_t>(bytes));

    // The previous frame's shader reads (WAR) and copy writes (WAW) of the source buffer must be done before the
    // copy overwrites it.
    vk_util::GlobalBarrier(m_device, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkBufferCopy copy{0, 0, bytes};
    vk.vkCmdCopyBuffer(cmd, slot.upload.buffer, slot.source.buffer, 1, &copy);

    VkBufferMemoryBarrier sourceBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    sourceBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    sourceBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sourceBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceBarrier.buffer = slot.source.buffer;
    sourceBarrier.offset = 0;
    sourceBarrier.size = bytes;
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                            1, &sourceBarrier, 0, nullptr);

    RecordConversion(images.m_slots[slotIndex], slot.set, w, h, settings, cmd);
    return true;
}

bool PresenterCore::PresentDevice(uint32_t slotIndex, OutputImages &images, VkBuffer source, uint32_t w, uint32_t h,
                                  const OutputSettings &settings, VkCommandBuffer cmd) {
    if (source == VK_NULL_HANDLE || !Accepts(slotIndex, images, w, h, settings)) {
        return false;
    }
    Slot &slot = m_slots[slotIndex];
    BindOutput(slot.deviceSet, slot.deviceSetImages, images, slotIndex);
    // Rewritten on every call: the slot's previous frame is done (precondition), so its device set is not in use, and
    // a handle value cannot be trusted to name the same buffer (a destroyed buffer's handle can be reused).
    const VkDescriptorBufferInfo bufferInfo{source, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = slot.deviceSet;
    write.dstBinding = kSourceBinding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;
    m_device.Table().vkUpdateDescriptorSets(m_device.Device(), 1, &write, 0, nullptr);

    // Whatever wrote the source (the VDP2 compose pass, a copy) is visible to the shader.
    vk_util::GlobalBarrier(m_device, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    RecordConversion(images.m_slots[slotIndex], slot.deviceSet, w, h, settings, cmd);
    return true;
}

void PresenterCore::RecordConversion(OutputImages::Slot &image, VkDescriptorSet set, uint32_t w, uint32_t h,
                                     const OutputSettings &settings, VkCommandBuffer cmd) {
    const auto &vk = m_device.Table();

    // Earlier users of the image (the frontend's reads, a readback) finish before the shader writes it. The old
    // contents are not needed, so the transition is always from UNDEFINED (independent of what was recorded or
    // submitted before, and of the order command buffers are submitted in).
    vk_util::ImageBarrier(m_device, cmd, image.output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT);
    image.presented = true;

    // At internal resolution N every quantity is N x the native one; the crop guard is native.
    const uint32_t n = std::max<uint32_t>(settings.scale, 1);
    const OutputSize size = ComputeOutputSize(w, h, settings);
    const bool crop = CropApplies(w, h, settings);
    const PushConstants push{
        .srcWidth = n * w,
        .cropLeft = crop ? n * (settings.cropH / 2) : 0,
        .cropTop = crop ? n * (settings.cropV / 2) : 0,
        .cropWidth = n * (crop ? w - settings.cropH : w),
        .outWidth = size.width,
        .outHeight = size.height,
        .rotation = EffectiveRotation(settings.rotation),
    };
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &set, 0, nullptr);
    vk.vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vk.vkCmdDispatch(cmd, (size.width + kGroupSize - 1) / kGroupSize, (size.height + kGroupSize - 1) / kGroupSize, 1);

    // Make the writes available to whatever reads the image next (frontend sampling or blit, a readback).
    vk_util::ImageBarrier(m_device, cmd, image.output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
}

// ---------------------------------------------------------------------------------------------------------------
// FramePresenter

FramePresenter::FramePresenter(VulkanDevice &device, uint32_t slotCount, OutputSize extent, uint32_t scale)
    : m_core(device, slotCount), m_images(device, slotCount, extent, scale) {}

// ---------------------------------------------------------------------------------------------------------------
// OutputRetryBackoff, ScaleRestoreSchedule

bool OutputRetryBackoff::MayTry(OutputSize extent, uint32_t scale, uint64_t frame) {
    if (!WouldTry(extent, scale, frame)) {
        ++m_backedOff;
        return false;
    }
    return true;
}

bool OutputRetryBackoff::WouldTry(OutputSize extent, uint32_t scale, uint64_t frame) const {
    const Entry &e = At(scale);
    return !(e.active && extent == e.extent && frame < e.retryAt);
}

bool OutputRetryBackoff::Failed(OutputSize extent, uint32_t scale, uint64_t frame) {
    ++m_failures;
    Entry &e = At(scale);
    const bool fresh = !e.active || extent != e.extent;
    e.delay = fresh ? 1 : std::min(e.delay * 2, kMaxDelay);
    e.active = true;
    e.extent = extent;
    e.retryAt = frame + e.delay;
    return fresh;
}

bool ScaleRestoreSchedule::Update(bool degraded, bool sizeChanged, uint64_t frame) {
    if (!degraded) {
        m_degraded = false;
        return false;
    }
    if (!m_degraded) {
        // The downgrade: the first attempt after the delay (or at the next size change)
        m_degraded = true;
        m_nextAt = frame + (kFirstDelay << m_attempts);
        return false;
    }
    if (m_attempts >= kMaxAttempts || (!sizeChanged && frame < m_nextAt)) {
        return false;
    }
    ++m_attempts;
    ++m_totalAttempts;
    m_nextAt = frame + (kFirstDelay << m_attempts);
    return true;
}

void ScaleRestoreSchedule::Reset() {
    m_degraded = false;
    m_attempts = 0;
}

} // namespace brimir::gpu
