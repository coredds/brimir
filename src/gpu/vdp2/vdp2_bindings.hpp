#pragma once

// Descriptor bindings of the five VDP2 compute shaders (src/gpu/shaders/vdp2/cs_vdp2_*.hlsl). Each pipeline has one
// descriptor set (set 0) and the 56-byte VDP2CommonRenderParams push-constant block (the NBG fetch pass: followed by
// its row count, VDP2NBGFetchPushConstants; the rescale pass: VDP2RescalePushConstants). A resource has the same
// binding in every shader that uses it. Bindings 0-15 are read-only buffers and sampled images (HLSL t registers, shifted by
// 0), bindings 16-31 are storage buffers and images (u registers, shifted by 16; tools/shaders/build_shaders.cmake).
// N is the internal resolution (VDP2CommonRenderParams::renderScale, 1-4): only the background layers and the
// composite are N x the native size (and, with VDP1 upscaling, the N x sprite images below); everything else is native
// at every N.
//
//  binding  register  resource                 descriptor type          image format / size                    used by
//        0  t0        g_layerRenderParams      STORAGE_BUFFER_DYNAMIC   VDP2LayerRenderParams, 1,464 B         sprite, bgs
//        1  t1        g_composeParams          STORAGE_BUFFER_DYNAMIC   VDP2ComposeParams, 80 B                compose
//        2  t2        g_vram                   STORAGE_BUFFER           512 KiB                                sprite, bgs
//        3  t3        g_cramColor              STORAGE_BUFFER           uint[2048] packed RGBA8                sprite, bgs
//        4  t4        g_cramRotCoeff           STORAGE_BUFFER           2 KiB (upper CRAM)                     bgs
//        5  t5        g_rotParamBases          STORAGE_BUFFER           VDP2RotParamBase[2 * 512]              sprite, bgs
//        6  t6        g_spriteFB               STORAGE_BUFFER           2 MiB, Ymir's slice layout             sprite
//        7  t7        g_nbgLines               STORAGE_BUFFER           VDP2NBGLine[4 * 512]                   bgs
//        8  t8        g_lnclBackIn             STORAGE_BUFFER           uint[2 * 512] packed RGBA8             compose
//        9  t9        g_spriteAttrsIn          SAMPLED_IMAGE            R32_UINT 704x512, 2 layers             bgs, compose
//       10  t10       g_layerIn                SAMPLED_IMAGE            R8G8B8A8_UINT 704Nx512N, 6 layers      compose
//       11  t11       g_rbgLineColorIn         SAMPLED_IMAGE            R8G8B8A8_UINT 352x512, 2 layers        compose
//       12  t12       g_colorCalcWindowIn      SAMPLED_IMAGE            R32_UINT 704x512 (2D)                  compose
//       13  t13       g_nbgChars               STORAGE_BUFFER           uint[4 * 512 * 704]                    bgs
//       14  t14       g_spriteLayerIn          SAMPLED_IMAGE            R8G8B8A8_UINT 704x512, 2 layers        compose
//       15  t15       g_rescaleSource          STORAGE_BUFFER           the previous composite (old N)         rescale
//       16  u0        g_layerOut               STORAGE_IMAGE            R8G8B8A8_UINT 704Nx512N, 6 layers      bgs
//       17  u1        g_spriteAttrsOut         STORAGE_IMAGE            R32_UINT 704x512, 2 layers             sprite
//       18  u2        g_rbgLineColorOut        STORAGE_IMAGE            R8G8B8A8_UINT 352x512, 2 layers        bgs
//       19  u3        g_colorCalcWindowOut     STORAGE_IMAGE            R32_UINT 704x512 (2D)                  bgs
//       20  u4        g_compositeOut           STORAGE_BUFFER           uint[704N * 512N] packed XBGR          compose
//       21  u5        g_nbgCharsOut            STORAGE_BUFFER           uint[4 * 512 * 704]                    nbg_fetch
//       22  u6        g_spriteLayerOut         STORAGE_IMAGE            R8G8B8A8_UINT 704x512, 2 layers        sprite
//       23  u7        g_spriteFBNx             STORAGE_BUFFER           the N x VDP1 surface (read only)       sprite
//
// VDP1 upscaling (GPU stage 3): the N x sprite pass and the compose dispatches that read its output use descriptor sets
// of their own (the sprite and compose layouts) in which 9/17 and 14/22 are the N x sprite attribute and sprite layer
// images (704N x 512N, 2 layers each) and 23 the N x VDP1 surface (Vdp1VulkanBackend::NxSurfaceBuffer,
// Vdp1NxFbramBufferSize(N)); in every other set 23 is the sprite framebuffer (6), which the shader never reads there.
//
// The NBG fetch pass (nbg_fetch) uses bindings 0, 2, 7 and 9 as bgs does. The rescale pass (cs_vdp2_rescale.hlsl, run
// once after an internal resolution change) copies the previous composite (15) into the new one (20); its push
// constants are VDP2RescalePushConstants.
// Sampled and storage bindings of the same data (9/17, 10/16, 11/18, 12/19, 14/22) are views of one image, used in
// VK_IMAGE_LAYOUT_GENERAL. The background layer image (10/16) holds NBG0-3 and RBG0-1, the sprite layer image (14/22)
// the sprite and mesh layers. Image views are 2D arrays except the colour calculation window (2D). Bindings 13 and 21
// are the same buffer, the NBG character table (vdp2_nbg_chars.hlsli: [nbg][dispatch row][x],
// ymir::vdp::Character::u32). g_nbgLines holds the per-line NBG state (Vdp2FrameRecord::nbgLines, [nbg * 512 +
// dispatch row]).
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
inline constexpr uint32_t kBindingNBGChars = 13;
inline constexpr uint32_t kBindingSpriteLayerIn = 14;
inline constexpr uint32_t kBindingRescaleSource = 15;
inline constexpr uint32_t kBindingLayerOut = 16;
inline constexpr uint32_t kBindingSpriteAttrsOut = 17;
inline constexpr uint32_t kBindingRBGLineColorOut = 18;
inline constexpr uint32_t kBindingColorCalcWindowOut = 19;
inline constexpr uint32_t kBindingCompositeOut = 20;
inline constexpr uint32_t kBindingNBGCharsOut = 21;
inline constexpr uint32_t kBindingSpriteLayerOut = 22;
inline constexpr uint32_t kBindingSpriteFBNx = 23;

/// @brief Size of the push-constant block (VDP2CommonRenderParams) the sprite, bgs and compose pipelines declare.
inline constexpr uint32_t kPushConstantSize = sizeof(VDP2CommonRenderParams);
static_assert(kPushConstantSize == 56);

/// @brief The NBG fetch pass's push constants (cs_vdp2_nbg_fetch.hlsl NBGFetchPushConstants): the common parameters,
/// then the number of dispatch rows from common.startY.
struct VDP2NBGFetchPushConstants {
    VDP2CommonRenderParams common;
    HLSLuint rows;
    HLSLuint pad; // std430 tail (the block aligns to 8)
};
inline constexpr uint32_t kNBGFetchPushConstantSize = sizeof(VDP2NBGFetchPushConstants);
static_assert(kNBGFetchPushConstantSize == 64);

/// @brief The rescale pass's push constants (cs_vdp2_rescale.hlsl RescaleParams).
struct VDP2RescalePushConstants {
    HLSLuint width;    // native frame size the previous composite holds
    HLSLuint height;
    HLSLuint srcScale; // its internal resolution
    HLSLuint dstScale; // the new composite's
};
inline constexpr uint32_t kRescalePushConstantSize = sizeof(VDP2RescalePushConstants);
static_assert(kRescalePushConstantSize == 16);

/// @brief The largest push-constant block of the VDP2 pipelines (gpu_version.hpp requires it).
inline constexpr uint32_t kMaxPushConstantSize = std::max(kPushConstantSize, kNBGFetchPushConstantSize);

/// @brief Buffer and image sizes the shaders index without further checks (they mask or guard everything else).
inline constexpr uint32_t kVRAMBufferSize = 512 * 1024;
inline constexpr uint32_t kSpriteFBBufferSize = 8 * 256 * 1024;
inline constexpr uint32_t kRotParamBaseCount = 2 * kMaxResV;
inline constexpr uint32_t kNBGLineCount = 4 * kMaxResV;
inline constexpr uint32_t kLnclBackCount = 2 * kMaxResV;
inline constexpr uint32_t kBGLayerImageLayers = 6;     // NBG0-3, RBG0-1 (N x the native size)
inline constexpr uint32_t kSpriteLayerImageLayers = 2; // sprite, mesh (native)
inline constexpr uint32_t kSpriteAttrImageLayers = 2;  // sprite, mesh
inline constexpr uint32_t kRBGLineColorWidth = 352;
inline constexpr uint32_t kRBGLineColorLayers = 2; // RBG0, RBG1

/// @brief Internal resolutions the back half renders at (VDP2CommonRenderParams::renderScale).
inline constexpr uint32_t kMaxRenderScale = 4;
/// @brief Size of the background layer image at internal resolution n: (kMaxResH * n) x (kMaxResV * n), at most
/// 2816 x 2048 (within the guaranteed maxImageDimension2D of 4096).
inline constexpr uint32_t ScaledResH(uint32_t n) {
    return kMaxResH * n;
}
inline constexpr uint32_t ScaledResV(uint32_t n) {
    return kMaxResV * n;
}
static_assert(ScaledResH(kMaxRenderScale) == 2816 && ScaledResV(kMaxRenderScale) <= 4096);
/// @brief The composite buffer at internal resolution n: uint[(704 n) * (512 n)], pitch n * HRes (23 MiB at 4, within
/// the guaranteed maxStorageBufferRange of 2^27).
inline constexpr uint32_t CompositeBufferSize(uint32_t n) {
    return ScaledResH(n) * ScaledResV(n) * sizeof(uint32_t);
}
inline constexpr uint32_t kCompositeBufferSize = CompositeBufferSize(1); // at internal resolution 1
static_assert(CompositeBufferSize(kMaxRenderScale) <= (1u << 27));
inline constexpr uint32_t kNBGCharCount = 4 * kMaxResV * kMaxResH;
inline constexpr uint32_t kNBGCharBufferSize = kNBGCharCount * sizeof(uint32_t);

/// @brief Workgroup sizes: sprite dispatches (HRes / 32, lines, 1 or 2 layers), compose (N * HRes / 32, N * lines,
/// 1), bgs (N * HRes / 32, N * lines, kBgsDispatchZ), the NBG fetch pass (lines, 1, kNBGFetchDispatchZ): one workgroup
/// per native row and NBG.
inline constexpr uint32_t kWorkgroupWidth = 32;
inline constexpr uint32_t kBgsDispatchZ = 7; // NBG0-3, RBG0-1, colour calculation window
inline constexpr uint32_t kNBGFetchDispatchZ = 4; // NBG0-3

/// @brief Brimir: whether NBG `index` takes its characters from the NBG character table (vdp2_nbg_chars.hlsli
/// NBGUsesCharTable, the shaders' copy): an enabled scroll NBG with a character pattern delay in some bank, or NBG0/1
/// with vertical cell scroll on 2x2 characters without mosaic. A batch needs the fetch pass when one does.
inline bool NBGUsesCharTable(const NBGParams &params, uint32_t index) {
    if (!params.base.enabled || params.base.bitmap) {
        return false;
    }
    if (params.base.charPatDelay != 0) {
        return true;
    }
    return index <= 1 && params.vcellScrollEnable && !params.base.mosaicEnable && params.base.cellSizeShift != 0;
}

/// @brief cs_frame_output.hlsl's [numthreads(8, 8, 1)] (gpu/frame_presenter.cpp), and the largest workgroup any
/// shader the renderer runs declares, which the device limit maxComputeWorkGroupInvocations must cover.
inline constexpr uint32_t kFrameOutputGroupSize = 8;
inline constexpr uint32_t kMaxWorkgroupInvocations =
    std::max(kWorkgroupWidth, kFrameOutputGroupSize * kFrameOutputGroupSize);
static_assert(kMaxWorkgroupInvocations == 64);

/// @brief Workgroup (groupshared) memory of the NBG fetch pass (cs_vdp2_nbg_fetch.hlsl: five uint[kMaxResH] arrays and
/// uint[kWorkgroupWidth] scan totals); the other shaders use none. The device limit maxComputeSharedMemorySize must
/// cover it, and the Vulkan 1.2 guaranteed minimum (16384) does.
inline constexpr uint32_t kNBGFetchSharedMemorySize = (5 * kMaxResH + kWorkgroupWidth) * sizeof(uint32_t);
inline constexpr uint32_t kMaxComputeSharedMemorySize = kNBGFetchSharedMemorySize;
static_assert(kMaxComputeSharedMemorySize <= 16384);

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

inline constexpr std::array<Binding, 8> kRenderSpriteBindings{{
    {kBindingLayerParams, DescriptorKind::StorageBufferDynamic},
    {kBindingVRAM, DescriptorKind::StorageBuffer},
    {kBindingCRAMColor, DescriptorKind::StorageBuffer},
    {kBindingRotParamBases, DescriptorKind::StorageBuffer},
    {kBindingSpriteFB, DescriptorKind::StorageBuffer},
    {kBindingSpriteLayerOut, DescriptorKind::StorageImage, ImageFormat::Rgba8ui},
    {kBindingSpriteAttrsOut, DescriptorKind::StorageImage, ImageFormat::R32ui},
    {kBindingSpriteFBNx, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 11> kRenderBgsBindings{{
    {kBindingLayerParams, DescriptorKind::StorageBufferDynamic},
    {kBindingVRAM, DescriptorKind::StorageBuffer},
    {kBindingCRAMColor, DescriptorKind::StorageBuffer},
    {kBindingCRAMRotCoeff, DescriptorKind::StorageBuffer},
    {kBindingRotParamBases, DescriptorKind::StorageBuffer},
    {kBindingNBGLines, DescriptorKind::StorageBuffer},
    {kBindingSpriteAttrsIn, DescriptorKind::SampledImage, ImageFormat::R32ui},
    {kBindingNBGChars, DescriptorKind::StorageBuffer},
    {kBindingLayerOut, DescriptorKind::StorageImage, ImageFormat::Rgba8ui},
    {kBindingRBGLineColorOut, DescriptorKind::StorageImage, ImageFormat::Rgba8ui},
    {kBindingColorCalcWindowOut, DescriptorKind::StorageImage, ImageFormat::R32ui},
}};

inline constexpr std::array<Binding, 8> kComposeBindings{{
    {kBindingComposeParams, DescriptorKind::StorageBufferDynamic},
    {kBindingLnclBack, DescriptorKind::StorageBuffer},
    {kBindingSpriteAttrsIn, DescriptorKind::SampledImage, ImageFormat::R32ui},
    {kBindingLayerIn, DescriptorKind::SampledImage, ImageFormat::Rgba8ui},
    {kBindingSpriteLayerIn, DescriptorKind::SampledImage, ImageFormat::Rgba8ui},
    {kBindingRBGLineColorIn, DescriptorKind::SampledImage, ImageFormat::Rgba8ui},
    {kBindingColorCalcWindowIn, DescriptorKind::SampledImage, ImageFormat::R32ui},
    {kBindingCompositeOut, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 5> kNBGFetchBindings{{
    {kBindingLayerParams, DescriptorKind::StorageBufferDynamic},
    {kBindingVRAM, DescriptorKind::StorageBuffer},
    {kBindingNBGLines, DescriptorKind::StorageBuffer},
    {kBindingSpriteAttrsIn, DescriptorKind::SampledImage, ImageFormat::R32ui},
    {kBindingNBGCharsOut, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 2> kRescaleBindings{{
    {kBindingRescaleSource, DescriptorKind::StorageBuffer},
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

/// @brief The largest per-stage use over the four VDP2 pipelines (gpu_version.hpp's device limits match these).
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
                                              std::span<const Binding>(kComposeBindings),
                                              std::span<const Binding>(kNBGFetchBindings),
                                              std::span<const Binding>(kRescaleBindings)}) {
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
