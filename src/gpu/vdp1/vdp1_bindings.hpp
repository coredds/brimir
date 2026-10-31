#pragma once

// Descriptor bindings of the VDP1 compute shaders (src/gpu/shaders/vdp1/cs_vdp1_*.hlsl). Each pipeline has one
// descriptor set (set 0) and one push-constant block (vdp1_hw_params.hpp). As for VDP2 (vdp2_bindings.hpp), a resource
// has the same binding in every shader that uses it, bindings 0-15 are read-only buffers (HLSL t registers, shifted by
// 0) and bindings 16-31 are storage buffers (u registers, shifted by 16; tools/shaders/build_shaders.cmake). VDP1 has
// no images: everything is a storage buffer.
//
// The passes (docs/ymir-hw-renderer-notes/stage2-vdp1-research.md section 4.3), per batch of spans, in this order with
// a compute-to-compute barrier between them:
//   raster       one thread per span: software's line loop; writes every fragment slot of the span (a fragment, or 0
//                for a slot that has none), counts the fragments per unit (an FBRAM word of one field) and accumulates
//                the batch's unit range (the lowest and highest unit with a fragment) in g_batchState
//   batch_range  one thread: turns the accumulated range into [unitLo, unitHi) and the indirect dispatch arguments of
//                the bucket and resolve passes (zero groups for a batch without fragments), and resets the accumulator
//   bucket       (indirect) per block of kScanBlockSize units of the range: the exclusive scan of the counts within the
//                block (g_unitOffsets) and the block's total (g_scanSums)
//   bucket_sums  one workgroup: the exclusive scan of the range's block totals, in place
//   scatter      writes each live fragment's slot index into its unit's bucket (g_scanSums[block] + g_unitOffsets,
//                advanced atomically: arbitrary order within a bucket)
//   resolve      (indirect) one thread per unit of the range: sorts its bucket's slot indices (the order keys), applies the fragments in key
//                order with software's write formulas to the unit's (main, mesh) FBRAM words and clears its count;
//                units with more than kResolveSmallMax fragments are resolved by the whole workgroup
//   erase        the erase region, one thread per word
//   fb_write_mark   one thread per CPU framebuffer write record of a run: InterlockedMax(record + 1) into the winner
//                   slot of each byte it writes (main field, and the alternate field when it mirrors)
//   fb_write        one thread per record: writes the bytes it won (the last record of the run that writes them: the
//                   in-order result)
//   fb_write_clear  one thread per record: zeroes its winner slots for the next run
//
//  binding  register  resource        descriptor type  size / element                          used by
//        0  t0        g_commands      STORAGE_BUFFER   CommandParams, 80 B each                 raster
//        1  t1        g_spans         STORAGE_BUFFER   Span, 32 B each                          raster
//        2  t2        g_vram          STORAGE_BUFFER   512 KiB, the GPU's copy of VDP1 VRAM     raster
//        3  t3        g_fbWrites      STORAGE_BUFFER   FbWrite, 16 B each                       fbwrite
//       16  u0        g_fragments     STORAGE_BUFFER   Fragment, 8 B each, kMaxFragmentsPerBatch raster, scatter, resolve
//       17  u1        g_unitCounts    STORAGE_BUFFER   uint[kUnitCount]                         raster, bucket, resolve
//       18  u2        g_bucket        STORAGE_BUFFER   uint[kMaxFragmentsPerBatch]              scatter, resolve
//       19  u3        g_fbram         STORAGE_BUFFER   2 MiB, the VDP2 sprite buffer's layout   resolve, erase, fbwrite
//       20  u4        g_scanSums      STORAGE_BUFFER   uint[kScanBlockCount]                    bucket, bucket_sums,
//                                                                                              scatter, resolve
//       21  u5        g_unitOffsets   STORAGE_BUFFER   uint[kUnitCount]                         bucket, scatter, resolve
//       22  u6        g_batchState    STORAGE_BUFFER   uint[kBatchStateWords], also the         raster, batch_range,
//                                                      INDIRECT_BUFFER of the bucket and        bucket, bucket_sums,
//                                                      resolve dispatches                       resolve
//       23  u7        g_fbWinners     STORAGE_BUFFER   uint[kFbWinnerCount] (4 MiB)             fb_write_mark, fb_write,
//                                                                                              fb_write_clear
//
// The unit range makes a batch's fixed cost proportional to the FBRAM area it touched instead of all 262,144 units
// (exact by construction: it is the hull of the units the raster pass counted). Units outside it have no fragments, a
// count of zero and are not looked at.
// g_vram (2) is the VDP1 back half's own buffer, the GPU's copy of VDP1 VRAM (kVRAMBufferSize, ymir::vdp::
// kVDP1VRAMSize; vdp1_front.hpp checks the two are equal): it receives each batch's Vdp1OpList::vramCopies before the
// batch's passes run, and starts zeroed. g_fbram (19) is a view of the buffer the VDP2 back half binds as
// kBindingSpriteFB (the GPU FBRAM is that sprite buffer: VDP1 writes it, the VDP2 sprite pass reads it).
//
// Contract with the back half (Task 5):
// - g_unitCounts is zero at creation and between batches: the raster pass counts into it and the resolve clears every
//   count it consumed, so a batch leaves it zero.
// - The fragment buffer needs no clearing: the raster pass writes all fragmentCount slots of its batch (every span
//   writes each of its 2 * Length() slots, a fragment or 0), and the spans' slots partition [0, fragmentCount). Zero
//   it at creation anyway, so no pass ever reads uninitialised memory.
// - g_unitOffsets, g_scanSums and g_bucket are written before they are read within a batch: no clearing.
// - g_batchState is zero at creation (the accumulator stores the range's low end inverted, so zero is "empty"); the
//   batch_range pass resets the accumulator. Its buffer needs VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, and the barrier after
//   batch_range must include VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT / VK_ACCESS_INDIRECT_COMMAND_READ_BIT.
// - g_fbWinners is zero at creation; fb_write_clear returns it to zero after each run.
// The buffer sizes and the scan sizing below are what stage 2 starts from; a shader that needs another resource adds a
// binding here.
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
inline constexpr uint32_t kBindingUnitOffsets = 21;
inline constexpr uint32_t kBindingBatchState = 22;
inline constexpr uint32_t kBindingFbWinners = 23;

/// @brief g_fbWinners: per (field, framebuffer, FBRAM word, byte) the last CPU write record of the current run that
/// writes it (record index + 1; 0 none). Index Vdp1FbWinnerIndex. 4 MiB.
inline constexpr uint32_t kFbWinnerCount = 2u * 2u * kWordsPerFramebuffer * 2u;
inline constexpr uint32_t kFbWinnersBufferSize = kFbWinnerCount * sizeof(uint32_t);
constexpr uint32_t Vdp1FbWinnerIndex(bool alt, uint32_t fb, uint32_t word, bool lowByte) {
    return ((((alt ? 2u : 0u) + (fb & 1u)) * kWordsPerFramebuffer + (word & kWordIndexMask)) << 1) | (lowByte ? 1u : 0u);
}

/// @brief g_batchState words: the bucket pass's VkDispatchIndirectCommand (x, 1, 1), the resolve pass's, the batch's
/// unit range [unitLo, unitHi) (written by batch_range) and the raster pass's accumulator (InterlockedMax of ~unit and
/// of unit + 1, so that zero means no unit).
inline constexpr uint32_t kBatchStateBucketArgs = 0;
inline constexpr uint32_t kBatchStateResolveArgs = 3;
inline constexpr uint32_t kBatchStateUnitLo = 6;
inline constexpr uint32_t kBatchStateUnitHi = 7;
inline constexpr uint32_t kBatchStateAccLoInv = 8;
inline constexpr uint32_t kBatchStateAccHi = 9;
inline constexpr uint32_t kBatchStateWords = 16;
inline constexpr uint32_t kBatchStateBufferSize = kBatchStateWords * sizeof(uint32_t);

/// @brief The GPU's copy of VDP1 VRAM: 512 KiB (ymir::vdp::kVDP1VRAMSize; the raster pass masks its addresses to it).
inline constexpr uint32_t kVRAMBufferSize = 512u * 1024;
static_assert(kFbramBufferSize == vdp2::kSpriteFBBufferSize);

/// @brief The most fragment slots of one batch: the front half (vdp1_front.hpp) closes a batch between spans when the
/// next span's slots would not fit. 4 Mi x 8 B = 32 MiB, the largest storage buffer range any binding needs
/// (gpu_version.hpp requires it of maxStorageBufferRange; Vulkan guarantees 2^27).
inline constexpr uint32_t kMaxFragmentsPerBatch = 4u * 1024 * 1024;
inline constexpr uint32_t kFragmentBufferSize = kMaxFragmentsPerBatch * sizeof(Vdp1Fragment);
inline constexpr uint32_t kBucketBufferSize = kMaxFragmentsPerBatch * sizeof(uint32_t);
inline constexpr uint32_t kUnitCountsBufferSize = kUnitCount * sizeof(uint32_t);
inline constexpr uint32_t kUnitOffsetsBufferSize = kUnitCount * sizeof(uint32_t);
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
    std::max({kFragmentBufferSize, kBucketBufferSize, kUnitCountsBufferSize, kUnitOffsetsBufferSize, kFbramBufferSize,
              kVRAMBufferSize, kSpanBufferSize, kCommandBufferSize, kFbWriteBufferSize, kFbWinnersBufferSize,
              kBatchStateBufferSize});
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

/// @brief Dispatch shapes (all 64 x 1 x 1 workgroups except the erase pass):
/// - raster: one thread per span, ceil(spanCount / 64) groups (at most 4096);
/// - batch_range: one group;
/// - bucket: vkCmdDispatchIndirect at kBatchStateBucketArgs: one group per 64-unit block of the batch's unit range (at
///   most kScanBlockCount, 4096; none for a batch without fragments); bucket_sums: one group;
/// - scatter: kScatterSlotsPerThread slots per thread, ceil(fragmentCount / kScatterSlotsPerGroup) groups (at most
///   16384; one slot per thread would need 65536 groups for a full batch, beyond the guaranteed 65535);
/// - resolve: vkCmdDispatchIndirect at kBatchStateResolveArgs: G = min(unitHi - unitLo, kResolveGroupCount = 4096)
///   groups (batch_range writes it); thread t of group g owns unit unitLo + G t + g;
/// - erase: kEraseGroupSize x kEraseGroupSize groups over the region (x3 - x1 by y3 - y1 + 1 words);
/// - fb_write_mark, fb_write, fb_write_clear: one thread per record of the run, ceil(count / 64) groups each (at most
///   4096), in that order.
inline constexpr uint32_t kScatterSlotsPerThread = 4;
inline constexpr uint32_t kScatterSlotsPerGroup = kScatterSlotsPerThread * kWorkgroupSize;
inline constexpr uint32_t kEraseGroupSize = 8;
inline constexpr uint32_t kResolveGroupCount = kUnitCount / kWorkgroupSize;
static_assert(kMaxFragmentsPerBatch / kScatterSlotsPerGroup <= 65535 && kMaxSpansPerBatch / kWorkgroupSize <= 65535 &&
              kScanBlockCount <= 65535 && kResolveGroupCount <= 65535);
static_assert(kEraseGroupSize * kEraseGroupSize == kWorkgroupSize);
/// @brief The bucket_sums pass scans kScanBlockCount sums with one group, kWorkgroupSize sums per thread.
static_assert(kScanBlockCount == kWorkgroupSize * kWorkgroupSize);

/// @brief The resolve pass. A unit with at most kResolveSmallMax fragments is sorted by its thread in registers; a
/// larger one by the whole workgroup, in windows of at most kResolveSortCapacity keys: each window is the next key
/// range holding at most that many of the unit's keys (found with a kResolveHistogramBins-bin histogram of the
/// remaining keys, narrowed to one bin when that bin alone does not fit), gathered into groupshared memory,
/// bitonic-sorted and applied in order by one thread, kResolveFoldChunk fragments at a time. Exact for any number of
/// fragments on a word.
inline constexpr uint32_t kResolveSmallMax = 32;
inline constexpr uint32_t kResolveSortCapacity = 2048;
inline constexpr uint32_t kResolveHistogramBins = 256;
inline constexpr uint32_t kResolveFoldChunk = 256;
static_assert((kResolveSortCapacity & (kResolveSortCapacity - 1)) == 0 && kResolveFoldChunk % kWorkgroupSize == 0);

/// @brief Workgroup (groupshared) memory of each VDP1 pass, in bytes (the test reads them from the SPIR-V):
/// - raster: the group's unit range (two uints);
/// - bucket, bucket_sums: one uint per thread;
/// - resolve: the window's keys, the histogram, the workgroup's large units, one fold chunk of fragments (target,
///   colour) and four scalars;
/// - the others: none.
inline constexpr uint32_t kRasterSharedMemorySize = 2 * 4;
inline constexpr uint32_t kScanSharedMemorySize = kWorkgroupSize * 4;
inline constexpr uint32_t kResolveSharedMemorySize =
    (kResolveSortCapacity + kResolveHistogramBins + kWorkgroupSize + 2 * kResolveFoldChunk + 4) * 4;
/// @brief The largest of them (the device limit maxComputeSharedMemorySize must cover it; gpu_version.hpp takes the
/// maximum with VDP2, whose NBG fetch pass needs more): the resolve pass's 11,536 bytes, within the guaranteed 16384.
inline constexpr uint32_t kMaxComputeSharedMemorySize =
    std::max({kRasterSharedMemorySize, kScanSharedMemorySize, kResolveSharedMemorySize});
static_assert(kMaxComputeSharedMemorySize == 11536 && kMaxComputeSharedMemorySize <= 16384);

inline constexpr std::array<Binding, 6> kRasterBindings{{
    {kBindingCommands, DescriptorKind::StorageBuffer},
    {kBindingSpans, DescriptorKind::StorageBuffer},
    {kBindingVRAM, DescriptorKind::StorageBuffer},
    {kBindingFragments, DescriptorKind::StorageBuffer},
    {kBindingUnitCounts, DescriptorKind::StorageBuffer},
    {kBindingBatchState, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 1> kBatchRangeBindings{{
    {kBindingBatchState, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 4> kBucketBindings{{
    {kBindingUnitCounts, DescriptorKind::StorageBuffer},
    {kBindingScanSums, DescriptorKind::StorageBuffer},
    {kBindingUnitOffsets, DescriptorKind::StorageBuffer},
    {kBindingBatchState, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 2> kBucketSumsBindings{{
    {kBindingScanSums, DescriptorKind::StorageBuffer},
    {kBindingBatchState, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 4> kScatterBindings{{
    {kBindingFragments, DescriptorKind::StorageBuffer},
    {kBindingBucket, DescriptorKind::StorageBuffer},
    {kBindingScanSums, DescriptorKind::StorageBuffer},
    {kBindingUnitOffsets, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 7> kResolveBindings{{
    {kBindingFragments, DescriptorKind::StorageBuffer},
    {kBindingUnitCounts, DescriptorKind::StorageBuffer},
    {kBindingBucket, DescriptorKind::StorageBuffer},
    {kBindingFbram, DescriptorKind::StorageBuffer},
    {kBindingScanSums, DescriptorKind::StorageBuffer},
    {kBindingUnitOffsets, DescriptorKind::StorageBuffer},
    {kBindingBatchState, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 1> kEraseBindings{{
    {kBindingFbram, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 2> kFbWriteMarkBindings{{
    {kBindingFbWrites, DescriptorKind::StorageBuffer},
    {kBindingFbWinners, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 3> kFbWriteBindings{{
    {kBindingFbWrites, DescriptorKind::StorageBuffer},
    {kBindingFbram, DescriptorKind::StorageBuffer},
    {kBindingFbWinners, DescriptorKind::StorageBuffer},
}};

inline constexpr std::array<Binding, 2> kFbWriteClearBindings{{
    {kBindingFbWrites, DescriptorKind::StorageBuffer},
    {kBindingFbWinners, DescriptorKind::StorageBuffer},
}};

/// @brief The layout probe (cs_vdp1_layout_probe.hlsl) declares every resource and every push-constant struct.
inline constexpr std::array<Binding, 12> kLayoutProbeBindings{{
    {kBindingCommands, DescriptorKind::StorageBuffer},
    {kBindingSpans, DescriptorKind::StorageBuffer},
    {kBindingVRAM, DescriptorKind::StorageBuffer},
    {kBindingFbWrites, DescriptorKind::StorageBuffer},
    {kBindingFragments, DescriptorKind::StorageBuffer},
    {kBindingUnitCounts, DescriptorKind::StorageBuffer},
    {kBindingBucket, DescriptorKind::StorageBuffer},
    {kBindingFbram, DescriptorKind::StorageBuffer},
    {kBindingScanSums, DescriptorKind::StorageBuffer},
    {kBindingUnitOffsets, DescriptorKind::StorageBuffer},
    {kBindingBatchState, DescriptorKind::StorageBuffer},
    {kBindingFbWinners, DescriptorKind::StorageBuffer},
}};

/// @brief The binding tables of the VDP1 pipelines, in the back half's pass order (Vdp1VulkanBackend::Pass: raster,
/// batch range, bucket, bucket sums, scatter, resolve, erase, fb write mark, fb write, fb write clear). The backend
/// builds its passes from this table and static_asserts its pass count against the size, so a pass added to one and not
/// the other fails to compile; MaxStageUsage and the params test iterate it.
inline constexpr std::array<std::span<const Binding>, 10> kPipelineBindings{
    std::span<const Binding>(kRasterBindings),     std::span<const Binding>(kBatchRangeBindings),
    std::span<const Binding>(kBucketBindings),     std::span<const Binding>(kBucketSumsBindings),
    std::span<const Binding>(kScatterBindings),    std::span<const Binding>(kResolveBindings),
    std::span<const Binding>(kEraseBindings),      std::span<const Binding>(kFbWriteMarkBindings),
    std::span<const Binding>(kFbWriteBindings),    std::span<const Binding>(kFbWriteClearBindings)};

/// @brief The largest per-stage use over the VDP1 pipelines (gpu_version.hpp takes the maximum with VDP2's).
constexpr StageUsage MaxStageUsage() {
    StageUsage usage;
    for (std::span<const Binding> bindings : kPipelineBindings) {
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
static_assert(kMaxStageUsage.storageBuffers == 7 && kMaxStageUsage.storageImages == 0 &&
              kMaxStageUsage.sampledImages == 0 && kMaxStageUsage.resources == 7);

} // namespace brimir::gpu::vdp1
