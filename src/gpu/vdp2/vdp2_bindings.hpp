#pragma once

// Descriptor bindings of the three VDP2 compute shaders (src/gpu/shaders/vdp2/cs_vdp2_*.hlsl). Each pipeline has one
// descriptor set (set 0) and the 48-byte VDP2CommonRenderParams push-constant block. A resource has the same binding
// in every shader that uses it. Bindings 0-15 are read-only buffers and sampled images (HLSL t registers, shifted by
// 0), bindings 16-31 are storage buffers and images (u registers, shifted by 16; tools/shaders/build_shaders.cmake).
//
//  binding  register  resource                 descriptor type          image format / size              used by
//        0  t0        g_layerRenderParams      STORAGE_BUFFER_DYNAMIC   VDP2LayerRenderParams, 1,464 B   sprite, bgs
//        1  t1        g_composeParams          STORAGE_BUFFER_DYNAMIC   VDP2ComposeParams, 80 B          compose
//        2  t2        g_vram                   STORAGE_BUFFER           512 KiB                          sprite, bgs
//        3  t3        g_cramColor              STORAGE_BUFFER           uint[2048] packed RGBA8          sprite, bgs
//        4  t4        g_cramRotCoeff           STORAGE_BUFFER           2 KiB (upper CRAM)               bgs
//        5  t5        g_rotParamBases          STORAGE_BUFFER           VDP2RotParamBase[2 * 512]        sprite, bgs
//        6  t6        g_spriteFB               STORAGE_BUFFER           2 MiB, Ymir's slice layout       sprite
//        7  t7        g_nbgLines               STORAGE_BUFFER           VDP2NBGLine[4 * 512]             bgs
//        8  t8        g_lnclBackIn             STORAGE_BUFFER           uint[2 * 512] packed RGBA8       compose
//        9  t9        g_spriteAttrsIn          SAMPLED_IMAGE            R32_UINT 704x512, 2 layers       bgs, compose
//       10  t10       g_layerIn                SAMPLED_IMAGE            R8G8B8A8_UINT 704x512, 8 layers  compose
//       11  t11       g_rbgLineColorIn         SAMPLED_IMAGE            R8G8B8A8_UINT 352x512, 2 layers  compose
//       12  t12       g_colorCalcWindowIn      SAMPLED_IMAGE            R32_UINT 704x512 (2D)            compose
//       16  u0        g_layerOut               STORAGE_IMAGE            R8G8B8A8_UINT 704x512, 8 layers  sprite, bgs
//       17  u1        g_spriteAttrsOut         STORAGE_IMAGE            R32_UINT 704x512, 2 layers       sprite
//       18  u2        g_rbgLineColorOut        STORAGE_IMAGE            R8G8B8A8_UINT 352x512, 2 layers  bgs
//       19  u3        g_colorCalcWindowOut     STORAGE_IMAGE            R32_UINT 704x512 (2D)            bgs
//       20  u4        g_compositeOut           STORAGE_BUFFER           uint[704 * 512] packed XBGR      compose
//
// Sampled and storage bindings of the same data (9/17, 10/16, 11/18, 12/19) are views of one image, used in
// VK_IMAGE_LAYOUT_GENERAL. Image views are 2D arrays except the colour calculation window (2D).
// g_nbgLines is declared and bound but not read yet (per-line NBG state, stage 1B Task 6).
// No Vulkan header may be included here.

#include "gpu/vdp2/vdp2_hw_params.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace brimir::gpu::vdp2 {

inline constexpr uint32_t kBindingLayerParams = 0;
inline constexpr uint32_t kBindingComposeParams = 1;
inline constexpr uint32_t kBindingVRAM = 2;
inline constexpr uint32_t kBindingCRAMColor = 3;
inline constexpr uint32_t kBindingCRAMRotCoeff = 4;
inline constexpr uint32_t kBindingRotParamBases = 5;
inline constexpr uint32_t kBindingSpriteFB = 6;
inline constexpr uint32_t kBindingNBGLines = 7;
inline constexpr uint32_t kBindingLnclBack = 8;
inline constexpr uint32_t kBindingSpriteAttrsIn = 9;
inline constexpr uint32_t kBindingLayerIn = 10;
inline constexpr uint32_t kBindingRBGLineColorIn = 11;
inline constexpr uint32_t kBindingColorCalcWindowIn = 12;
inline constexpr uint32_t kBindingLayerOut = 16;
inline constexpr uint32_t kBindingSpriteAttrsOut = 17;
inline constexpr uint32_t kBindingRBGLineColorOut = 18;
inline constexpr uint32_t kBindingColorCalcWindowOut = 19;
inline constexpr uint32_t kBindingCompositeOut = 20;

/// @brief Size of the push-constant block (VDP2CommonRenderParams) every VDP2 pipeline declares.
inline constexpr uint32_t kPushConstantSize = sizeof(VDP2CommonRenderParams);
static_assert(kPushConstantSize == 48);

/// @brief Buffer and image sizes the shaders index without further checks (they mask or guard everything else).
inline constexpr uint32_t kVRAMBufferSize = 512 * 1024;
inline constexpr uint32_t kSpriteFBBufferSize = 8 * 256 * 1024;
inline constexpr uint32_t kRotParamBaseCount = 2 * kMaxResV;
inline constexpr uint32_t kNBGLineCount = 4 * kMaxResV;
inline constexpr uint32_t kLnclBackCount = 2 * kMaxResV;
inline constexpr uint32_t kLayerImageLayers = 8;      // NBG0-3, RBG0-1, sprite, mesh
inline constexpr uint32_t kSpriteAttrImageLayers = 2; // sprite, mesh
inline constexpr uint32_t kRBGLineColorWidth = 352;
inline constexpr uint32_t kRBGLineColorLayers = 2; // RBG0, RBG1
inline constexpr uint32_t kCompositeBufferSize = kMaxResH * kMaxResV * sizeof(uint32_t);

/// @brief Workgroup sizes: sprite and compose dispatch (HRes / 32, lines, sprite: 1 or 2 layers / compose: 1), bgs
/// dispatches (HRes / 32, lines, kBgsDispatchZ).
inline constexpr uint32_t kWorkgroupWidth = 32;
inline constexpr uint32_t kBgsDispatchZ = 7; // NBG0-3, RBG0-1, colour calculation window

/// @brief Descriptor kinds, mapped to VkDescriptorType by the Vulkan side.
enum class DescriptorKind : uint8_t { StorageBuffer, StorageBufferDynamic, SampledImage, StorageImage };

/// @brief Format of an image binding's image (storage images declare it in the shader; sampled views read it as
/// Unknown). None for buffers.
enum class ImageFormat : uint8_t { None, Rgba8ui, R32ui };

struct Binding {
    uint32_t binding;
    DescriptorKind kind;
    ImageFormat format = ImageFormat::None;
};

inline constexpr std::array<Binding, 7> kRenderSpriteBindings{{
    {kBindingLayerParams, DescriptorKind::StorageBufferDynamic},
    {kBindingVRAM, DescriptorKind::StorageBuffer},
    {kBindingCRAMColor, DescriptorKind::StorageBuffer},
    {kBindingRotParamBases, DescriptorKind::StorageBuffer},
    {kBindingSpriteFB, DescriptorKind::StorageBuffer},
    {kBindingLayerOut, DescriptorKind::StorageImage, ImageFormat::Rgba8ui},
    {kBindingSpriteAttrsOut, DescriptorKind::StorageImage, ImageFormat::R32ui},
}};

inline constexpr std::array<Binding, 10> kRenderBgsBindings{{
    {kBindingLayerParams, DescriptorKind::StorageBufferDynamic},
    {kBindingVRAM, DescriptorKind::StorageBuffer},
    {kBindingCRAMColor, DescriptorKind::StorageBuffer},
    {kBindingCRAMRotCoeff, DescriptorKind::StorageBuffer},
    {kBindingRotParamBases, DescriptorKind::StorageBuffer},
    {kBindingNBGLines, DescriptorKind::StorageBuffer},
    {kBindingSpriteAttrsIn, DescriptorKind::SampledImage, ImageFormat::R32ui},
    {kBindingLayerOut, DescriptorKind::StorageImage, ImageFormat::Rgba8ui},
    {kBindingRBGLineColorOut, DescriptorKind::StorageImage, ImageFormat::Rgba8ui},
    {kBindingColorCalcWindowOut, DescriptorKind::StorageImage, ImageFormat::R32ui},
}};

inline constexpr std::array<Binding, 7> kComposeBindings{{
    {kBindingComposeParams, DescriptorKind::StorageBufferDynamic},
    {kBindingLnclBack, DescriptorKind::StorageBuffer},
    {kBindingSpriteAttrsIn, DescriptorKind::SampledImage, ImageFormat::R32ui},
    {kBindingLayerIn, DescriptorKind::SampledImage, ImageFormat::Rgba8ui},
    {kBindingRBGLineColorIn, DescriptorKind::SampledImage, ImageFormat::Rgba8ui},
    {kBindingColorCalcWindowIn, DescriptorKind::SampledImage, ImageFormat::R32ui},
    {kBindingCompositeOut, DescriptorKind::StorageBuffer},
}};

/// @brief Descriptors of the given kinds in one pipeline's set (a compute pipeline has one stage).
constexpr uint32_t CountBindings(std::span<const Binding> bindings, DescriptorKind a, DescriptorKind b) {
    uint32_t count = 0;
    for (const Binding &binding : bindings) {
        if (binding.kind == a || binding.kind == b) {
            ++count;
        }
    }
    return count;
}

/// @brief The largest per-stage use over the three VDP2 pipelines (gpu_version.hpp's device limits match these).
struct StageUsage {
    uint32_t storageBuffers = 0; // STORAGE_BUFFER and STORAGE_BUFFER_DYNAMIC
    uint32_t storageImages = 0;
    uint32_t sampledImages = 0;
    uint32_t resources = 0; // all descriptors (maxPerStageResources)
};

constexpr StageUsage MaxStageUsage() {
    StageUsage usage;
    for (std::span<const Binding> bindings : {std::span<const Binding>(kRenderSpriteBindings),
                                              std::span<const Binding>(kRenderBgsBindings),
                                              std::span<const Binding>(kComposeBindings)}) {
        const uint32_t storageBuffers =
            CountBindings(bindings, DescriptorKind::StorageBuffer, DescriptorKind::StorageBufferDynamic);
        const uint32_t storageImages =
            CountBindings(bindings, DescriptorKind::StorageImage, DescriptorKind::StorageImage);
        const uint32_t sampledImages =
            CountBindings(bindings, DescriptorKind::SampledImage, DescriptorKind::SampledImage);
        usage.storageBuffers = std::max(usage.storageBuffers, storageBuffers);
        usage.storageImages = std::max(usage.storageImages, storageImages);
        usage.sampledImages = std::max(usage.sampledImages, sampledImages);
        usage.resources = std::max(usage.resources, static_cast<uint32_t>(bindings.size()));
    }
    return usage;
}

inline constexpr StageUsage kMaxStageUsage = MaxStageUsage();

} // namespace brimir::gpu::vdp2
