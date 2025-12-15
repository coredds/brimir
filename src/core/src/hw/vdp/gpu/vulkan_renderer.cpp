// Brimir - Vulkan Renderer Implementation
// GPU-accelerated rendering using Vulkan

#ifdef BRIMIR_GPU_VULKAN_ENABLED

#include <brimir/hw/vdp/vdp_renderer.hpp>
#include <array>
#include <vector>
#include <stdexcept>
#include <fstream>

#ifdef BRIMIR_GPU_VULKAN_ENABLED
#include <vulkan/vulkan.h>
#endif

namespace brimir::vdp {

class VulkanRenderer final : public IVDPRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer() override {
        if (m_initialized) {
            Shutdown();
        }
    }
    
    // ===== Lifecycle =====
    
    bool Initialize() override {
        try {
            // Step 1: Create Vulkan instance
            if (!CreateInstance()) {
                return false;
            }
            
            // Step 2: Select physical device (GPU)
            if (!SelectPhysicalDevice()) {
                Shutdown();
                return false;
            }
            
            // Step 3: Create logical device
            if (!CreateDevice()) {
                Shutdown();
                return false;
            }
            
            // Step 4: Create command pool
            if (!CreateCommandPool()) {
                Shutdown();
                return false;
            }
            
            // Set default resolution
            m_width = 320;
            m_height = 224;
            m_pitch = m_width * 4;  // XRGB8888 = 4 bytes per pixel
            
            // Step 5: Create framebuffer image
            if (!CreateFramebufferResources()) {
                Shutdown();
                return false;
            }
            
            // Step 6: Create render pass
            if (!CreateRenderPass()) {
                Shutdown();
                return false;
            }
            
            // Step 7: Create graphics pipeline
            if (!CreateGraphicsPipeline()) {
                Shutdown();
                return false;
            }
            
            // Step 8: Allocate command buffer
            if (!AllocateCommandBuffer()) {
                Shutdown();
                return false;
            }
            
            // Allocate CPU-side framebuffer for readback
            m_framebuffer.resize(m_width * m_height);
            
            m_initialized = true;
            return true;
            
        } catch (const std::exception& e) {
            // Vulkan initialization failed
            Shutdown();
            return false;
        }
    }
    
    void Shutdown() override {
        if (!m_initialized) return;
        
        // Cleanup Vulkan resources in reverse order
        if (m_device) {
            // Wait for device to be idle before cleanup
            vkDeviceWaitIdle(m_device);
            
            // Destroy pipeline
            if (m_graphicsPipeline) {
                vkDestroyPipeline(m_device, m_graphicsPipeline, nullptr);
                m_graphicsPipeline = VK_NULL_HANDLE;
            }
            
            if (m_pipelineLayout) {
                vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
                m_pipelineLayout = VK_NULL_HANDLE;
            }
            
            if (m_vertShaderModule) {
                vkDestroyShaderModule(m_device, m_vertShaderModule, nullptr);
                m_vertShaderModule = VK_NULL_HANDLE;
            }
            
            if (m_fragShaderModule) {
                vkDestroyShaderModule(m_device, m_fragShaderModule, nullptr);
                m_fragShaderModule = VK_NULL_HANDLE;
            }
            
            // Destroy rendering resources
            if (m_vkFramebuffer) {
                vkDestroyFramebuffer(m_device, m_vkFramebuffer, nullptr);
                m_vkFramebuffer = VK_NULL_HANDLE;
            }
            
            if (m_renderPass) {
                vkDestroyRenderPass(m_device, m_renderPass, nullptr);
                m_renderPass = VK_NULL_HANDLE;
            }
            
            if (m_framebufferView) {
                vkDestroyImageView(m_device, m_framebufferView, nullptr);
                m_framebufferView = VK_NULL_HANDLE;
            }
            
            if (m_framebufferImage) {
                vkDestroyImage(m_device, m_framebufferImage, nullptr);
                m_framebufferImage = VK_NULL_HANDLE;
            }
            
            if (m_framebufferMemory) {
                vkFreeMemory(m_device, m_framebufferMemory, nullptr);
                m_framebufferMemory = VK_NULL_HANDLE;
            }
            
            if (m_stagingImage) {
                vkDestroyImage(m_device, m_stagingImage, nullptr);
                m_stagingImage = VK_NULL_HANDLE;
            }
            
            if (m_stagingMemory) {
                vkFreeMemory(m_device, m_stagingMemory, nullptr);
                m_stagingMemory = VK_NULL_HANDLE;
            }
            
            if (m_commandPool) {
                vkDestroyCommandPool(m_device, m_commandPool, nullptr);
                m_commandPool = VK_NULL_HANDLE;
            }
            
            vkDestroyDevice(m_device, nullptr);
            m_device = VK_NULL_HANDLE;
        }
        
        if (m_instance) {
            vkDestroyInstance(m_instance, nullptr);
            m_instance = VK_NULL_HANDLE;
        }
        
        m_physicalDevice = VK_NULL_HANDLE;
        m_graphicsQueue = VK_NULL_HANDLE;
        m_framebuffer.clear();
        m_initialized = false;
    }
    
    void Reset() override {
        if (m_framebuffer.size() > 0) {
            m_framebuffer.assign(m_framebuffer.size(), 0);
        }
        m_statistics = {};
    }
    
    // ===== Frame Management =====
    
    void BeginFrame() override {
        if (!m_initialized) return;
        
        m_frameStartTime = std::chrono::high_resolution_clock::now();
        
        // Reset command buffer
        vkResetCommandBuffer(m_commandBuffer, 0);
        
        // Begin command buffer
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        
        vkBeginCommandBuffer(m_commandBuffer, &beginInfo);
        
        // Begin render pass
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_renderPass;
        renderPassInfo.framebuffer = m_vkFramebuffer;
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = {m_width, m_height};
        
        VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearColor;
        
        vkCmdBeginRenderPass(m_commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    }
    
    void EndFrame() override {
        if (!m_initialized) return;
        
        // End render pass
        vkCmdEndRenderPass(m_commandBuffer);
        
        // End command buffer
        vkEndCommandBuffer(m_commandBuffer);
        
        // Submit command buffer
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_commandBuffer;
        
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        
        // Copy image to staging for CPU readback
        CopyFramebufferToCPU();
        
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            now - m_frameStartTime);
        
        m_statistics.frameCount++;
        m_statistics.lastFrameTime = duration.count() / 1000.0f;
        
        if (m_statistics.averageFrameTime == 0.0f) {
            m_statistics.averageFrameTime = m_statistics.lastFrameTime;
        } else {
            m_statistics.averageFrameTime = 
                m_statistics.averageFrameTime * 0.95f + 
                m_statistics.lastFrameTime * 0.05f;
        }
    }
    
    const void* GetFramebuffer() const override {
        return m_framebuffer.data();
    }
    
    uint32_t GetFramebufferWidth() const override {
        return m_width;
    }
    
    uint32_t GetFramebufferHeight() const override {
        return m_height;
    }
    
    uint32_t GetFramebufferPitch() const override {
        return m_pitch;
    }
    
    FramebufferFormat GetFramebufferFormat() const override {
        return FramebufferFormat::XRGB8888;
    }
    
    // ===== VDP1 Rendering =====
    
    void VDP1DrawPolygon(const VDP1Command& cmd) override {
        // TODO: Implement GPU polygon rendering
        m_statistics.drawCallCount++;
    }
    
    void VDP1DrawSprite(const VDP1Command& cmd) override {
        // TODO: Implement GPU sprite rendering
        m_statistics.drawCallCount++;
    }
    
    void VDP1DrawScaledSprite(const VDP1Command& cmd) override {
        // TODO: Implement GPU scaled sprite rendering
        m_statistics.drawCallCount++;
    }
    
    void VDP1DrawDistortedSprite(const VDP1Command& cmd) override {
        // TODO: Implement GPU distorted sprite rendering
        m_statistics.drawCallCount++;
    }
    
    void VDP1DrawLine(const VDP1Command& cmd) override {
        // TODO: Implement GPU line rendering
        m_statistics.drawCallCount++;
    }
    
    void VDP1DrawPolyline(const VDP1Command& cmd) override {
        // TODO: Implement GPU polyline rendering
        m_statistics.drawCallCount++;
    }
    
    // ===== VDP2 Rendering =====
    
    void VDP2DrawBackground(int layer, const VDP2LayerState& state) override {
        // TODO: Implement GPU background rendering
        m_statistics.drawCallCount++;
    }
    
    void VDP2DrawRotation(int layer, const VDP2RotationState& state) override {
        // TODO: Implement GPU rotation layer rendering
        m_statistics.drawCallCount++;
    }
    
    void VDP2DrawSpriteLayer(const VDP2LayerState& state) override {
        // TODO: Implement GPU sprite layer rendering
        m_statistics.drawCallCount++;
    }
    
    // ===== Compositing =====
    
    void CompositeLayers() override {
        // TODO: Implement GPU layer compositing
    }
    
    void ApplyColorCalculation() override {
        // TODO: Implement GPU color calculation
    }
    
    void ApplyDeinterlacing() override {
        // TODO: Implement GPU deinterlacing
    }
    
    // ===== Configuration =====
    
    void SetInternalScale(uint32_t scale) override {
        // Clamp to supported range
        if (scale < 1) scale = 1;
        if (scale > 8) scale = 8;
        m_internalScale = scale;
        
        // TODO: Recreate render targets at new resolution
    }
    
    uint32_t GetInternalScale() const override {
        return m_internalScale;
    }
    
    void SetTextureFiltering(bool enable) override {
        m_textureFiltering = enable;
        // TODO: Update sampler state
    }
    
    void SetMSAA(uint32_t samples) override {
        m_msaaSamples = samples;
        // TODO: Recreate render targets with MSAA
    }
    
    // ===== Capabilities =====
    
    RendererType GetType() const override {
        return RendererType::Vulkan;
    }
    
    RendererCapabilities GetCapabilities() const override {
        RendererCapabilities caps{};
        caps.type = RendererType::Vulkan;
        caps.supportsUpscaling = true;
        caps.supportsAntiAliasing = true;
        caps.supportsTextureFiltering = true;
        caps.supportsComputeShaders = true;
        caps.maxTextureSize = 8192;  // TODO: Query from device
        caps.maxInternalScale = 8;
        return caps;
    }
    
    bool SupportsFeature(RendererFeature feature) const override {
        switch (feature) {
        case RendererFeature::BasicRendering:
        case RendererFeature::InternalUpscaling:
        case RendererFeature::AntiAliasing:
        case RendererFeature::TextureFiltering:
        case RendererFeature::ComputeShaders:
        case RendererFeature::FastDeinterlacing:
        case RendererFeature::PostProcessing:
            return true;
        default:
            return false;
        }
    }
    
    // ===== Debugging =====
    
    Statistics GetStatistics() const override {
        return m_statistics;
    }
    
    void ResetStatistics() override {
        m_statistics = {};
    }
    
private:
    bool m_initialized = false;
    uint32_t m_width = 320;
    uint32_t m_height = 224;
    uint32_t m_pitch = 1280;
    uint32_t m_internalScale = 1;
    bool m_textureFiltering = false;
    uint32_t m_msaaSamples = 1;
    
    std::vector<uint32_t> m_framebuffer;
    
    Statistics m_statistics{};
    std::chrono::high_resolution_clock::time_point m_frameStartTime;
    
    // Vulkan resources
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = 0;
    
    // Rendering resources
    VkImage m_framebufferImage = VK_NULL_HANDLE;
    VkDeviceMemory m_framebufferMemory = VK_NULL_HANDLE;
    VkImageView m_framebufferView = VK_NULL_HANDLE;
    VkImage m_stagingImage = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkFramebuffer m_vkFramebuffer = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    
    // Pipeline resources
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_graphicsPipeline = VK_NULL_HANDLE;
    VkShaderModule m_vertShaderModule = VK_NULL_HANDLE;
    VkShaderModule m_fragShaderModule = VK_NULL_HANDLE;
    
    // Initialization helpers
    bool CreateInstance() {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Brimir";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.pEngineName = "Brimir GPU";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.apiVersion = VK_API_VERSION_1_2;
        
        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        
        // No extensions needed for headless rendering
        createInfo.enabledExtensionCount = 0;
        createInfo.enabledLayerCount = 0;
        
        VkResult result = vkCreateInstance(&createInfo, nullptr, &m_instance);
        return (result == VK_SUCCESS);
    }
    
    bool SelectPhysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
        
        if (deviceCount == 0) {
            return false;
        }
        
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());
        
        // Pick first suitable device
        for (const auto& device : devices) {
            if (IsDeviceSuitable(device)) {
                m_physicalDevice = device;
                return true;
            }
        }
        
        return false;
    }
    
    bool IsDeviceSuitable(VkPhysicalDevice device) {
        // Find graphics queue family
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());
        
        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                m_graphicsQueueFamily = i;
                return true;
            }
        }
        
        return false;
    }
    
    bool CreateDevice() {
        float queuePriority = 1.0f;
        
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = m_graphicsQueueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        
        VkPhysicalDeviceFeatures deviceFeatures{};
        
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pQueueCreateInfos = &queueCreateInfo;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pEnabledFeatures = &deviceFeatures;
        createInfo.enabledExtensionCount = 0;
        createInfo.enabledLayerCount = 0;
        
        VkResult result = vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
        if (result != VK_SUCCESS) {
            return false;
        }
        
        vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
        return true;
    }
    
    bool CreateCommandPool() {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        
        VkResult result = vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool);
        return (result == VK_SUCCESS);
    }
    
    bool CreateFramebufferResources() {
        // Create framebuffer image (R8G8B8A8)
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = m_width;
        imageInfo.extent.height = m_height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_framebufferImage) != VK_SUCCESS) {
            return false;
        }
        
        // Allocate memory for image
        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(m_device, m_framebufferImage, &memRequirements);
        
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, 
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_framebufferMemory) != VK_SUCCESS) {
            return false;
        }
        
        vkBindImageMemory(m_device, m_framebufferImage, m_framebufferMemory, 0);
        
        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_framebufferImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_framebufferView) != VK_SUCCESS) {
            return false;
        }
        
        // Create staging image for CPU readback
        imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        
        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_stagingImage) != VK_SUCCESS) {
            return false;
        }
        
        vkGetImageMemoryRequirements(m_device, m_stagingImage, &memRequirements);
        
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_stagingMemory) != VK_SUCCESS) {
            return false;
        }
        
        vkBindImageMemory(m_device, m_stagingImage, m_stagingMemory, 0);
        
        return true;
    }
    
    bool CreateRenderPass() {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        
        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;
        
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        
        if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
            return false;
        }
        
        // Create framebuffer
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &m_framebufferView;
        framebufferInfo.width = m_width;
        framebufferInfo.height = m_height;
        framebufferInfo.layers = 1;
        
        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_vkFramebuffer) != VK_SUCCESS) {
            return false;
        }
        
        return true;
    }
    
    bool AllocateCommandBuffer() {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        
        return vkAllocateCommandBuffers(m_device, &allocInfo, &m_commandBuffer) == VK_SUCCESS;
    }
    
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);
        
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && 
                (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        
        return 0;
    }
    
    void CopyFramebufferToCPU() {
        // Create one-time command buffer for copy
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool = m_commandPool;
        allocInfo.commandBufferCount = 1;
        
        VkCommandBuffer commandBuffer;
        vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer);
        
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        
        vkBeginCommandBuffer(commandBuffer, &beginInfo);
        
        // Transition staging image to transfer dst
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_stagingImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        
        // Copy image
        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.layerCount = 1;
        copyRegion.extent.width = m_width;
        copyRegion.extent.height = m_height;
        copyRegion.extent.depth = 1;
        
        vkCmdCopyImage(commandBuffer, m_framebufferImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      m_stagingImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
        
        // Transition staging to readable
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        
        vkEndCommandBuffer(commandBuffer);
        
        // Submit and wait
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        
        // Map staging memory and copy to CPU framebuffer
        void* data;
        vkMapMemory(m_device, m_stagingMemory, 0, VK_WHOLE_SIZE, 0, &data);
        
        // Copy RGBA8 to XRGB8888 framebuffer
        uint8_t* src = static_cast<uint8_t*>(data);
        for (uint32_t i = 0; i < m_width * m_height; i++) {
            uint8_t r = src[i * 4 + 0];
            uint8_t g = src[i * 4 + 1];
            uint8_t b = src[i * 4 + 2];
            m_framebuffer[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
        
        vkUnmapMemory(m_device, m_stagingMemory);
        
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
    }
    
    bool CreateGraphicsPipeline() {
        // Load shader bytecode (compiled SPIR-V)
        auto vertShaderCode = ReadShaderFile("src/core/src/hw/vdp/gpu/shaders/vdp1_vertex.spv");
        auto fragShaderCode = ReadShaderFile("src/core/src/hw/vdp/gpu/shaders/vdp1_fragment.spv");
        
        if (vertShaderCode.empty() || fragShaderCode.empty()) {
            return false;
        }
        
        // Create shader modules
        m_vertShaderModule = CreateShaderModule(vertShaderCode);
        m_fragShaderModule = CreateShaderModule(fragShaderCode);
        
        if (!m_vertShaderModule || !m_fragShaderModule) {
            return false;
        }
        
        // Shader stage creation
        VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
        vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertShaderStageInfo.module = m_vertShaderModule;
        vertShaderStageInfo.pName = "main";
        
        VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
        fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageInfo.module = m_fragShaderModule;
        fragShaderStageInfo.pName = "main";
        
        VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};
        
        // Vertex input (position, color, texcoord)
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(float) * 8;  // 2 (pos) + 4 (color) + 2 (texcoord)
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        
        std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};
        
        // Position
        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[0].offset = 0;
        
        // Color
        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributeDescriptions[1].offset = sizeof(float) * 2;
        
        // TexCoord
        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[2].offset = sizeof(float) * 6;
        
        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
        vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();
        
        // Input assembly
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;
        
        // Viewport and scissor
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(m_width);
        viewport.height = static_cast<float>(m_height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        
        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {m_width, m_height};
        
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;
        
        // Rasterizer
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_NONE;  // Saturn doesn't cull
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;
        
        // Multisampling (disabled)
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        
        // Color blending (per-attachment)
        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        
        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;
        
        // Push constants (for per-draw uniforms)
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = 16;  // vec2 screenSize + 2x uint32
        
        // Pipeline layout
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
        
        if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
            return false;
        }
        
        // Create graphics pipeline
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.layout = m_pipelineLayout;
        pipelineInfo.renderPass = m_renderPass;
        pipelineInfo.subpass = 0;
        
        if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_graphicsPipeline) != VK_SUCCESS) {
            return false;
        }
        
        return true;
    }
    
    std::vector<char> ReadShaderFile(const std::string& filename) {
        std::ifstream file(filename, std::ios::ate | std::ios::binary);
        
        if (!file.is_open()) {
            return {};
        }
        
        size_t fileSize = static_cast<size_t>(file.tellg());
        std::vector<char> buffer(fileSize);
        
        file.seekg(0);
        file.read(buffer.data(), fileSize);
        file.close();
        
        return buffer;
    }
    
    VkShaderModule CreateShaderModule(const std::vector<char>& code) {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        
        VkShaderModule shaderModule;
        if (vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        
        return shaderModule;
    }
};

// Factory function
std::unique_ptr<IVDPRenderer> CreateVulkanRenderer() {
    return std::make_unique<VulkanRenderer>();
}

} // namespace brimir::vdp

#endif // BRIMIR_GPU_VULKAN_ENABLED

