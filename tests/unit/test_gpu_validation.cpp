#include <catch_amalgamated.hpp>
#include <brimir/hw/vdp/vdp_renderer.hpp>
#include <cstring>
#include <vector>

using namespace brimir::vdp;

// ===== GPU Renderer Availability Tests =====

TEST_CASE("GPU Validation: Renderer factory creates software renderer", "[gpu][validation]") {
    auto renderer = CreateRenderer(RendererType::Software);
    REQUIRE(renderer != nullptr);
    REQUIRE(renderer->Initialize());
    REQUIRE(renderer->GetType() == RendererType::Software);
    
    auto caps = renderer->GetCapabilities();
    REQUIRE(caps.type == RendererType::Software);
    REQUIRE(caps.maxInternalScale == 1);
    
    renderer->Shutdown();
}

TEST_CASE("GPU Validation: Vulkan renderer availability", "[gpu][validation]") {
    bool available = IsRendererAvailable(RendererType::Vulkan);
    
    if (available) {
        auto renderer = CreateRenderer(RendererType::Vulkan);
        REQUIRE(renderer != nullptr);
        
        bool initOk = renderer->Initialize();
        if (initOk) {
            REQUIRE(renderer->GetType() == RendererType::Vulkan);
            
            auto caps = renderer->GetCapabilities();
            REQUIRE(caps.supportsUpscaling == true);
            
            renderer->Shutdown();
        } else {
            WARN("Vulkan renderer available but failed to initialize: " << renderer->GetLastError());
        }
    } else {
        WARN("Vulkan renderer not available on this system");
    }
}

TEST_CASE("GPU Validation: Software renderer upscaling returns false", "[gpu][validation]") {
    auto renderer = CreateRenderer(RendererType::Software);
    REQUIRE(renderer != nullptr);
    REQUIRE(renderer->Initialize());
    
    // Software renderer should not support upscaling
    REQUIRE(renderer->RenderUpscaled() == false);
    REQUIRE(renderer->GetUpscaledFramebuffer() == nullptr);
    
    renderer->Shutdown();
}
