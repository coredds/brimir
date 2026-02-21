#pragma once

// Forward declaration for VulkanRenderer
// Allows tests to dynamic_cast to VulkanRenderer for direct draw calls

#ifdef BRIMIR_GPU_VULKAN_ENABLED

namespace brimir::vdp {

class VulkanRenderer;

} // namespace brimir::vdp

#endif // BRIMIR_GPU_VULKAN_ENABLED

