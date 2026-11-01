// The N x VDP1 compute passes (GPU stage 3: src/gpu/shaders/vdp1/cs_vdp1_raster_nx, bucket_sums_nx(_add), resolve_nx,
// erase_nx, fb_write_nx, rebuild_nx, with the native batch_range, bucket and scatter) on a device against the CPU
// reference of the N x rasteriser (vdp1_nx_reference.hpp), pass by pass, at N = 2, 3 and 4:
// - raster_nx: every slot of each N x sub-batch (the reference's fragment at its key with its N x target, 0 elsewhere),
//   the per-N x-unit counts and the unit range equal the reference's RasterSpan over the production generator's spans
//   (vdp1_nx_front.hpp GenerateNx);
// - bucket chain: the range, the indirect arguments, each unit's bucket at the exclusive prefix of the counts holding
//   exactly its slots (ranges up to all N^2 * 262,144 units: bucket groups over many blocks, two-level sums);
// - resolve_nx: the N x surface (all eight slices) equals the reference's Resolve, the counts are zero again, also for
//   more than 250,000 fragments on one N x unit;
// - erase_nx, fb_write_nx, rebuild_nx: the N x surface equals the CPU model of research section 2 (the reference's N x
//   Erase and ApplyFbWrites; replication of the native slices of a mask).
// Inputs: hand-made scenes through the front half (every span kind, rect directions, HSS and end codes, Gouraud, 8 bpp
// with MSB-on, meshes, double interlace, 13-bit wraps), seeded random tables (vdp1_scenario.hpp; some with small N x
// sub-batch capacities), synthetic fragment sets, and the Task 1 captures ([.captures], BRIMIR_VDP1_CAPTURES). Each
// scene's final N x surface also equals the reference driver's own replay (ReferenceNx: its generator and driver).
// [gpu]: SKIP without a device. The random tables default to 60 seeds (6 on a CPU device such as lavapipe);
// BRIMIR_VDP1_NX_GPU_SEEDS=N runs N. With BRIMIR_VK_SYNC_VALIDATION=1 synchronization validation runs too.
#include "catch_amalgamated.hpp"

#include "gpu_test_device.hpp"
#include "vdp1_nx_reference.hpp"
#include "vdp1_raster_reference.hpp"
#include "vdp1_scenario.hpp"

#include <gpu/shaders/generated/vdp1_batch_range.spv.h>
#include <gpu/shaders/generated/vdp1_bucket.spv.h>
#include <gpu/shaders/generated/vdp1_bucket_sums_nx.spv.h>
#include <gpu/shaders/generated/vdp1_bucket_sums_nx_add.spv.h>
#include <gpu/shaders/generated/vdp1_erase_nx.spv.h>
#include <gpu/shaders/generated/vdp1_fb_write_nx.spv.h>
#include <gpu/shaders/generated/vdp1_raster_nx.spv.h>
#include <gpu/shaders/generated/vdp1_rebuild_nx.spv.h>
#include <gpu/shaders/generated/vdp1_resolve_nx.spv.h>
#include <gpu/shaders/generated/vdp1_scatter.spv.h>
#include <gpu/vdp1/vdp1_bindings.hpp>
#include <gpu/vdp1/vdp1_front.hpp>
#include <gpu/vdp1/vdp1_nx_front.hpp>
#include <gpu/vdp1/vdp1_op_dump.hpp>
#include <gpu/vk_util.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace brimir::test::vdp1scene;
namespace nx = brimir::test::vdp1nx;
namespace vu = brimir::gpu::vk_util;
namespace sh = brimir::gpu::shaders;
using brimir::gpu::VulkanDevice;
using brimir::test::CloseAndCheckValidation;
using brimir::test::OpenDevice;

enum class Pass : uint32 {
    Raster, BatchRange, Bucket, BucketSums, BucketSumsAdd, Scatter, Resolve, Erase, FbWrite, Rebuild, Count
};
constexpr uint32 kPassCount = static_cast<uint32>(Pass::Count);
constexpr const char *kPassNames[kPassCount] = {"raster_nx",  "batch_range", "bucket",   "bucket_sums_nx",
                                                "bucket_sums_nx_add", "scatter", "resolve_nx", "erase_nx",
                                                "fb_write_nx", "rebuild_nx"};

struct Timings {
    std::array<double, kPassCount> ms{};
    std::array<uint64, kPassCount> dispatches{};
    void Clear() {
        ms.fill(0);
        dispatches.fill(0);
    }
    double Total() const {
        double t = 0;
        for (double v : ms) {
            t += v;
        }
        return t;
    }
    std::string Format() const {
        std::string out;
        char buf[96];
        for (uint32 p = 0; p < kPassCount; ++p) {
            if (dispatches[p] != 0) {
                std::snprintf(buf, sizeof(buf), "%s%s %.3f ms (%llu)", out.empty() ? "" : ", ", kPassNames[p], ms[p],
                              static_cast<unsigned long long>(dispatches[p]));
                out += buf;
            }
        }
        return out;
    }
};

vdp::SpriteFB &Slice(ref::Fbram &fb, uint32 slice) {
    switch (slice) {
    case 0: return fb.main[0];
    case 1: return fb.main[1];
    case 2: return fb.alt[0];
    case 3: return fb.alt[1];
    default: return fb.mesh[(slice - 4) >> 1][slice & 1];
    }
}
const vdp::SpriteFB &Slice(const ref::Fbram &fb, uint32 slice) {
    return Slice(const_cast<ref::Fbram &>(fb), slice);
}

// The N x passes at one scale with their buffers (host-visible; device-local when asked and available), one descriptor
// set per pipeline. Every submit starts with a barrier from host and earlier compute writes and ends with one to host
// reads; consecutive passes are separated by compute-to-compute barriers (indirect argument reads included).
class GpuVdp1Nx {
public:
    enum Buf : uint32 {
        Commands, Spans, Vram, FbWrites, Fragments, UnitCounts, Bucket, Fbram, ScanSums, UnitOffsets, BatchState, FbramNx,
        kBufCount
    };
    enum class Memory { HostCached, DeviceLocal };

    GpuVdp1Nx(VulkanDevice &device, uint32 scale, Memory memory = Memory::HostCached)
        : m_device(device)
        , m_scale(scale) {
        m_ok = Init(memory);
    }
    ~GpuVdp1Nx() {
        const auto &vk = m_device.Table();
        for (Pipe &p : m_pipes) {
            vk.vkDestroyPipeline(m_device.Device(), p.pipeline, nullptr);
            vk.vkDestroyPipelineLayout(m_device.Device(), p.layout, nullptr);
            vk.vkDestroyDescriptorSetLayout(m_device.Device(), p.setLayout, nullptr);
        }
        vk.vkDestroyDescriptorPool(m_device.Device(), m_pool, nullptr);
        vk.vkDestroyFence(m_device.Device(), m_fence, nullptr);
        vk.vkDestroyCommandPool(m_device.Device(), m_cmdPool, nullptr);
        if (m_queries != VK_NULL_HANDLE) {
            vk.vkDestroyQueryPool(m_device.Device(), m_queries, nullptr);
        }
        for (vu::Buffer &b : m_buffers) {
            vu::DestroyBuffer(m_device, b);
        }
    }

    bool Ok() const {
        return m_ok;
    }
    uint32 Scale() const {
        return m_scale;
    }
    uint32 UnitCount() const {
        return gpu::Vdp1NxUnitCount(m_scale);
    }
    template <typename T>
    T *Map(Buf b) {
        return static_cast<T *>(m_buffers[b].mapped);
    }
    Timings &Times() {
        return m_timings;
    }

    void Copy(Buf b, const void *data, size_t size, size_t offset = 0) {
        REQUIRE(offset + size <= m_buffers[b].size);
        if (size != 0) {
            std::memcpy(static_cast<uint8 *>(m_buffers[b].mapped) + offset, data, size);
        }
    }

    // VRAM zero, counts zero, native FBRAM and the N x surface as given
    void Reset(const ref::Fbram &native, const nx::NxFbram &surface) {
        std::memset(Map<uint8>(Vram), 0, gpu::kVRAMBufferSize);
        std::memset(Map<uint8>(UnitCounts), 0, gpu::Vdp1NxUnitCountsBufferSize(m_scale));
        SetFbram(native);
        SetSurface(surface);
    }
    void SetFbram(const ref::Fbram &fb) {
        uint8 *p = Map<uint8>(Fbram);
        for (uint32 slice = 0; slice < 8; ++slice) {
            std::memcpy(p + slice * gpu::kFbramSliceSize, Slice(fb, slice).data(), gpu::kFbramSliceSize);
        }
    }
    // The surface's words big-endian, as FBRAM stores them
    void SetSurface(const nx::NxFbram &s) {
        REQUIRE(s.scale == m_scale);
        uint8 *p = Map<uint8>(FbramNx);
        for (uint32 slice = 0; slice < 8; ++slice) {
            const std::vector<uint16> &words = s.slices[slice];
            uint8 *out = p + gpu::Vdp1NxFbramByteAddress(slice, 0, m_scale);
            for (size_t k = 0; k < words.size(); ++k) {
                out[2 * k] = static_cast<uint8>(words[k] >> 8);
                out[2 * k + 1] = static_cast<uint8>(words[k]);
            }
        }
    }
    // First difference of the GPU surface from `s`, or ""
    std::string CompareSurface(const nx::NxFbram &s) {
        static constexpr const char *kSlices[8] = {"main fb0",    "main fb1",    "alt fb0",     "alt fb1",
                                                   "mesh[0] fb0", "mesh[0] fb1", "mesh[1] fb0", "mesh[1] fb1"};
        const uint8 *p = Map<uint8>(FbramNx);
        const uint32 sub = m_scale * m_scale;
        uint64 differing = 0;
        std::string first;
        for (uint32 slice = 0; slice < 8; ++slice) {
            const std::vector<uint16> &words = s.slices[slice];
            const uint8 *in = p + gpu::Vdp1NxFbramByteAddress(slice, 0, m_scale);
            for (size_t k = 0; k < words.size(); ++k) {
                const uint16 got = static_cast<uint16>((in[2 * k] << 8) | in[2 * k + 1]);
                if (got != words[k] && differing++ == 0) {
                    char buf[200];
                    const uint32 w = static_cast<uint32>(k / sub);
                    std::snprintf(buf, sizeof(buf), "%s native word 0x%05X (x %u, y %u at 512) sub %u: GPU %04X, reference %04X",
                                  kSlices[slice], w, w % 512, w / 512, static_cast<uint32>(k % sub), got, words[k]);
                    first = buf;
                }
            }
        }
        return differing == 0 ? std::string{} : first + " (" + std::to_string(differing) + " words differ)";
    }
    // The words of N x units `units` of draw framebuffer `fb` (the main or alternate slice and the field's mesh slice)
    // against `s`, and their counts zero; "" when equal
    std::string CompareUnits(const std::map<uint32, std::vector<uint32>> &units, uint32 fb, const nx::NxFbram &s) {
        const uint8 *p = Map<uint8>(FbramNx);
        const uint32 *counts = Map<uint32>(UnitCounts);
        const uint32 sub = m_scale * m_scale;
        for (const auto &[unit, slots] : units) {
            if (counts[unit] != 0) {
                return "N x unit " + std::to_string(unit) + ": count not zero after the resolve";
            }
            const uint32 native = unit / sub;
            const uint32 index = (native & gpu::kWordIndexMask) * sub + unit % sub;
            const uint32 alt = (native >> 17) & 1;
            for (const uint32 slice : {alt != 0 ? gpu::Vdp1AltSlice(fb) : gpu::Vdp1MainSlice(fb), gpu::Vdp1MeshSlice(alt, fb)}) {
                const uint8 *in = p + gpu::Vdp1NxFbramByteAddress(slice, index, m_scale);
                const uint16 got = static_cast<uint16>((in[0] << 8) | in[1]);
                if (got != s.slices[slice][index]) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "N x unit %u (slice %u, native word 0x%05X, sub %u): GPU %04X, reference %04X",
                                  unit, slice, native & gpu::kWordIndexMask, unit % sub, got, s.slices[slice][index]);
                    return buf;
                }
            }
        }
        return {};
    }
    bool CountsZero() {
        const uint32 *c = Map<uint32>(UnitCounts);
        return std::all_of(c, c + UnitCount(), [](uint32 v) { return v == 0; });
    }

    // One N x sub-batch: raster_nx, then the bucket chain, then resolve_nx; with `afterRaster` / `afterScatter`, a submit
    // per stage and the callbacks between
    bool SubBatch(const gpu::Vdp1BatchPushConstants &pc, const std::function<void()> &afterRaster = {},
                  const std::function<void()> &afterScatter = {}) {
        const uint32 rasterGroups = (pc.spanCount + gpu::kWorkgroupSize - 1) / gpu::kWorkgroupSize;
        auto raster = [&](VkCommandBuffer cmd) {
            if (rasterGroups != 0) {
                Dispatch(cmd, Pass::Raster, &pc, rasterGroups);
            }
        };
        auto bucket = [&](VkCommandBuffer cmd) { BucketChain(cmd, pc); };
        auto resolve = [&](VkCommandBuffer cmd) { DispatchIndirect(cmd, Pass::Resolve, &pc, gpu::kBatchStateResolveArgs); };
        if (!afterRaster && !afterScatter) {
            return Submit([&](VkCommandBuffer cmd) {
                raster(cmd);
                bucket(cmd);
                resolve(cmd);
            });
        }
        if (!Submit(raster)) {
            return false;
        }
        if (afterRaster) {
            afterRaster();
        }
        if (!Submit(bucket)) {
            return false;
        }
        if (afterScatter) {
            afterScatter();
        }
        return Submit(resolve);
    }

    // The bucket chain and resolve_nx over a fragment buffer written here (counts and range computed here, as raster_nx
    // would)
    bool ResolveFragments(const std::vector<gpu::Vdp1Fragment> &slots, uint32 drawFB) {
        Copy(Fragments, slots.data(), slots.size() * sizeof(gpu::Vdp1Fragment));
        uint32 *counts = Map<uint32>(UnitCounts);
        uint32 lo = ~0u, hi = 0;
        for (const gpu::Vdp1Fragment &f : slots) {
            if (gpu::Vdp1FragLive(f.target)) {
                const uint32 unit = gpu::Vdp1NxFragUnit(f.target);
                ++counts[unit];
                lo = std::min(lo, unit);
                hi = std::max(hi, unit + 1);
            }
        }
        uint32 *state = Map<uint32>(BatchState);
        state[gpu::kBatchStateAccLoInv] = hi != 0 ? ~lo : 0;
        state[gpu::kBatchStateAccHi] = hi;
        const gpu::Vdp1BatchPushConstants pc{0, 0, 0, static_cast<uint32>(slots.size()), drawFB, m_scale};
        return Submit([&](VkCommandBuffer cmd) {
            BucketChain(cmd, pc);
            DispatchIndirect(cmd, Pass::Resolve, &pc, gpu::kBatchStateResolveArgs);
        });
    }

    bool Erase(const gpu::Vdp1ErasePushConstants &e) {
        if (e.x3 <= e.x1 || e.y3 < e.y1) {
            return true;
        }
        const gpu::Vdp1EraseNxPushConstants pc{e, m_scale};
        const uint32 gx = (e.x3 - e.x1 + gpu::kEraseGroupSize - 1) / gpu::kEraseGroupSize;
        const uint32 gy = (e.y3 - e.y1 + 1 + gpu::kEraseGroupSize - 1) / gpu::kEraseGroupSize;
        return Submit([&](VkCommandBuffer cmd) { Dispatch(cmd, Pass::Erase, &pc, gx, gy); });
    }

    // fb_write_nx over records [run.offset, + run.count) of the FbWrite buffer (native FBRAM holds the run's result)
    bool FbWrite(const gpu::Vdp1FbWritePushConstants &run) {
        const uint32 groups = (run.count + gpu::kWorkgroupSize - 1) / gpu::kWorkgroupSize;
        if (groups == 0) {
            return true;
        }
        const gpu::Vdp1FbWriteNxPushConstants pc{run, m_scale};
        return Submit([&](VkCommandBuffer cmd) { Dispatch(cmd, Pass::FbWrite, &pc, groups); });
    }

    bool Rebuild(uint32 sliceMask) {
        const gpu::Vdp1RebuildNxPushConstants pc{sliceMask, m_scale};
        return Submit([&](VkCommandBuffer cmd) { Dispatch(cmd, Pass::Rebuild, &pc, gpu::Vdp1RebuildNxGroups(m_scale), 8); });
    }

private:
    static constexpr uint32 kMaxQueries = 64;

    struct Pipe {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32 pushSize = 0;
    };

    void BucketChain(VkCommandBuffer cmd, const gpu::Vdp1BatchPushConstants &pc) {
        Dispatch(cmd, Pass::BatchRange, nullptr, 1);
        DispatchIndirect(cmd, Pass::Bucket, nullptr, gpu::kBatchStateBucketArgs);
        Dispatch(cmd, Pass::BucketSums, &pc, m_scale * m_scale);
        Dispatch(cmd, Pass::BucketSumsAdd, &pc, m_scale * m_scale);
        const uint32 scatterGroups = (pc.fragmentCount + gpu::kScatterSlotsPerGroup - 1) / gpu::kScatterSlotsPerGroup;
        if (scatterGroups != 0) {
            Dispatch(cmd, Pass::Scatter, &pc, scatterGroups);
        }
    }

    bool Init(Memory memory) {
        const auto &vk = m_device.Table();
        const std::array<uint32, kBufCount> bindings{
            gpu::kBindingCommands, gpu::kBindingSpans,    gpu::kBindingVRAM,    gpu::kBindingFbWrites,
            gpu::kBindingFragments, gpu::kBindingUnitCounts, gpu::kBindingBucket, gpu::kBindingFbram,
            gpu::kBindingScanSums, gpu::kBindingUnitOffsets, gpu::kBindingBatchState, gpu::kBindingFbramNx};
        const std::array<uint32, kBufCount> sizes{
            gpu::kCommandBufferSize,  gpu::kSpanBufferSize,       gpu::kVRAMBufferSize,   gpu::kFbWriteBufferSize,
            gpu::kFragmentBufferSize, gpu::Vdp1NxUnitCountsBufferSize(m_scale), gpu::kBucketBufferSize,
            gpu::kFbramBufferSize,    gpu::Vdp1NxScanSumsBufferSize(m_scale),   gpu::Vdp1NxUnitCountsBufferSize(m_scale),
            gpu::kBatchStateBufferSize, gpu::Vdp1NxFbramBufferSize(m_scale)};
        constexpr VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        const VkMemoryPropertyFlags preferred =
            kHost | (memory == Memory::DeviceLocal ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        for (uint32 b = 0; b < kBufCount; ++b) {
            m_bindingOf[b] = bindings[b];
            const VkBufferUsageFlags usage =
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | (b == BatchState ? VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT : 0u);
            VkResult r = vu::CreateBuffer(m_device, sizes[b], usage, preferred, m_buffers[b]);
            if (r != VK_SUCCESS) {
                r = vu::CreateBuffer(m_device, sizes[b], usage, kHost, m_buffers[b]);
            }
            if (r != VK_SUCCESS || m_buffers[b].mapped == nullptr) {
                return false;
            }
            std::memset(m_buffers[b].mapped, 0, sizes[b]);
        }

        struct PassDesc {
            std::span<const uint32_t> spirv;
            std::span<const gpu::Binding> bindings;
            uint32 pushSize;
        };
        const std::array<PassDesc, kPassCount> passes{{
            {sh::kVdp1RasterNxSpv, gpu::kRasterNxBindings, sizeof(gpu::Vdp1BatchPushConstants)},
            {sh::kVdp1BatchRangeSpv, gpu::kBatchRangeBindings, 0},
            {sh::kVdp1BucketSpv, gpu::kBucketBindings, 0},
            {sh::kVdp1BucketSumsNxSpv, gpu::kBucketSumsNxBindings, sizeof(gpu::Vdp1BatchPushConstants)},
            {sh::kVdp1BucketSumsNxAddSpv, gpu::kBucketSumsNxBindings, sizeof(gpu::Vdp1BatchPushConstants)},
            {sh::kVdp1ScatterSpv, gpu::kScatterBindings, sizeof(gpu::Vdp1BatchPushConstants)},
            {sh::kVdp1ResolveNxSpv, gpu::kResolveNxBindings, sizeof(gpu::Vdp1BatchPushConstants)},
            {sh::kVdp1EraseNxSpv, gpu::kEraseNxBindings, sizeof(gpu::Vdp1EraseNxPushConstants)},
            {sh::kVdp1FbWriteNxSpv, gpu::kFbWriteNxBindings, sizeof(gpu::Vdp1FbWriteNxPushConstants)},
            {sh::kVdp1RebuildNxSpv, gpu::kRebuildNxBindings, sizeof(gpu::Vdp1RebuildNxPushConstants)},
        }};
        uint32 descriptors = 0;
        for (const PassDesc &p : passes) {
            descriptors += static_cast<uint32>(p.bindings.size());
        }
        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descriptors};
        if (vu::CreateDescriptorPool(m_device, {&poolSize, 1}, kPassCount, m_pool) != VK_SUCCESS) {
            return false;
        }
        for (uint32 i = 0; i < kPassCount; ++i) {
            const PassDesc &p = passes[i];
            Pipe &pipe = m_pipes[i];
            std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
            for (const gpu::Binding &b : p.bindings) {
                layoutBindings.push_back(
                    {b.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
            }
            const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, p.pushSize};
            pipe.pushSize = p.pushSize;
            if (vu::CreateDescriptorSetLayout(m_device, layoutBindings, pipe.setLayout) != VK_SUCCESS ||
                vu::CreatePipelineLayout(m_device, {&pipe.setLayout, 1},
                                         p.pushSize != 0 ? std::span(&range, 1) : std::span<const VkPushConstantRange>(),
                                         pipe.layout) != VK_SUCCESS ||
                vu::CreateComputePipeline(m_device, p.spirv, pipe.layout, pipe.pipeline) != VK_SUCCESS ||
                vu::AllocateDescriptorSet(m_device, m_pool, pipe.setLayout, pipe.set) != VK_SUCCESS) {
                return false;
            }
            for (const gpu::Binding &b : p.bindings) {
                for (uint32 k = 0; k < kBufCount; ++k) {
                    if (m_bindingOf[k] == b.binding) {
                        vu::WriteStorageBuffer(m_device, pipe.set, b.binding, m_buffers[k]);
                    }
                }
            }
        }

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_device.QueueFamily();
        if (vk.vkCreateCommandPool(m_device.Device(), &poolInfo, nullptr, &m_cmdPool) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = m_cmdPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vk.vkAllocateCommandBuffers(m_device.Device(), &allocInfo, &m_cmd) != VK_SUCCESS ||
            vk.vkCreateFence(m_device.Device(), &fenceInfo, nullptr, &m_fence) != VK_SUCCESS) {
            return false;
        }
        uint32 families = 0;
        volk::vkGetPhysicalDeviceQueueFamilyProperties(m_device.Physical(), &families, nullptr);
        std::vector<VkQueueFamilyProperties> props(families);
        volk::vkGetPhysicalDeviceQueueFamilyProperties(m_device.Physical(), &families, props.data());
        if (m_device.QueueFamily() < families && props[m_device.QueueFamily()].timestampValidBits != 0) {
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = kMaxQueries;
            if (vk.vkCreateQueryPool(m_device.Device(), &info, nullptr, &m_queries) != VK_SUCCESS) {
                m_queries = VK_NULL_HANDLE;
            }
            m_timestampBits = props[m_device.QueueFamily()].timestampValidBits;
        }
        return true;
    }

    void Dispatch(VkCommandBuffer cmd, Pass pass, const void *push, uint32 x, uint32 y = 1) {
        Record(cmd, pass, push, x, y, ~0u);
    }
    void DispatchIndirect(VkCommandBuffer cmd, Pass pass, const void *push, uint32 argsWord) {
        Record(cmd, pass, push, 0, 0, argsWord);
    }
    void Record(VkCommandBuffer cmd, Pass pass, const void *push, uint32 x, uint32 y, uint32 argsWord) {
        const auto &vk = m_device.Table();
        const Pipe &p = m_pipes[static_cast<uint32>(pass)];
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &p.set, 0, nullptr);
        if (p.pushSize != 0) {
            vk.vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, p.pushSize, push);
        }
        const bool timed = m_queries != VK_NULL_HANDLE && m_queryCount + 2 <= kMaxQueries;
        if (timed) {
            vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queries, m_queryCount);
        }
        if (argsWord == ~0u) {
            vk.vkCmdDispatch(cmd, x, y, 1);
        } else {
            vk.vkCmdDispatchIndirect(cmd, m_buffers[BatchState].buffer, argsWord * sizeof(uint32));
        }
        if (timed) {
            vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queries, m_queryCount + 1);
            m_queryPass[m_queryCount / 2] = pass;
            m_queryCount += 2;
        }
        vu::GlobalBarrier(m_device, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    }

    bool Submit(const std::function<void(VkCommandBuffer)> &record) {
        const auto &vk = m_device.Table();
        const VkDevice dev = m_device.Device();
        m_queryCount = 0;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vk.vkResetCommandBuffer(m_cmd, 0) != VK_SUCCESS || vk.vkBeginCommandBuffer(m_cmd, &begin) != VK_SUCCESS) {
            return false;
        }
        if (m_queries != VK_NULL_HANDLE) {
            vk.vkCmdResetQueryPool(m_cmd, m_queries, 0, kMaxQueries);
        }
        vu::GlobalBarrier(m_device, m_cmd, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        record(m_cmd);
        vu::GlobalBarrier(m_device, m_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT);
        if (vk.vkEndCommandBuffer(m_cmd) != VK_SUCCESS) {
            return false;
        }
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &m_cmd;
        if (m_device.Submit(submit, m_fence) != VK_SUCCESS ||
            vk.vkWaitForFences(dev, 1, &m_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS ||
            vk.vkResetFences(dev, 1, &m_fence) != VK_SUCCESS) {
            return false;
        }
        if (m_queryCount != 0) {
            std::array<uint64_t, kMaxQueries> stamps{};
            if (vk.vkGetQueryPoolResults(dev, m_queries, 0, m_queryCount, sizeof(stamps), stamps.data(), sizeof(uint64_t),
                                         VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
                const uint64_t mask = m_timestampBits >= 64 ? ~0ull : ((1ull << m_timestampBits) - 1);
                const double period = m_device.Properties().limits.timestampPeriod;
                for (uint32 q = 0; q < m_queryCount; q += 2) {
                    const uint64_t ticks = (stamps[q + 1] - stamps[q]) & mask;
                    const uint32 pass = static_cast<uint32>(m_queryPass[q / 2]);
                    m_timings.ms[pass] += static_cast<double>(ticks) * period * 1e-6;
                    ++m_timings.dispatches[pass];
                }
            }
        }
        return true;
    }

    VulkanDevice &m_device;
    uint32 m_scale;
    bool m_ok = false;
    std::array<vu::Buffer, kBufCount> m_buffers{};
    std::array<uint32, kBufCount> m_bindingOf{};
    std::array<Pipe, kPassCount> m_pipes{};
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    VkQueryPool m_queries = VK_NULL_HANDLE;
    uint32 m_timestampBits = 0;
    uint32 m_queryCount = 0;
    std::array<Pass, kMaxQueries / 2> m_queryPass{};
    Timings m_timings;
};

bool IsCpuDevice(const VulkanDevice &device) {
    return device.Properties().deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
}

// -----------------------------------------------------------------------------
// Reference expectations

nx::NxSpan ToReference(const gpu::Vdp1NxSpan &s, uint32 slotEnd) {
    nx::NxSpan r;
    switch (gpu::Vdp1NxSpanKind(s.flags)) {
    case gpu::kNxSpanRect: r.kind = nx::SpanKind::Rect; break;
    case gpu::kNxSpanQuad: r.kind = nx::SpanKind::Quad; break;
    default: r.kind = nx::SpanKind::Replicate; break;
    }
    r.antiAlias = gpu::Vdp1NxSpanAntiAlias(s.flags);
    r.cmdIndex = s.cmdIndex;
    r.a = {static_cast<sint32>(s.x0), static_cast<sint32>(s.y0)};
    r.b = {static_cast<sint32>(s.x1), static_cast<sint32>(s.y1)};
    r.subRow = gpu::Vdp1NxSpanSubRow(s.flags);
    r.texV = static_cast<sint32>(gpu::Vdp1NxSpanTexV(s.flags));
    r.gouraudL.u16 = static_cast<uint16>(gpu::Vdp1Lo16(s.gouraudLR));
    r.gouraudR.u16 = static_cast<uint16>(gpu::Vdp1Hi16(s.gouraudLR));
    r.slotBase = s.slotBase;
    r.slots = slotEnd - s.slotBase;
    return r;
}

// raster_nx's expected output for a sub-batch: every slot, the counts per N x unit, the hull of the units; and the
// reference's fragments for its resolve
struct NxRasterExpectation {
    std::vector<gpu::Vdp1Fragment> slots;
    std::vector<nx::NxFragment> fragments;
    std::map<uint32, std::vector<uint32>> units; // N x unit -> its slots, ascending
    uint32 maxPerUnit = 0;
    uint32 unitLo = 0, unitHi = 0;
};

NxRasterExpectation ExpectedRaster(uint32 n, const gpu::Vdp1OpList &ops, const gpu::Vdp1DrawBatch &b,
                                   const gpu::Vdp1NxArena &arena, const gpu::Vdp1NxSubBatch &sb, const uint8 *vram) {
    NxRasterExpectation e;
    e.slots.assign(sb.slotCount, gpu::Vdp1Fragment{0, 0});
    for (uint32 i = 0; i < sb.spanCount; ++i) {
        const gpu::Vdp1NxSpan &span = arena.spans[sb.spanOffset + i];
        const uint32 end = i + 1 < sb.spanCount ? arena.spans[sb.spanOffset + i + 1].slotBase : sb.slotCount;
        const nx::NxSpan r = ToReference(span, end);
        const gpu::Vdp1CommandParams &c = ops.commands[b.cmdOffset + span.cmdIndex];
        const size_t before = e.fragments.size();
        nx::RasterSpan(n, ref::DecodeRasterState(c.rasterFlags, b.drawFB), ref::DecodeCommand(c), r, vram, e.fragments);
        for (size_t f = before; f < e.fragments.size(); ++f) {
            if (e.fragments[f].key < span.slotBase || e.fragments[f].key >= end) {
                throw std::logic_error("ExpectedRaster: a fragment key outside its span's slots");
            }
        }
    }
    for (const nx::NxFragment &f : e.fragments) {
        if (gpu::Vdp1FragLive(e.slots[f.key].target)) {
            throw std::logic_error("ExpectedRaster: two fragments in one slot");
        }
        const uint32 unit = gpu::Vdp1NxUnit(f.target & ref::kTargetUnitMask, f.sub, n);
        e.slots[f.key] = {gpu::Vdp1MakeNxFragmentTarget(unit, f.target), f.color};
        std::vector<uint32> &slots = e.units[unit];
        slots.push_back(f.key);
        e.maxPerUnit = std::max(e.maxPerUnit, static_cast<uint32>(slots.size()));
    }
    for (auto &[unit, slots] : e.units) {
        std::sort(slots.begin(), slots.end());
    }
    if (!e.units.empty()) {
        e.unitLo = e.units.begin()->first;
        e.unitHi = e.units.rbegin()->first + 1;
    }
    return e;
}

std::string CompareRaster(const NxRasterExpectation &e, GpuVdp1Nx &g) {
    const auto *slots = g.Map<gpu::Vdp1Fragment>(GpuVdp1Nx::Fragments);
    for (size_t i = 0; i < e.slots.size(); ++i) {
        if (slots[i].target != e.slots[i].target || slots[i].color != e.slots[i].color) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "slot %zu: GPU %08X/%04X, reference %08X/%04X", i, slots[i].target,
                          slots[i].color, e.slots[i].target, e.slots[i].color);
            return buf;
        }
    }
    // The range accumulated by raster_nx is the hull of the units it counted, so equal ranges and equal counts inside
    // the range mean equal counts everywhere (the counts are zero between sub-batches)
    const uint32 *state = g.Map<uint32>(GpuVdp1Nx::BatchState);
    const uint32 hi = state[gpu::kBatchStateAccHi];
    const uint32 lo = hi != 0 ? ~state[gpu::kBatchStateAccLoInv] : 0;
    if (lo != e.unitLo || hi != e.unitHi) {
        return "unit range: GPU [" + std::to_string(lo) + ", " + std::to_string(hi) + "), reference [" +
               std::to_string(e.unitLo) + ", " + std::to_string(e.unitHi) + ")";
    }
    const uint32 *counts = g.Map<uint32>(GpuVdp1Nx::UnitCounts);
    auto it = e.units.begin();
    for (uint32 u = lo; u < hi && u < g.UnitCount(); ++u) {
        uint32 expected = 0;
        if (it != e.units.end() && it->first == u) {
            expected = static_cast<uint32>(it->second.size());
            ++it;
        }
        if (counts[u] != expected) {
            return "N x unit " + std::to_string(u) + " count: GPU " + std::to_string(counts[u]) + ", reference " +
                   std::to_string(expected);
        }
    }
    return {};
}

// After the scatter pass: the range, the accumulator reset, the indirect arguments, and each unit's bucket at the
// exclusive prefix of the range's counts holding exactly its slots
std::string CompareBuckets(const std::map<uint32, std::vector<uint32>> &units, uint32 unitLo, uint32 unitHi,
                           GpuVdp1Nx &g) {
    const uint32 *sums = g.Map<uint32>(GpuVdp1Nx::ScanSums);
    const uint32 *offsets = g.Map<uint32>(GpuVdp1Nx::UnitOffsets);
    const uint32 *bucket = g.Map<uint32>(GpuVdp1Nx::Bucket);
    const uint32 *state = g.Map<uint32>(GpuVdp1Nx::BatchState);
    const uint32 lo = state[gpu::kBatchStateUnitLo], hi = state[gpu::kBatchStateUnitHi];
    if (lo != unitLo || hi != unitHi) {
        return "batch range [" + std::to_string(lo) + ", " + std::to_string(hi) + ") differs from the hull";
    }
    if (state[gpu::kBatchStateAccLoInv] != 0 || state[gpu::kBatchStateAccHi] != 0) {
        return "the range accumulator was not reset";
    }
    if (state[gpu::kBatchStateBucketArgs] != std::min(((hi + 63) >> 6) - (lo >> 6), gpu::kScanBlockCount) ||
        state[gpu::kBatchStateResolveArgs] != std::min(hi - lo, gpu::kResolveGroupCount)) {
        return "indirect dispatch arguments do not match the range";
    }
    uint32 prefix = 0;
    for (const auto &[unit, slots] : units) {
        const uint32 end = sums[unit >> 6] + offsets[unit];
        if (end != prefix + slots.size()) {
            return "N x unit " + std::to_string(unit) + " bucket ends at " + std::to_string(end) + ", expected " +
                   std::to_string(prefix + slots.size());
        }
        std::vector<uint32> got(bucket + prefix, bucket + end);
        std::sort(got.begin(), got.end());
        if (got != slots) {
            return "N x unit " + std::to_string(unit) + " bucket holds other slots";
        }
        prefix = end;
    }
    return {};
}

// Op i of `ops` as an op list of its own (offsets rebased), for the native reference to replay op by op
gpu::Vdp1OpList SingleOp(const gpu::Vdp1OpList &ops, size_t i) {
    gpu::Vdp1OpList out;
    const gpu::Vdp1Op &op = ops.ops[i];
    out.ops.push_back({op.kind, 0});
    out.segments.push_back({0, 1});
    switch (op.kind) {
    case gpu::Vdp1OpKind::Draw: {
        gpu::Vdp1DrawBatch b = ops.batches[op.index];
        out.commands.assign(ops.commands.begin() + b.cmdOffset, ops.commands.begin() + b.cmdOffset + b.cmdCount);
        out.spans.assign(ops.spans.begin() + b.spanOffset, ops.spans.begin() + b.spanOffset + b.spanCount);
        for (uint32 k = 0; k < b.copyCount; ++k) {
            gpu::Vdp1VramCopy c = ops.vramCopies[b.firstCopy + k];
            const uint32 at = static_cast<uint32>(out.arena.size());
            out.arena.insert(out.arena.end(), ops.arena.begin() + c.arenaOffset,
                             ops.arena.begin() + c.arenaOffset + c.size);
            c.arenaOffset = at;
            out.vramCopies.push_back(c);
        }
        b.cmdOffset = 0;
        b.spanOffset = 0;
        b.firstCopy = 0;
        out.batches.push_back(b);
        break;
    }
    case gpu::Vdp1OpKind::Erase: out.erases.push_back(ops.erases[op.index]); break;
    case gpu::Vdp1OpKind::FbWrites: {
        const gpu::Vdp1FbWritePushConstants run = ops.fbWriteRuns[op.index];
        out.fbWrites.assign(ops.fbWrites.begin() + run.offset, ops.fbWrites.begin() + run.offset + run.count);
        out.fbWriteRuns.push_back({0, run.count});
        break;
    }
    }
    return out;
}

struct NxSceneStats {
    uint64 ops = 0, batches = 0, splitBatches = 0, subBatches = 0, slots = 0, fragments = 0, erases = 0, fbWriteRuns = 0;
    std::array<uint64, 3> spans{};
    uint32 maxPerUnit = 0;
    void Merge(const NxSceneStats &o) {
        ops += o.ops;
        batches += o.batches;
        subBatches += o.subBatches;
        splitBatches += o.splitBatches;
        slots += o.slots;
        fragments += o.fragments;
        erases += o.erases;
        fbWriteRuns += o.fbWriteRuns;
        for (uint32 k = 0; k < 3; ++k) {
            spans[k] += o.spans[k];
        }
        maxPerUnit = std::max(maxPerUnit, o.maxPerUnit);
    }
};

// Replays op lists at N x on the GPU (the production generator's sub-batches through the N x passes; erases and CPU
// write runs through erase_nx and fb_write_nx) and, op by op, on the references: native FBRAM (ReferenceVDP1, which the
// CPU write runs read) and the N x surface (RasterSpan over the same spans, Resolve, Erase, ApplyFbWrites). With
// `inspect`, each sub-batch's raster and bucket output and its units' words after its resolve, and the whole surface
// after every erase and write run; always the whole surface at the end of each list.
class NxRunner {
public:
    NxRunner(GpuVdp1Nx &g, const ref::Fbram &initial, bool inspect, gpu::Vdp1NxLimits limits = {})
        : m_gpu(g)
        , m_n(g.Scale())
        , m_native(std::make_unique<ref::Fbram>(initial))
        , m_reference(*m_native)
        , m_surface(std::make_unique<nx::NxFbram>(g.Scale()))
        , m_vram(gpu::kVRAMBufferSize, 0)
        , m_inspect(inspect) {
        m_arena.limits = limits;
        m_surface->ReplicateFrom(initial);
        // The GPU surface starts from garbage and is rebuilt from native FBRAM by rebuild_nx
        nx::NxFbram garbage(m_n);
        for (auto &slice : garbage.slices) {
            for (size_t k = 0; k < slice.size(); ++k) {
                slice[k] = static_cast<uint16>(k * 0x9E37u + 0x1234u);
            }
        }
        g.Reset(initial, garbage);
        m_rebuildOk = g.Rebuild(0xFF);
    }

    std::string Replay(const gpu::Vdp1OpList &ops) {
        if (!m_rebuildOk) {
            return "rebuild_nx submit failed";
        }
        if (m_verify && stats.ops == 0) {
            const std::string d = m_gpu.CompareSurface(*m_surface);
            if (!d.empty()) {
                return "after rebuild_nx: " + d;
            }
        }
        for (size_t i = 0; i < ops.ops.size(); ++i) {
            const gpu::Vdp1Op &op = ops.ops[i];
            const std::string where = "op " + std::to_string(stats.ops++);
            if (m_verify) {
                m_reference.Replay(SingleOp(ops, i));
            }
            std::string diff;
            switch (op.kind) {
            case gpu::Vdp1OpKind::Draw: diff = Draw(ops, ops.batches[op.index]); break;
            case gpu::Vdp1OpKind::Erase: {
                ++stats.erases;
                const gpu::Vdp1ErasePushConstants &e = ops.erases[op.index];
                if (m_verify) {
                    nx::Erase(e, *m_surface);
                }
                if (!m_gpu.Erase(e)) {
                    diff = "erase_nx submit failed";
                }
                break;
            }
            case gpu::Vdp1OpKind::FbWrites: {
                ++stats.fbWriteRuns;
                const gpu::Vdp1FbWritePushConstants &run = ops.fbWriteRuns[op.index];
                const auto writes = std::span(ops.fbWrites).subspan(run.offset, run.count);
                if (m_verify) {
                    nx::ApplyFbWrites(writes, *m_surface);
                    m_gpu.SetFbram(*m_native); // the native run's result, as the native passes leave it
                }
                m_gpu.Copy(GpuVdp1Nx::FbWrites, writes.data(), writes.size_bytes());
                if (!m_gpu.FbWrite({0, run.count})) {
                    diff = "fb_write_nx submit failed";
                }
                break;
            }
            }
            if (!diff.empty()) {
                return where + " " + diff;
            }
            // The whole surface at the end of the list and, when inspecting, after every erase and write run (a draw's
            // units are compared after each of its sub-batches)
            if (!m_verify || (i + 1 < ops.ops.size() && (!m_inspect || op.kind == gpu::Vdp1OpKind::Draw))) {
                continue;
            }
            if (!m_gpu.CountsZero()) {
                return where + ": unit counts not zero after the resolve";
            }
            const std::string d = m_gpu.CompareSurface(*m_surface);
            if (!d.empty()) {
                static constexpr const char *kKinds[] = {"draw", "erase", "CPU writes"};
                return where + " (" + kKinds[static_cast<uint32>(op.kind)] + "): " + d;
            }
        }
        return {};
    }

    const ref::Fbram &Native() const {
        return *m_native;
    }

    // GPU passes only (timings): no reference, no comparison
    void TimingOnly() {
        m_verify = false;
    }

    // VRAM before the first list (the N x expectation's and the GPU's)
    void SeedVram(const std::vector<uint8> &vram) {
        const size_t size = std::min(vram.size(), m_vram.size());
        std::copy_n(vram.begin(), size, m_vram.begin());
        std::memcpy(m_gpu.Map<uint8>(GpuVdp1Nx::Vram), vram.data(), size);
    }

    NxSceneStats stats;

private:
    std::string Draw(const gpu::Vdp1OpList &ops, const gpu::Vdp1DrawBatch &b) {
        ++stats.batches;
        uint8 *gpuVram = m_gpu.Map<uint8>(GpuVdp1Nx::Vram);
        for (uint32 k = 0; k < b.copyCount; ++k) {
            const gpu::Vdp1VramCopy &c = ops.vramCopies[b.firstCopy + k];
            std::memcpy(m_vram.data() + c.vramOffset, ops.arena.data() + c.arenaOffset, c.size);
            std::memcpy(gpuVram + c.vramOffset, ops.arena.data() + c.arenaOffset, c.size);
        }
        m_gpu.Copy(GpuVdp1Nx::Commands, ops.commands.data() + b.cmdOffset, b.cmdCount * sizeof(gpu::Vdp1CommandParams));
        const uint32 batchIndex = static_cast<uint32>(&b - ops.batches.data());
        m_arena.Clear();
        const auto subs = gpu::GenerateNx(ops, batchIndex, m_n, m_arena);
        stats.splitBatches += subs.size() > 1;
        for (size_t s = 0; s < subs.size(); ++s) {
            const gpu::Vdp1NxSubBatch &sb = subs[s];
            ++stats.subBatches;
            stats.slots += sb.slotCount;
            for (uint32 i = 0; i < sb.spanCount; ++i) {
                ++stats.spans[std::min<uint32>(gpu::Vdp1NxSpanKind(m_arena.spans[sb.spanOffset + i].flags), 2)];
            }
            m_gpu.Copy(GpuVdp1Nx::Spans, m_arena.spans.data() + sb.spanOffset, sb.spanCount * sizeof(gpu::Vdp1NxSpan));
            const gpu::Vdp1BatchPushConstants pc{0, sb.spanCount, 0, sb.slotCount, b.drawFB & 1, m_n};
            if (!m_verify) {
                if (!m_gpu.SubBatch(pc)) {
                    return "sub-batch " + std::to_string(s) + ": submit failed";
                }
                continue;
            }
            NxRasterExpectation e = ExpectedRaster(m_n, ops, b, m_arena, sb, m_vram.data());
            stats.fragments += e.fragments.size();
            stats.maxPerUnit = std::max(stats.maxPerUnit, e.maxPerUnit);
            std::string passDiff;
            bool ok;
            if (m_inspect) {
                ok = m_gpu.SubBatch(
                    pc,
                    [&] {
                        const std::string d = CompareRaster(e, m_gpu);
                        passDiff = d.empty() ? "" : "raster_nx: " + d;
                    },
                    [&] {
                        if (passDiff.empty()) {
                            const std::string d = CompareBuckets(e.units, e.unitLo, e.unitHi, m_gpu);
                            passDiff = d.empty() ? "" : "bucket chain: " + d;
                        }
                    });
            } else {
                ok = m_gpu.SubBatch(pc);
            }
            nx::Resolve(b.drawFB & 1, std::move(e.fragments), *m_surface);
            if (!ok) {
                return "sub-batch " + std::to_string(s) + ": submit failed";
            }
            if (!passDiff.empty()) {
                return "sub-batch " + std::to_string(s) + " " + passDiff;
            }
            if (m_inspect) {
                const std::string d = m_gpu.CompareUnits(e.units, b.drawFB & 1, *m_surface);
                if (!d.empty()) {
                    return "sub-batch " + std::to_string(s) + " resolve_nx: " + d;
                }
            }
        }
        return {};
    }

    GpuVdp1Nx &m_gpu;
    uint32 m_n;
    std::unique_ptr<ref::Fbram> m_native;
    ref::ReferenceVDP1 m_reference;
    std::unique_ptr<nx::NxFbram> m_surface;
    std::vector<uint8> m_vram;
    gpu::Vdp1NxArena m_arena;
    bool m_inspect;
    bool m_verify = true;
    bool m_rebuildOk = false;
};

// A scenario's op lists through the GPU N x passes and the references op by op; the final GPU surface also against the
// reference driver's own replay (ReferenceNx: its generator, the default rules)
std::string RunScene(GpuVdp1Nx &g, const Scenario &s, bool inspect, NxSceneStats *stats = nullptr,
                     gpu::Vdp1NxLimits limits = {}) {
    std::vector<gpu::Vdp1OpList> lists;
    RunReference(s, {}, 0, [&](const gpu::Vdp1OpList &ops) { lists.push_back(ops); });
    auto initial = std::make_unique<ref::Fbram>();
    FillMain(s, initial->main);
    NxRunner runner(g, *initial, inspect, limits);
    for (size_t l = 0; l < lists.size(); ++l) {
        const std::string d = runner.Replay(lists[l]);
        if (!d.empty()) {
            return "list " + std::to_string(l) + " " + d;
        }
    }
    if (stats != nullptr) {
        stats->Merge(runner.stats);
    }
    auto native = std::make_unique<ref::Fbram>(*initial);
    auto surface = std::make_unique<nx::NxFbram>(g.Scale());
    surface->ReplicateFrom(*initial);
    nx::Options o;
    o.scale = g.Scale();
    o.subBatchSlots = limits.slots;
    o.subBatchSpans = limits.spans;
    nx::ReferenceNx r{*native, *surface, o};
    for (const gpu::Vdp1OpList &l : lists) {
        r.Replay(l);
    }
    const std::string d = g.CompareSurface(*surface);
    if (!d.empty()) {
        return "against ReferenceNx: " + d;
    }
    return CompareFbram(*native, runner.Native(), "ReferenceNx native", "op-by-op native").empty()
               ? std::string{}
               : "native references differ";
}

std::vector<uint8> CodeTexture(uint32 w, uint32 h, uint64 seed) {
    // colour mode 4 (8 bpp bank): transparent (0) and end-code (0xFF) texels among opaque ones
    std::vector<uint8> t(w * h);
    TestRng rng{seed};
    for (auto &v : t) {
        const uint32 r = static_cast<uint32>(rng.Range(0, 99));
        v = r < 8 ? 0x00 : r < 11 ? 0xFF : static_cast<uint8>(rng.Range(1, 254));
    }
    return t;
}

// Hand-made scenes: every span kind and the rect directions, flips, shrinks, HSS and end codes, Gouraud, half-
// transparency and shadow, 8 bpp with MSB-on, meshes with transparent meshes on and off, double interlace, the
// deinterlaced alternate field, 13-bit wraps (replicate), lines and polylines (parallel lines), thin quads, user clip
std::vector<std::pair<std::string, Scenario>> HandScenes() {
    std::vector<std::pair<std::string, Scenario>> out;
    auto base = [](uint64 seed) {
        Scenario s;
        TestRng rng{seed};
        s.fbFill = {0x80, 0x10, 0x9C, 0xE7, 0x00, 0x42, 0xFF, 0x7F, 0x12};
        s.clip.sysClipH = 352;
        s.clip.sysClipV = 255;
        for (uint32 i = Scenario::kTextureBase + 0x8000; i < Scenario::kVRAMSize; ++i) {
            s.vram[i] = static_cast<uint8>(rng.Next());
        }
        return s;
    };
    {
        Scenario s = base(1);
        const uint16 tex = s.AddTexture(RGBTexture(32, 24, 1));
        const uint16 g = s.AddGouraud({0x001F, 0x03E0, 0x7C00, 0x4210});
        s.Op0(Op::Kind::BeginFrame);
        for (const uint16 ctrl : {uint16{0x0000}, uint16{0x0010}, uint16{0x0020}, uint16{0x0030}}) {
            const uint16 off = static_cast<uint16>(ctrl * 2);
            s.AddCommand({ctrl, 0, 0x00EC, 0, tex, 0x0418, U(10 + off), 20, 0, 0, 0, 0, 0, 0, g, 0}); // normal
            s.AddCommand({static_cast<uint16>(0x0001 | ctrl), 0, 0x00EC, 0, tex, 0x0418, U(20 + off), 60, 50, 30, 0, 0,
                          0, 0, g, 0}); // scaled (zoom)
            s.AddCommand({static_cast<uint16>(0x0001 | ctrl), 0, 0x00C8, 0, tex, 0x0418, U(150), U(150 - off),
                          U(100 + off), U(40), 0, 0, 0, 0, 0, 0}); // scaled, reversed corners (rows up, right to left)
            s.AddCommand({static_cast<uint16>(0x0001 | ctrl), 0, 0x00CB, 0, tex, 0x0418, U(200 + off), 150, U(213 + off),
                          159, 0, 0, 0, 0, 0, 0}); // shrink, half-transparency
        }
        s.Distorted(0, 0x00EC, 0, tex, 0x0418, {40, 120}, {140, 100}, {160, 200}, {30, 180}, g);
        s.Distorted(0x0030, 0x00C8, 0, tex, 0x0418, {200, 100}, {120, 100}, {120, 60}, {200, 60}); // reversed rect
        s.Polygon(0x00C4, 0x4210, {300, 140}, {269, 140}, {269, 109}, {300, 109}, g); // rect, right to left, up
        s.Polygon(0x00C7, 0x7FFF, {250, 10}, {310, 30}, {290, 90}, {240, 60}, g);      // Gouraud half-transparent quad
        s.Polygon(0x00C1, 0x7FFF, {230, 20}, {330, 50}, {280, 120}, {220, 70});        // shadow
        s.Polygon(0x00C4, 0x7FFF, {10, 200}, {80, 210}, {80, 210}, {10, 200}, g);       // thin (collinear)
        s.Polygon(0x00C0, 0x03FF, {100, 100}, {100, 100}, {100, 100}, {100, 100});      // point
        s.AddCommand({0x0005, 0, 0x00C4, 0x83E0, 0, 0, U(5), U(5), U(310), U(40), U(200), U(220), U(15), U(150), g, 0});
        s.AddCommand({0x0007, 0, 0x00C0, 0x801F, 0, 0, U(60), U(70), U(160), U(70), U(160), U(120), U(60), U(120), 0, 0});
        s.AddCommand({0x0006, 0, 0x00C4, 0xFC00, 0, 0, U(300), U(200), U(20), U(30), 0, 0, 0, 0, g, 0}); // line
        s.AddCommand({0x0006, 0, 0x00C0, 0xFC00, 0, 0, U(40), U(150), U(40), U(60), 0, 0, 0, 0, 0, 0});   // line up
        s.Op0(Op::Kind::EndFrame);
        out.emplace_back("command types", s);
    }
    {
        Scenario s = base(2);
        const uint16 srca = s.AddTexture(CodeTexture(64, 32, 2));
        s.Op0(Op::Kind::BeginFrame);
        s.AddCommand({0x0001, 0, 0x1020, 0, srca, 0x0820, 10, 10, 20, 9, 0, 0, 0, 0, 0, 0});   // HSS shrink
        s.AddCommand({0x0001, 0, 0x0020, 0, srca, 0x0820, 40, 10, 150, 90, 0, 0, 0, 0, 0, 0}); // end codes, zoom
        s.AddCommand({0x0011, 0, 0x0020, 0, srca, 0x0820, 230, 10, 180, 70, 0, 0, 0, 0, 0, 0}); // flipped, end codes
        s.Distorted(0, 0x1020, 0, srca, 0x0820, {200, 10}, {230, 14}, {226, 30}, {198, 28});
        s.Distorted(0, 0x0020, 0, srca, 0x0820, {20, 120}, {300, 110}, {280, 210}, {30, 200});
        s.Distorted(0, 0x0060, 0, srca, 0x0820, {320, 120}, {250, 120}, {250, 180}, {320, 180}); // TPD, rect backward
        s.Op0(Op::Kind::EndFrame);
        out.emplace_back("HSS, end codes, transparent texels", s);
    }
    for (const bool meshes : {false, true}) {
        Scenario s = base(3);
        s.tvmr = 1;
        s.transparentMeshes = meshes;
        const uint16 tex = s.AddTexture(RGBTexture(16, 16, 3));
        s.Op0(Op::Kind::BeginFrame);
        for (sint32 x = 1; x < 200; x += 37) {
            s.AddCommand({0, 0, 0x80E8, 0, tex, 0x0210, U(x), U(x / 2 + 1), 0, 0, 0, 0, 0, 0, 0, 0}); // MSB on
            s.Polygon(0x01C0, 0x00AB, {x, 120}, {x + 21, 125}, {x + 25, 160}, {x + 3, 150});         // mesh
            s.Polygon(0x00C0, 0x00CD, {x + 1, 170}, {x + 22, 170}, {x + 22, 190}, {x + 1, 190});
        }
        s.Op0(Op::Kind::EndFrame);
        out.emplace_back(meshes ? "8 bpp, MSB-on, mesh (transparent meshes)" : "8 bpp, MSB-on, mesh", s);
    }
    {
        Scenario s = base(8);
        s.transparentMeshes = true;
        const uint16 g = s.AddGouraud({0x7FFF, 0x0000, 0x0000, 0x7C1F});
        s.Op0(Op::Kind::BeginFrame);
        s.Polygon(0x05C3, 0xFC1F, {30, 30}, {300, 40}, {280, 200}, {20, 190}, g); // user clip, half-transparent
        s.Polygon(0x07C4, 0x83E0, {10, 10}, {340, 15}, {330, 240}, {5, 230}, g);  // clipping mode 1
        s.Polygon(0x01C3, 0x801F, {60, 60}, {200, 70}, {190, 180}, {50, 170});    // transparent mesh
        s.Op0(Op::Kind::EndFrame);
        out.emplace_back("user clip, meshes, half-transparency", s);
    }
    for (const uint16 fbcr : {uint16{0x0008}, uint16{0x000C}}) {
        for (const bool deinterlace : {false, true}) {
            Scenario s = base(4);
            s.tvmd = 0x80C0;
            s.fbcr = deinterlace ? 0 : fbcr;
            s.deinterlace = deinterlace;
            s.vres = 448;
            const uint16 tex = s.AddTexture(RGBTexture(24, 24, 4));
            s.Op0(Op::Kind::BeginFrame);
            s.AddCommand({0, 0, 0x00E8, 0, tex, 0x0318, 30, 31, 0, 0, 0, 0, 0, 0, 0, 0});
            s.Polygon(0x00C0, 0x7C1F, {100, 21}, {200, 41}, {180, 161}, {90, 141});
            s.Distorted(0, 0x00E8, 0, tex, 0x0318, {20, 100}, {60, 100}, {60, 140}, {20, 140});
            s.AddCommand({0x0006, 0, 0x00C0, 0x83E0, 0, 0, U(10), U(201), U(300), U(211), 0, 0, 0, 0, 0, 0});
            s.Op0(Op::Kind::EndFrame);
            out.emplace_back(std::string("double interlace fbcr ") + std::to_string(fbcr) +
                                 (deinterlace ? " deinterlace" : ""),
                             s);
        }
    }
    {
        Scenario s = base(5);
        s.clip.sysClipH = 0x3FF;
        s.clip.sysClipV = 0x1FF;
        s.Op0(Op::Kind::BeginFrame);
        s.Polygon(0x00C0, 0x7FFF, {-4000, 10}, {300, 20}, {4000, 200}, {20, 100});  // edge delta > 4095
        s.Polygon(0x00C0, 0x7FE0, {10, 10}, {40, 10}, {60, 2100}, {5, 2090});       // major >= 2048
        s.Polygon(0x00C0, 0x03FF, {-800, 30}, {800, 30}, {800, 60}, {-800, 60});    // rect row > 1364
        s.Polygon(0x00C0, 0x001F, {-700, 100}, {700, 90}, {720, 130}, {-690, 140}); // quad lines > 1364
        s.AddCommand({0x0006, 0, 0x00C0, 0x83E0, 0, 0, U(-700), U(10), U(700), U(20), 0, 0, 0, 0, 0, 0});
        s.Op0(Op::Kind::EndFrame);
        out.emplace_back("13-bit wraps (replicate)", s);
    }
    return out;
}

} // namespace

// =====================================================================================================================

TEST_CASE("VDP1 N x passes: hand-made scenes equal the reference pass by pass at N = 2, 3, 4", "[gpu][vdp1][nx][passes]") {
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);
    {
        NxSceneStats total;
        // N = 3 only on a GPU (a CPU device such as lavapipe runs 2 and 4; the random tables cover 3 there)
        for (uint32 n = 2; n <= gpu::kMaxNxScale; n += IsCpuDevice(*device) ? 2 : 1) {
            GpuVdp1Nx g(*device, n);
            REQUIRE(g.Ok());
            for (const auto &[name, s] : HandScenes()) {
                CAPTURE(n, name);
                NxSceneStats stats;
                CHECK(RunScene(g, s, true, &stats) == "");
                CHECK(stats.batches > 0);
                total.Merge(stats);
            }
            // a scene cut into many small sub-batches (capacities lowered)
            CAPTURE(n);
            NxSceneStats split;
            CHECK(RunScene(g, HandScenes()[0].second, true, &split, {4000, 16}) == "");
            CHECK(split.splitBatches > 0);
            total.Merge(split);
            UNSCOPED_INFO("N=" << n << " timings: " << g.Times().Format());
        }
        UNSCOPED_INFO("spans rect " << total.spans[0] << " quad " << total.spans[1] << " replicate " << total.spans[2]
                                    << ", sub-batches " << total.subBatches << ", fragments " << total.fragments);
        CHECK(total.spans[0] > 0);
        CHECK(total.spans[1] > 0);
        CHECK(total.spans[2] > 0);
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("VDP1 N x passes: random command tables through the generator equal the reference", "[gpu][vdp1][nx][passes]") {
    // The seeded tables of the native tests (every command type and colour mode, draw modes, Gouraud, meshes, MSB-on,
    // 8 bpp, double interlace, end codes, HSS, clipping, erases, CPU writes, swaps, batch limits), N cycling 2, 3, 4,
    // every fourth with small N x sub-batch capacities; every other table inspected pass by pass
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);
    {
        std::array<std::unique_ptr<GpuVdp1Nx>, 3> gpus;
        for (uint32 n = 2; n <= 4; ++n) {
            gpus[n - 2] = std::make_unique<GpuVdp1Nx>(*device, n);
            REQUIRE(gpus[n - 2]->Ok());
        }
        uint64 seeds = IsCpuDevice(*device) ? 6 : 60;
        if (const char *env = std::getenv("BRIMIR_VDP1_NX_GPU_SEEDS")) {
            seeds = std::strtoull(env, nullptr, 10);
        }
        const auto t0 = std::chrono::steady_clock::now();
        NxSceneStats stats;
        uint64 mismatches = 0;
        Coverage cov;
        for (uint64 seed = 1; seed <= seeds; ++seed) {
            const Scenario s = RandomScenario(seed, cov);
            const uint32 n = 2 + static_cast<uint32>(seed % 3);
            const gpu::Vdp1NxLimits limits =
                seed % 4 == 0 ? gpu::Vdp1NxLimits{2000 + static_cast<uint32>(seed % 3000), 16 + static_cast<uint32>(seed % 48)}
                              : gpu::Vdp1NxLimits{};
            std::string d;
            try {
                d = RunScene(*gpus[n - 2], s, seed % 2 == 1, &stats, limits);
            } catch (const std::exception &e) {
                d = std::string("exception: ") + e.what();
            }
            if (!d.empty() && mismatches++ < 5) {
                UNSCOPED_INFO("seed " << seed << " N=" << n << ": " << d);
            }
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        UNSCOPED_INFO("seeds " << seeds << " in " << seconds << " s: ops " << stats.ops << ", batches " << stats.batches
                               << ", sub-batches " << stats.subBatches << ", slots " << stats.slots << ", fragments "
                               << stats.fragments << ", erases " << stats.erases << ", write runs "
                               << stats.fbWriteRuns << ", max per N x unit " << stats.maxPerUnit);
        CHECK(mismatches == 0);
        CHECK(stats.splitBatches > 0);
        CHECK(stats.erases > 0);
        CHECK(stats.fbWriteRuns > 0);
        CHECK(stats.spans[0] > 0);
        CHECK(stats.spans[1] > 0);
        CHECK(stats.spans[2] > 0);
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("VDP1 N x resolve: synthetic fragment sets and more than 250000 fragments on one N x unit",
          "[gpu][vdp1][nx][passes]") {
    // Order-sensitive targets (half-transparency, shadow, MSB-on, 8 bpp bytes, meshes) on few N x units: both sides of
    // the small/large boundary, many windows, and one unit with 260,000; plus units at both ends of the N x unit space
    // so the range covers all N^2 * 262,144 units (bucket groups over 16 blocks, 16 sum chunks, 16 resolve rounds at 4x)
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);
    {
        TestRng rng{4242};
        for (uint32 n = 2; n <= gpu::kMaxNxScale; ++n) {
            CAPTURE(n);
            GpuVdp1Nx g(*device, n);
            REQUIRE(g.Ok());
            const uint32 units = g.UnitCount();
            const uint32 deep = n == 4 ? 260000 : 40000;
            struct UnitLoad {
                uint32 unit, count;
            };
            const std::vector<UnitLoad> loads = {
                {0, 3},
                {units - 1, 2},
                {gpu::Vdp1NxUnit(0x20000 | 777, n * n - 1, n), 33},
                {gpu::Vdp1NxUnit(5000, 1, n), 2049},
                {gpu::Vdp1NxUnit(0x20000 | 54321, n + 1, n), deep},
                {gpu::Vdp1NxUnit(54321, n + 1, n), 32},
            };
            uint32 total = 0;
            for (const UnitLoad &l : loads) {
                total += l.count;
            }
            for (const uint32 drawFB : {0u, 1u}) {
                CAPTURE(drawFB);
                std::vector<gpu::Vdp1Fragment> slots(total + total / 4, gpu::Vdp1Fragment{0, 0});
                std::vector<uint32> order(slots.size());
                for (uint32 i = 0; i < order.size(); ++i) {
                    order[i] = i;
                }
                for (size_t i = order.size(); i > 1; --i) {
                    std::swap(order[i - 1], order[rng.Next() % i]);
                }
                size_t next = 0;
                for (const UnitLoad &l : loads) {
                    for (uint32 k = 0; k < l.count; ++k) {
                        const uint32 kind = static_cast<uint32>(rng.Range(0, 99));
                        const bool tm = rng.Chance(40);
                        const uint32 nativeUnit = l.unit / (n * n);
                        const uint32 native =
                            gpu::Vdp1MakeFragmentTarget(nativeUnit & gpu::kWordIndexMask, (nativeUnit >> 17) != 0,
                                                        rng.Chance(50), static_cast<uint32>(rng.Range(1, 3)),
                                                        tm && rng.Chance(40), kind < 3, kind >= 3 && kind < 12, tm);
                        slots[order[next++]] = {gpu::Vdp1MakeNxFragmentTarget(l.unit, native),
                                                static_cast<uint32>(rng.Next() & 0xFFFF)};
                    }
                }
                auto initial = std::make_unique<ref::Fbram>();
                for (uint32 s = 0; s < 2; ++s) {
                    for (size_t i = 0; i < initial->main[s].size(); ++i) {
                        initial->main[s][i] = static_cast<uint8>(rng.Next() | (i % 2 == 0 ? 0x80 : 0));
                        initial->alt[s][i] = static_cast<uint8>(rng.Next());
                        initial->mesh[0][s][i] = static_cast<uint8>(rng.Next());
                        initial->mesh[1][s][i] = static_cast<uint8>(rng.Next());
                    }
                }
                auto surface = std::make_unique<nx::NxFbram>(n);
                surface->ReplicateFrom(*initial);
                // sub-texels that differ from their native word, so a wrong sub-texel shows
                for (auto &slice : surface->slices) {
                    for (size_t k = 0; k < slice.size(); k += 3) {
                        slice[k] ^= static_cast<uint16>(k * 0x2545u);
                    }
                }
                g.Reset(*initial, *surface);
                g.Times().Clear();
                const auto start = std::chrono::steady_clock::now();
                REQUIRE(g.ResolveFragments(slots, drawFB));
                const double wallMs =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                UNSCOPED_INFO("N=" << n << " " << deep << " on one unit: " << g.Times().Format() << "; wall " << wallMs
                                   << " ms");
                // Bounded: under 1 s of GPU time on a GPU; the submit well within any device-loss timeout
                if (!IsCpuDevice(*device) && g.Times().dispatches[static_cast<uint32>(Pass::Resolve)] != 0) {
                    CHECK(g.Times().Total() < 1000.0);
                }
                CHECK(wallMs < 30000.0);
                std::vector<nx::NxFragment> fragments;
                for (uint32 i = 0; i < slots.size(); ++i) {
                    if (gpu::Vdp1FragLive(slots[i].target)) {
                        const uint32 unit = gpu::Vdp1NxFragUnit(slots[i].target);
                        const uint32 nativeUnit = unit / (n * n);
                        fragments.push_back({i, (gpu::Vdp1NxFragNativeTarget(slots[i].target) & ~gpu::kFragLiveBit) |
                                                    nativeUnit,
                                             unit - nativeUnit * n * n, slots[i].color});
                    }
                }
                const nx::NxResolveStats rs = nx::Resolve(drawFB, fragments, *surface);
                CHECK(rs.maxPerUnit == deep);
                CHECK(g.CompareSurface(*surface) == "");
                CHECK(g.CountsZero());
                const uint32 *state = g.Map<uint32>(GpuVdp1Nx::BatchState);
                CHECK(state[gpu::kBatchStateUnitLo] == 0);
                CHECK(state[gpu::kBatchStateUnitHi] == units);
                // ... and in descending key order the result differs (the sets are order-sensitive)
                auto reversed = std::make_unique<nx::NxFbram>(n);
                reversed->ReplicateFrom(*initial);
                for (auto &slice : reversed->slices) {
                    for (size_t k = 0; k < slice.size(); k += 3) {
                        slice[k] ^= static_cast<uint16>(k * 0x2545u);
                    }
                }
                nx::Resolve(drawFB, fragments, *reversed, nx::ResolveOrder::KeyDescendingForTesting);
                CHECK(g.CompareSurface(*reversed) != "");
            }
        }
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("VDP1 N x consistency passes: erase_nx, fb_write_nx and rebuild_nx equal the CPU model",
          "[gpu][vdp1][nx][passes]") {
    // Research section 2: a native word's change by an erase or CPU writes applies to its whole N x block, masked as the
    // native change (erase: the write limit stopping mid-row, the alternate field and the meshes; CPU writes: byte
    // masks, the alternate-field mirror, records of one run overlapping); rebuild_nx replicates the native slices of its
    // mask and leaves the others
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);
    {
        TestRng rng{2718};
        for (uint32 n = 2; n <= gpu::kMaxNxScale; ++n) {
            CAPTURE(n);
            GpuVdp1Nx g(*device, n);
            REQUIRE(g.Ok());
            auto native = std::make_unique<ref::Fbram>();
            for (uint32 slice = 0; slice < 8; ++slice) {
                for (auto &b : Slice(*native, slice)) {
                    b = static_cast<uint8>(rng.Next());
                }
            }
            auto surface = std::make_unique<nx::NxFbram>(n);
            for (auto &slice : surface->slices) {
                for (auto &w : slice) {
                    w = static_cast<uint16>(rng.Next());
                }
            }
            g.Reset(*native, *surface);

            // Erases
            struct EraseCase {
                uint32 x1, x3, y1, y3, shift, limit, flags;
            };
            const EraseCase erases[] = {
                {4, 12, 10, 10, 9, 0xFFFFFFFFu, gpu::Vdp1MakeEraseFlags(0, false, false)},
                {0, 400, 0, 223, 9, 0xFFFFFFFFu, gpu::Vdp1MakeEraseFlags(1, false, true)},
                {8, 300, 100, 300, 9, 1000, gpu::Vdp1MakeEraseFlags(0, true, true)}, // stops mid-row
                {296, 428, 200, 511, 8, 0xFFFFFFFFu, gpu::Vdp1MakeEraseFlags(1, true, false)}, // overlapping rows
                {3, 333, 7, 9, 9, 401, gpu::Vdp1MakeEraseFlags(0, false, true)},              // mid-row, odd start
                {10, 10, 0, 5, 9, 0xFFFFFFFFu, gpu::Vdp1MakeEraseFlags(0, false, false)},     // empty
            };
            for (const EraseCase &c : erases) {
                CAPTURE(c.x1, c.x3, c.y1, c.y3, c.shift, c.limit, c.flags);
                const gpu::Vdp1ErasePushConstants e{c.x1, c.x3, c.y1, c.y3, static_cast<uint32>(rng.Next() & 0xFFFF),
                                                    c.shift, c.limit, c.flags};
                nx::Erase(e, *surface);
                REQUIRE(g.Erase(e));
                CHECK(g.CompareSurface(*surface) == "");
            }

            // CPU write runs: few words (records collide within and across groups), byte and word masks, both
            // framebuffers, mirrored or not; native FBRAM is the run's in-order result
            for (const uint32 count : {1u, 63u, 64u, 65u, 1000u, 20000u}) {
                CAPTURE(count);
                std::vector<gpu::Vdp1FbWrite> writes;
                for (uint32 k = 0; k < count; ++k) {
                    const uint32 word = rng.Chance(70) ? static_cast<uint32>(rng.Range(0, 40))
                                                       : static_cast<uint32>(rng.Next() & 0x1FFFF);
                    static constexpr uint32 kMasks[] = {0xFF00, 0x00FF, 0xFFFF};
                    writes.push_back(
                        {gpu::Vdp1MakeFbWriteAddress(word, static_cast<uint32>(rng.Range(0, 1)), rng.Chance(40)),
                         static_cast<uint32>(rng.Next() & 0xFFFF), rng.Pick(kMasks), 0});
                }
                ref::ApplyFbWrites(writes, *native);
                nx::ApplyFbWrites(writes, *surface);
                g.SetFbram(*native);
                g.Copy(GpuVdp1Nx::FbWrites, writes.data(), writes.size() * sizeof(gpu::Vdp1FbWrite));
                REQUIRE(g.FbWrite({0, count}));
                CHECK(g.CompareSurface(*surface) == "");
            }

            // Rebuild per slice mask
            for (const uint32 mask : {0x01u, 0x0Cu, 0xA5u, 0x00u, 0xFFu}) {
                CAPTURE(mask);
                for (uint32 slice = 0; slice < 8; ++slice) {
                    for (auto &b : Slice(*native, slice)) {
                        b = static_cast<uint8>(rng.Next());
                    }
                }
                g.SetFbram(*native);
                nx::NxFbram replicated(n);
                replicated.ReplicateFrom(*native);
                for (uint32 slice = 0; slice < 8; ++slice) {
                    if ((mask >> slice) & 1) {
                        surface->slices[slice] = replicated.slices[slice];
                    }
                }
                REQUIRE(g.Rebuild(mask));
                CHECK(g.CompareSurface(*surface) == "");
            }
            UNSCOPED_INFO("N=" << n << " timings: " << g.Times().Format());
        }
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("VDP1 N x passes on captured frames (BRIMIR_VDP1_CAPTURES)", "[gpu][vdp1][nx][passes][.captures]") {
    // Every vdp1ops_*.bin under the directory (the Task 1 captures) at N = 2, 3, 4: the lists through the GPU N x
    // passes and the references (host-cached memory); the final N x surface equals the op-by-op reference's and the
    // reference driver's own replay. Then the GPU passes alone on device-local memory (the second of two runs), the
    // GPU time per pass printed
    const char *dir = std::getenv("BRIMIR_VDP1_CAPTURES");
    if (dir == nullptr) {
        SKIP("BRIMIR_VDP1_CAPTURES not set");
    }
    std::vector<std::filesystem::path> paths;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.path().extension() == ".bin" && entry.path().filename().string().rfind("vdp1ops_", 0) == 0) {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    REQUIRE(!paths.empty());
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);
    for (const uint32 n : {2u, 3u, 4u}) {
        for (const auto &path : paths) {
            CAPTURE(n, path.string());
            gpu::Vdp1OpDump d;
            std::string error;
            REQUIRE(gpu::ReadVdp1OpDump(path.string(), d, &error));
            auto initial = std::make_unique<ref::Fbram>();
            for (uint32 slice = 0; slice < 8; ++slice) {
                std::memcpy(Slice(*initial, slice).data(), d.fbramStart.data() + slice * gpu::kFbramSliceSize,
                            gpu::kFbramSliceSize);
            }
            NxSceneStats stats;
            {
                // VRAM starts as captured (the N x expectation's and the GPU's; the native reference, which only the
                // CPU write runs read under their masks, starts zeroed)
                GpuVdp1Nx g(*device, n);
                REQUIRE(g.Ok());
                NxRunner runner(g, *initial, false);
                runner.SeedVram(d.vramStart);
                for (const auto &l : d.lists) {
                    CHECK(runner.Replay(l) == "");
                }
                stats = runner.stats;
                auto native = std::make_unique<ref::Fbram>(*initial);
                auto surface = std::make_unique<nx::NxFbram>(n);
                surface->ReplicateFrom(*initial);
                nx::ReferenceNx r{*native, *surface, nx::Options{.scale = n}, d.vramStart};
                for (const auto &l : d.lists) {
                    r.Replay(l);
                }
                CHECK(g.CompareSurface(*surface) == "");
            }
            GpuVdp1Nx g(*device, n, GpuVdp1Nx::Memory::DeviceLocal);
            REQUIRE(g.Ok());
            Timings t;
            for (int run = 0; run < 2; ++run) {
                NxRunner runner(g, *initial, false);
                runner.TimingOnly();
                runner.SeedVram(d.vramStart);
                g.Times().Clear();
                for (const auto &l : d.lists) {
                    CHECK(runner.Replay(l) == "");
                }
                t = g.Times();
            }
            std::printf("nx-passes: %-12s N=%u batches %llu sub-batches %llu slots %llu fragments %llu (max %u per "
                        "unit): GPU %.3f ms (%s)\n",
                        path.parent_path().filename().string().c_str(), n,
                        static_cast<unsigned long long>(stats.batches), static_cast<unsigned long long>(stats.subBatches),
                        static_cast<unsigned long long>(stats.slots), static_cast<unsigned long long>(stats.fragments),
                        stats.maxPerUnit, t.Total(), t.Format().c_str());
            std::fflush(stdout);
        }
    }
    CloseAndCheckValidation(device);
}