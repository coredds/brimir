// Brimir - Vulkan Renderer Implementation
// GPU-accelerated rendering using Vulkan

#ifdef BRIMIR_GPU_VULKAN_ENABLED

#include <brimir/hw/vdp/vdp_renderer.hpp>
#include <brimir/hw/vdp/vdp_defs.hpp>
#include <brimir/hw/vdp/vdp1_defs.hpp>
#include <brimir/hw/vdp/vdp.hpp>
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
            
            // Cleanup GPU Full Pipeline layer resources
            DestroyLayerRenderTargets();
            
            // GPU VDP1 hi-res resources removed (approach abandoned)
            
            // Cleanup upscale resources
            DestroyUpscaleResources();
            
            // Cleanup VRAM textures
            if (m_vdp1VramView) {
                vkDestroyImageView(m_device, m_vdp1VramView, nullptr);
                m_vdp1VramView = VK_NULL_HANDLE;
            }
            if (m_vdp1VramTexture) {
                vkDestroyImage(m_device, m_vdp1VramTexture, nullptr);
                m_vdp1VramTexture = VK_NULL_HANDLE;
            }
            if (m_vdp1VramMemory) {
                vkFreeMemory(m_device, m_vdp1VramMemory, nullptr);
                m_vdp1VramMemory = VK_NULL_HANDLE;
            }
            if (m_vdp2VramView) {
                vkDestroyImageView(m_device, m_vdp2VramView, nullptr);
                m_vdp2VramView = VK_NULL_HANDLE;
            }
            if (m_vdp2VramTexture) {
                vkDestroyImage(m_device, m_vdp2VramTexture, nullptr);
                m_vdp2VramTexture = VK_NULL_HANDLE;
            }
            if (m_vdp2VramMemory) {
                vkFreeMemory(m_device, m_vdp2VramMemory, nullptr);
                m_vdp2VramMemory = VK_NULL_HANDLE;
            }
            if (m_cramView) {
                vkDestroyImageView(m_device, m_cramView, nullptr);
                m_cramView = VK_NULL_HANDLE;
            }
            if (m_cramTexture) {
                vkDestroyImage(m_device, m_cramTexture, nullptr);
                m_cramTexture = VK_NULL_HANDLE;
            }
            if (m_cramMemory) {
                vkFreeMemory(m_device, m_cramMemory, nullptr);
                m_cramMemory = VK_NULL_HANDLE;
            }
            
            // Cleanup staging buffers
            if (m_vramStagingMapped && m_vramStagingMemory) {
                vkUnmapMemory(m_device, m_vramStagingMemory);
                m_vramStagingMapped = nullptr;
            }
            if (m_vramStagingBuffer) {
                vkDestroyBuffer(m_device, m_vramStagingBuffer, nullptr);
                m_vramStagingBuffer = VK_NULL_HANDLE;
            }
            if (m_vramStagingMemory) {
                vkFreeMemory(m_device, m_vramStagingMemory, nullptr);
                m_vramStagingMemory = VK_NULL_HANDLE;
            }
            if (m_cramStagingMapped && m_cramStagingMemory) {
                vkUnmapMemory(m_device, m_cramStagingMemory);
                m_cramStagingMapped = nullptr;
            }
            if (m_cramStagingBuffer) {
                vkDestroyBuffer(m_device, m_cramStagingBuffer, nullptr);
                m_cramStagingBuffer = VK_NULL_HANDLE;
            }
            if (m_cramStagingMemory) {
                vkFreeMemory(m_device, m_cramStagingMemory, nullptr);
                m_cramStagingMemory = VK_NULL_HANDLE;
            }
            
            // Cleanup compositor resources
            if (m_compositorPipeline) {
                vkDestroyPipeline(m_device, m_compositorPipeline, nullptr);
                m_compositorPipeline = VK_NULL_HANDLE;
            }
            if (m_compositorPipelineLayout) {
                vkDestroyPipelineLayout(m_device, m_compositorPipelineLayout, nullptr);
                m_compositorPipelineLayout = VK_NULL_HANDLE;
            }
            if (m_compositorDescriptorPool) {
                vkDestroyDescriptorPool(m_device, m_compositorDescriptorPool, nullptr);
                m_compositorDescriptorPool = VK_NULL_HANDLE;
            }
            if (m_compositorDescriptorLayout) {
                vkDestroyDescriptorSetLayout(m_device, m_compositorDescriptorLayout, nullptr);
                m_compositorDescriptorLayout = VK_NULL_HANDLE;
            }
            if (m_compositorRenderPass) {
                vkDestroyRenderPass(m_device, m_compositorRenderPass, nullptr);
                m_compositorRenderPass = VK_NULL_HANDLE;
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
    
    // ===== Memory Synchronization (GPU Full Pipeline) =====
    
    void SyncVDP1VRAM(const uint8_t* data, size_t size) override {
        if (!m_initialized || !data || size == 0) return;
        if (!m_vramStagingMapped || !m_vdp1VramTexture) return;
        
        // Copy to staging buffer
        size_t copySize = std::min(size, kVramStagingSize);
        std::memcpy(m_vramStagingMapped, data, copySize);
        
        // Upload to GPU texture
        UploadVRAMToGPU(m_vdp1VramTexture, copySize, 512, 256);
        
        m_statistics.vramSyncCount++;
    }
    
    void SyncVDP2VRAM(const uint8_t* data, size_t size) override {
        if (!m_initialized || !data || size == 0) return;
        if (!m_vramStagingMapped || !m_vdp2VramTexture) return;
        
        // Copy to staging buffer
        size_t copySize = std::min(size, kVramStagingSize);
        std::memcpy(m_vramStagingMapped, data, copySize);
        
        // Upload to GPU texture
        UploadVRAMToGPU(m_vdp2VramTexture, copySize, 512, 256);
        
        m_statistics.vramSyncCount++;
    }
    
    void SyncCRAM(const uint8_t* data, size_t size, uint8_t mode) override {
        if (!m_initialized || !data || size == 0) return;
        if (!m_cramStagingMapped || !m_cramTexture) return;
        
        m_cramMode = mode;
        
        // Convert CRAM to RGBA8888 format for GPU texture
        // Mode 0: 1024 colors (RGB555), Mode 1: 2048 colors (RGB555), Mode 2: 1024 colors (RGB888)
        uint8_t* dst = static_cast<uint8_t*>(m_cramStagingMapped);
        const uint16_t* src16 = reinterpret_cast<const uint16_t*>(data);
        const uint8_t* src8 = data;
        
        size_t numColors = (mode == 1) ? 2048 : 1024;
        numColors = std::min(numColors, size / 2);  // Ensure we don't read past buffer
        
        if (mode == 2) {
            // RGB888 mode - 3 bytes per color
            numColors = std::min(size / 4, static_cast<size_t>(1024));
            for (size_t i = 0; i < numColors; ++i) {
                // RGB888 stored as 0xXXBBGGRR
                uint32_t color = *reinterpret_cast<const uint32_t*>(src8 + i * 4);
                dst[i * 4 + 0] = color & 0xFF;         // R
                dst[i * 4 + 1] = (color >> 8) & 0xFF;  // G
                dst[i * 4 + 2] = (color >> 16) & 0xFF; // B
                dst[i * 4 + 3] = 255;                  // A
            }
        } else {
            // RGB555 mode
            for (size_t i = 0; i < numColors; ++i) {
                uint16_t color = src16[i];
                // RGB555: xBBBBBGG GGGRRRRR
                dst[i * 4 + 0] = ((color >> 0) & 0x1F) << 3;  // R (expand 5-bit to 8-bit)
                dst[i * 4 + 1] = ((color >> 5) & 0x1F) << 3;  // G
                dst[i * 4 + 2] = ((color >> 10) & 0x1F) << 3; // B
                dst[i * 4 + 3] = 255;                         // A
            }
        }
        
        // Upload to GPU texture (64x32 RGBA = 8KB max)
        UploadCRAMToGPU(numColors);
        
        m_statistics.cramSyncCount++;
    }
    
    void SyncVDP1Framebuffer(const uint8_t* data, size_t size, bool is8bit) override {
        if (!m_initialized || !data || size == 0) return;
        
        // TODO: Upload VDP1 sprite framebuffer to GPU texture
        // This contains the raw VDP1 rendered output with priority bits
        m_vdp1Framebuffer8bit = is8bit;
        m_statistics.vramSyncCount++;
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
            uint32_t scanlines;
            float brightness;
            float gamma;
        } pushConstants;
        
        pushConstants.inputWidth = static_cast<float>(m_softwareWidth);
        pushConstants.inputHeight = static_cast<float>(m_softwareHeight);
        pushConstants.outputWidth = static_cast<float>(finalWidth);
        pushConstants.outputHeight = static_cast<float>(finalHeight);
        pushConstants.filterMode = m_upscaleFilterMode;
        pushConstants.scanlines = m_scanlinesEnabled ? 1 : 0;
        pushConstants.brightness = m_brightnessValue;
        pushConstants.gamma = m_gammaValue;
        
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
        if (m_upscaledStagingBuffer) {
            const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(finalWidth) * finalHeight * 4;
            
            // If using HOST_CACHED (non-coherent), invalidate before reading
            if (m_upscaledStagingIsCached) {
                VkMappedMemoryRange range{};
                range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                range.memory = m_upscaledStagingMemory;
                range.offset = 0;
                range.size = VK_WHOLE_SIZE;
                vkInvalidateMappedMemoryRanges(m_device, 1, &range);
            }
            
            void* mappedData = nullptr;
            if (vkMapMemory(m_device, m_upscaledStagingMemory, 0,
                            bufferSize, 0, &mappedData) == VK_SUCCESS) {
                
                m_upscaledFramebuffer.resize(finalWidth * finalHeight);
                
                // B8G8R8A8 in memory is [B,G,R,A] = XRGB8888 on little-endian
                // No per-pixel conversion needed - just memcpy
                std::memcpy(m_upscaledFramebuffer.data(), mappedData,
                            static_cast<size_t>(finalWidth) * finalHeight * sizeof(uint32_t));
                
                vkUnmapMemory(m_device, m_upscaledStagingMemory);
            }
        }
        
        m_softwareFramebufferDirty = false;
        m_upscaledReady = true;
        
        static bool s_logSuccess = false;
        if (!s_logSuccess) { 
            printf("[VkUpscale] SUCCESS: %ux%u -> %ux%u, fb size=%zu\n", 
                   m_softwareWidth, m_softwareHeight, 
                   m_currentUpscaleWidth, m_currentUpscaleHeight,
                   m_upscaledFramebuffer.size()); 
            s_logSuccess = true; 
        }
        return true;
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
    
    // ===== GPU VDP1 High-Res Rendering (stubs - approach abandoned) =====
    
    void SubmitVDP1Commands(const struct VDP1GPUCommand* commands, size_t count) override {
        (void)commands; (void)count; // No-op: GPU VDP1 rendering removed
    }
    
    bool RenderVDP1Frame(uint32_t fbWidth, uint32_t fbHeight) override {
        (void)fbWidth; (void)fbHeight;
        return false; // GPU VDP1 rendering removed
    }
    
    const uint32_t* GetVDP1HiResBuffer() const override { return nullptr; }
    uint32_t GetVDP1HiResWidth() const override { return 0; }
    uint32_t GetVDP1HiResHeight() const override { return 0; }
    
    void UploadVDP1TextureAtlas(const uint32_t* data, uint32_t width, uint32_t height) override {
        (void)data; (void)width; (void)height; // No-op: GPU VDP1 rendering removed
    }
    
    // ===== VDP1 Rendering =====
    
    void VDP1DrawPolygon(const VDP1Command& cmd) override {
        if (!m_initialized) return;
        
        // Batch polygon vertices for GPU rendering
        // VDP1 polygons are untextured quadrilaterals
        // Command structure provides 4 vertices and color
        
        // Note: Full implementation would read coordinates from cmd
        // For now, track the call - actual vertex batching requires
        // VDP1 command parsing which is done by the software renderer
        
        m_statistics.drawCallCount++;
    }
    
    void VDP1DrawSprite(const VDP1Command& cmd) override {
        if (!m_initialized) return;
        
        // Normal sprites are textured quads at fixed size
        // Need to batch for GPU rendering
        
        m_statistics.drawCallCount++;
    }
    
    void VDP1DrawScaledSprite(const VDP1Command& cmd) override {
        if (!m_initialized) return;
        
        // Scaled sprites can be any size
        
        m_statistics.drawCallCount++;
    }
    
    void VDP1DrawDistortedSprite(const VDP1Command& cmd) override {
        if (!m_initialized) return;
        
        // Distorted sprites have arbitrary vertex positions
        // Most complex VDP1 command - requires proper UV mapping
        
        m_statistics.drawCallCount++;
    }
    
    void VDP1DrawLine(const VDP1Command& cmd) override {
        if (!m_initialized) return;
        
        // Lines are single pixels wide
        
        m_statistics.drawCallCount++;
    }
    
    void VDP1DrawPolyline(const VDP1Command& cmd) override {
        if (!m_initialized) return;
        
        // Polylines are connected lines
        
        m_statistics.drawCallCount++;
    }
    
    // Helper: Add a quad to the VDP1 sprite vertex batch
    void BatchVDP1Quad(float x0, float y0, float x1, float y1, 
                       float x2, float y2, float x3, float y3,
                       float u0, float v0, float u1, float v1,
                       float r, float g, float b, float a,
                       uint32_t flags) {
        if (m_vdp1SpriteVertices.size() + 6 > kMaxVDP1SpriteVertices) {
            return;  // Buffer full
        }
        
        // Triangle 1: 0-1-2
        m_vdp1SpriteVertices.push_back({{x0, y0}, {u0, v0}, {r, g, b, a}, flags});
        m_vdp1SpriteVertices.push_back({{x1, y1}, {u1, v0}, {r, g, b, a}, flags});
        m_vdp1SpriteVertices.push_back({{x2, y2}, {u1, v1}, {r, g, b, a}, flags});
        
        // Triangle 2: 0-2-3
        m_vdp1SpriteVertices.push_back({{x0, y0}, {u0, v0}, {r, g, b, a}, flags});
        m_vdp1SpriteVertices.push_back({{x2, y2}, {u1, v1}, {r, g, b, a}, flags});
        m_vdp1SpriteVertices.push_back({{x3, y3}, {u0, v1}, {r, g, b, a}, flags});
    }
    
    // Flush batched VDP1 sprites to the sprite layer
    void FlushVDP1SpriteBatch() {
        if (m_vdp1SpriteVertices.empty()) return;
        if (!m_vdp1SpriteVertexMapped) return;
        
        // Copy vertices to GPU buffer
        size_t copySize = m_vdp1SpriteVertices.size() * sizeof(VDP1SpriteVertex);
        std::memcpy(m_vdp1SpriteVertexMapped, m_vdp1SpriteVertices.data(), copySize);
        
        // Note: Actual rendering would happen here
        // For now, just clear the batch
        m_vdp1SpriteVertices.clear();
    }
    
    // ===== VDP2 Layer Rendering (GPU Full Pipeline) =====
    
    void SetLayerConfig(GPULayer layer, const GPULayerConfig& config) override {
        if (static_cast<size_t>(layer) >= m_layerConfigs.size()) return;
        m_layerConfigs[static_cast<size_t>(layer)] = config;
    }
    
    void RenderNBGLayer(GPULayer layer, uint32_t y) override {
        if (!m_initialized) return;
        
        // Get layer index (NBG0-3)
        size_t layerIdx = static_cast<size_t>(layer);
        if (layerIdx < static_cast<size_t>(GPULayer::NBG3) || 
            layerIdx > static_cast<size_t>(GPULayer::NBG0)) {
            return;  // Not an NBG layer
        }
        
        // NBG layer rendering is done per-line (y) for accuracy
        // In full implementation, we would:
        // 1. Set up push constants with layer parameters (scroll, pattern addr, etc.)
        // 2. Bind NBG pipeline and descriptor sets
        // 3. Render a horizontal line strip to layer render target
        // 4. Handle line scroll and other per-line effects
        
        // For now, track the call - actual rendering will be implemented
        // when the NBG pipeline is fully created
        (void)y;
        m_statistics.drawCallCount++;
    }
    
    void RenderRBGLayer(GPULayer layer, uint32_t y) override {
        if (!m_initialized) return;
        
        // TODO: Implement full GPU RBG (rotation) layer rendering
        // This is more complex - requires rotation/scaling matrix transforms
        
        (void)layer;
        (void)y;
        m_statistics.drawCallCount++;
    }
    
    void RenderBackLayer(uint32_t color) override {
        if (!m_initialized) return;
        
        // TODO: Render back screen to back layer render target
        // For now, store the color for compositing
        m_backColor = color;
    }
    
    // ===== Legacy VDP2 Rendering interface =====
    
    void VDP2DrawBackground(int layer, const VDP2LayerState& state) override {
        // Legacy interface - TODO: Forward to new layer system
        m_statistics.drawCallCount++;
    }
    
    void VDP2DrawRotation(int layer, const VDP2RotationState& state) override {
        // Legacy interface - TODO: Forward to new layer system
        m_statistics.drawCallCount++;
    }
    
    void VDP2DrawSpriteLayer(const VDP2LayerState& state) override {
        // Legacy interface - TODO: Forward to new layer system
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
    
    void SetTextureFiltering(bool enable) override {
        m_textureFiltering = enable;
        // TODO: Update sampler state
    }
    
    void SetUpscaleFilter(uint32_t mode) override {
        m_upscaleFilterMode = mode;
    }
    
    void SetScanlines(bool enable) override {
        m_scanlinesEnabled = enable;
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
    
    void SetMSAA(uint32_t samples) override {
        m_msaaSamples = samples;
        // TODO: Recreate render targets with MSAA
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
        caps.supportsFullPipeline = true;  // GPU handles VDP1+VDP2+compositing
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
        case RendererFeature::FullPipeline:
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
    bool m_scanlinesEnabled = false;
    float m_brightnessValue = 1.0f;
    float m_gammaValue = 1.0f;
    bool m_fxaaEnabled = false;
    uint32_t m_sharpeningMode = 0;  // 0 = off, 1 = FXAA, 2 = RCAS
    void* m_hwContext = nullptr;  // Frontend's hardware context (unused for now)
    
    // GPU Full Pipeline state
    uint8_t m_cramMode = 0;           // CRAM color mode (0=RGB555x1024, 1=RGB555x2048, 2=RGB888x1024)
    bool m_vdp1Framebuffer8bit = false;  // VDP1 framebuffer pixel mode
    uint32_t m_backColor = 0xFF000000;   // Back screen color
    
    // Per-layer configuration for GPU full pipeline
    std::array<GPULayerConfig, static_cast<size_t>(GPULayer::Count)> m_layerConfigs{};
    
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
    
    // ===== GPU Full Pipeline Layer Resources =====
    // Each layer has its own render target for priority-based compositing
    
    struct LayerRenderTarget {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        bool dirty = false;  // Needs re-render
    };
    std::array<LayerRenderTarget, static_cast<size_t>(GPULayer::Count)> m_layerTargets{};
    
    // VRAM textures for GPU-side pattern/character data
    VkImage m_vdp1VramTexture = VK_NULL_HANDLE;
    VkDeviceMemory m_vdp1VramMemory = VK_NULL_HANDLE;
    VkImageView m_vdp1VramView = VK_NULL_HANDLE;
    
    VkImage m_vdp2VramTexture = VK_NULL_HANDLE;
    VkDeviceMemory m_vdp2VramMemory = VK_NULL_HANDLE;
    VkImageView m_vdp2VramView = VK_NULL_HANDLE;
    
    // CRAM texture for GPU-side palette lookups
    VkImage m_cramTexture = VK_NULL_HANDLE;
    VkDeviceMemory m_cramMemory = VK_NULL_HANDLE;
    VkImageView m_cramView = VK_NULL_HANDLE;
    
    // Staging buffers for VRAM/CRAM uploads
    VkBuffer m_vramStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vramStagingMemory = VK_NULL_HANDLE;
    void* m_vramStagingMapped = nullptr;
    static constexpr size_t kVramStagingSize = 512 * 1024;  // 512KB for VDP1/VDP2 VRAM
    
    VkBuffer m_cramStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_cramStagingMemory = VK_NULL_HANDLE;
    void* m_cramStagingMapped = nullptr;
    static constexpr size_t kCramStagingSize = 4 * 1024;    // 4KB for CRAM
    
    // VDP1 Sprite pipeline (full GPU rendering)
    VkPipeline m_vdp1SpritePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_vdp1SpritePipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_vdp1SpriteDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_vdp1SpriteDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_vdp1SpriteDescriptorSet = VK_NULL_HANDLE;
    VkSampler m_vramSampler = VK_NULL_HANDLE;
    
    // VDP1 sprite vertex buffer (batched rendering)
    struct VDP1SpriteVertex {
        float position[2];
        float texCoord[2];
        float color[4];
        uint32_t flags;
    };
    std::vector<VDP1SpriteVertex> m_vdp1SpriteVertices;
    VkBuffer m_vdp1SpriteVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vdp1SpriteVertexMemory = VK_NULL_HANDLE;
    void* m_vdp1SpriteVertexMapped = nullptr;
    static constexpr size_t kMaxVDP1SpriteVertices = 16384;  // ~2700 quads
    
    // GPU VDP1 hi-res rendering state removed (approach abandoned)
    // FSR 1.0 spatial upscaling is used instead.
    
    // VDP2 NBG layer pipeline
    VkPipeline m_nbgPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_nbgPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_nbgDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_nbgDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_nbgDescriptorSet = VK_NULL_HANDLE;
    
    // NBG push constants structure
    struct NBGPushConstants {
        float screenSize[2];
        float scrollOffset[2];
        float scrollInc[2];
        uint32_t layerIndex;
        uint32_t flags;
        uint32_t patternNameAddr;
        uint32_t characterAddr;
        uint32_t colorFormat;
        uint32_t cramOffset;
        uint32_t pageSize;
        uint32_t planeSize;
        uint32_t cellSize;
        uint32_t bitmapMode;
        uint32_t bitmapWidth;
        uint32_t bitmapHeight;
        uint32_t bitmapAddr;
        uint32_t reserved;
    };
    
    // VDP1 framebuffer texture (for sprite layer when using software VDP1)
    VkImage m_vdp1FramebufferTexture = VK_NULL_HANDLE;
    VkDeviceMemory m_vdp1FramebufferMemory = VK_NULL_HANDLE;
    VkImageView m_vdp1FramebufferView = VK_NULL_HANDLE;
    
    // Compositor pipeline
    VkRenderPass m_compositorRenderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_compositorPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_compositorPipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_compositorDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_compositorDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_compositorDescriptorSet = VK_NULL_HANDLE;
    
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
        // Use embedded shader bytecode (compiled SPIR-V)
        // This ensures shaders are available regardless of working directory
        std::vector<char> vertShaderCode(
            reinterpret_cast<const char*>(shaders::vdp1_vert_data),
            reinterpret_cast<const char*>(shaders::vdp1_vert_data) + shaders::vdp1_vert_size
        );
        std::vector<char> fragShaderCode(
            reinterpret_cast<const char*>(shaders::vdp1_frag_data),
            reinterpret_cast<const char*>(shaders::vdp1_frag_data) + shaders::vdp1_frag_size
        );
        
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
        // Use embedded textured shaders
        std::vector<char> vertShaderCode(
            shaders::vdp1_textured_vert_data,
            shaders::vdp1_textured_vert_data + shaders::vdp1_textured_vert_size
        );
        std::vector<char> fragShaderCode(
            shaders::vdp1_textured_frag_data,
            shaders::vdp1_textured_frag_data + shaders::vdp1_textured_frag_size
        );
        
        if (vertShaderCode.empty() || fragShaderCode.empty()) {
            // Fallback: use untextured shaders if textured ones not available
            return true;  // Not a fatal error
        }
        
        m_texturedVertShaderModule = CreateShaderModule(vertShaderCode);
        m_texturedFragShaderModule = CreateShaderModule(fragShaderCode);
        
        if (!m_texturedVertShaderModule || !m_texturedFragShaderModule) {
            return true;  // Not fatal
        }
        
        // Shader stages
        VkPipelineShaderStageCreateInfo vertStage{};
        vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = m_texturedVertShaderModule;
        vertStage.pName = "main";
        
        VkPipelineShaderStageCreateInfo fragStage{};
        fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStage.module = m_texturedFragShaderModule;
        fragStage.pName = "main";
        
        VkPipelineShaderStageCreateInfo shaderStages[] = {vertStage, fragStage};
        
        // Vertex input
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(VDP1Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        
        std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};
        attributeDescriptions[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(VDP1Vertex, pos)};
        attributeDescriptions[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VDP1Vertex, color)};
        attributeDescriptions[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(VDP1Vertex, texCoord)};
        
        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
        vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();
        
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        
        VkViewport viewport{};
        viewport.width = static_cast<float>(m_width);
        viewport.height = static_cast<float>(m_height);
        viewport.maxDepth = 1.0f;
        
        VkRect2D scissor{{0, 0}, {m_width, m_height}};
        
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;
        
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        
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
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;
        
        // Push constants
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = 16;
        
        // Pipeline layout with descriptor set
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
        
        if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_texturedPipelineLayout) != VK_SUCCESS) {
            return false;
        }
        
        // Create pipeline
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
        pipelineInfo.layout = m_texturedPipelineLayout;
        pipelineInfo.renderPass = m_renderPass;
        pipelineInfo.subpass = 0;
        
        if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_texturedPipeline) != VK_SUCCESS) {
            return false;
        }
        
        return true;
    }
    
    // Create render targets for each VDP layer (GPU Full Pipeline)
    bool CreateLayerRenderTargets() {
        // Each layer gets a render target at native Saturn resolution
        // These will be upscaled during the final compositor pass
        const uint32_t layerWidth = 704;   // Max Saturn horizontal resolution
        const uint32_t layerHeight = 512;  // Max Saturn vertical resolution (interlaced)
        
        for (size_t i = 0; i < m_layerTargets.size(); ++i) {
            auto& layer = m_layerTargets[i];
            
            // Create image
            VkImageCreateInfo imageInfo{};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;  // RGBA for alpha/transparency
            imageInfo.extent = {layerWidth, layerHeight, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            
            if (vkCreateImage(m_device, &imageInfo, nullptr, &layer.image) != VK_SUCCESS) {
                m_lastError = "Failed to create layer render target image";
                return false;
            }
            
            // Allocate memory
            VkMemoryRequirements memReqs;
            vkGetImageMemoryRequirements(m_device, layer.image, &memReqs);
            
            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memReqs.size;
            allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            
            if (vkAllocateMemory(m_device, &allocInfo, nullptr, &layer.memory) != VK_SUCCESS) {
                m_lastError = "Failed to allocate layer render target memory";
                return false;
            }
            
            vkBindImageMemory(m_device, layer.image, layer.memory, 0);
            
            // Create image view
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = layer.image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            
            if (vkCreateImageView(m_device, &viewInfo, nullptr, &layer.view) != VK_SUCCESS) {
                m_lastError = "Failed to create layer render target view";
                return false;
            }
            
            layer.dirty = true;  // Mark as needing initial render
        }
        
        return true;
    }
    
    // Cleanup layer render targets
    void DestroyLayerRenderTargets() {
        for (auto& layer : m_layerTargets) {
            if (layer.framebuffer) {
                vkDestroyFramebuffer(m_device, layer.framebuffer, nullptr);
                layer.framebuffer = VK_NULL_HANDLE;
            }
            if (layer.view) {
                vkDestroyImageView(m_device, layer.view, nullptr);
                layer.view = VK_NULL_HANDLE;
            }
            if (layer.image) {
                vkDestroyImage(m_device, layer.image, nullptr);
                layer.image = VK_NULL_HANDLE;
            }
            if (layer.memory) {
                vkFreeMemory(m_device, layer.memory, nullptr);
                layer.memory = VK_NULL_HANDLE;
            }
        }
    }
    
    // Create VRAM textures for GPU-side data access
    bool CreateVRAMTextures() {
        // VDP1 VRAM: 512KB as 512x256 R32 texture (4 bytes per texel = raw data)
        // VDP2 VRAM: 512KB as 512x256 R32 texture (4 banks of 128KB)
        // For now, create placeholder textures - actual format may need adjustment
        
        // VDP1 VRAM texture (512KB = 512x256x4 bytes)
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32_UINT;  // Raw 32-bit data per texel
        imageInfo.extent = {512, 256, 1};       // 512*256*4 = 512KB
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        
        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_vdp1VramTexture) != VK_SUCCESS) {
            m_lastError = "Failed to create VDP1 VRAM texture";
            return false;
        }
        
        // Allocate memory for VDP1 VRAM
        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_vdp1VramTexture, &memReqs);
        
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_vdp1VramMemory) != VK_SUCCESS) {
            m_lastError = "Failed to allocate VDP1 VRAM memory";
            return false;
        }
        
        vkBindImageMemory(m_device, m_vdp1VramTexture, m_vdp1VramMemory, 0);
        
        // Create VDP1 VRAM view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_vdp1VramTexture;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32_UINT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_vdp1VramView) != VK_SUCCESS) {
            m_lastError = "Failed to create VDP1 VRAM view";
            return false;
        }
        
        // VDP2 VRAM texture (512KB = 512x256x4 bytes)
        imageInfo.format = VK_FORMAT_R32_UINT;
        imageInfo.extent = {512, 256, 1};
        
        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_vdp2VramTexture) != VK_SUCCESS) {
            m_lastError = "Failed to create VDP2 VRAM texture";
            return false;
        }
        
        vkGetImageMemoryRequirements(m_device, m_vdp2VramTexture, &memReqs);
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_vdp2VramMemory) != VK_SUCCESS) {
            m_lastError = "Failed to allocate VDP2 VRAM memory";
            return false;
        }
        
        vkBindImageMemory(m_device, m_vdp2VramTexture, m_vdp2VramMemory, 0);
        
        viewInfo.image = m_vdp2VramTexture;
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_vdp2VramView) != VK_SUCCESS) {
            m_lastError = "Failed to create VDP2 VRAM view";
            return false;
        }
        
        // CRAM texture (4KB = 2048 colors as RGBA, stored as 64x32 RGBA texture)
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {64, 32, 1};  // 64*32 = 2048 color entries
        
        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_cramTexture) != VK_SUCCESS) {
            m_lastError = "Failed to create CRAM texture";
            return false;
        }
        
        vkGetImageMemoryRequirements(m_device, m_cramTexture, &memReqs);
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_cramMemory) != VK_SUCCESS) {
            m_lastError = "Failed to allocate CRAM memory";
            return false;
        }
        
        vkBindImageMemory(m_device, m_cramTexture, m_cramMemory, 0);
        
        viewInfo.image = m_cramTexture;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_cramView) != VK_SUCCESS) {
            m_lastError = "Failed to create CRAM view";
            return false;
        }
        
        // Create VRAM staging buffer (shared for VDP1/VDP2)
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = kVramStagingSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_vramStagingBuffer) != VK_SUCCESS) {
            m_lastError = "Failed to create VRAM staging buffer";
            return false;
        }
        
        VkMemoryRequirements stagingReqs;
        vkGetBufferMemoryRequirements(m_device, m_vramStagingBuffer, &stagingReqs);
        
        allocInfo.allocationSize = stagingReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(stagingReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_vramStagingMemory) != VK_SUCCESS) {
            m_lastError = "Failed to allocate VRAM staging memory";
            return false;
        }
        
        vkBindBufferMemory(m_device, m_vramStagingBuffer, m_vramStagingMemory, 0);
        vkMapMemory(m_device, m_vramStagingMemory, 0, kVramStagingSize, 0, &m_vramStagingMapped);
        
        // Create CRAM staging buffer
        bufferInfo.size = kCramStagingSize;
        
        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_cramStagingBuffer) != VK_SUCCESS) {
            m_lastError = "Failed to create CRAM staging buffer";
            return false;
        }
        
        vkGetBufferMemoryRequirements(m_device, m_cramStagingBuffer, &stagingReqs);
        
        allocInfo.allocationSize = stagingReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(stagingReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_cramStagingMemory) != VK_SUCCESS) {
            m_lastError = "Failed to allocate CRAM staging memory";
            return false;
        }
        
        vkBindBufferMemory(m_device, m_cramStagingBuffer, m_cramStagingMemory, 0);
        vkMapMemory(m_device, m_cramStagingMemory, 0, kCramStagingSize, 0, &m_cramStagingMapped);
        
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
        pushConstant.size = sizeof(float) * 6 + sizeof(uint32_t) * 2;  // inputSize(2f), outputSize(2f), filterMode(u), scanlines(u), brightness(f), gamma(f)
        
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
    
    // Create VDP1 sprite rendering pipeline for full GPU rendering
    bool CreateVDP1SpritePipeline() {
        // Create VRAM sampler
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        
        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_vramSampler) != VK_SUCCESS) {
            m_lastError = "Failed to create VRAM sampler";
            return false;
        }
        
        // Create descriptor set layout (VRAM + CRAM textures)
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings = bindings;
        
        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_vdp1SpriteDescriptorLayout) != VK_SUCCESS) {
            m_lastError = "Failed to create VDP1 sprite descriptor layout";
            return false;
        }
        
        // Create descriptor pool
        VkDescriptorPoolSize poolSizes[1]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = 2;
        
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = poolSizes;
        poolInfo.maxSets = 1;
        
        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_vdp1SpriteDescriptorPool) != VK_SUCCESS) {
            m_lastError = "Failed to create VDP1 sprite descriptor pool";
            return false;
        }
        
        // Allocate descriptor set
        VkDescriptorSetAllocateInfo descAllocInfo{};
        descAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descAllocInfo.descriptorPool = m_vdp1SpriteDescriptorPool;
        descAllocInfo.descriptorSetCount = 1;
        descAllocInfo.pSetLayouts = &m_vdp1SpriteDescriptorLayout;
        
        if (vkAllocateDescriptorSets(m_device, &descAllocInfo, &m_vdp1SpriteDescriptorSet) != VK_SUCCESS) {
            m_lastError = "Failed to allocate VDP1 sprite descriptor set";
            return false;
        }
        
        // Create pipeline layout with push constants
        VkPushConstantRange pushConstant{};
        pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(float) * 2 + sizeof(uint32_t) * 2;  // screenSize, frameFlags, reserved
        
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_vdp1SpriteDescriptorLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstant;
        
        if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_vdp1SpritePipelineLayout) != VK_SUCCESS) {
            m_lastError = "Failed to create VDP1 sprite pipeline layout";
            return false;
        }
        
        // Create vertex buffer
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = kMaxVDP1SpriteVertices * sizeof(VDP1SpriteVertex);
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_vdp1SpriteVertexBuffer) != VK_SUCCESS) {
            m_lastError = "Failed to create VDP1 sprite vertex buffer";
            return false;
        }
        
        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(m_device, m_vdp1SpriteVertexBuffer, &memReqs);
        
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_vdp1SpriteVertexMemory) != VK_SUCCESS) {
            m_lastError = "Failed to allocate VDP1 sprite vertex memory";
            return false;
        }
        
        vkBindBufferMemory(m_device, m_vdp1SpriteVertexBuffer, m_vdp1SpriteVertexMemory, 0);
        vkMapMemory(m_device, m_vdp1SpriteVertexMemory, 0, bufferInfo.size, 0, &m_vdp1SpriteVertexMapped);
        
        m_vdp1SpriteVertices.reserve(kMaxVDP1SpriteVertices);
        
        // Note: Pipeline creation requires a render pass, which is already created
        // The actual pipeline creation using shaders is done separately
        
        return true;
    }
    
    // Upload VRAM data from staging buffer to GPU texture
    void UploadVRAMToGPU(VkImage targetImage, size_t dataSize, uint32_t width, uint32_t height) {
        if (!m_commandBuffer || !targetImage) return;
        
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
        barrier.image = targetImage;
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
        
        vkCmdCopyBufferToImage(m_commandBuffer, m_vramStagingBuffer,
            targetImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        
        // Transition to shader read
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        
        vkCmdPipelineBarrier(m_commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        
        vkEndCommandBuffer(m_commandBuffer);
        
        // Submit and wait
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_commandBuffer;
        
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
    }
    
    // Upload CRAM data from staging buffer to GPU texture
    void UploadCRAMToGPU(size_t numColors) {
        if (!m_commandBuffer || !m_cramTexture) return;
        
        // CRAM texture is 64x32 = 2048 color entries
        uint32_t width = 64;
        uint32_t height = (numColors + 63) / 64;  // Calculate required rows
        height = std::min(height, 32u);
        
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
        barrier.image = m_cramTexture;
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
        region.bufferImageHeight = 32;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {width, height, 1};
        
        vkCmdCopyBufferToImage(m_commandBuffer, m_cramStagingBuffer,
            m_cramTexture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        
        // Transition to shader read
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        
        vkCmdPipelineBarrier(m_commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        
        vkEndCommandBuffer(m_commandBuffer);
        
        // Submit and wait
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_commandBuffer;
        
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
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
    
public:
    // ===== VDP1 Command Processing (Direct API for VDP) =====
    
    // Draw a solid-color polygon (4 vertices)
    // Coordinates are in Saturn screen space, scaled by m_internalScale for high-res rendering
    void DrawSolidPolygon(int32_t xa, int32_t ya, int32_t xb, int32_t yb,
                          int32_t xc, int32_t yc, int32_t xd, int32_t yd,
                          Color555 color) override {
        float r, g, b, a;
        Color555ToFloat(color, r, g, b, a);
        
        // Scale coordinates by internal resolution factor
        const float scale = static_cast<float>(m_internalScale);
        float fx0 = static_cast<float>(xa) * scale;
        float fy0 = static_cast<float>(ya) * scale;
        float fx1 = static_cast<float>(xb) * scale;
        float fy1 = static_cast<float>(yb) * scale;
        float fx2 = static_cast<float>(xc) * scale;
        float fy2 = static_cast<float>(yc) * scale;
        float fx3 = static_cast<float>(xd) * scale;
        float fy3 = static_cast<float>(yd) * scale;
        
        // Triangle 1: (A, B, C)
        m_vertices.push_back({{fx0, fy0}, {r, g, b, a}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx1, fy1}, {r, g, b, a}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx2, fy2}, {r, g, b, a}, {0.0f, 0.0f}});
        
        // Triangle 2: (A, C, D)
        m_vertices.push_back({{fx0, fy0}, {r, g, b, a}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx2, fy2}, {r, g, b, a}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx3, fy3}, {r, g, b, a}, {0.0f, 0.0f}});
        
        m_statistics.drawCallCount++;
        m_statistics.triangleCount += 2;
    }
    
    // Draw a textured quad with pre-decoded RGBA texture
    // Full GPU texturing implementation
    void DrawTexturedQuad(int32_t xa, int32_t ya, int32_t xb, int32_t yb,
                          int32_t xc, int32_t yc, int32_t xd, int32_t yd,
                          const uint32_t* textureData, uint32_t texWidth, uint32_t texHeight,
                          bool flipH, bool flipV) override {
        if (!textureData || texWidth == 0 || texHeight == 0) {
            return;  // Invalid texture
        }
        
        if (!m_texturedPipeline || !m_spriteTexture) {
            // Textured pipeline not available, skip
            return;
        }
        
        // Store sprite for batched rendering
        TexturedSprite sprite;
        sprite.texWidth = texWidth;
        sprite.texHeight = texHeight;
        sprite.textureData.assign(textureData, textureData + texWidth * texHeight);
        
        // Scale coordinates by internal resolution factor for high-res rendering
        const float scale = static_cast<float>(m_internalScale);
        float fx0 = static_cast<float>(xa) * scale;
        float fy0 = static_cast<float>(ya) * scale;
        float fx1 = static_cast<float>(xb) * scale;
        float fy1 = static_cast<float>(yb) * scale;
        float fx2 = static_cast<float>(xc) * scale;
        float fy2 = static_cast<float>(yc) * scale;
        float fx3 = static_cast<float>(xd) * scale;
        float fy3 = static_cast<float>(yd) * scale;
        
        // UV coordinates (handle flipping)
        // We use normalized UVs based on actual texture size within max texture
        float uMin = 0.0f;
        float uMax = static_cast<float>(texWidth) / static_cast<float>(kMaxSpriteTextureSize);
        float vMin = 0.0f;
        float vMax = static_cast<float>(texHeight) / static_cast<float>(kMaxSpriteTextureSize);
        
        if (flipH) { std::swap(uMin, uMax); }
        if (flipV) { std::swap(vMin, vMax); }
        
        // White vertex color (texture provides color)
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
        
        // Triangle 1: (A, B, C)
        sprite.vertices[0] = {{fx0, fy0}, {r, g, b, a}, {uMin, vMin}};
        sprite.vertices[1] = {{fx1, fy1}, {r, g, b, a}, {uMax, vMin}};
        sprite.vertices[2] = {{fx2, fy2}, {r, g, b, a}, {uMax, vMax}};
        
        // Triangle 2: (A, C, D)
        sprite.vertices[3] = {{fx0, fy0}, {r, g, b, a}, {uMin, vMin}};
        sprite.vertices[4] = {{fx2, fy2}, {r, g, b, a}, {uMax, vMax}};
        sprite.vertices[5] = {{fx3, fy3}, {r, g, b, a}, {uMin, vMax}};
        
        m_pendingTexturedSprites.push_back(std::move(sprite));
        
        m_statistics.drawCallCount++;
        m_statistics.triangleCount += 2;
        m_statistics.textureUploadCount++;
    }
    
    // Draw a line as a thin quad (scaled for high-res)
    void DrawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, Color555 color) override {
        float r, g, b, a;
        Color555ToFloat(color, r, g, b, a);
        
        const float scale = static_cast<float>(m_internalScale);
        float fx0 = static_cast<float>(x0) * scale;
        float fy0 = static_cast<float>(y0) * scale;
        float fx1 = static_cast<float>(x1) * scale;
        float fy1 = static_cast<float>(y1) * scale;
        
        // Calculate perpendicular offset for line thickness (1 pixel at scaled res)
        float dx = fx1 - fx0;
        float dy = fy1 - fy0;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.001f) return;
        
        float thickness = 0.5f * scale;
        float nx = -dy / len * thickness;
        float ny = dx / len * thickness;
        
        // Quad vertices
        m_vertices.push_back({{fx0 + nx, fy0 + ny}, {r, g, b, a}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx0 - nx, fy0 - ny}, {r, g, b, a}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx1 - nx, fy1 - ny}, {r, g, b, a}, {0.0f, 0.0f}});
        
        m_vertices.push_back({{fx0 + nx, fy0 + ny}, {r, g, b, a}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx1 - nx, fy1 - ny}, {r, g, b, a}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx1 + nx, fy1 + ny}, {r, g, b, a}, {0.0f, 0.0f}});
        
        m_statistics.drawCallCount++;
        m_statistics.triangleCount += 2;
    }
    
    void DrawGouraudLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                          Color555 colorA, Color555 colorB) override {
        float r0, g0, b0, a0;
        float r1, g1, b1, a1;
        Color555ToFloat(colorA, r0, g0, b0, a0);
        Color555ToFloat(colorB, r1, g1, b1, a1);
        
        const float scale = static_cast<float>(m_internalScale);
        float fx0 = static_cast<float>(x0) * scale;
        float fy0 = static_cast<float>(y0) * scale;
        float fx1 = static_cast<float>(x1) * scale;
        float fy1 = static_cast<float>(y1) * scale;
        
        // Calculate perpendicular offset for line thickness
        float dx = fx1 - fx0;
        float dy = fy1 - fy0;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.001f) return;
        
        float thickness = 0.5f * scale;
        float nx = -dy / len * thickness;
        float ny = dx / len * thickness;
        
        // Quad vertices with Gouraud interpolation
        m_vertices.push_back({{fx0 + nx, fy0 + ny}, {r0, g0, b0, a0}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx0 - nx, fy0 - ny}, {r0, g0, b0, a0}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx1 - nx, fy1 - ny}, {r1, g1, b1, a1}, {0.0f, 0.0f}});
        
        m_vertices.push_back({{fx0 + nx, fy0 + ny}, {r0, g0, b0, a0}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx1 - nx, fy1 - ny}, {r1, g1, b1, a1}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx1 + nx, fy1 + ny}, {r1, g1, b1, a1}, {0.0f, 0.0f}});
        
        m_statistics.drawCallCount++;
        m_statistics.triangleCount += 2;
    }
    
    // Draw a Gouraud-shaded polygon (4 vertices, 4 colors)
    // Coordinates are scaled by m_internalScale for high-res rendering
    void DrawGouraudPolygon(int32_t xa, int32_t ya, int32_t xb, int32_t yb,
                            int32_t xc, int32_t yc, int32_t xd, int32_t yd,
                            Color555 colorA, Color555 colorB, Color555 colorC, Color555 colorD) override {
        float r0, g0, b0, a0;
        float r1, g1, b1, a1;
        float r2, g2, b2, a2;
        float r3, g3, b3, a3;
        
        Color555ToFloat(colorA, r0, g0, b0, a0);
        Color555ToFloat(colorB, r1, g1, b1, a1);
        Color555ToFloat(colorC, r2, g2, b2, a2);
        Color555ToFloat(colorD, r3, g3, b3, a3);
        
        // Scale coordinates by internal resolution factor
        const float scale = static_cast<float>(m_internalScale);
        float fx0 = static_cast<float>(xa) * scale;
        float fy0 = static_cast<float>(ya) * scale;
        float fx1 = static_cast<float>(xb) * scale;
        float fy1 = static_cast<float>(yb) * scale;
        float fx2 = static_cast<float>(xc) * scale;
        float fy2 = static_cast<float>(yc) * scale;
        float fx3 = static_cast<float>(xd) * scale;
        float fy3 = static_cast<float>(yd) * scale;
        
        // Triangle 1: (A, B, C)
        m_vertices.push_back({{fx0, fy0}, {r0, g0, b0, a0}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx1, fy1}, {r1, g1, b1, a1}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx2, fy2}, {r2, g2, b2, a2}, {0.0f, 0.0f}});
        
        // Triangle 2: (A, C, D)
        m_vertices.push_back({{fx0, fy0}, {r0, g0, b0, a0}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx2, fy2}, {r2, g2, b2, a2}, {0.0f, 0.0f}});
        m_vertices.push_back({{fx3, fy3}, {r3, g3, b3, a3}, {0.0f, 0.0f}});
        
        m_statistics.drawCallCount++;
        m_statistics.triangleCount += 2;
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
