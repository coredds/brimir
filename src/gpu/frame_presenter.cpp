#include "frame_presenter.hpp"

#include "shaders/generated/cs_frame_output.spv.h"

#include <array>
#include <cstring>

namespace brimir::gpu {

namespace {

constexpr VkFormat kOutputFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t kSourceBinding = 0;  // t0
constexpr uint32_t kOutputBinding = 16; // u0
constexpr uint32_t kGroupSize = 8;
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

// The bridge's crop guard: crop only when the frame exceeds the crop by more than 32 px on both axes.
bool CropApplies(uint32_t w, uint32_t h, const OutputSettings &settings) {
    if (settings.cropH == 0 && settings.cropV == 0) {
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
    return size;
}

FramePresenter::FramePresenter(VulkanDevice &device, uint32_t slotCount) : m_device(device) {
    m_result = Init(slotCount);
    if (m_result != VK_SUCCESS) {
        Destroy();
    }
}

FramePresenter::~FramePresenter() {
    Destroy();
}

VkResult FramePresenter::Init(uint32_t slotCount) {
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
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, slotCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, slotCount},
    }};
    result = vk_util::CreateDescriptorPool(m_device, poolSizes, slotCount, m_pool);
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

    // libretro output images: MUTABLE_FORMAT, SAMPLED | TRANSFER_SRC | STORAGE (stage1-libretro-vulkan.md).
    const vk_util::ImageDesc imageDesc{
        .width = kMaxOutputSize,
        .height = kMaxOutputSize,
        .format = kOutputFormat,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
    };
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
            result = vk_util::CreateImage2D(m_device, imageDesc, slot.output);
        }
        if (result == VK_SUCCESS) {
            result = vk_util::AllocateDescriptorSet(m_device, m_pool, m_setLayout, slot.set);
        }
        if (result != VK_SUCCESS) {
            return result;
        }
        vk_util::WriteStorageBuffer(m_device, slot.set, kSourceBinding, slot.source);
        vk_util::WriteStorageImage(m_device, slot.set, kOutputBinding, slot.output.view);
    }
    return VK_SUCCESS;
}

void FramePresenter::Destroy() {
    const auto &vk = m_device.Table();
    const VkDevice dev = m_device.Device();
    for (Slot &slot : m_slots) {
        vk_util::DestroyImage(m_device, slot.output);
        vk_util::DestroyBuffer(m_device, slot.source);
        vk_util::DestroyBuffer(m_device, slot.upload);
    }
    m_slots.clear();
    vk_util::DestroyBuffer(m_device, m_readback);
    vk.vkDestroyPipeline(dev, m_pipeline, nullptr);
    vk.vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    vk.vkDestroyDescriptorPool(dev, m_pool, nullptr); // frees the sets
    vk.vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
}

bool FramePresenter::Present(uint32_t slotIndex, const uint32_t *xbgr, uint32_t w, uint32_t h,
                             const OutputSettings &settings, VkCommandBuffer cmd) {
    if (!Ok() || slotIndex >= m_slots.size() || xbgr == nullptr || w == 0 || h == 0 || w > kMaxSourceWidth ||
        h > kMaxSourceHeight) {
        return false;
    }
    const auto &vk = m_device.Table();
    Slot &slot = m_slots[slotIndex];

    // Host-coherent: the write is visible to the device at vkQueueSubmit, no flush or host barrier needed.
    const VkDeviceSize bytes = VkDeviceSize{w} * h * sizeof(uint32_t);
    std::memcpy(slot.upload.mapped, xbgr, static_cast<size_t>(bytes));

    // The previous frame's shader reads of the source buffer must finish before the copy overwrites it.
    vk_util::GlobalBarrier(m_device, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, 0);
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

    // Earlier users of the image (the frontend's reads, a readback) finish before the shader writes it. The first
    // use leaves UNDEFINED; afterwards the contents outside the new frame are not needed either, but GENERAL keeps
    // the layout the frontend was handed.
    vk_util::ImageBarrier(m_device, cmd, slot.output.image,
                          slot.written ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT);
    slot.written = true;

    const OutputSize size = ComputeOutputSize(w, h, settings);
    const bool crop = CropApplies(w, h, settings);
    const PushConstants push{
        .srcWidth = w,
        .cropLeft = crop ? settings.cropH / 2 : 0,
        .cropTop = crop ? settings.cropV / 2 : 0,
        .cropWidth = crop ? w - settings.cropH : w,
        .outWidth = size.width,
        .outHeight = size.height,
        .rotation = EffectiveRotation(settings.rotation),
    };
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &slot.set, 0, nullptr);
    vk.vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vk.vkCmdDispatch(cmd, (size.width + kGroupSize - 1) / kGroupSize, (size.height + kGroupSize - 1) / kGroupSize, 1);

    // Make the writes available to whatever reads the image next (frontend sampling or blit, a readback).
    vk_util::ImageBarrier(m_device, cmd, slot.output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
    return true;
}

VkImage FramePresenter::OutputImage(uint32_t slot) const {
    return slot < m_slots.size() ? m_slots[slot].output.image : VK_NULL_HANDLE;
}

VkImageView FramePresenter::OutputView(uint32_t slot) const {
    return slot < m_slots.size() ? m_slots[slot].output.view : VK_NULL_HANDLE;
}

std::vector<uint32_t> FramePresenter::ReadbackXRGB(uint32_t slotIndex, OutputSize size) {
    std::vector<uint32_t> pixels;
    if (!Ok() || slotIndex >= m_slots.size() || !m_slots[slotIndex].written || size.width == 0 || size.height == 0 ||
        size.width > kMaxOutputSize || size.height > kMaxOutputSize) {
        return pixels;
    }
    const auto &vk = m_device.Table();
    const Slot &slot = m_slots[slotIndex];

    // Created on first use (only tests and vkhost read back), sized for the largest output. Host-cached when the
    // device has such memory: CPU reads from uncached (write-combined) memory are very slow.
    vk_util::Buffer &readback = m_readback;
    if (readback.buffer == VK_NULL_HANDLE) {
        const VkDeviceSize maxBytes = VkDeviceSize{kMaxOutputSize} * kMaxOutputSize * sizeof(uint32_t);
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

} // namespace brimir::gpu
