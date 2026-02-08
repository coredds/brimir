// Brimir - Vulkan Renderer Implementation
// GPU-accelerated rendering using Vulkan

#ifdef BRIMIR_GPU_VULKAN_ENABLED

#include <brimir/hw/vdp/vdp_renderer.hpp>
#include <brimir/hw/vdp/vdp_defs.hpp>
#include "shaders/embedded_shaders.hpp"
#include <array>
#include <vector>
#include <string>
#include <stdexcept>
#include <fstream>
#include <cstring>
#include <algorithm>

#ifdef BRIMIR_GPU_VULKAN_ENABLED
#include <vulkan/vulkan.h>
#endif

namespace brimir::vdp {

// Helper: Convert Saturn Color555 to GPU RGBA float
// Matches VDP's ConvertRGB555to888() from vdp_defs.hpp line 74
inline void Color555ToFloat(Color555 color, float& r, float& g, float& b, float& a) {
    // Saturn uses 5-bit RGB (0-31 range)
    // VDP expands by left-shifting by 3: val << 3 (0-248 range)
    // Convert to float: (val << 3) / 255.0
    // This matches the real VDP behavior exactly
    r = static_cast<float>(color.r << 3) / 255.0f;
    g = static_cast<float>(color.g << 3) / 255.0f;
    b = static_cast<float>(color.b << 3) / 255.0f;
    // For solid-color polygons, always use full opacity
    // MSB bit is used for texture transparency, not vertex colors
    a = 1.0f;
}

// Make VulkanRenderer visible for tests
class VulkanRenderer : public IVDPRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer() override {
        if (m_initialized) {
            Shutdown();
        }
    }
    
    // ===== Lifecycle =====
    
    bool Initialize() override {
        // Already initialized - return success
        if (m_initialized) {
            return true;
        }
        
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
            
            // Step 8: Create vertex buffer
            if (!CreateVertexBuffer()) {
                Shutdown();
                return false;
            }
            
            // Step 9: Allocate command buffer
            if (!AllocateCommandBuffer()) {
                Shutdown();
                return false;
            }
            
            // Step 10: Create sprite texture resources
            if (!CreateSpriteTextureResources()) {
                Shutdown();
                return false;
            }
            
            // Step 11: Create descriptor resources
            if (!CreateDescriptorResources()) {
                Shutdown();
                return false;
            }
            
            // Step 12: Create textured pipeline
            if (!CreateTexturedPipeline()) {
                Shutdown();
                return false;
            }
            
            // Step 13: Create upscale resources for hybrid mode
            if (!CreateUpscaleResources()) {
                // Non-fatal - upscaling just won't work
                m_lastError = "";  // Clear error, this is optional
            }
            
            // Allocate CPU-side framebuffer for readback
            m_framebuffer.resize(m_width * m_height);
            
            // Reserve space for vertices (typical VDP1 frame has ~1000 polygons)
            m_vertices.reserve(6000);  // ~1000 quads = 6000 vertices
            m_texturedVertices.reserve(6000);
            
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
            
            if (m_renderPassLoad) {
                vkDestroyRenderPass(m_device, m_renderPassLoad, nullptr);
                m_renderPassLoad = VK_NULL_HANDLE;
            }
            
            if (m_vkFramebufferLoad) {
                vkDestroyFramebuffer(m_device, m_vkFramebufferLoad, nullptr);
                m_vkFramebufferLoad = VK_NULL_HANDLE;
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
            
            // Destroy vertex buffer
            if (m_vertexBuffer) {
                vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
                m_vertexBuffer = VK_NULL_HANDLE;
            }
            
            if (m_vertexBufferMemory) {
                vkFreeMemory(m_device, m_vertexBufferMemory, nullptr);
                m_vertexBufferMemory = VK_NULL_HANDLE;
            }
            
            // Destroy textured pipeline resources
            if (m_texturedPipeline) {
                vkDestroyPipeline(m_device, m_texturedPipeline, nullptr);
                m_texturedPipeline = VK_NULL_HANDLE;
            }
            if (m_texturedPipelineLayout) {
                vkDestroyPipelineLayout(m_device, m_texturedPipelineLayout, nullptr);
                m_texturedPipelineLayout = VK_NULL_HANDLE;
            }
            if (m_texturedVertShaderModule) {
                vkDestroyShaderModule(m_device, m_texturedVertShaderModule, nullptr);
                m_texturedVertShaderModule = VK_NULL_HANDLE;
            }
            if (m_texturedFragShaderModule) {
                vkDestroyShaderModule(m_device, m_texturedFragShaderModule, nullptr);
                m_texturedFragShaderModule = VK_NULL_HANDLE;
            }
            
            // Destroy descriptor resources
            if (m_descriptorPool) {
                vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
                m_descriptorPool = VK_NULL_HANDLE;
            }
            if (m_descriptorSetLayout) {
                vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
                m_descriptorSetLayout = VK_NULL_HANDLE;
            }
            
            // Destroy sprite texture resources
            if (m_spriteSampler) {
                vkDestroySampler(m_device, m_spriteSampler, nullptr);
                m_spriteSampler = VK_NULL_HANDLE;
            }
            if (m_spriteTextureView) {
                vkDestroyImageView(m_device, m_spriteTextureView, nullptr);
                m_spriteTextureView = VK_NULL_HANDLE;
            }
            if (m_spriteTexture) {
                vkDestroyImage(m_device, m_spriteTexture, nullptr);
                m_spriteTexture = VK_NULL_HANDLE;
            }
            if (m_spriteTextureMemory) {
                vkFreeMemory(m_device, m_spriteTextureMemory, nullptr);
                m_spriteTextureMemory = VK_NULL_HANDLE;
            }
            if (m_textureStagingBuffer) {
                vkDestroyBuffer(m_device, m_textureStagingBuffer, nullptr);
                m_textureStagingBuffer = VK_NULL_HANDLE;
            }
            if (m_textureStagingMemory) {
                vkFreeMemory(m_device, m_textureStagingMemory, nullptr);
                m_textureStagingMemory = VK_NULL_HANDLE;
            }
            
            // Cleanup upscale resources
            DestroyUpscaleResources();
            
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
        
        // Check if framebuffer needs to be resized
        const uint32_t targetWidth = m_width * m_internalScale;
        const uint32_t targetHeight = m_height * m_internalScale;
        
        if (m_framebufferDirty || m_scaledWidth != targetWidth || m_scaledHeight != targetHeight) {
            ResizeFramebuffer(targetWidth, targetHeight);
            m_framebufferDirty = false;
        }
        
        // Reset command buffer
        vkResetCommandBuffer(m_commandBuffer, 0);
        
        // Begin command buffer
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        
        vkBeginCommandBuffer(m_commandBuffer, &beginInfo);
        
        // Begin render pass with scaled dimensions
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_renderPass;
        renderPassInfo.framebuffer = m_vkFramebuffer;
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = {m_scaledWidth, m_scaledHeight};
        
        // Clear to transparent black (so VDP1 content can be overlaid on VDP2)
        VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 0.0f}}};
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearColor;
        
        vkCmdBeginRenderPass(m_commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        
        // Set viewport to scaled dimensions
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(m_scaledWidth);
        viewport.height = static_cast<float>(m_scaledHeight);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(m_commandBuffer, 0, 1, &viewport);
        
        // Set scissor to scaled dimensions
        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {m_scaledWidth, m_scaledHeight};
        vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);
        
        // Bind pipeline
        vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline);
        
        // Bind vertex buffer
        VkBuffer vertexBuffers[] = {m_vertexBuffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(m_commandBuffer, 0, 1, vertexBuffers, offsets);
        
        // Clear vertices and textured sprites for this frame
        m_vertices.clear();
        m_texturedVertices.clear();
        m_pendingTexturedSprites.clear();
    }
    
    void EndFrame() override {
        if (!m_initialized) return;
        
        // Draw untextured polygons first (solid color and Gouraud shaded)
        if (!m_vertices.empty()) {
            UploadVertexData();
            
            // Use scaled dimensions for high-res rendering
            const uint32_t scaledWidth = m_width * m_internalScale;
            const uint32_t scaledHeight = m_height * m_internalScale;
            
            PushConstants pc;
            pc.screenSize[0] = static_cast<float>(scaledWidth);
            pc.screenSize[1] = static_cast<float>(scaledHeight);
            pc.flags = 0;  // Untextured
            pc.priority = 0;
            
            vkCmdPushConstants(m_commandBuffer, m_pipelineLayout,
                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, sizeof(PushConstants), &pc);
            
            vkCmdDraw(m_commandBuffer, static_cast<uint32_t>(m_vertices.size()), 1, 0, 0);
        }
        
        // Draw textured sprites using texture atlas (single GPU submission)
        if (!m_pendingTexturedSprites.empty() && m_texturedPipeline) {
            // Build texture atlas and collect all vertices
            BuildTextureAtlasAndRender();
        }
        
        // End render pass and submit
        vkCmdEndRenderPass(m_commandBuffer);
        vkEndCommandBuffer(m_commandBuffer);
        
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_commandBuffer;
        
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        
        // Copy image to staging for CPU readback
        CopyFramebufferToCPU();
        
        m_pendingTexturedSprites.clear();
        
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
        return m_scaledWidth;
    }
    
    uint32_t GetFramebufferHeight() const override {
        return m_scaledHeight;
    }
    
    uint32_t GetFramebufferPitch() const override {
        return m_scaledWidth * 4;  // XRGB8888 = 4 bytes per pixel
    }
    
    FramebufferFormat GetFramebufferFormat() const override {
        return FramebufferFormat::XRGB8888;
    }
    
    // ===== GPU Upscaling (Hybrid Mode) =====
    
    void UploadSoftwareFramebuffer(const uint32_t* data, uint32_t width, 
                                   uint32_t height, uint32_t pitch) override {
        if (!m_initialized || !data || width == 0 || height == 0) return;
        if (!m_softwareInputStaging || !m_softwareInputTexture) return;
        
        m_softwareWidth = width;
        m_softwareHeight = height;
        m_softwarePitch = pitch;
        
        // Copy data to staging buffer - direct memcpy since B8G8R8A8 matches XRGB8888 layout
        void* mappedData = nullptr;
        if (vkMapMemory(m_device, m_softwareInputStagingMemory, 0, 
                        width * height * 4, 0, &mappedData) != VK_SUCCESS) {
            return;
        }
        
        const uint32_t dstPitch = width * 4;
        if (pitch == dstPitch) {
            // Pitches match - single fast memcpy
            std::memcpy(mappedData, data, static_cast<size_t>(width) * height * 4);
        } else {
            // Different pitches - copy row by row
            uint8_t* dst = static_cast<uint8_t*>(mappedData);
            const uint8_t* src = reinterpret_cast<const uint8_t*>(data);
            for (uint32_t y = 0; y < height; ++y) {
                std::memcpy(dst + y * dstPitch, src + y * pitch, dstPitch);
            }
        }
        
        vkUnmapMemory(m_device, m_softwareInputStagingMemory);
        
        // Transfer from staging buffer to GPU texture
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        
        vkResetCommandBuffer(m_commandBuffer, 0);
        vkBeginCommandBuffer(m_commandBuffer, &beginInfo);
        
        // Transition to transfer destination
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_softwareInputTexture;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        
        vkCmdPipelineBarrier(m_commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        
        // Copy buffer to image
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = width;
        region.bufferImageHeight = height;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {width, height, 1};
        
        vkCmdCopyBufferToImage(m_commandBuffer, m_softwareInputStaging,
            m_softwareInputTexture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        
        // Transition to shader read
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        
        vkCmdPipelineBarrier(m_commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        
        // DON'T submit yet - RenderUpscaled will continue this command buffer!
        // The texture is now ready for shader sampling
        m_softwareFramebufferDirty = true;
        m_statistics.textureUploadCount++;
        
        // Note: We keep the command buffer open for RenderUpscaled to continue
    }
    
    bool RenderUpscaled() override {
        if (!m_initialized || !m_softwareFramebufferDirty) {
            static bool s_logOnce1 = false;
            if (!s_logOnce1) { printf("[VkUpscale] FAIL: init=%d dirty=%d\n", m_initialized, m_softwareFramebufferDirty); s_logOnce1 = true; }
            return false;
        }
        
        if (m_softwareWidth == 0 || m_softwareHeight == 0) {
            static bool s_logOnce2 = false;
            if (!s_logOnce2) { printf("[VkUpscale] FAIL: sw size %ux%u\n", m_softwareWidth, m_softwareHeight); s_logOnce2 = true; }
            return false;
        }
        
        if (!m_upscalePipeline || !m_upscaleRenderPass) {
            static bool s_logOnce3 = false;
            if (!s_logOnce3) { printf("[VkUpscale] FAIL: pipeline=%p renderPass=%p\n", (void*)m_upscalePipeline, (void*)m_upscaleRenderPass); s_logOnce3 = true; }
            m_softwareFramebufferDirty = false;
            return false;  // Pipeline not ready, use software framebuffer
        }
        
        // Calculate upscaled dimensions
        const uint32_t scale = m_internalScale > 0 ? m_internalScale : 1;
        const uint32_t upscaledWidth = m_softwareWidth * scale;
        const uint32_t upscaledHeight = m_softwareHeight * scale;
        
        // Clamp to max size
        const uint32_t finalWidth = std::min(upscaledWidth, kMaxUpscaleWidth);
        const uint32_t finalHeight = std::min(upscaledHeight, kMaxUpscaleHeight);
        
        // Recreate output image if size changed
        if (m_currentUpscaleWidth != finalWidth || m_currentUpscaleHeight != finalHeight) {
            // Need to submit pending commands and wait before recreating
            vkEndCommandBuffer(m_commandBuffer);
            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &m_commandBuffer;
            vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
            vkQueueWaitIdle(m_graphicsQueue);
            
            if (!CreateUpscaleOutputImage(finalWidth, finalHeight)) {
                m_softwareFramebufferDirty = false;
                return false;
            }
            m_currentUpscaleWidth = finalWidth;
            m_currentUpscaleHeight = finalHeight;
            
            // Start fresh command buffer since we had to sync
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkResetCommandBuffer(m_commandBuffer, 0);
            vkBeginCommandBuffer(m_commandBuffer, &beginInfo);
        }
        
        if (!m_upscaledOutputImage || !m_upscaleFramebuffer) {
            vkEndCommandBuffer(m_commandBuffer);
            m_softwareFramebufferDirty = false;
            return false;
        }
        
        // Determine if second pass (FXAA or RCAS) should run
        const bool doFXAA = m_fxaaEnabled && m_fxaaResourcesCreated && m_fxaaDescriptorSet &&
                            ((m_sharpeningMode == 1 && m_fxaaPipeline) || (m_sharpeningMode == 2 && m_rcasPipeline));
        
        // Continue command buffer (started in UploadSoftwareFramebuffer, or fresh if resized)
        // Begin upscale render pass
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        
        if (doFXAA) {
            // When FXAA is enabled: upscale renders to intermediate target
            renderPassInfo.renderPass = m_fxaaIntermediateRenderPass;
            renderPassInfo.framebuffer = m_fxaaIntermediateFramebuffer;
        } else {
            // When FXAA is disabled: upscale renders directly to final output
            renderPassInfo.renderPass = m_upscaleRenderPass;
            renderPassInfo.framebuffer = m_upscaleFramebuffer;
        }
        renderPassInfo.renderArea.extent = {finalWidth, finalHeight};
        
        vkCmdBeginRenderPass(m_commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        
        // Bind upscale pipeline
        vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_upscalePipeline);
        
        // Set viewport and scissor
        VkViewport viewport{};
        viewport.width = static_cast<float>(finalWidth);
        viewport.height = static_cast<float>(finalHeight);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(m_commandBuffer, 0, 1, &viewport);
        
        VkRect2D scissor{};
        scissor.extent = {finalWidth, finalHeight};
        vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);
        
        // Bind descriptor set
        vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_upscalePipelineLayout, 0, 1, &m_upscaleDescriptorSet, 0, nullptr);
        
        // Set push constants
        struct UpscalePushConstants {
            float inputWidth, inputHeight;
            float outputWidth, outputHeight;
            uint32_t filterMode;
            float brightness;
            float gamma;
            uint32_t debanding;
        } pushConstants;
        
        pushConstants.inputWidth = static_cast<float>(m_softwareWidth);
        pushConstants.inputHeight = static_cast<float>(m_softwareHeight);
        pushConstants.outputWidth = static_cast<float>(finalWidth);
        pushConstants.outputHeight = static_cast<float>(finalHeight);
        pushConstants.filterMode = m_upscaleFilterMode;
        pushConstants.brightness = m_brightnessValue;
        pushConstants.gamma = m_gammaValue;
        pushConstants.debanding = m_debandingEnabled ? 1 : 0;
        
        vkCmdPushConstants(m_commandBuffer, m_upscalePipelineLayout,
            VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushConstants), &pushConstants);
        
        // Draw fullscreen triangle (3 vertices, generated in shader)
        vkCmdDraw(m_commandBuffer, 3, 1, 0, 0);
        
        vkCmdEndRenderPass(m_commandBuffer);
        
        // === Second Pass: FXAA or RCAS (optional) ===
        if (doFXAA) {
            // Begin second pass (renders from intermediate to final output)
            VkRenderPassBeginInfo secondPassInfo{};
            secondPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            secondPassInfo.renderPass = m_fxaaRenderPass;
            secondPassInfo.framebuffer = m_fxaaOutputFramebuffer;
            secondPassInfo.renderArea.extent = {finalWidth, finalHeight};
            
            vkCmdBeginRenderPass(m_commandBuffer, &secondPassInfo, VK_SUBPASS_CONTENTS_INLINE);
            
            // Select pipeline: FXAA (mode 1) or RCAS (mode 2)
            VkPipeline secondPassPipeline = (m_sharpeningMode == 2) ? m_rcasPipeline : m_fxaaPipeline;
            vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, secondPassPipeline);
            
            vkCmdSetViewport(m_commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);
            
            // Bind descriptor set (samples intermediate texture)
            vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_upscalePipelineLayout, 0, 1, &m_fxaaDescriptorSet, 0, nullptr);
            
            // Push constants (outputSize matters for texel size calculation)
            vkCmdPushConstants(m_commandBuffer, m_upscalePipelineLayout,
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushConstants), &pushConstants);
            
            vkCmdDraw(m_commandBuffer, 3, 1, 0, 0);
            
            vkCmdEndRenderPass(m_commandBuffer);
        }
        
        // VDP1 inline overlay was removed (quad-as-triangle issues).
        // GPU post-processing (FSR) is used instead.
        
        // Copy render target to staging buffer for CPU readback
        if (m_upscaledStagingBuffer) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = m_upscaledOutputImage;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            
            vkCmdPipelineBarrier(m_commandBuffer,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);
            
            // Copy image to staging buffer
            VkBufferImageCopy copyRegion{};
            copyRegion.bufferOffset = 0;
            copyRegion.bufferRowLength = finalWidth;
            copyRegion.bufferImageHeight = finalHeight;
            copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copyRegion.imageOffset = {0, 0, 0};
            copyRegion.imageExtent = {finalWidth, finalHeight, 1};
            
            vkCmdCopyImageToBuffer(m_commandBuffer,
                m_upscaledOutputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                m_upscaledStagingBuffer, 1, &copyRegion);
        }
        
        vkEndCommandBuffer(m_commandBuffer);
        
        // Submit and wait
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_commandBuffer;
        
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        
        // Readback to CPU buffer - direct memcpy since B8G8R8A8 matches XRGB8888
        bool readbackOk = false;
        if (m_upscaledStagingBuffer) {
            const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(finalWidth) * finalHeight * 4;
            
            void* mappedData = nullptr;
            if (vkMapMemory(m_device, m_upscaledStagingMemory, 0,
                            bufferSize, 0, &mappedData) == VK_SUCCESS) {
                
                // If using HOST_CACHED (non-coherent), invalidate AFTER mapping
                // so the CPU cache sees the GPU-written data
                if (m_upscaledStagingIsCached) {
                    VkMappedMemoryRange range{};
                    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                    range.memory = m_upscaledStagingMemory;
                    range.offset = 0;
                    range.size = VK_WHOLE_SIZE;
                    vkInvalidateMappedMemoryRanges(m_device, 1, &range);
                }
                
                m_upscaledFramebuffer.resize(finalWidth * finalHeight);
                
                // B8G8R8A8 in memory is [B,G,R,A] = XRGB8888 on little-endian
                // No per-pixel conversion needed - just memcpy
                std::memcpy(m_upscaledFramebuffer.data(), mappedData,
                            static_cast<size_t>(finalWidth) * finalHeight * sizeof(uint32_t));
                
                vkUnmapMemory(m_device, m_upscaledStagingMemory);
                readbackOk = true;
            }
        }
        
        m_softwareFramebufferDirty = false;
        
        if (readbackOk) {
            m_upscaledReady = true;
            return true;
        } else {
            // Readback failed - don't mark as ready, fall back to software framebuffer
            m_upscaledReady = false;
            return false;
        }
    }
    
    const void* GetUpscaledFramebuffer() const override {
        if (!m_upscaledReady || m_upscaledFramebuffer.empty()) {
            return nullptr;
        }
        return m_upscaledFramebuffer.data();
    }
    
    uint32_t GetUpscaledWidth() const override {
        // Return actual upscaled dimensions, not computed from m_outputWidth
        return m_currentUpscaleWidth > 0 ? m_currentUpscaleWidth : m_softwareWidth * m_internalScale;
    }
    
    uint32_t GetUpscaledHeight() const override {
        // Return actual upscaled dimensions, not computed from m_outputHeight
        return m_currentUpscaleHeight > 0 ? m_currentUpscaleHeight : m_softwareHeight * m_internalScale;
    }
    
    uint32_t GetUpscaledPitch() const override {
        return GetUpscaledWidth() * sizeof(uint32_t);
    }
    
    // ===== Configuration =====
    
    void SetInternalScale(uint32_t scale) override {
        // Clamp to supported range
        if (scale < 1) scale = 1;
        if (scale > 8) scale = 8;
        
        if (m_internalScale == scale) return;
        
        m_internalScale = scale;
        m_framebufferDirty = true;  // Mark for resize on next frame
    }
    
    uint32_t GetInternalScale() const override {
        return m_internalScale;
    }
    
    void SetOutputResolution(uint32_t width, uint32_t height) override {
        if (width == 0 || height == 0) return;
        
        m_outputWidth = width;
        m_outputHeight = height;
        
        // TODO: Recreate output framebuffer at new resolution
        // This is separate from internal scale - output resolution
        // determines final presentation size
    }
    
    void SetUpscaleFilter(uint32_t mode) override {
        m_upscaleFilterMode = mode;
    }
    
    
    void SetDebanding(bool enable) override {
        m_debandingEnabled = enable;
    }
    
    
    void SetBrightness(float brightness) override {
        m_brightnessValue = brightness;
    }
    
    void SetGamma(float gamma) override {
        m_gammaValue = gamma;
    }
    
    void SetFXAA(bool enable) override {
        // Legacy interface: enable/disable second pass
        SetSharpeningMode(enable ? 1 : 0);
    }
    
    void SetSharpeningMode(uint32_t mode) override {
        // mode: 0 = off, 1 = FXAA, 2 = RCAS
        bool needSecondPass = (mode > 0);
        bool hadSecondPass = m_fxaaEnabled;
        
        m_sharpeningMode = mode;
        m_fxaaEnabled = needSecondPass;
        
        // Trigger resource creation/destruction on next frame
        if (m_currentUpscaleWidth > 0 && m_currentUpscaleHeight > 0) {
            if (needSecondPass && !m_fxaaResourcesCreated) {
                CreateFXAAResources(m_currentUpscaleWidth, m_currentUpscaleHeight);
            } else if (!needSecondPass && m_fxaaResourcesCreated) {
                vkQueueWaitIdle(m_graphicsQueue);
                DestroyFXAAResources();
            }
        }
    }
    
    void SetWireframeMode(bool enable) override {
        // TODO: Recreate pipeline with VK_POLYGON_MODE_LINE
        (void)enable;
    }
    
    void SetHWContext(void* hw_context) override {
        // Store hardware context for potential use with frontend's Vulkan instance
        // For now, we create our own Vulkan instance for headless rendering
        // In the future, this could be used to share the frontend's Vulkan device
        m_hwContext = hw_context;
    }
    
    const char* GetLastError() const override {
        return m_lastError.c_str();
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
    uint32_t m_width = 320;           // Base (native) width
    uint32_t m_height = 224;          // Base (native) height  
    uint32_t m_scaledWidth = 320;     // Actual framebuffer width (m_width * m_internalScale)
    uint32_t m_scaledHeight = 224;    // Actual framebuffer height (m_height * m_internalScale)
    uint32_t m_pitch = 1280;
    uint32_t m_internalScale = 1;
    uint32_t m_outputWidth = 320;     // Final output resolution
    uint32_t m_outputHeight = 224;
    bool m_framebufferDirty = false;  // Framebuffer needs resize
    bool m_textureFiltering = false;
    uint32_t m_msaaSamples = 1;
    std::string m_lastError;
    
    // GPU post-processing settings
    uint32_t m_upscaleFilterMode = 2;   // 0=nearest, 1=bilinear, 2=sharp bilinear
    bool m_debandingEnabled = false;
    float m_brightnessValue = 1.0f;
    float m_gammaValue = 1.0f;
    bool m_fxaaEnabled = false;
    uint32_t m_sharpeningMode = 0;  // 0 = off, 1 = FXAA, 2 = RCAS
    void* m_hwContext = nullptr;  // Frontend's hardware context (unused for now)
    
    // GPU Upscaling state (Hybrid mode - software rendering + GPU upscaling)
    uint32_t m_softwareWidth = 0;
    uint32_t m_softwareHeight = 0;
    uint32_t m_softwarePitch = 0;
    bool m_softwareFramebufferDirty = false;
    bool m_upscaledReady = false;
    std::vector<uint32_t> m_upscaledFramebuffer;  // Upscaled output buffer
    
    // Upscaling Vulkan resources
    VkImage m_softwareInputTexture = VK_NULL_HANDLE;
    VkDeviceMemory m_softwareInputMemory = VK_NULL_HANDLE;
    VkImageView m_softwareInputView = VK_NULL_HANDLE;
    VkSampler m_upscaleSampler = VK_NULL_HANDLE;
    VkBuffer m_softwareInputStaging = VK_NULL_HANDLE;
    VkDeviceMemory m_softwareInputStagingMemory = VK_NULL_HANDLE;
    
    VkImage m_upscaledOutputImage = VK_NULL_HANDLE;
    VkDeviceMemory m_upscaledOutputMemory = VK_NULL_HANDLE;
    VkImageView m_upscaledOutputView = VK_NULL_HANDLE;
    VkBuffer m_upscaledStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_upscaledStagingMemory = VK_NULL_HANDLE;
    bool m_upscaledStagingIsCached = false;
    
    VkRenderPass m_upscaleRenderPass = VK_NULL_HANDLE;
    VkFramebuffer m_upscaleFramebuffer = VK_NULL_HANDLE;
    VkPipelineLayout m_upscalePipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_upscalePipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_upscaleDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_upscaleDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_upscaleDescriptorSet = VK_NULL_HANDLE;
    
    static constexpr uint32_t kMaxUpscaleWidth = 2816;   // 704 * 4
    static constexpr uint32_t kMaxUpscaleHeight = 2048;  // 512 * 4
    uint32_t m_currentUpscaleWidth = 0;
    uint32_t m_currentUpscaleHeight = 0;
    
    // FXAA resources (second pass: intermediate -> final output)
    VkImage m_fxaaIntermediateImage = VK_NULL_HANDLE;
    VkDeviceMemory m_fxaaIntermediateMemory = VK_NULL_HANDLE;
    VkImageView m_fxaaIntermediateView = VK_NULL_HANDLE;
    VkRenderPass m_fxaaIntermediateRenderPass = VK_NULL_HANDLE;   // upscale -> intermediate (finalLayout=SHADER_READ_ONLY)
    VkFramebuffer m_fxaaIntermediateFramebuffer = VK_NULL_HANDLE;
    VkRenderPass m_fxaaRenderPass = VK_NULL_HANDLE;               // FXAA -> final output (finalLayout=TRANSFER_SRC)
    VkFramebuffer m_fxaaOutputFramebuffer = VK_NULL_HANDLE;       // final output framebuffer for FXAA pass
    VkPipeline m_fxaaPipeline = VK_NULL_HANDLE;
    VkPipeline m_rcasPipeline = VK_NULL_HANDLE;        // FSR 1.0 RCAS sharpening pipeline
    VkDescriptorPool m_fxaaDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_fxaaDescriptorSet = VK_NULL_HANDLE;
    VkSampler m_fxaaSampler = VK_NULL_HANDLE;
    bool m_fxaaResourcesCreated = false;
    
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
    VkRenderPass m_renderPass = VK_NULL_HANDLE;          // Clears framebuffer
    VkRenderPass m_renderPassLoad = VK_NULL_HANDLE;      // Loads existing framebuffer content
    VkFramebuffer m_vkFramebuffer = VK_NULL_HANDLE;
    VkFramebuffer m_vkFramebufferLoad = VK_NULL_HANDLE;  // For load render pass
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    
    // Pipeline resources
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_graphicsPipeline = VK_NULL_HANDLE;
    VkShaderModule m_vertShaderModule = VK_NULL_HANDLE;
    VkShaderModule m_fragShaderModule = VK_NULL_HANDLE;
    
    // Vertex buffer resources
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vertexBufferMemory = VK_NULL_HANDLE;
    size_t m_vertexBufferSize = 0;
    
    // VDP1 vertex data
    struct VDP1Vertex {
        float pos[2];          // x, y (location 0)
        float color[4];        // r, g, b, a (location 1)
        float texCoord[2];     // u, v (location 2)
    };
    std::vector<VDP1Vertex> m_vertices;
    std::vector<VDP1Vertex> m_texturedVertices;  // Separate buffer for textured draws
    
    // Push constants structure
    struct PushConstants {
        float screenSize[2];
        uint32_t flags;
        uint32_t priority;
    };
    
    // Textured rendering constants
    static constexpr uint32_t FLAG_TEXTURED = 1u << 0;
    
    // Sprite texture resources
    VkImage m_spriteTexture = VK_NULL_HANDLE;
    VkDeviceMemory m_spriteTextureMemory = VK_NULL_HANDLE;
    VkImageView m_spriteTextureView = VK_NULL_HANDLE;
    VkSampler m_spriteSampler = VK_NULL_HANDLE;
    VkBuffer m_textureStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_textureStagingMemory = VK_NULL_HANDLE;
    uint32_t m_spriteTextureWidth = 0;
    uint32_t m_spriteTextureHeight = 0;
    static constexpr uint32_t kMaxSpriteTextureSize = 512;  // Max 512x512 sprite texture
    
    // Descriptor resources for texturing
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    
    // Textured pipeline (separate from untextured)
    VkPipelineLayout m_texturedPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_texturedPipeline = VK_NULL_HANDLE;
    VkShaderModule m_texturedVertShaderModule = VK_NULL_HANDLE;
    VkShaderModule m_texturedFragShaderModule = VK_NULL_HANDLE;
    
    // Per-sprite texture tracking for batching
    struct TexturedSprite {
        VDP1Vertex vertices[6];  // 2 triangles
        std::vector<uint32_t> textureData;
        uint32_t texWidth;
        uint32_t texHeight;
    };
    std::vector<TexturedSprite> m_pendingTexturedSprites;
    
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
    
    void ResizeFramebuffer(uint32_t width, uint32_t height) {
        if (width == 0 || height == 0) return;
        if (width == m_scaledWidth && height == m_scaledHeight) return;
        
        vkDeviceWaitIdle(m_device);
        
        // Destroy old framebuffer resources
        if (m_vkFramebuffer) {
            vkDestroyFramebuffer(m_device, m_vkFramebuffer, nullptr);
            m_vkFramebuffer = VK_NULL_HANDLE;
        }
        if (m_vkFramebufferLoad) {
            vkDestroyFramebuffer(m_device, m_vkFramebufferLoad, nullptr);
            m_vkFramebufferLoad = VK_NULL_HANDLE;
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
        
        // Update dimensions
        m_scaledWidth = width;
        m_scaledHeight = height;
        m_pitch = m_scaledWidth * 4;
        
        // Recreate image resources at new size
        CreateFramebufferResources();
        
        // Recreate Vulkan framebuffers for render passes
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &m_framebufferView;
        framebufferInfo.width = m_scaledWidth;
        framebufferInfo.height = m_scaledHeight;
        framebufferInfo.layers = 1;
        
        vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_vkFramebuffer);
        
        // Create framebuffer for load render pass
        framebufferInfo.renderPass = m_renderPassLoad;
        vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_vkFramebufferLoad);
        
        // Resize CPU framebuffer
        m_framebuffer.resize(m_scaledWidth * m_scaledHeight);
    }
    
    bool CreateFramebufferResources() {
        // Create framebuffer image (R8G8B8A8) at scaled dimensions
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = m_scaledWidth;
        imageInfo.extent.height = m_scaledHeight;
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
        // Render pass that clears the framebuffer (used at frame start)
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        
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
        
        // Render pass that loads existing content (used for textured overlay)
        VkAttachmentDescription colorAttachmentLoad{};
        colorAttachmentLoad.format = VK_FORMAT_R8G8B8A8_UNORM;
        colorAttachmentLoad.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachmentLoad.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // Preserve existing content
        colorAttachmentLoad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachmentLoad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachmentLoad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachmentLoad.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachmentLoad.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        
        VkRenderPassCreateInfo renderPassLoadInfo{};
        renderPassLoadInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassLoadInfo.attachmentCount = 1;
        renderPassLoadInfo.pAttachments = &colorAttachmentLoad;
        renderPassLoadInfo.subpassCount = 1;
        renderPassLoadInfo.pSubpasses = &subpass;
        
        if (vkCreateRenderPass(m_device, &renderPassLoadInfo, nullptr, &m_renderPassLoad) != VK_SUCCESS) {
            return false;
        }
        
        // Create framebuffer for clear render pass
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
        
        // Create framebuffer for load render pass
        framebufferInfo.renderPass = m_renderPassLoad;
        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_vkFramebufferLoad) != VK_SUCCESS) {
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
        
        // Transition framebuffer image from COLOR_ATTACHMENT_OPTIMAL to TRANSFER_SRC_OPTIMAL
        VkImageMemoryBarrier fbBarrier{};
        fbBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        fbBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        fbBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        fbBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fbBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fbBarrier.image = m_framebufferImage;
        fbBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        fbBarrier.subresourceRange.baseMipLevel = 0;
        fbBarrier.subresourceRange.levelCount = 1;
        fbBarrier.subresourceRange.baseArrayLayer = 0;
        fbBarrier.subresourceRange.layerCount = 1;
        fbBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        fbBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &fbBarrier);
        
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
        
        // Transition framebuffer back to COLOR_ATTACHMENT_OPTIMAL for next frame
        fbBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        fbBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        fbBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        fbBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &fbBarrier);
        
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
        // GPU VDP1 rendering pipeline removed (approach abandoned)
        // Only GPU upscaling/post-processing pipelines are used now
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
    
    bool CreateVertexBuffer() {
        // Create a dynamic vertex buffer (64KB initial size, can grow)
        m_vertexBufferSize = 64 * 1024;  // 64KB = ~2000 vertices
        
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = m_vertexBufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_vertexBuffer) != VK_SUCCESS) {
            return false;
        }
        
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(m_device, m_vertexBuffer, &memRequirements);
        
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_vertexBufferMemory) != VK_SUCCESS) {
            return false;
        }
        
        vkBindBufferMemory(m_device, m_vertexBuffer, m_vertexBufferMemory, 0);
        
        return true;
    }
    
    bool CreateSpriteTextureResources() {
        // Create sprite texture (max size, will be partially used)
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = kMaxSpriteTextureSize;
        imageInfo.extent.height = kMaxSpriteTextureSize;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_spriteTexture) != VK_SUCCESS) {
            return false;
        }
        
        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(m_device, m_spriteTexture, &memRequirements);
        
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_spriteTextureMemory) != VK_SUCCESS) {
            return false;
        }
        
        vkBindImageMemory(m_device, m_spriteTexture, m_spriteTextureMemory, 0);
        
        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_spriteTexture;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_spriteTextureView) != VK_SUCCESS) {
            return false;
        }
        
        // Create sampler
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_NEAREST;  // Saturn uses nearest filtering
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        
        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_spriteSampler) != VK_SUCCESS) {
            return false;
        }
        
        // Create staging buffer for texture uploads
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = kMaxSpriteTextureSize * kMaxSpriteTextureSize * 4;  // RGBA
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_textureStagingBuffer) != VK_SUCCESS) {
            return false;
        }
        
        VkMemoryRequirements stagingMemReqs;
        vkGetBufferMemoryRequirements(m_device, m_textureStagingBuffer, &stagingMemReqs);
        
        VkMemoryAllocateInfo stagingAllocInfo{};
        stagingAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        stagingAllocInfo.allocationSize = stagingMemReqs.size;
        stagingAllocInfo.memoryTypeIndex = FindMemoryType(stagingMemReqs.memoryTypeBits,
                                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        if (vkAllocateMemory(m_device, &stagingAllocInfo, nullptr, &m_textureStagingMemory) != VK_SUCCESS) {
            return false;
        }
        
        vkBindBufferMemory(m_device, m_textureStagingBuffer, m_textureStagingMemory, 0);
        
        return true;
    }
    
    bool CreateDescriptorResources() {
        // Descriptor set layout
        VkDescriptorSetLayoutBinding samplerBinding{};
        samplerBinding.binding = 0;
        samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBinding.descriptorCount = 1;
        samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &samplerBinding;
        
        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
            return false;
        }
        
        // Descriptor pool
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 1;
        
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = 1;
        
        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
            return false;
        }
        
        // Allocate descriptor set
        VkDescriptorSetAllocateInfo descAllocInfo{};
        descAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descAllocInfo.descriptorPool = m_descriptorPool;
        descAllocInfo.descriptorSetCount = 1;
        descAllocInfo.pSetLayouts = &m_descriptorSetLayout;
        
        if (vkAllocateDescriptorSets(m_device, &descAllocInfo, &m_descriptorSet) != VK_SUCCESS) {
            return false;
        }
        
        // Update descriptor set with texture
        VkDescriptorImageInfo imageDescInfo{};
        imageDescInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageDescInfo.imageView = m_spriteTextureView;
        imageDescInfo.sampler = m_spriteSampler;
        
        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_descriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageDescInfo;
        
        vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);
        
        return true;
    }
    
    bool CreateTexturedPipeline() {
        // GPU VDP1 textured rendering pipeline removed (approach abandoned)
        // Only GPU upscaling/post-processing pipelines are used now
        return true;
    }
    
    // Create resources for GPU upscaling of software framebuffer
    bool CreateUpscaleResources() {
        // Create software input texture (max Saturn resolution: 704x512)
        // Use B8G8R8A8 to match XRGB8888 memory layout (bytes: B,G,R,X on little-endian)
        // This eliminates per-pixel format conversion on upload/readback
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        imageInfo.extent = {704, 512, 1};  // Max Saturn resolution
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        
        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_softwareInputTexture) != VK_SUCCESS) {
            m_lastError = "Failed to create software input texture";
            return false;
        }
        
        // Allocate memory for software input texture
        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_softwareInputTexture, &memReqs);
        
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_softwareInputMemory) != VK_SUCCESS) {
            m_lastError = "Failed to allocate software input memory";
            return false;
        }
        
        vkBindImageMemory(m_device, m_softwareInputTexture, m_softwareInputMemory, 0);
        
        // Create software input view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_softwareInputTexture;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_softwareInputView) != VK_SUCCESS) {
            m_lastError = "Failed to create software input view";
            return false;
        }
        
        // Create staging buffer for software framebuffer upload
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = 704 * 512 * 4;  // Max size
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_softwareInputStaging) != VK_SUCCESS) {
            m_lastError = "Failed to create software input staging buffer";
            return false;
        }
        
        VkMemoryRequirements stagingMemReqs;
        vkGetBufferMemoryRequirements(m_device, m_softwareInputStaging, &stagingMemReqs);
        
        allocInfo.allocationSize = stagingMemReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(stagingMemReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_softwareInputStagingMemory) != VK_SUCCESS) {
            m_lastError = "Failed to allocate software input staging memory";
            return false;
        }
        
        vkBindBufferMemory(m_device, m_softwareInputStaging, m_softwareInputStagingMemory, 0);
        
        // Create upscale sampler (bilinear filtering)
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        
        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_upscaleSampler) != VK_SUCCESS) {
            m_lastError = "Failed to create upscale sampler";
            return false;
        }
        
        // Create upscale descriptor set layout
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        
        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_upscaleDescriptorLayout) != VK_SUCCESS) {
            m_lastError = "Failed to create upscale descriptor layout";
            return false;
        }
        
        // Create upscale descriptor pool
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 1;
        
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = 1;
        
        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_upscaleDescriptorPool) != VK_SUCCESS) {
            m_lastError = "Failed to create upscale descriptor pool";
            return false;
        }
        
        // Allocate upscale descriptor set
        VkDescriptorSetAllocateInfo descAllocInfo{};
        descAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descAllocInfo.descriptorPool = m_upscaleDescriptorPool;
        descAllocInfo.descriptorSetCount = 1;
        descAllocInfo.pSetLayouts = &m_upscaleDescriptorLayout;
        
        if (vkAllocateDescriptorSets(m_device, &descAllocInfo, &m_upscaleDescriptorSet) != VK_SUCCESS) {
            m_lastError = "Failed to allocate upscale descriptor set";
            return false;
        }
        
        // Update descriptor set with software input texture
        VkDescriptorImageInfo imageDescInfo{};
        imageDescInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageDescInfo.imageView = m_softwareInputView;
        imageDescInfo.sampler = m_upscaleSampler;
        
        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_upscaleDescriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageDescInfo;
        
        vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);
        
        // Create upscale render pass (B8G8R8A8 to match XRGB8888)
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = VK_FORMAT_B8G8R8A8_UNORM;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        
        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        
        if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_upscaleRenderPass) != VK_SUCCESS) {
            m_lastError = "Failed to create upscale render pass";
            return false;
        }
        
        // Create upscale pipeline layout with push constants
        VkPushConstantRange pushConstant{};
        pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(float) * 6 + sizeof(uint32_t) * 2;  // inputSize(2f), outputSize(2f), filterMode(u), brightness(f), gamma(f), debanding(u)
        
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_upscaleDescriptorLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstant;
        
        if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_upscalePipelineLayout) != VK_SUCCESS) {
            m_lastError = "Failed to create upscale pipeline layout";
            return false;
        }
        
        // Create shader modules from embedded SPIR-V
        VkShaderModuleCreateInfo vertShaderInfo{};
        vertShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vertShaderInfo.codeSize = shaders::upscale_vert_size;
        vertShaderInfo.pCode = reinterpret_cast<const uint32_t*>(shaders::upscale_vert_data);
        
        VkShaderModule vertModule;
        if (vkCreateShaderModule(m_device, &vertShaderInfo, nullptr, &vertModule) != VK_SUCCESS) {
            m_lastError = "Failed to create upscale vertex shader";
            return false;
        }
        
        VkShaderModuleCreateInfo fragShaderInfo{};
        fragShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        fragShaderInfo.codeSize = shaders::upscale_frag_size;
        fragShaderInfo.pCode = reinterpret_cast<const uint32_t*>(shaders::upscale_frag_data);
        
        VkShaderModule fragModule;
        if (vkCreateShaderModule(m_device, &fragShaderInfo, nullptr, &fragModule) != VK_SUCCESS) {
            vkDestroyShaderModule(m_device, vertModule, nullptr);
            m_lastError = "Failed to create upscale fragment shader";
            return false;
        }
        
        // Create upscale pipeline
        VkPipelineShaderStageCreateInfo shaderStages[2]{};
        shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        shaderStages[0].module = vertModule;
        shaderStages[0].pName = "main";
        shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shaderStages[1].module = fragModule;
        shaderStages[1].pName = "main";
        
        // No vertex input (fullscreen triangle generated in shader)
        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.lineWidth = 1.0f;
        
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        
        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;
        
        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;
        
        VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;
        
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
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = m_upscalePipelineLayout;
        pipelineInfo.renderPass = m_upscaleRenderPass;
        pipelineInfo.subpass = 0;
        
        if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_upscalePipeline) != VK_SUCCESS) {
            vkDestroyShaderModule(m_device, vertModule, nullptr);
            vkDestroyShaderModule(m_device, fragModule, nullptr);
            m_lastError = "Failed to create upscale pipeline";
            return false;
        }
        
        vkDestroyShaderModule(m_device, vertModule, nullptr);
        vkDestroyShaderModule(m_device, fragModule, nullptr);
        
        return true;
    }
    
    // Create FXAA resources (called from CreateUpscaleOutputImage when FXAA is enabled)
    bool CreateFXAAResources(uint32_t width, uint32_t height) {
        DestroyFXAAResources();
        
        // Create intermediate render target (upscale writes here, FXAA reads from it)
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        
        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_fxaaIntermediateImage) != VK_SUCCESS) {
            return false;
        }
        
        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_fxaaIntermediateImage, &memReqs);
        
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_fxaaIntermediateMemory) != VK_SUCCESS) {
            return false;
        }
        
        vkBindImageMemory(m_device, m_fxaaIntermediateImage, m_fxaaIntermediateMemory, 0);
        
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_fxaaIntermediateImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_fxaaIntermediateView) != VK_SUCCESS) {
            return false;
        }
        
        // Render pass for upscale -> intermediate (finalLayout = SHADER_READ_ONLY_OPTIMAL)
        if (!m_fxaaIntermediateRenderPass) {
            VkAttachmentDescription colorAttachment{};
            colorAttachment.format = VK_FORMAT_B8G8R8A8_UNORM;
            colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            
            VkAttachmentReference colorRef{};
            colorRef.attachment = 0;
            colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorRef;
            
            VkSubpassDependency dependency{};
            dependency.srcSubpass = 0;
            dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            
            VkRenderPassCreateInfo renderPassInfo{};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            renderPassInfo.attachmentCount = 1;
            renderPassInfo.pAttachments = &colorAttachment;
            renderPassInfo.subpassCount = 1;
            renderPassInfo.pSubpasses = &subpass;
            renderPassInfo.dependencyCount = 1;
            renderPassInfo.pDependencies = &dependency;
            
            if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_fxaaIntermediateRenderPass) != VK_SUCCESS) {
                return false;
            }
        }
        
        // Render pass for FXAA -> final output (finalLayout = TRANSFER_SRC_OPTIMAL, same as upscale render pass)
        if (!m_fxaaRenderPass) {
            VkAttachmentDescription colorAttachment{};
            colorAttachment.format = VK_FORMAT_B8G8R8A8_UNORM;
            colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            colorAttachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            
            VkAttachmentReference colorRef{};
            colorRef.attachment = 0;
            colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorRef;
            
            VkRenderPassCreateInfo renderPassInfo{};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            renderPassInfo.attachmentCount = 1;
            renderPassInfo.pAttachments = &colorAttachment;
            renderPassInfo.subpassCount = 1;
            renderPassInfo.pSubpasses = &subpass;
            
            if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_fxaaRenderPass) != VK_SUCCESS) {
                return false;
            }
        }
        
        // Create intermediate framebuffer (upscale renders into this)
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = m_fxaaIntermediateRenderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &m_fxaaIntermediateView;
        fbInfo.width = width;
        fbInfo.height = height;
        fbInfo.layers = 1;
        
        if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_fxaaIntermediateFramebuffer) != VK_SUCCESS) {
            return false;
        }
        
        // Create FXAA output framebuffer (FXAA renders to the final upscaled output image)
        fbInfo.renderPass = m_fxaaRenderPass;
        fbInfo.pAttachments = &m_upscaledOutputView;
        
        if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_fxaaOutputFramebuffer) != VK_SUCCESS) {
            return false;
        }
        
        // Create sampler for intermediate texture
        if (!m_fxaaSampler) {
            VkSamplerCreateInfo samplerInfo{};
            samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.anisotropyEnable = VK_FALSE;
            samplerInfo.unnormalizedCoordinates = VK_FALSE;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            
            if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_fxaaSampler) != VK_SUCCESS) {
                return false;
            }
        }
        
        // Create FXAA descriptor pool and set
        if (!m_fxaaDescriptorPool) {
            VkDescriptorPoolSize poolSize{};
            poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            poolSize.descriptorCount = 1;
            
            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &poolSize;
            poolInfo.maxSets = 1;
            
            if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_fxaaDescriptorPool) != VK_SUCCESS) {
                return false;
            }
        }
        
        // Allocate FXAA descriptor set (reuses upscale descriptor layout - same binding)
        VkDescriptorSetAllocateInfo descAllocInfo{};
        descAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descAllocInfo.descriptorPool = m_fxaaDescriptorPool;
        descAllocInfo.descriptorSetCount = 1;
        descAllocInfo.pSetLayouts = &m_upscaleDescriptorLayout;
        
        if (vkAllocateDescriptorSets(m_device, &descAllocInfo, &m_fxaaDescriptorSet) != VK_SUCCESS) {
            return false;
        }
        
        // Update FXAA descriptor set to point to intermediate texture
        VkDescriptorImageInfo imageDescInfo{};
        imageDescInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageDescInfo.imageView = m_fxaaIntermediateView;
        imageDescInfo.sampler = m_fxaaSampler;
        
        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_fxaaDescriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageDescInfo;
        
        vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);
        
        // Create FXAA pipeline (reuses upscale vertex shader + pipeline layout, uses FXAA fragment shader)
        if (!m_fxaaPipeline) {
            VkShaderModuleCreateInfo vertShaderInfo{};
            vertShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            vertShaderInfo.codeSize = shaders::upscale_vert_size;
            vertShaderInfo.pCode = reinterpret_cast<const uint32_t*>(shaders::upscale_vert_data);
            
            VkShaderModule vertModule;
            if (vkCreateShaderModule(m_device, &vertShaderInfo, nullptr, &vertModule) != VK_SUCCESS) {
                return false;
            }
            
            VkShaderModuleCreateInfo fragShaderInfo{};
            fragShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            fragShaderInfo.codeSize = shaders::fxaa_frag_size;
            fragShaderInfo.pCode = reinterpret_cast<const uint32_t*>(shaders::fxaa_frag_data);
            
            VkShaderModule fragModule;
            if (vkCreateShaderModule(m_device, &fragShaderInfo, nullptr, &fragModule) != VK_SUCCESS) {
                vkDestroyShaderModule(m_device, vertModule, nullptr);
                return false;
            }
            
            VkPipelineShaderStageCreateInfo shaderStages[2]{};
            shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            shaderStages[0].module = vertModule;
            shaderStages[0].pName = "main";
            shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            shaderStages[1].module = fragModule;
            shaderStages[1].pName = "main";
            
            VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
            vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            
            VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            
            VkPipelineViewportStateCreateInfo viewportState{};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.scissorCount = 1;
            
            VkPipelineRasterizationStateCreateInfo rasterizer{};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.cullMode = VK_CULL_MODE_NONE;
            rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterizer.lineWidth = 1.0f;
            
            VkPipelineMultisampleStateCreateInfo multisampling{};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            
            VkPipelineColorBlendAttachmentState colorBlendAttachment{};
            colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            colorBlendAttachment.blendEnable = VK_FALSE;
            
            VkPipelineColorBlendStateCreateInfo colorBlending{};
            colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlending.attachmentCount = 1;
            colorBlending.pAttachments = &colorBlendAttachment;
            
            VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamicState{};
            dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamicState.dynamicStateCount = 2;
            dynamicState.pDynamicStates = dynamicStates;
            
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
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.layout = m_upscalePipelineLayout;  // Reuse same layout (same push constants + descriptor)
            pipelineInfo.renderPass = m_fxaaRenderPass;
            pipelineInfo.subpass = 0;
            
            if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_fxaaPipeline) != VK_SUCCESS) {
                vkDestroyShaderModule(m_device, vertModule, nullptr);
                vkDestroyShaderModule(m_device, fragModule, nullptr);
                return false;
            }
            
            vkDestroyShaderModule(m_device, vertModule, nullptr);
            vkDestroyShaderModule(m_device, fragModule, nullptr);
        }
        
        // Create RCAS pipeline (same infrastructure as FXAA, different fragment shader)
        if (!m_rcasPipeline) {
            VkShaderModuleCreateInfo vertShaderInfo{};
            vertShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            vertShaderInfo.codeSize = shaders::upscale_vert_size;
            vertShaderInfo.pCode = reinterpret_cast<const uint32_t*>(shaders::upscale_vert_data);
            
            VkShaderModule vertModule;
            if (vkCreateShaderModule(m_device, &vertShaderInfo, nullptr, &vertModule) != VK_SUCCESS) {
                return false;
            }
            
            VkShaderModuleCreateInfo fragShaderInfo{};
            fragShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            fragShaderInfo.codeSize = shaders::fsr_rcas_frag_size;
            fragShaderInfo.pCode = reinterpret_cast<const uint32_t*>(shaders::fsr_rcas_frag_data);
            
            VkShaderModule fragModule;
            if (vkCreateShaderModule(m_device, &fragShaderInfo, nullptr, &fragModule) != VK_SUCCESS) {
                vkDestroyShaderModule(m_device, vertModule, nullptr);
                return false;
            }
            
            VkPipelineShaderStageCreateInfo shaderStages[2]{};
            shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            shaderStages[0].module = vertModule;
            shaderStages[0].pName = "main";
            shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            shaderStages[1].module = fragModule;
            shaderStages[1].pName = "main";
            
            VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
            vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            
            VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            
            VkPipelineViewportStateCreateInfo viewportState{};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.scissorCount = 1;
            
            VkPipelineRasterizationStateCreateInfo rasterizer{};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.cullMode = VK_CULL_MODE_NONE;
            rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterizer.lineWidth = 1.0f;
            
            VkPipelineMultisampleStateCreateInfo multisampling{};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            
            VkPipelineColorBlendAttachmentState colorBlendAttachment{};
            colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            colorBlendAttachment.blendEnable = VK_FALSE;
            
            VkPipelineColorBlendStateCreateInfo colorBlending{};
            colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlending.attachmentCount = 1;
            colorBlending.pAttachments = &colorBlendAttachment;
            
            VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamicState{};
            dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamicState.dynamicStateCount = 2;
            dynamicState.pDynamicStates = dynamicStates;
            
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
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.layout = m_upscalePipelineLayout;  // Same layout as FXAA
            pipelineInfo.renderPass = m_fxaaRenderPass;      // Same render pass as FXAA
            pipelineInfo.subpass = 0;
            
            if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_rcasPipeline) != VK_SUCCESS) {
                vkDestroyShaderModule(m_device, vertModule, nullptr);
                vkDestroyShaderModule(m_device, fragModule, nullptr);
                return false;
            }
            
            vkDestroyShaderModule(m_device, vertModule, nullptr);
            vkDestroyShaderModule(m_device, fragModule, nullptr);
        }
        
        m_fxaaResourcesCreated = true;
        return true;
    }
    
    void DestroyFXAAResources() {
        // Destroy size-dependent resources
        if (m_fxaaOutputFramebuffer) {
            vkDestroyFramebuffer(m_device, m_fxaaOutputFramebuffer, nullptr);
            m_fxaaOutputFramebuffer = VK_NULL_HANDLE;
        }
        if (m_fxaaIntermediateFramebuffer) {
            vkDestroyFramebuffer(m_device, m_fxaaIntermediateFramebuffer, nullptr);
            m_fxaaIntermediateFramebuffer = VK_NULL_HANDLE;
        }
        if (m_fxaaIntermediateView) {
            vkDestroyImageView(m_device, m_fxaaIntermediateView, nullptr);
            m_fxaaIntermediateView = VK_NULL_HANDLE;
        }
        if (m_fxaaIntermediateImage) {
            vkDestroyImage(m_device, m_fxaaIntermediateImage, nullptr);
            m_fxaaIntermediateImage = VK_NULL_HANDLE;
        }
        if (m_fxaaIntermediateMemory) {
            vkFreeMemory(m_device, m_fxaaIntermediateMemory, nullptr);
            m_fxaaIntermediateMemory = VK_NULL_HANDLE;
        }
        // Reset descriptor set (pool will be reset)
        m_fxaaDescriptorSet = VK_NULL_HANDLE;
        if (m_fxaaDescriptorPool) {
            vkResetDescriptorPool(m_device, m_fxaaDescriptorPool, 0);
        }
        m_fxaaResourcesCreated = false;
    }
    
    void DestroyFXAAResourcesFull() {
        DestroyFXAAResources();
        if (m_fxaaPipeline) {
            vkDestroyPipeline(m_device, m_fxaaPipeline, nullptr);
            m_fxaaPipeline = VK_NULL_HANDLE;
        }
        if (m_rcasPipeline) {
            vkDestroyPipeline(m_device, m_rcasPipeline, nullptr);
            m_rcasPipeline = VK_NULL_HANDLE;
        }
        if (m_fxaaRenderPass) {
            vkDestroyRenderPass(m_device, m_fxaaRenderPass, nullptr);
            m_fxaaRenderPass = VK_NULL_HANDLE;
        }
        if (m_fxaaIntermediateRenderPass) {
            vkDestroyRenderPass(m_device, m_fxaaIntermediateRenderPass, nullptr);
            m_fxaaIntermediateRenderPass = VK_NULL_HANDLE;
        }
        if (m_fxaaDescriptorPool) {
            vkDestroyDescriptorPool(m_device, m_fxaaDescriptorPool, nullptr);
            m_fxaaDescriptorPool = VK_NULL_HANDLE;
        }
        if (m_fxaaSampler) {
            vkDestroySampler(m_device, m_fxaaSampler, nullptr);
            m_fxaaSampler = VK_NULL_HANDLE;
        }
    }
    
    // Create/recreate upscale output image for a given size
    bool CreateUpscaleOutputImage(uint32_t width, uint32_t height) {
        // Cleanup existing output resources
        if (m_upscaleFramebuffer) {
            vkDestroyFramebuffer(m_device, m_upscaleFramebuffer, nullptr);
            m_upscaleFramebuffer = VK_NULL_HANDLE;
        }
        if (m_upscaledOutputView) {
            vkDestroyImageView(m_device, m_upscaledOutputView, nullptr);
            m_upscaledOutputView = VK_NULL_HANDLE;
        }
        if (m_upscaledOutputImage) {
            vkDestroyImage(m_device, m_upscaledOutputImage, nullptr);
            m_upscaledOutputImage = VK_NULL_HANDLE;
        }
        if (m_upscaledOutputMemory) {
            vkFreeMemory(m_device, m_upscaledOutputMemory, nullptr);
            m_upscaledOutputMemory = VK_NULL_HANDLE;
        }
        if (m_upscaledStagingBuffer) {
            vkDestroyBuffer(m_device, m_upscaledStagingBuffer, nullptr);
            m_upscaledStagingBuffer = VK_NULL_HANDLE;
        }
        if (m_upscaledStagingMemory) {
            vkFreeMemory(m_device, m_upscaledStagingMemory, nullptr);
            m_upscaledStagingMemory = VK_NULL_HANDLE;
        }
        
        // Create output image (B8G8R8A8 to match XRGB8888)
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        
        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_upscaledOutputImage) != VK_SUCCESS) {
            return false;
        }
        
        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_upscaledOutputImage, &memReqs);
        
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_upscaledOutputMemory) != VK_SUCCESS) {
            return false;
        }
        
        vkBindImageMemory(m_device, m_upscaledOutputImage, m_upscaledOutputMemory, 0);
        
        // Create output view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_upscaledOutputImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_upscaledOutputView) != VK_SUCCESS) {
            return false;
        }
        
        // Create staging buffer for CPU readback (buffer is faster than linear image)
        // Use HOST_CACHED for fast CPU reads when available
        const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(width) * height * 4;
        
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_upscaledStagingBuffer) != VK_SUCCESS) {
            return false;
        }
        
        vkGetBufferMemoryRequirements(m_device, m_upscaledStagingBuffer, &memReqs);
        
        // Try HOST_CACHED for fast CPU reads, fall back to HOST_COHERENT
        allocInfo.allocationSize = memReqs.size;
        m_upscaledStagingIsCached = false;
        
        // Search for HOST_VISIBLE + HOST_CACHED memory type
        {
            VkPhysicalDeviceMemoryProperties memProps;
            vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
            
            bool foundCached = false;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                if ((memReqs.memoryTypeBits & (1 << i)) &&
                    (memProps.memoryTypes[i].propertyFlags & 
                     (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
                     (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
                    allocInfo.memoryTypeIndex = i;
                    m_upscaledStagingIsCached = 
                        !(memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                    foundCached = true;
                    break;
                }
            }
            
            if (!foundCached) {
                allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            }
        }
        
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_upscaledStagingMemory) != VK_SUCCESS) {
            return false;
        }
        
        vkBindBufferMemory(m_device, m_upscaledStagingBuffer, m_upscaledStagingMemory, 0);
        
        // Create framebuffer
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_upscaleRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &m_upscaledOutputView;
        framebufferInfo.width = width;
        framebufferInfo.height = height;
        framebufferInfo.layers = 1;
        
        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_upscaleFramebuffer) != VK_SUCCESS) {
            return false;
        }
        
        // Create FXAA resources if FXAA is enabled
        if (m_fxaaEnabled) {
            if (!CreateFXAAResources(width, height)) {
                // Non-fatal: FXAA just won't work, upscale still functions
                printf("[VkFXAA] Failed to create FXAA resources\n");
            }
        } else {
            DestroyFXAAResources();
        }
        
        return true;
    }
    
    // Cleanup upscale resources
    void DestroyUpscaleResources() {
        DestroyFXAAResourcesFull();
        if (m_upscalePipeline) {
            vkDestroyPipeline(m_device, m_upscalePipeline, nullptr);
            m_upscalePipeline = VK_NULL_HANDLE;
        }
        if (m_upscalePipelineLayout) {
            vkDestroyPipelineLayout(m_device, m_upscalePipelineLayout, nullptr);
            m_upscalePipelineLayout = VK_NULL_HANDLE;
        }
        if (m_upscaleFramebuffer) {
            vkDestroyFramebuffer(m_device, m_upscaleFramebuffer, nullptr);
            m_upscaleFramebuffer = VK_NULL_HANDLE;
        }
        if (m_upscaleRenderPass) {
            vkDestroyRenderPass(m_device, m_upscaleRenderPass, nullptr);
            m_upscaleRenderPass = VK_NULL_HANDLE;
        }
        if (m_upscaleDescriptorPool) {
            vkDestroyDescriptorPool(m_device, m_upscaleDescriptorPool, nullptr);
            m_upscaleDescriptorPool = VK_NULL_HANDLE;
        }
        if (m_upscaleDescriptorLayout) {
            vkDestroyDescriptorSetLayout(m_device, m_upscaleDescriptorLayout, nullptr);
            m_upscaleDescriptorLayout = VK_NULL_HANDLE;
        }
        if (m_upscaleSampler) {
            vkDestroySampler(m_device, m_upscaleSampler, nullptr);
            m_upscaleSampler = VK_NULL_HANDLE;
        }
        if (m_softwareInputView) {
            vkDestroyImageView(m_device, m_softwareInputView, nullptr);
            m_softwareInputView = VK_NULL_HANDLE;
        }
        if (m_softwareInputTexture) {
            vkDestroyImage(m_device, m_softwareInputTexture, nullptr);
            m_softwareInputTexture = VK_NULL_HANDLE;
        }
        if (m_softwareInputMemory) {
            vkFreeMemory(m_device, m_softwareInputMemory, nullptr);
            m_softwareInputMemory = VK_NULL_HANDLE;
        }
        if (m_softwareInputStaging) {
            vkDestroyBuffer(m_device, m_softwareInputStaging, nullptr);
            m_softwareInputStaging = VK_NULL_HANDLE;
        }
        if (m_softwareInputStagingMemory) {
            vkFreeMemory(m_device, m_softwareInputStagingMemory, nullptr);
            m_softwareInputStagingMemory = VK_NULL_HANDLE;
        }
        if (m_upscaledOutputView) {
            vkDestroyImageView(m_device, m_upscaledOutputView, nullptr);
            m_upscaledOutputView = VK_NULL_HANDLE;
        }
        if (m_upscaledOutputImage) {
            vkDestroyImage(m_device, m_upscaledOutputImage, nullptr);
            m_upscaledOutputImage = VK_NULL_HANDLE;
        }
        if (m_upscaledOutputMemory) {
            vkFreeMemory(m_device, m_upscaledOutputMemory, nullptr);
            m_upscaledOutputMemory = VK_NULL_HANDLE;
        }
        if (m_upscaledStagingBuffer) {
            vkDestroyBuffer(m_device, m_upscaledStagingBuffer, nullptr);
            m_upscaledStagingBuffer = VK_NULL_HANDLE;
        }
        if (m_upscaledStagingMemory) {
            vkFreeMemory(m_device, m_upscaledStagingMemory, nullptr);
            m_upscaledStagingMemory = VK_NULL_HANDLE;
        }
    }
    
    void UploadSpriteTexture(const uint32_t* data, uint32_t width, uint32_t height) {
        if (width > kMaxSpriteTextureSize || height > kMaxSpriteTextureSize) {
            return;
        }
        
        m_spriteTextureWidth = width;
        m_spriteTextureHeight = height;
        
        // Copy data to staging buffer
        void* mappedData;
        vkMapMemory(m_device, m_textureStagingMemory, 0, width * height * 4, 0, &mappedData);
        
        // Convert from XRGB8888 (0xAARRGGBB) to RGBA8888 for Vulkan
        uint8_t* dst = static_cast<uint8_t*>(mappedData);
        for (uint32_t i = 0; i < width * height; ++i) {
            uint32_t pixel = data[i];
            dst[i * 4 + 0] = (pixel >> 16) & 0xFF;  // R
            dst[i * 4 + 1] = (pixel >> 8) & 0xFF;   // G
            dst[i * 4 + 2] = pixel & 0xFF;          // B
            dst[i * 4 + 3] = (pixel >> 24) & 0xFF;  // A
        }
        
        vkUnmapMemory(m_device, m_textureStagingMemory);
        
        // Create command buffer for transfer
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool = m_commandPool;
        allocInfo.commandBufferCount = 1;
        
        VkCommandBuffer cmdBuffer;
        vkAllocateCommandBuffers(m_device, &allocInfo, &cmdBuffer);
        
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmdBuffer, &beginInfo);
        
        // Transition texture to transfer dst
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_spriteTexture;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        
        vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        
        // Copy buffer to image
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {width, height, 1};
        
        vkCmdCopyBufferToImage(cmdBuffer, m_textureStagingBuffer, m_spriteTexture,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        
        // Transition to shader read
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        
        vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        
        vkEndCommandBuffer(cmdBuffer);
        
        // Submit and wait
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuffer;
        
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmdBuffer);
    }
    
    void UploadVertexData() {
        size_t dataSize = m_vertices.size() * sizeof(VDP1Vertex);
        
        // Resize buffer if needed
        if (dataSize > m_vertexBufferSize) {
            // Destroy old buffer
            vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
            vkFreeMemory(m_device, m_vertexBufferMemory, nullptr);
            
            // Create larger buffer
            m_vertexBufferSize = dataSize * 2;  // Double size for growth
            CreateVertexBuffer();
        }
        
        // Map memory and copy data
        void* data;
        vkMapMemory(m_device, m_vertexBufferMemory, 0, dataSize, 0, &data);
        std::memcpy(data, m_vertices.data(), dataSize);
        vkUnmapMemory(m_device, m_vertexBufferMemory);
    }
    
    // Build texture atlas from pending sprites and render them in a single batch
    void BuildTextureAtlasAndRender() {
        if (m_pendingTexturedSprites.empty()) return;
        
        // Simple row-based packing into 512x512 atlas
        // This is a basic implementation - could be optimized with better packing algorithms
        std::vector<uint32_t> atlasData(kMaxSpriteTextureSize * kMaxSpriteTextureSize, 0);
        std::vector<VDP1Vertex> texturedVertices;
        texturedVertices.reserve(m_pendingTexturedSprites.size() * 6);
        
        uint32_t atlasX = 0;
        uint32_t atlasY = 0;
        uint32_t rowHeight = 0;
        
        for (auto& sprite : m_pendingTexturedSprites) {
            // Check if sprite fits in current row
            if (atlasX + sprite.texWidth > kMaxSpriteTextureSize) {
                // Move to next row
                atlasX = 0;
                atlasY += rowHeight;
                rowHeight = 0;
            }
            
            // Check if sprite fits in atlas
            if (atlasY + sprite.texHeight > kMaxSpriteTextureSize) {
                // Atlas is full, skip remaining sprites
                break;
            }
            
            // Copy sprite texture to atlas
            for (uint32_t y = 0; y < sprite.texHeight; ++y) {
                for (uint32_t x = 0; x < sprite.texWidth; ++x) {
                    uint32_t srcIdx = y * sprite.texWidth + x;
                    uint32_t dstIdx = (atlasY + y) * kMaxSpriteTextureSize + (atlasX + x);
                    atlasData[dstIdx] = sprite.textureData[srcIdx];
                }
            }
            
            // Calculate UV coordinates in atlas (normalized 0-1)
            float uMin = static_cast<float>(atlasX) / static_cast<float>(kMaxSpriteTextureSize);
            float vMin = static_cast<float>(atlasY) / static_cast<float>(kMaxSpriteTextureSize);
            float uMax = static_cast<float>(atlasX + sprite.texWidth) / static_cast<float>(kMaxSpriteTextureSize);
            float vMax = static_cast<float>(atlasY + sprite.texHeight) / static_cast<float>(kMaxSpriteTextureSize);
            
            // Update sprite vertices with atlas UVs
            // Vertex order: A(0,0), B(1,0), C(1,1), A(0,0), C(1,1), D(0,1)
            VDP1Vertex v0 = sprite.vertices[0];
            VDP1Vertex v1 = sprite.vertices[1];
            VDP1Vertex v2 = sprite.vertices[2];
            VDP1Vertex v3 = sprite.vertices[5];  // D vertex
            
            // Apply atlas UVs (handle flipping by checking original UV orientation)
            bool flipH = sprite.vertices[0].texCoord[0] > sprite.vertices[1].texCoord[0];
            bool flipV = sprite.vertices[0].texCoord[1] > sprite.vertices[2].texCoord[1];
            
            float u0 = flipH ? uMax : uMin;
            float u1 = flipH ? uMin : uMax;
            float v0f = flipV ? vMax : vMin;
            float v1f = flipV ? vMin : vMax;
            
            // Triangle 1: (A, B, C)
            v0.texCoord[0] = u0; v0.texCoord[1] = v0f;
            v1.texCoord[0] = u1; v1.texCoord[1] = v0f;
            v2.texCoord[0] = u1; v2.texCoord[1] = v1f;
            texturedVertices.push_back(v0);
            texturedVertices.push_back(v1);
            texturedVertices.push_back(v2);
            
            // Triangle 2: (A, C, D)
            v3.texCoord[0] = u0; v3.texCoord[1] = v1f;
            texturedVertices.push_back(v0);
            texturedVertices.push_back(v2);
            texturedVertices.push_back(v3);
            
            // Advance in atlas
            atlasX += sprite.texWidth;
            rowHeight = std::max(rowHeight, sprite.texHeight);
        }
        
        if (texturedVertices.empty()) return;
        
        // End current render pass to upload texture atlas
        vkCmdEndRenderPass(m_commandBuffer);
        vkEndCommandBuffer(m_commandBuffer);
        
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_commandBuffer;
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        
        // Upload atlas texture
        UploadSpriteTexture(atlasData.data(), kMaxSpriteTextureSize, kMaxSpriteTextureSize);
        
        // Upload all textured vertices
        size_t vertexDataSize = texturedVertices.size() * sizeof(VDP1Vertex);
        if (vertexDataSize > m_vertexBufferSize) {
            vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
            vkFreeMemory(m_device, m_vertexBufferMemory, nullptr);
            m_vertexBufferSize = vertexDataSize * 2;
            CreateVertexBuffer();
        }
        
        void* mappedData;
        vkMapMemory(m_device, m_vertexBufferMemory, 0, vertexDataSize, 0, &mappedData);
        std::memcpy(mappedData, texturedVertices.data(), vertexDataSize);
        vkUnmapMemory(m_device, m_vertexBufferMemory);
        
        // Start new command buffer for textured rendering
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(m_commandBuffer, 0);
        vkBeginCommandBuffer(m_commandBuffer, &beginInfo);
        
        // Begin render pass (load existing content using load render pass)
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_renderPassLoad;  // Use load render pass to preserve existing content
        renderPassInfo.framebuffer = m_vkFramebufferLoad;
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = {m_width, m_height};
        
        vkCmdBeginRenderPass(m_commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        
        // Bind textured pipeline
        vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_texturedPipeline);
        
        // Bind descriptor set (texture atlas)
        vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               m_texturedPipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);
        
        // Push constants
        PushConstants pc;
        pc.screenSize[0] = static_cast<float>(m_width);
        pc.screenSize[1] = static_cast<float>(m_height);
        pc.flags = FLAG_TEXTURED;
        pc.priority = 0;
        
        vkCmdPushConstants(m_commandBuffer, m_texturedPipelineLayout,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(PushConstants), &pc);
        
        // Bind vertex buffer
        VkBuffer vertexBuffers[] = {m_vertexBuffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(m_commandBuffer, 0, 1, vertexBuffers, offsets);
        
        // Draw all textured sprites in one call
        vkCmdDraw(m_commandBuffer, static_cast<uint32_t>(texturedVertices.size()), 1, 0, 0);
        
        // Note: render pass will be ended by caller (EndFrame)
    }
    
    // ===== Test/Debug API =====
    
    // Add a colored quad (for testing)
    void DrawQuad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
                  float r, float g, float b, float a) {
        // Triangle 1: (0, 1, 2)
        m_vertices.push_back({{x0, y0}, {r, g, b, a}, {0.0f, 0.0f}});
        m_vertices.push_back({{x1, y1}, {r, g, b, a}, {0.0f, 0.0f}});
        m_vertices.push_back({{x2, y2}, {r, g, b, a}, {0.0f, 0.0f}});
        
        // Triangle 2: (0, 2, 3)
        m_vertices.push_back({{x0, y0}, {r, g, b, a}, {0.0f, 0.0f}});
        m_vertices.push_back({{x2, y2}, {r, g, b, a}, {0.0f, 0.0f}});
        m_vertices.push_back({{x3, y3}, {r, g, b, a}, {0.0f, 0.0f}});
    }
    
    // Add a triangle (for testing)
    void DrawTriangle(float x0, float y0, float x1, float y1, float x2, float y2,
                      float r, float g, float b, float a) {
        m_vertices.push_back({{x0, y0}, {r, g, b, a}, {0.0f, 0.0f}});
        m_vertices.push_back({{x1, y1}, {r, g, b, a}, {0.0f, 0.0f}});
        m_vertices.push_back({{x2, y2}, {r, g, b, a}, {0.0f, 0.0f}});
    }
    
    // Add a Gouraud-shaded quad (for testing)
    void DrawGouraudQuad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
                         float r0, float g0, float b0, float a0,
                         float r1, float g1, float b1, float a1,
                         float r2, float g2, float b2, float a2,
                         float r3, float g3, float b3, float a3) {
        // Triangle 1
        m_vertices.push_back({{x0, y0}, {r0, g0, b0, a0}, {0.0f, 0.0f}});
        m_vertices.push_back({{x1, y1}, {r1, g1, b1, a1}, {0.0f, 0.0f}});
        m_vertices.push_back({{x2, y2}, {r2, g2, b2, a2}, {0.0f, 0.0f}});
        
        // Triangle 2
        m_vertices.push_back({{x0, y0}, {r0, g0, b0, a0}, {0.0f, 0.0f}});
        m_vertices.push_back({{x2, y2}, {r2, g2, b2, a2}, {0.0f, 0.0f}});
        m_vertices.push_back({{x3, y3}, {r3, g3, b3, a3}, {0.0f, 0.0f}});
    }
};

// Factory function
std::unique_ptr<IVDPRenderer> CreateVulkanRenderer() {
    return std::make_unique<VulkanRenderer>();
}

} // namespace brimir::vdp

#endif // BRIMIR_GPU_VULKAN_ENABLED
