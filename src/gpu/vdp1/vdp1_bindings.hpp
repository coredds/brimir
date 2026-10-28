#pragma once

// Descriptor bindings of the VDP1 compute shaders (src/gpu/shaders/vdp1/cs_vdp1_*.hlsl). Each pipeline has one
// descriptor set (set 0) and one push-constant block (vdp1_hw_params.hpp). As for VDP2 (vdp2_bindings.hpp), a resource
// has the same binding in every shader that uses it, bindings 0-15 are read-only buffers (HLSL t registers, shifted by
// 0) and bindings 16-31 are storage buffers (u registers, shifted by 16; tools/shaders/build_shaders.cmake). VDP1 has
// no images: everything is a storage buffer.
//
// The passes (docs/ymir-hw-renderer-notes/stage2-vdp1-research.md section 4.3), per batch of spans:
//   raster    one thread per span: software's line loop; writes the fragments (slot = order key) and counts them per
//             unit (an FBRAM word of one field)
//   bucket    exclusive scan of the per-unit counts (offsets of each unit's bucket)
//   scatter   writes each live fragment's slot index into its unit's bucket
//   resolve   one thread per unit: sorts its bucket's indices, applies the fragments in key order with software's
//             write formulas to the FBRAM words
//   erase     the erase region, one thread per word
//   fbwrite   CPU framebuffer writes, in order
//
//  binding  register  resource        descriptor type  size / element                          used by
//        0  t0        g_commands      STORAGE_BUFFER   CommandParams, 80 B each                 raster
//        1  t1        g_spans         STORAGE_BUFFER   Span, 32 B each                          raster
//        2  t2        g_vram          STORAGE_BUFFER   512 KiB (the VDP2 VRAM buffer)           raster
//        3  t3        g_fbWrites      STORAGE_BUFFER   FbWrite, 16 B each                       fbwrite
//       16  u0        g_fragments     STORAGE_BUFFER   Fragment, 8 B each, kMaxFragmentsPerBatch raster, scatter, resolve
//       17  u1        g_unitCounts    STORAGE_BUFFER   uint[kUnitCount]                         raster, bucket, scatter,
//                                                                                              resolve
//       18  u2        g_bucket        STORAGE_BUFFER   uint[kMaxFragmentsPerBatch]              scatter, resolve
//       19  u3        g_fbram         STORAGE_BUFFER   2 MiB, the VDP2 sprite buffer's layout   resolve, erase, fbwrite
//       20  u4        g_scanSums      STORAGE_BUFFER   uint[kScanBlockCount]                    bucket
//
// g_vram (2) and g_fbram (19) are views of the buffers the VDP2 back half binds as kBindingVRAM and kBindingSpriteFB
// (the GPU FBRAM is that sprite buffer: VDP1 writes it, the VDP2 sprite pass reads it). g_unitCounts is zero between
// batches (the resolve clears what it consumed). The buffer sizes and the scan sizing below are what stage 2 starts
// from; a shader that needs another resource adds a binding here.
// No Vulkan header may be included here.

#include "gpu/vdp1/vdp1_hw_params.hpp"
#include "gpu/vdp2/vdp2_bindings.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace brimir::gpu::vdp1 {

using vdp2::Binding;
using vdp2::DescriptorKind;
using vdp2::StageUsage;

inline constexpr uint32_t kBindingCommands = 0;
inline constexpr uint32_t kBindingSpans = 1;
inline constexpr uint32_t kBindingVRAM = 2;
inline constexpr uint32_t kBindingFbWrites = 3;
inline constexpr uint32_t kBindingFragments = 16;
inline constexpr uint32_t kBindingUnitCounts = 17;
inline constexpr uint32_t kBindingBucket = 18;
inline constexpr uint32_t kBindingFbram = 19;
inline constexpr uint32_t kBindingScanSums = 20;

/// @brief Sizes the shaders index without further checks (they guard the rest by the push constants).
inline constexpr uint32_t kVRAMBufferSize = vdp2::kVRAMBufferSize;
static_assert(kFbramBufferSize == vdp2::kSpriteFBBufferSize);

/// @brief The most fragment slots of one batch: the front half (vdp1_front.hpp) closes a batch between spans when the
/// next span's slots would not fit. 4 Mi x 8 B = 32 MiB, the largest storage buffer range any binding needs
/// (gpu_version.hpp requires it of maxStorageBufferRange; Vulkan guarantees 2^27).
inline constexpr uint32_t kMaxFragmentsPerBatch = 4u * 1024 * 1024;
inline constexpr uint32_t kFragmentBufferSize = kMaxFragmentsPerBatch * sizeof(Vdp1Fragment);
inline constexpr uint32_t kBucketBufferSize = kMaxFragmentsPerBatch * sizeof(uint32_t);
inline constexpr uint32_t kUnitCountsBufferSize = kUnitCount * sizeof(uint32_t);
static_assert(kFragmentBufferSize == 32u * 1024 * 1024 && kFragmentBufferSize <= (1u << 27));

/// @brief The most fragment slots of one span: 2 * LineStepper::Length() after SystemClip. SystemClip keeps a line in
/// the padded clip box [-1, clip + 2] of its major axis (or leaves it whole when both ends are inside that box), and
/// the clip is the raw 16-bit system clip of the command: CMDXC up to 0xFFFF and CMDYC up to 0xFFFF, which doubleV
/// makes (0xFFFF << 1) | 1. So a line has at most 0x1FFFF + 4 pixels: 262,150 slots, which always fit a fresh batch.
inline constexpr uint32_t kMaxSpanSlots = 2u * ((2u * 0xFFFFu + 1u) + 4u);
static_assert(kMaxSpanSlots == 262150 && kMaxSpanSlots <= kMaxFragmentsPerBatch);

/// @brief The most spans, commands and CPU framebuffer write records of one batch (one op). The front half closes a
/// batch between spans (or a write run between records) at any of them.
/// - spans: 256 Ki x 32 B = 8 MiB (a polygon-heavy frame has about 10 K);
/// - commands: 16 Ki x 80 B = 1.25 MiB, as many command tables as VDP1 VRAM holds;
/// - writes: 256 Ki x 16 B = 4 MiB, one per byte of a framebuffer.
inline constexpr uint32_t kMaxSpansPerBatch = 256u * 1024;
inline constexpr uint32_t kMaxCommandsPerBatch = 16u * 1024;
inline constexpr uint32_t kMaxFbWritesPerBatch = 256u * 1024;
inline constexpr uint32_t kSpanBufferSize = kMaxSpansPerBatch * sizeof(Vdp1Span);
inline constexpr uint32_t kCommandBufferSize = kMaxCommandsPerBatch * sizeof(Vdp1CommandParams);
inline constexpr uint32_t kFbWriteBufferSize = kMaxFbWritesPerBatch * sizeof(Vdp1FbWrite);
static_assert(kMaxSpansPerBatch <= kMaxFragmentsPerBatch); // span indices fit the fragment-indexed passes
static_assert(kMaxCommandsPerBatch * 0x20 == kVRAMBufferSize); // 32-byte command tables in 512 KiB of VDP1 VRAM
static_assert(kMaxFbWritesPerBatch == kFbramSliceSize);

/// @brief The raster pass's `scale` push constant (Vdp1BatchPushConstants::scale): stage 2 draws at native resolution.
inline constexpr uint32_t kRasterScale = 1;

/// @brief The largest range of one storage buffer binding (the device limit maxStorageBufferRange must cover it).
inline constexpr uint32_t kMaxStorageBufferRange =
    std::max({kFragmentBufferSize, kBucketBufferSize, kUnitCountsBufferSize, kFbramBufferSize, kVRAMBufferSize,
              kSpanBufferSize, kCommandBufferSize, kFbWriteBufferSize});
static_assert(kMaxStorageBufferRange <= (1u << 27));

/// @brief Workgroup size of the VDP1 passes (one dimension, or 8 x 8 for the erase pass): 64 invocations, within the
/// 128 Vulkan guarantees and what VDP2 already requires (vdp2::kMaxWorkgroupInvocations).
inline constexpr uint32_t kWorkgroupSize = 64;
/// @brief The exclusive scan over kUnitCount counts: blocks of one workgroup, with a scan of the block sums.
inline constexpr uint32_t kScanBlockSize = kWorkgroupSize;
inline constexpr uint32_t kScanBlockCount = kUnitCount / kScanBlockSize;
inline constexpr uint32_t kScanSumsBufferSize = kScanBlockCount * sizeof(uint32_t);
static_assert(kUnitCount % kScanBlockSize == 0);

/// @brief Push-constant blocks per pipeline: the largest is the erase pass's 32 bytes, below VDP2's 64.
inline constexpr uint32_t kMaxPushConstantSize =
    std::max({static_cast<uint32_t>(sizeof(Vdp1BatchPushConstants)), static_cast<uint32_t>(sizeof(Vdp1ErasePushConstants)),
              static_cast<uint32_t>(sizeof(Vdp1FbWritePushConstants))});
static_assert(kMaxPushConstantSize == 32);

/// @brief Workgroup (groupshared) memory of the VDP1 passes. None so far; the resolve pass adds its sort's share when it
/// needs one (the device limit maxComputeSharedMemorySize must cover it; gpu_version.hpp takes the maximum with VDP2).
inline constexpr uint32_t kMaxComputeSharedMemorySize = 0;

inline constexpr std::array<Binding, 5> kRasterBindings{{
    {kBindingCommands, DescriptorKind::StorageBuffer},
    {kBindingSpans, DescriptorKind::StorageBuffer},
    {kBindingVRAM, DescriptorKind::StorageBuffer},
    {kBindingFragments, DescriptorKind::StorageBuffer},
    {kBindingUnitCounts, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 2> kBucketBindings{{
    {kBindingUnitCounts, DescriptorKind::StorageBuffer},
    {kBindingScanSums, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 3> kScatterBindings{{
    {kBindingFragments, DescriptorKind::StorageBuffer},
    {kBindingUnitCounts, DescriptorKind::StorageBuffer},
    {kBindingBucket, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 4> kResolveBindings{{
    {kBindingFragments, DescriptorKind::StorageBuffer},
    {kBindingUnitCounts, DescriptorKind::StorageBuffer},
    {kBindingBucket, DescriptorKind::StorageBuffer},
    {kBindingFbram, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 1> kEraseBindings{{
    {kBindingFbram, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 2> kFbWriteBindings{{
    {kBindingFbWrites, DescriptorKind::StorageBuffer},
    {kBindingFbram, DescriptorKind::StorageBuffer},
}};

/// @brief The layout probe (cs_vdp1_layout_probe.hlsl) declares every resource and every push-constant struct.
inline constexpr std::array<Binding, 9> kLayoutProbeBindings{{
    {kBindingCommands, DescriptorKind::StorageBuffer},
    {kBindingSpans, DescriptorKind::StorageBuffer},
    {kBindingVRAM, DescriptorKind::StorageBuffer},
    {kBindingFbWrites, DescriptorKind::StorageBuffer},
    {kBindingFragments, DescriptorKind::StorageBuffer},
    {kBindingUnitCounts, DescriptorKind::StorageBuffer},
    {kBindingBucket, DescriptorKind::StorageBuffer},
    {kBindingFbram, DescriptorKind::StorageBuffer},
    {kBindingScanSums, DescriptorKind::StorageBuffer},
}};

/// @brief The largest per-stage use over the VDP1 pipelines (gpu_version.hpp takes the maximum with VDP2's).
constexpr StageUsage MaxStageUsage() {
    StageUsage usage;
    for (std::span<const Binding> bindings : {std::span<const Binding>(kRasterBindings),
                                              std::span<const Binding>(kBucketBindings),
                                              std::span<const Binding>(kScatterBindings),
                                              std::span<const Binding>(kResolveBindings),
                                              std::span<const Binding>(kEraseBindings),
                                              std::span<const Binding>(kFbWriteBindings)}) {
        const uint32_t storageBuffers =
            vdp2::CountBindings(bindings, DescriptorKind::StorageBuffer, DescriptorKind::StorageBufferDynamic);
        const uint32_t storageImages =
            vdp2::CountBindings(bindings, DescriptorKind::StorageImage, DescriptorKind::StorageImage);
        const uint32_t sampledImages =
            vdp2::CountBindings(bindings, DescriptorKind::SampledImage, DescriptorKind::SampledImage);
        usage.storageBuffers = std::max(usage.storageBuffers, storageBuffers);
        usage.storageImages = std::max(usage.storageImages, storageImages);
        usage.sampledImages = std::max(usage.sampledImages, sampledImages);
        usage.resources = std::max(usage.resources, static_cast<uint32_t>(bindings.size()));
    }
    return usage;
}

inline constexpr StageUsage kMaxStageUsage = MaxStageUsage();
static_assert(kMaxStageUsage.storageBuffers == 5 && kMaxStageUsage.storageImages == 0 &&
              kMaxStageUsage.sampledImages == 0 && kMaxStageUsage.resources == 5);

} // namespace brimir::gpu::vdp1
