// Brimir - VDP Renderer Implementation
// Factory functions and renderer utilities

#include <brimir/hw/vdp/vdp_renderer.hpp>

namespace brimir::vdp {

// ===== Renderer Type Names =====

const char* GetRendererTypeName(RendererType type) {
    switch (type) {
    case RendererType::Auto:     return "Auto";
    case RendererType::Software: return "Software";
    case RendererType::GPU:      return "GPU";
    case RendererType::Vulkan:   return "Vulkan";
    case RendererType::OpenGL:   return "OpenGL";
    default:                     return "Unknown";
    }
}

// ===== Forward Declarations =====

// Declare renderer classes (defined in separate .cpp files)
class SoftwareRenderer;

#ifdef BRIMIR_GPU_VULKAN_ENABLED
class VulkanRenderer;
#endif

#ifdef BRIMIR_GPU_OPENGL_ENABLED
class OpenGLRenderer;
#endif

// Declare factory functions for each renderer
extern std::unique_ptr<IVDPRenderer> CreateSoftwareRenderer();

#ifdef BRIMIR_GPU_VULKAN_ENABLED
extern std::unique_ptr<IVDPRenderer> CreateVulkanRenderer();
#endif

// ===== Renderer Availability Checks =====

bool IsRendererAvailable(RendererType type) {
    switch (type) {
    case RendererType::Auto:
        return true;  // Auto always falls back to something
        
    case RendererType::Software:
        return true;  // Software always available
        
    case RendererType::GPU:
        // Check if any GPU renderer is available
#ifdef BRIMIR_GPU_VULKAN_ENABLED
        if (IsRendererAvailable(RendererType::Vulkan)) return true;
#endif
#ifdef BRIMIR_GPU_OPENGL_ENABLED
        if (IsRendererAvailable(RendererType::OpenGL)) return true;
#endif
        return false;
        
    case RendererType::Vulkan:
#ifdef BRIMIR_GPU_VULKAN_ENABLED
        // TODO: Check if Vulkan is actually available
        // For now, assume it's available if compiled in
        return true;
#else
        return false;
#endif
        
    case RendererType::OpenGL:
#ifdef BRIMIR_GPU_OPENGL_ENABLED
        // TODO: Check if OpenGL is actually available
        return true;
#else
        return false;
#endif
        
    default:
        return false;
    }
}

// ===== Renderer Detection =====

RendererType DetectBestRenderer() {
    // Try Vulkan first (best for accuracy and performance)
#ifdef BRIMIR_GPU_VULKAN_ENABLED
    if (IsRendererAvailable(RendererType::Vulkan)) {
        return RendererType::Vulkan;
    }
#endif
    
    // Fall back to OpenGL
#ifdef BRIMIR_GPU_OPENGL_ENABLED
    if (IsRendererAvailable(RendererType::OpenGL)) {
        return RendererType::OpenGL;
    }
#endif
    
    // Always have software as fallback
    return RendererType::Software;
}

// ===== Renderer Factory =====

std::unique_ptr<IVDPRenderer> CreateRenderer(RendererType type) {
    // Handle Auto type
    if (type == RendererType::Auto) {
        type = DetectBestRenderer();
    }
    
    // Handle GPU type (select best GPU renderer)
    if (type == RendererType::GPU) {
#ifdef BRIMIR_GPU_VULKAN_ENABLED
        if (IsRendererAvailable(RendererType::Vulkan)) {
            type = RendererType::Vulkan;
        }
#endif
#ifdef BRIMIR_GPU_OPENGL_ENABLED
        if (IsRendererAvailable(RendererType::OpenGL)) {
            type = RendererType::OpenGL;
        }
#endif
    }
    
    // Create specific renderer
    switch (type) {
    case RendererType::Software:
        return CreateSoftwareRenderer();
        
#ifdef BRIMIR_GPU_VULKAN_ENABLED
    case RendererType::Vulkan:
        return CreateVulkanRenderer();
#endif
        
#ifdef BRIMIR_GPU_OPENGL_ENABLED
    case RendererType::OpenGL:
        // return CreateOpenGLRenderer();
        break;
#endif
        
    default:
        // Fallback to software
        return CreateSoftwareRenderer();
    }
}

} // namespace brimir::vdp

