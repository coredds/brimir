#include "vk_util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace brimir::gpu::vk_util {

const char *ResultName(VkResult result) {
    switch (result) {
#define BRIMIR_VK_RESULT(name)                                                                                         \
    case name: return #name;
        BRIMIR_VK_RESULT(VK_SUCCESS)
        BRIMIR_VK_RESULT(VK_NOT_READY)
        BRIMIR_VK_RESULT(VK_TIMEOUT)
        BRIMIR_VK_RESULT(VK_EVENT_SET)
        BRIMIR_VK_RESULT(VK_EVENT_RESET)
        BRIMIR_VK_RESULT(VK_INCOMPLETE)
        BRIMIR_VK_RESULT(VK_ERROR_OUT_OF_HOST_MEMORY)
        BRIMIR_VK_RESULT(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        BRIMIR_VK_RESULT(VK_ERROR_INITIALIZATION_FAILED)
        BRIMIR_VK_RESULT(VK_ERROR_DEVICE_LOST)
        BRIMIR_VK_RESULT(VK_ERROR_MEMORY_MAP_FAILED)
        BRIMIR_VK_RESULT(VK_ERROR_LAYER_NOT_PRESENT)
        BRIMIR_VK_RESULT(VK_ERROR_EXTENSION_NOT_PRESENT)
        BRIMIR_VK_RESULT(VK_ERROR_FEATURE_NOT_PRESENT)
        BRIMIR_VK_RESULT(VK_ERROR_INCOMPATIBLE_DRIVER)
        BRIMIR_VK_RESULT(VK_ERROR_TOO_MANY_OBJECTS)
        BRIMIR_VK_RESULT(VK_ERROR_FORMAT_NOT_SUPPORTED)
        BRIMIR_VK_RESULT(VK_ERROR_FRAGMENTED_POOL)
        BRIMIR_VK_RESULT(VK_ERROR_UNKNOWN)
        BRIMIR_VK_RESULT(VK_ERROR_OUT_OF_POOL_MEMORY)
        BRIMIR_VK_RESULT(VK_ERROR_INVALID_EXTERNAL_HANDLE)
        BRIMIR_VK_RESULT(VK_ERROR_FRAGMENTATION)
        BRIMIR_VK_RESULT(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS)
        BRIMIR_VK_RESULT(VK_ERROR_SURFACE_LOST_KHR)
        BRIMIR_VK_RESULT(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
        BRIMIR_VK_RESULT(VK_SUBOPTIMAL_KHR)
        BRIMIR_VK_RESULT(VK_ERROR_OUT_OF_DATE_KHR)
#undef BRIMIR_VK_RESULT
    default: {
        thread_local char text[32];
        std::snprintf(text, sizeof(text), "VkResult(%d)", static_cast<int>(result));
        return text;
    }
    }
}

std::optional<uint32_t> FindMemoryType(const VulkanDevice &device, uint32_t typeBits, VkMemoryPropertyFlags required) {
    const VkPhysicalDeviceMemoryProperties &memory = device.MemoryProperties();
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    return std::nullopt;
}

namespace {

VkResult AllocateFor(const VulkanDevice &device, const VkMemoryRequirements &requirements,
                     VkMemoryPropertyFlags memoryFlags, VkDeviceMemory &out) {
    const std::optional<uint32_t> type = FindMemoryType(device, requirements.memoryTypeBits, memoryFlags);
    if (!type) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = requirements.size;
    info.memoryTypeIndex = *type;
    return device.Table().vkAllocateMemory(device.Device(), &info, nullptr, &out);
}

} // namespace

VkResult CreateBuffer(const VulkanDevice &device, VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags memoryFlags, Buffer &out) {
    const auto &vk = device.Table();
    Buffer buffer;
    buffer.size = size;

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vk.vkCreateBuffer(device.Device(), &info, nullptr, &buffer.buffer);
    if (result == VK_SUCCESS) {
        VkMemoryRequirements requirements{};
        vk.vkGetBufferMemoryRequirements(device.Device(), buffer.buffer, &requirements);
        result = AllocateFor(device, requirements, memoryFlags, buffer.memory);
    }
    if (result == VK_SUCCESS) {
        result = vk.vkBindBufferMemory(device.Device(), buffer.buffer, buffer.memory, 0);
    }
    if (result == VK_SUCCESS && (memoryFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        result = vk.vkMapMemory(device.Device(), buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped);
    }
    if (result != VK_SUCCESS) {
        DestroyBuffer(device, buffer);
        return result;
    }
    out = buffer;
    return VK_SUCCESS;
}

void DestroyBuffer(const VulkanDevice &device, Buffer &buffer) {
    const auto &vk = device.Table();
    // Freeing mapped memory unmaps it.
    vk.vkDestroyBuffer(device.Device(), buffer.buffer, nullptr);
    vk.vkFreeMemory(device.Device(), buffer.memory, nullptr);
    buffer = Buffer{};
}

VkResult CreateImage2D(const VulkanDevice &device, const ImageDesc &desc, Image &out) {
    const auto &vk = device.Table();
    Image image;
    image.format = desc.format;
    image.extent = {desc.width, desc.height};

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = desc.flags;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = desc.format;
    info.extent = {desc.width, desc.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = std::max(desc.arrayLayers, 1u);
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = desc.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = vk.vkCreateImage(device.Device(), &info, nullptr, &image.image);
    if (result == VK_SUCCESS) {
        VkMemoryRequirements requirements{};
        vk.vkGetImageMemoryRequirements(device.Device(), image.image, &requirements);
        result = AllocateFor(device, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image.memory);
    }
    if (result == VK_SUCCESS) {
        result = vk.vkBindImageMemory(device.Device(), image.image, image.memory, 0);
    }
    if (result == VK_SUCCESS && desc.createView) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image.image;
        viewInfo.viewType = info.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = desc.viewFormat != VK_FORMAT_UNDEFINED ? desc.viewFormat : desc.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, info.arrayLayers};
        result = vk.vkCreateImageView(device.Device(), &viewInfo, nullptr, &image.view);
    }
    if (result != VK_SUCCESS) {
        DestroyImage(device, image);
        return result;
    }
    out = image;
    return VK_SUCCESS;
}

void DestroyImage(const VulkanDevice &device, Image &image) {
    const auto &vk = device.Table();
    vk.vkDestroyImageView(device.Device(), image.view, nullptr);
    vk.vkDestroyImage(device.Device(), image.image, nullptr);
    vk.vkFreeMemory(device.Device(), image.memory, nullptr);
    image = Image{};
}

VkResult SubmitOneShot(VulkanDevice &device, const std::function<void(VkCommandBuffer)> &record) {
    const auto &vk = device.Table();
    const VkDevice dev = device.Device();
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = device.QueueFamily();
    VkResult result = vk.vkCreateCommandPool(dev, &poolInfo, nullptr, &pool);
    if (result == VK_SUCCESS) {
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        result = vk.vkAllocateCommandBuffers(dev, &allocInfo, &cmd);
    }
    if (result == VK_SUCCESS) {
        const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vk.vkCreateFence(dev, &fenceInfo, nullptr, &fence);
    }
    if (result == VK_SUCCESS) {
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vk.vkBeginCommandBuffer(cmd, &beginInfo);
    }
    if (result == VK_SUCCESS) {
        record(cmd);
        result = vk.vkEndCommandBuffer(cmd);
    }
    if (result == VK_SUCCESS) {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        result = device.Submit(submit, fence);
        if (result == VK_SUCCESS) {
            result = vk.vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        }
    }
    vk.vkDestroyFence(dev, fence, nullptr);
    vk.vkDestroyCommandPool(dev, pool, nullptr); // frees cmd
    return result;
}

VkResult WaitForFence(const VulkanDevice &device, VkFence fence, uint32_t pollMicroseconds,
                      uint64_t timeoutNanoseconds) {
    const auto &vk = device.Table();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(pollMicroseconds);
    for (;;) {
        const VkResult status = vk.vkGetFenceStatus(device.Device(), fence);
        if (status != VK_NOT_READY) {
            return status;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::yield();
    }
    return vk.vkWaitForFences(device.Device(), 1, &fence, VK_TRUE, timeoutNanoseconds);
}

VkResult CreatePipelineLayout(const VulkanDevice &device, std::span<const VkDescriptorSetLayout> setLayouts,
                              std::span<const VkPushConstantRange> pushConstants, VkPipelineLayout &out) {
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    info.pSetLayouts = setLayouts.data();
    info.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size());
    info.pPushConstantRanges = pushConstants.data();
    return device.Table().vkCreatePipelineLayout(device.Device(), &info, nullptr, &out);
}

VkResult CreateComputePipeline(const VulkanDevice &device, std::span<const uint32_t> spirv, VkPipelineLayout layout,
                               VkPipeline &out, const char *entryPoint) {
    const auto &vk = device.Table();
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = spirv.size_bytes();
    moduleInfo.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult result = vk.vkCreateShaderModule(device.Device(), &moduleInfo, nullptr, &module);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = entryPoint;
    info.layout = layout;
    result = vk.vkCreateComputePipelines(device.Device(), VK_NULL_HANDLE, 1, &info, nullptr, &out);
    vk.vkDestroyShaderModule(device.Device(), module, nullptr);
    return result;
}

VkResult CreateDescriptorSetLayout(const VulkanDevice &device, std::span<const VkDescriptorSetLayoutBinding> bindings,
                                   VkDescriptorSetLayout &out) {
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    return device.Table().vkCreateDescriptorSetLayout(device.Device(), &info, nullptr, &out);
}

VkResult CreateDescriptorPool(const VulkanDevice &device, std::span<const VkDescriptorPoolSize> sizes,
                              uint32_t maxSets, VkDescriptorPool &out) {
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets = maxSets;
    info.poolSizeCount = static_cast<uint32_t>(sizes.size());
    info.pPoolSizes = sizes.data();
    return device.Table().vkCreateDescriptorPool(device.Device(), &info, nullptr, &out);
}

VkResult AllocateDescriptorSet(const VulkanDevice &device, VkDescriptorPool pool, VkDescriptorSetLayout layout,
                               VkDescriptorSet &out) {
    VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    return device.Table().vkAllocateDescriptorSets(device.Device(), &info, &out);
}

void WriteStorageBuffer(const VulkanDevice &device, VkDescriptorSet set, uint32_t binding, const Buffer &buffer) {
    const VkDescriptorBufferInfo bufferInfo{buffer.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;
    device.Table().vkUpdateDescriptorSets(device.Device(), 1, &write, 0, nullptr);
}

void WriteStorageImage(const VulkanDevice &device, VkDescriptorSet set, uint32_t binding, VkImageView view) {
    const VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &imageInfo;
    device.Table().vkUpdateDescriptorSets(device.Device(), 1, &write, 0, nullptr);
}

void ImageBarrier(const VulkanDevice &device, VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                  VkImageLayout newLayout, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                  VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    device.Table().vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void GlobalBarrier(const VulkanDevice &device, VkCommandBuffer cmd, VkPipelineStageFlags srcStage,
                   VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    device.Table().vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

} // namespace brimir::gpu::vk_util
