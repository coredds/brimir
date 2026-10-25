#pragma once

// Small Vulkan helpers for the GPU renderer's compute passes. One memory allocation per resource (the renderer has
// few). Create* functions return VK_SUCCESS or the failing result and leave nothing behind on failure; every created
// object is released with the matching Destroy* (or the table's vkDestroy*). Destroy* accept empty objects.

#include "vk_device.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace brimir::gpu::vk_util {

// "VK_ERROR_DEVICE_LOST" etc.; unknown values as "VkResult(<n>)".
const char *ResultName(VkResult result);

// The first memory type in typeBits with all of the required property flags.
std::optional<uint32_t> FindMemoryType(const VulkanDevice &device, uint32_t typeBits, VkMemoryPropertyFlags required);

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr; // persistently mapped when the memory is host-visible
};

VkResult CreateBuffer(const VulkanDevice &device, VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags memoryFlags, Buffer &out);
void DestroyBuffer(const VulkanDevice &device, Buffer &buffer);

struct ImageDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags flags = 0;
    // A 2D color view in viewFormat (format when VK_FORMAT_UNDEFINED); needs a usage a view is valid for.
    bool createView = true;
    VkFormat viewFormat = VK_FORMAT_UNDEFINED;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
};

// A device-local, optimal-tiling, single-mip 2D image in VK_IMAGE_LAYOUT_UNDEFINED.
VkResult CreateImage2D(const VulkanDevice &device, const ImageDesc &desc, Image &out);
void DestroyImage(const VulkanDevice &device, Image &image);

// Records a primary command buffer with `record`, submits it through VulkanDevice::Submit and waits for it on a
// fence (outside the queue lock). The command pool and fence are created and destroyed per call.
VkResult SubmitOneShot(VulkanDevice &device, const std::function<void(VkCommandBuffer)> &record);

VkResult CreatePipelineLayout(const VulkanDevice &device, std::span<const VkDescriptorSetLayout> setLayouts,
                              std::span<const VkPushConstantRange> pushConstants, VkPipelineLayout &out);

// A compute pipeline from SPIR-V (src/gpu/shaders/generated); the shader module is destroyed before returning.
VkResult CreateComputePipeline(const VulkanDevice &device, std::span<const uint32_t> spirv, VkPipelineLayout layout,
                               VkPipeline &out, const char *entryPoint = "CSMain");

VkResult CreateDescriptorSetLayout(const VulkanDevice &device, std::span<const VkDescriptorSetLayoutBinding> bindings,
                                   VkDescriptorSetLayout &out);
VkResult CreateDescriptorPool(const VulkanDevice &device, std::span<const VkDescriptorPoolSize> sizes,
                              uint32_t maxSets, VkDescriptorPool &out);
// Freed with the pool.
VkResult AllocateDescriptorSet(const VulkanDevice &device, VkDescriptorPool pool, VkDescriptorSetLayout layout,
                               VkDescriptorSet &out);

// The whole buffer as a storage buffer at `binding`.
void WriteStorageBuffer(const VulkanDevice &device, VkDescriptorSet set, uint32_t binding, const Buffer &buffer);
// A storage image at `binding`, accessed in VK_IMAGE_LAYOUT_GENERAL.
void WriteStorageImage(const VulkanDevice &device, VkDescriptorSet set, uint32_t binding, VkImageView view);

// A layout transition and/or execution + memory dependency on all of a color image's subresources.
void ImageBarrier(const VulkanDevice &device, VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                  VkImageLayout newLayout, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                  VkPipelineStageFlags dstStage, VkAccessFlags dstAccess);

// A global memory dependency (for example compute writes before host reads).
void GlobalBarrier(const VulkanDevice &device, VkCommandBuffer cmd, VkPipelineStageFlags srcStage,
                   VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess);

} // namespace brimir::gpu::vk_util
