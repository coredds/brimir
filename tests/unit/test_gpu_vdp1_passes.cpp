// The VDP1 compute passes (src/gpu/shaders/vdp1/cs_vdp1_*.hlsl) on a device against the CPU reference of the GPU back
// half (vdp1_raster_reference.hpp), pass by pass:
// - raster: the fragment buffer (every slot of the batch: the reference's fragment at its key, 0 elsewhere) and the
//   per-unit counts equal the reference's RasterSpan output;
// - bucket and scatter: each unit's bucket starts at the exclusive prefix of the counts and holds exactly its slots;
// - resolve: FBRAM (main, alternate fields, meshes) equals the reference's Resolve, and the counts are zero again;
// - erase and CPU framebuffer writes: FBRAM equals the reference's Erase and ApplyFbWrites.
// Inputs: hand-made span lists (one command, spans built here), synthetic fragment sets on few words (up to 50,000
// fragments on one word, every resolve window size), the front half's op lists for hand-made scenes (apex stacks of
// more than 8,000 fragments per word, every draw mode, 8 bpp, meshes, double interlace, erases) and for seeded random
// command tables (vdp1_scenario.hpp). Every op of an op list is replayed on both, FBRAM compared after each one; each
// scene's final FBRAM is also compared with the software renderer's.
// [gpu]: SKIP without a device. The random tables default to 40 seeds (10 on a CPU device such as lavapipe);
// BRIMIR_VDP1_GPU_SEEDS=N runs N and prints the totals. The [timing] case is hidden ([.]): run it explicitly. With BRIMIR_VK_SYNC_VALIDATION=1 synchronization validation runs too.
#include "catch_amalgamated.hpp"

#include "gpu_test_device.hpp"
#include "vdp1_raster_reference.hpp"
#include "vdp1_scenario.hpp"

#include <gpu/shaders/generated/vdp1_batch_range.spv.h>
#include <gpu/shaders/generated/vdp1_bucket.spv.h>
#include <gpu/shaders/generated/vdp1_bucket_sums.spv.h>
#include <gpu/shaders/generated/vdp1_erase.spv.h>
#include <gpu/shaders/generated/vdp1_fb_write.spv.h>
#include <gpu/shaders/generated/vdp1_fb_write_clear.spv.h>
#include <gpu/shaders/generated/vdp1_fb_write_mark.spv.h>
#include <gpu/shaders/generated/vdp1_raster.spv.h>
#include <gpu/shaders/generated/vdp1_resolve.spv.h>
#include <gpu/shaders/generated/vdp1_scatter.spv.h>
#include <gpu/vdp1/vdp1_bindings.hpp>
#include <gpu/vdp1/vdp1_front.hpp>
#include <gpu/vk_util.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace brimir::test::vdp1scene;
namespace vu = brimir::gpu::vk_util;
namespace sh = brimir::gpu::shaders;
using brimir::gpu::VulkanDevice;
using brimir::test::CloseAndCheckValidation;
using brimir::test::OpenDevice;

enum class Pass : uint32 {
    Raster, BatchRange, Bucket, BucketSums, Scatter, Resolve, Erase, FbWriteMark, FbWrite, FbWriteClear, Count
};
constexpr uint32 kPassCount = static_cast<uint32>(Pass::Count);
constexpr const char *kPassNames[kPassCount] = {"raster", "batch_range", "bucket", "bucket_sums", "scatter",
                                                "resolve", "erase", "fb_write_mark", "fb_write", "fb_write_clear"};

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

// What a test can look at between the passes of a batch (the buffers are host-visible)
struct Inspect {
    std::function<void()> afterRaster;
    std::function<void()> afterScatter;
};

// The VDP1 passes with their buffers (all host-visible, device-local when the device has such memory), one descriptor
// set per pipeline. Every submit starts with a barrier from host and earlier compute writes and ends with one to host
// reads; consecutive passes are separated by compute-to-compute barriers.
class GpuVdp1 {
public:
    enum Buf : uint32 {
        Commands, Spans, Vram, FbWrites, Fragments, UnitCounts, Bucket, Fbram, ScanSums, UnitOffsets, BatchState, FbWinners,
        kBufCount
    };

    // Memory for the buffers: host-cached (fast readback for the comparisons; the GPU reads and writes system memory) or
    // device-local and host-visible (for the timings, when the device has such memory)
    enum class Memory { HostCached, DeviceLocal };

    explicit GpuVdp1(VulkanDevice &device, Memory memory = Memory::HostCached)
        : m_device(device) {
        m_ok = Init(memory);
    }
    ~GpuVdp1() {
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
    bool DeviceLocal() const {
        return m_deviceLocal;
    }
    template <typename T>
    T *Map(Buf b) {
        return static_cast<T *>(m_buffers[b].mapped);
    }
    Timings &Times() {
        return m_timings;
    }

    // Fresh state: VRAM zero, FBRAM from `fb`, counts zero (they must be zero between batches anyway)
    void Reset(const ref::Fbram &fb) {
        std::memset(Map<uint8>(Vram), 0, gpu::kVRAMBufferSize);
        std::memset(Map<uint8>(UnitCounts), 0, gpu::kUnitCountsBufferSize);
        SetFbram(fb);
    }
    void SetFbram(const ref::Fbram &fb) {
        uint8 *p = Map<uint8>(Fbram);
        for (uint32 slice = 0; slice < 8; ++slice) {
            std::memcpy(p + slice * gpu::kFbramSliceSize, Slice(fb, slice).data(), gpu::kFbramSliceSize);
        }
    }
    void GetFbram(ref::Fbram &fb) {
        const uint8 *p = Map<uint8>(Fbram);
        for (uint32 slice = 0; slice < 8; ++slice) {
            std::memcpy(Slice(fb, slice).data(), p + slice * gpu::kFbramSliceSize, gpu::kFbramSliceSize);
        }
    }
    bool CountsZero() {
        const uint32 *c = Map<uint32>(UnitCounts);
        return std::all_of(c, c + gpu::kUnitCount, [](uint32 v) { return v == 0; });
    }

    // The op list's commands, spans and CPU writes go to the buffers whole when they fit (the passes then use the real
    // offsets), else per op at 0
    void BeginOpList(const gpu::Vdp1OpList &ops) {
        m_whole = ops.commands.size() <= gpu::kMaxCommandsPerBatch && ops.spans.size() <= gpu::kMaxSpansPerBatch &&
                  ops.fbWrites.size() <= gpu::kMaxFbWritesPerBatch;
        if (m_whole) {
            Copy(Commands, ops.commands.data(), ops.commands.size() * sizeof(gpu::Vdp1CommandParams));
            Copy(Spans, ops.spans.data(), ops.spans.size() * sizeof(gpu::Vdp1Span));
            Copy(FbWrites, ops.fbWrites.data(), ops.fbWrites.size() * sizeof(gpu::Vdp1FbWrite));
        }
    }

    // Op i of the list (after BeginOpList)
    bool RunOp(const gpu::Vdp1OpList &ops, size_t i, const Inspect *inspect = nullptr) {
        const gpu::Vdp1Op &op = ops.ops[i];
        switch (op.kind) {
        case gpu::Vdp1OpKind::Draw: {
            const gpu::Vdp1DrawBatch &b = ops.batches[op.index];
            uint8 *vram = Map<uint8>(Vram);
            for (uint32 k = 0; k < b.copyCount; ++k) {
                const gpu::Vdp1VramCopy &c = ops.vramCopies[b.firstCopy + k];
                std::memcpy(vram + c.vramOffset, ops.arena.data() + c.arenaOffset, c.size);
            }
            gpu::Vdp1BatchPushConstants pc = gpu::Vdp1BatchConstants(b);
            if (!m_whole) {
                Copy(Commands, ops.commands.data() + b.cmdOffset, b.cmdCount * sizeof(gpu::Vdp1CommandParams));
                Copy(Spans, ops.spans.data() + b.spanOffset, b.spanCount * sizeof(gpu::Vdp1Span));
                pc.cmdOffset = 0;
                pc.spanOffset = 0;
            }
            return DrawBatch(pc, inspect);
        }
        case gpu::Vdp1OpKind::Erase: return Erase(ops.erases[op.index]);
        case gpu::Vdp1OpKind::FbWrites: {
            gpu::Vdp1FbWritePushConstants run = ops.fbWriteRuns[op.index];
            if (!m_whole) {
                Copy(FbWrites, ops.fbWrites.data() + run.offset, run.count * sizeof(gpu::Vdp1FbWrite));
                run.offset = 0;
            }
            return FbWrite(run);
        }
        }
        return false;
    }

    bool DrawBatch(const gpu::Vdp1BatchPushConstants &pc, const Inspect *inspect = nullptr) {
        const uint32 rasterGroups = (pc.spanCount + kWorkgroupSize - 1) / kWorkgroupSize;
        auto raster = [&](VkCommandBuffer cmd) {
            if (rasterGroups != 0) {
                Dispatch(cmd, Pass::Raster, &pc, rasterGroups);
            }
        };
        auto bucket = [&](VkCommandBuffer cmd) {
            Dispatch(cmd, Pass::BatchRange, nullptr, 1);
            DispatchIndirect(cmd, Pass::Bucket, nullptr, gpu::kBatchStateBucketArgs);
            Dispatch(cmd, Pass::BucketSums, nullptr, 1);
            const uint32 scatterGroups = (pc.fragmentCount + gpu::kScatterSlotsPerGroup - 1) / gpu::kScatterSlotsPerGroup;
            if (scatterGroups != 0) {
                Dispatch(cmd, Pass::Scatter, &pc, scatterGroups);
            }
        };
        auto resolve = [&](VkCommandBuffer cmd) {
            DispatchIndirect(cmd, Pass::Resolve, &pc, gpu::kBatchStateResolveArgs);
        };
        if (inspect == nullptr) {
            return Submit([&](VkCommandBuffer cmd) {
                raster(cmd);
                bucket(cmd);
                resolve(cmd);
            });
        }
        if (!Submit(raster)) {
            return false;
        }
        if (inspect->afterRaster) {
            inspect->afterRaster();
        }
        if (!Submit(bucket)) {
            return false;
        }
        if (inspect->afterScatter) {
            inspect->afterScatter();
        }
        return Submit(resolve);
    }

    // The bucket, scatter and resolve passes over a fragment buffer written here (the counts computed here, as the
    // raster pass would)
    bool ResolveFragments(const std::vector<gpu::Vdp1Fragment> &slots, uint32 drawFB) {
        Copy(Fragments, slots.data(), slots.size() * sizeof(gpu::Vdp1Fragment));
        uint32 *counts = Map<uint32>(UnitCounts);
        uint32 lo = ~0u, hi = 0;
        for (const gpu::Vdp1Fragment &f : slots) {
            if (gpu::Vdp1FragLive(f.target)) {
                const uint32 unit = gpu::Vdp1FragUnit(f.target);
                ++counts[unit];
                lo = std::min(lo, unit);
                hi = std::max(hi, unit + 1);
            }
        }
        uint32 *state = Map<uint32>(BatchState);
        state[gpu::kBatchStateAccLoInv] = hi != 0 ? ~lo : 0;
        state[gpu::kBatchStateAccHi] = hi;
        const gpu::Vdp1BatchPushConstants pc{0, 0, 0, static_cast<uint32>(slots.size()), drawFB, 1};
        return Submit([&](VkCommandBuffer cmd) {
            Dispatch(cmd, Pass::BatchRange, nullptr, 1);
            DispatchIndirect(cmd, Pass::Bucket, nullptr, gpu::kBatchStateBucketArgs);
            Dispatch(cmd, Pass::BucketSums, nullptr, 1);
            Dispatch(cmd, Pass::Scatter, &pc,
                     (pc.fragmentCount + gpu::kScatterSlotsPerGroup - 1) / gpu::kScatterSlotsPerGroup);
            DispatchIndirect(cmd, Pass::Resolve, &pc, gpu::kBatchStateResolveArgs);
        });
    }

    bool Erase(const gpu::Vdp1ErasePushConstants &pc) {
        if (pc.x3 <= pc.x1 || pc.y3 < pc.y1) {
            return true;
        }
        const uint32 gx = (pc.x3 - pc.x1 + gpu::kEraseGroupSize - 1) / gpu::kEraseGroupSize;
        const uint32 gy = (pc.y3 - pc.y1 + 1 + gpu::kEraseGroupSize - 1) / gpu::kEraseGroupSize;
        return Submit([&](VkCommandBuffer cmd) { Dispatch(cmd, Pass::Erase, &pc, gx, gy); });
    }

    bool FbWrite(const gpu::Vdp1FbWritePushConstants &pc) {
        const uint32 groups = (pc.count + kWorkgroupSize - 1) / kWorkgroupSize;
        if (groups == 0) {
            return true;
        }
        return Submit([&](VkCommandBuffer cmd) {
            Dispatch(cmd, Pass::FbWriteMark, &pc, groups);
            Dispatch(cmd, Pass::FbWrite, &pc, groups);
            Dispatch(cmd, Pass::FbWriteClear, &pc, groups);
        });
    }

    void Copy(Buf b, const void *data, size_t size) {
        REQUIRE(size <= m_buffers[b].size);
        if (size != 0) {
            std::memcpy(m_buffers[b].mapped, data, size);
        }
    }

private:
    static constexpr uint32 kWorkgroupSize = gpu::kWorkgroupSize;
    static constexpr uint32 kMaxQueries = 64;

    struct Pipe {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32 pushSize = 0;
    };

    static vdp::SpriteFB &Slice(ref::Fbram &fb, uint32 slice) {
        switch (slice) {
        case 0: return fb.main[0];
        case 1: return fb.main[1];
        case 2: return fb.alt[0];
        case 3: return fb.alt[1];
        default: return fb.mesh[(slice - 4) >> 1][slice & 1];
        }
    }
    static const vdp::SpriteFB &Slice(const ref::Fbram &fb, uint32 slice) {
        return Slice(const_cast<ref::Fbram &>(fb), slice);
    }

    bool Init(Memory memory) {
        const auto &vk = m_device.Table();
        static constexpr std::array<uint32, kBufCount> kBindings{
            gpu::kBindingCommands, gpu::kBindingSpans,  gpu::kBindingVRAM,     gpu::kBindingFbWrites,
            gpu::kBindingFragments, gpu::kBindingUnitCounts, gpu::kBindingBucket, gpu::kBindingFbram,
            gpu::kBindingScanSums, gpu::kBindingUnitOffsets, gpu::kBindingBatchState, gpu::kBindingFbWinners};
        static constexpr std::array<uint32, kBufCount> kSizes{
            gpu::kCommandBufferSize,  gpu::kSpanBufferSize,       gpu::kVRAMBufferSize,   gpu::kFbWriteBufferSize,
            gpu::kFragmentBufferSize, gpu::kUnitCountsBufferSize, gpu::kBucketBufferSize, gpu::kFbramBufferSize,
            gpu::kScanSumsBufferSize, gpu::kUnitOffsetsBufferSize, gpu::kBatchStateBufferSize,
            gpu::kFbWinnersBufferSize};
        constexpr VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        const VkMemoryPropertyFlags preferred =
            kHost | (memory == Memory::DeviceLocal ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        m_deviceLocal = memory == Memory::DeviceLocal;
        for (uint32 b = 0; b < kBufCount; ++b) {
            m_bindingOf[b] = kBindings[b];
            // the batch state is also the bucket and resolve passes' indirect dispatch arguments
            const VkBufferUsageFlags usage =
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | (b == BatchState ? VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT : 0u);
            VkResult r = vu::CreateBuffer(m_device, kSizes[b], usage, preferred, m_buffers[b]);
            if (r != VK_SUCCESS) {
                m_deviceLocal = false;
                r = vu::CreateBuffer(m_device, kSizes[b], usage, kHost, m_buffers[b]);
            }
            if (r != VK_SUCCESS || m_buffers[b].mapped == nullptr) {
                return false;
            }
            std::memset(m_buffers[b].mapped, 0, kSizes[b]); // zero at creation (the fragment buffer contract)
        }

        struct PassDesc {
            std::span<const uint32_t> spirv;
            std::span<const gpu::Binding> bindings;
            uint32 pushSize;
        };
        const std::array<PassDesc, kPassCount> passes{{
            {sh::kVdp1RasterSpv, gpu::kRasterBindings, sizeof(gpu::Vdp1BatchPushConstants)},
            {sh::kVdp1BatchRangeSpv, gpu::kBatchRangeBindings, 0},
            {sh::kVdp1BucketSpv, gpu::kBucketBindings, 0},
            {sh::kVdp1BucketSumsSpv, gpu::kBucketSumsBindings, 0},
            {sh::kVdp1ScatterSpv, gpu::kScatterBindings, sizeof(gpu::Vdp1BatchPushConstants)},
            {sh::kVdp1ResolveSpv, gpu::kResolveBindings, sizeof(gpu::Vdp1BatchPushConstants)},
            {sh::kVdp1EraseSpv, gpu::kEraseBindings, sizeof(gpu::Vdp1ErasePushConstants)},
            {sh::kVdp1FbWriteMarkSpv, gpu::kFbWriteMarkBindings, sizeof(gpu::Vdp1FbWritePushConstants)},
            {sh::kVdp1FbWriteSpv, gpu::kFbWriteBindings, sizeof(gpu::Vdp1FbWritePushConstants)},
            {sh::kVdp1FbWriteClearSpv, gpu::kFbWriteClearBindings, sizeof(gpu::Vdp1FbWritePushConstants)},
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
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            for (const gpu::Binding &b : p.bindings) {
                bindings.push_back({b.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
            }
            const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, p.pushSize};
            pipe.pushSize = p.pushSize;
            if (vu::CreateDescriptorSetLayout(m_device, bindings, pipe.setLayout) != VK_SUCCESS ||
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

        // Timestamps, when the queue has them
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

    // vkCmdDispatchIndirect with the arguments at word `argsWord` of the batch state
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
        // compute writes before the next pass's shader accesses and indirect argument reads
        vu::GlobalBarrier(m_device, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    }

    bool Submit(const std::function<void(VkCommandBuffer)> &record) {
        const auto &vk = m_device.Table();
        const VkDevice dev = m_device.Device();
        m_queryCount = 0;
        // One command buffer and fence, reused (a pool and fence per submit cost more than the passes of a small batch)
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
            if (vk.vkGetQueryPoolResults(m_device.Device(), m_queries, 0, m_queryCount, sizeof(stamps), stamps.data(),
                                         sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) ==
                VK_SUCCESS) {
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
    bool m_ok = false;
    bool m_deviceLocal = false;
    bool m_whole = false;
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

uint64 SeedCount(const VulkanDevice &device, uint64 gpuCount, uint64 cpuCount) {
    if (const char *env = std::getenv("BRIMIR_VDP1_GPU_SEEDS")) {
        return std::strtoull(env, nullptr, 10);
    }
    return IsCpuDevice(device) ? cpuCount : gpuCount;
}

// -----------------------------------------------------------------------------
// Reference expectations

// Op i of `ops` as an op list of its own (offsets rebased), for the reference to replay op by op
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

// The raster pass's expected output for a batch: every slot (the fragment, live, at its key; 0 elsewhere) and the
// per-unit counts
struct RasterExpectation {
    std::vector<gpu::Vdp1Fragment> slots;
    std::vector<uint32> counts;
    uint32 maxPerUnit = 0;
    uint32 unitLo = 0, unitHi = 0; // the hull of the fragments' units (0, 0 without fragments)
};

RasterExpectation ExpectedRaster(const gpu::Vdp1OpList &ops, const gpu::Vdp1DrawBatch &b, const uint8 *vram) {
    RasterExpectation e;
    e.slots.assign(b.fragmentCount, gpu::Vdp1Fragment{0, 0});
    e.counts.assign(gpu::kUnitCount, 0);
    std::vector<ref::Fragment> fragments;
    for (uint32 s = 0; s < b.spanCount; ++s) {
        const ref::Span span = ref::DecodeSpan(ops.spans[b.spanOffset + s]);
        const gpu::Vdp1CommandParams &c = ops.commands[b.cmdOffset + span.cmdIndex];
        ref::RasterSpan(ref::DecodeRasterState(c.rasterFlags, b.drawFB), ref::DecodeCommand(c), span, vram, {},
                        fragments);
    }
    for (const ref::Fragment &f : fragments) {
        if (f.key >= b.fragmentCount) {
            throw std::logic_error("ExpectedRaster: a fragment key outside the batch's slots");
        }
        e.slots[f.key] = {f.target | gpu::kFragLiveBit, f.color};
        const uint32 unit = f.target & gpu::kFragUnitMask;
        e.maxPerUnit = std::max(e.maxPerUnit, ++e.counts[unit]);
        e.unitLo = e.unitHi == 0 ? unit : std::min(e.unitLo, unit);
        e.unitHi = std::max(e.unitHi, unit + 1);
    }
    return e;
}

std::string CompareRaster(const RasterExpectation &e, GpuVdp1 &g) {
    const auto *slots = g.Map<gpu::Vdp1Fragment>(GpuVdp1::Fragments);
    for (size_t i = 0; i < e.slots.size(); ++i) {
        if (slots[i].target != e.slots[i].target || slots[i].color != e.slots[i].color) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "slot %zu: GPU %08X/%04X, reference %08X/%04X", i, slots[i].target,
                          slots[i].color, e.slots[i].target, e.slots[i].color);
            return buf;
        }
    }
    const uint32 *counts = g.Map<uint32>(GpuVdp1::UnitCounts);
    for (uint32 u = 0; u < gpu::kUnitCount; ++u) {
        if (counts[u] != e.counts[u]) {
            return "unit " + std::to_string(u) + " count: GPU " + std::to_string(counts[u]) + ", reference " +
                   std::to_string(e.counts[u]);
        }
    }
    // The batch's unit range accumulated by the raster pass is the hull of the reference's fragments: every fragment
    // lies inside it (the bucket and resolve passes cover only it) and it is no wider than needed
    const uint32 *state = g.Map<uint32>(GpuVdp1::BatchState);
    const uint32 hi = state[gpu::kBatchStateAccHi];
    const uint32 lo = hi != 0 ? ~state[gpu::kBatchStateAccLoInv] : 0;
    if (lo != e.unitLo || hi != e.unitHi) {
        return "unit range: GPU [" + std::to_string(lo) + ", " + std::to_string(hi) + "), reference [" +
               std::to_string(e.unitLo) + ", " + std::to_string(e.unitHi) + ")";
    }
    return {};
}

// After the scatter pass: the batch range is the hull, and unit u of the range's bucket is [prefix(u), prefix(u) +
// count(u)) of the exclusive prefix of the range's counts, holding exactly the unit's live slots (units outside the range
// have no fragment and stale offsets)
std::string CompareBuckets(const RasterExpectation &e, GpuVdp1 &g) {
    const uint32 *sums = g.Map<uint32>(GpuVdp1::ScanSums);
    const uint32 *offsets = g.Map<uint32>(GpuVdp1::UnitOffsets);
    const uint32 *bucket = g.Map<uint32>(GpuVdp1::Bucket);
    std::map<uint32, std::vector<uint32>> expected;
    for (uint32 i = 0; i < e.slots.size(); ++i) {
        if (gpu::Vdp1FragLive(e.slots[i].target)) {
            expected[gpu::Vdp1FragUnit(e.slots[i].target)].push_back(i);
        }
    }
    const uint32 *state = g.Map<uint32>(GpuVdp1::BatchState);
    const uint32 lo = state[gpu::kBatchStateUnitLo], hi = state[gpu::kBatchStateUnitHi];
    if (lo != e.unitLo || hi != e.unitHi) {
        return "batch range [" + std::to_string(lo) + ", " + std::to_string(hi) + ") differs from the hull";
    }
    if (state[gpu::kBatchStateAccLoInv] != 0 || state[gpu::kBatchStateAccHi] != 0) {
        return "the range accumulator was not reset";
    }
    if (state[gpu::kBatchStateBucketArgs] != ((hi + 63) >> 6) - (lo >> 6) ||
        state[gpu::kBatchStateResolveArgs] != std::min(hi - lo, gpu::kResolveGroupCount)) {
        return "indirect dispatch arguments do not match the range";
    }
    uint32 prefix = 0;
    for (uint32 u = (lo >> 6) << 6; u < hi; ++u) {
        const uint32 end = sums[u >> 6] + offsets[u];
        if (end != prefix + e.counts[u]) {
            return "unit " + std::to_string(u) + " bucket ends at " + std::to_string(end) + ", expected " +
                   std::to_string(prefix + e.counts[u]);
        }
        if (e.counts[u] != 0) {
            std::vector<uint32> got(bucket + prefix, bucket + end);
            std::sort(got.begin(), got.end());
            if (got != expected[u]) {
                return "unit " + std::to_string(u) + " bucket holds other slots";
            }
        }
        prefix = end;
    }
    return {};
}

struct SceneStats {
    uint64 ops = 0, batches = 0, fragments = 0, erases = 0, fbWriteRuns = 0;
    uint32 maxPerUnit = 0;
    void Merge(const SceneStats &o) {
        ops += o.ops;
        batches += o.batches;
        fragments += o.fragments;
        erases += o.erases;
        fbWriteRuns += o.fbWriteRuns;
        maxPerUnit = std::max(maxPerUnit, o.maxPerUnit);
    }
};

// Replays op lists on the GPU and, op by op, on the reference; with `inspect`, checks each batch's raster and scatter
// output too. Returns the first difference (empty: none).
class SceneRunner {
public:
    SceneRunner(GpuVdp1 &g, const ref::Fbram &initial, bool inspect)
        : m_gpu(g)
        , m_fb(std::make_unique<ref::Fbram>(initial))
        , m_gpuFb(std::make_unique<ref::Fbram>())
        , m_reference(*m_fb)
        , m_vram(gpu::kVRAMBufferSize, 0)
        , m_inspect(inspect) {
        g.Reset(initial);
    }

    std::string Replay(const gpu::Vdp1OpList &ops) {
        m_gpu.BeginOpList(ops);
        for (size_t i = 0; i < ops.ops.size(); ++i) {
            const gpu::Vdp1Op &op = ops.ops[i];
            ++stats.ops;
            std::string where = "op " + std::to_string(stats.ops - 1);
            std::string passDiff;
            Inspect inspect;
            RasterExpectation expect;
            if (op.kind == gpu::Vdp1OpKind::Draw) {
                const gpu::Vdp1DrawBatch &b = ops.batches[op.index];
                ++stats.batches;
                stats.fragments += b.fragmentCount;
                for (uint32 k = 0; k < b.copyCount; ++k) {
                    const gpu::Vdp1VramCopy &c = ops.vramCopies[b.firstCopy + k];
                    std::memcpy(m_vram.data() + c.vramOffset, ops.arena.data() + c.arenaOffset, c.size);
                }
                if (m_inspect) {
                    expect = ExpectedRaster(ops, b, m_vram.data());
                    stats.maxPerUnit = std::max(stats.maxPerUnit, expect.maxPerUnit);
                    inspect.afterRaster = [&] {
                        if (passDiff.empty()) {
                            const std::string d = CompareRaster(expect, m_gpu);
                            passDiff = d.empty() ? "" : "raster: " + d;
                        }
                    };
                    inspect.afterScatter = [&] {
                        if (passDiff.empty()) {
                            const std::string d = CompareBuckets(expect, m_gpu);
                            passDiff = d.empty() ? "" : "bucket/scatter: " + d;
                        }
                    };
                }
            } else if (op.kind == gpu::Vdp1OpKind::Erase) {
                ++stats.erases;
            } else {
                ++stats.fbWriteRuns;
            }
            m_reference.Replay(SingleOp(ops, i));
            if (!m_gpu.RunOp(ops, i, m_inspect ? &inspect : nullptr)) {
                return where + ": submit failed";
            }
            if (!passDiff.empty()) {
                return where + " " + passDiff;
            }
            // Without inspection FBRAM is compared at the end of the list only
            if (!m_inspect && i + 1 < ops.ops.size()) {
                continue;
            }
            if (!m_gpu.CountsZero()) {
                return where + ": unit counts not zero after the resolve";
            }
            m_gpu.GetFbram(*m_gpuFb);
            const std::string d = CompareFbram(*m_fb, *m_gpuFb, "reference", "GPU");
            if (!d.empty()) {
                static constexpr const char *kKinds[] = {"draw", "erase", "CPU writes"};
                return where + " (" + kKinds[static_cast<uint32>(op.kind)] + "): " + d;
            }
        }
        return {};
    }

    const ref::Fbram &GpuFbram() {
        m_gpu.GetFbram(*m_gpuFb);
        return *m_gpuFb;
    }

    SceneStats stats;

private:
    GpuVdp1 &m_gpu;
    std::unique_ptr<ref::Fbram> m_fb; // the reference's
    std::unique_ptr<ref::Fbram> m_gpuFb;
    ref::ReferenceVDP1 m_reference;
    std::vector<uint8> m_vram;
    bool m_inspect;
};

// A scenario through the front half; its op lists replayed on the GPU and the reference op by op; the final GPU FBRAM
// against software too
std::string RunScene(GpuVdp1 &g, const Scenario &s, bool inspect, SceneStats *stats = nullptr) {
    std::vector<gpu::Vdp1OpList> lists;
    RunReference(s, {}, 0, [&](const gpu::Vdp1OpList &ops) { lists.push_back(ops); });
    auto initial = std::make_unique<ref::Fbram>();
    FillMain(s, initial->main);
    SceneRunner runner(g, *initial, inspect);
    for (size_t l = 0; l < lists.size(); ++l) {
        const std::string d = runner.Replay(lists[l]);
        if (!d.empty()) {
            return "list " + std::to_string(l) + " " + d;
        }
    }
    if (stats != nullptr) {
        stats->Merge(runner.stats);
    }
    const auto sw = RunSoftware(s);
    const std::string d = CompareFbram(*sw, runner.GpuFbram());
    return d.empty() ? std::string{} : "against software: " + d;
}

// -----------------------------------------------------------------------------
// Hand-made op lists

struct HandBatch {
    gpu::Vdp1OpList ops;
    uint32 fragmentCount = 0;
};

// Appends span (a, b) of command cmdIndex (relative to the batch's first command) with its slots
void AddSpan(gpu::Vdp1OpList &ops, gpu::Vdp1DrawBatch &batch, CoordS32 a, CoordS32 b, uint32 quadStep, sint32 texV,
             uint16 gL, uint16 gR, uint32 cmdIndex = 0) {
    gpu::Vdp1Span span{static_cast<uint32>(a.x()), static_cast<uint32>(a.y()), static_cast<uint32>(b.x()),
                       static_cast<uint32>(b.y()), cmdIndex, batch.fragmentCount, gpu::Vdp1Pack16(gL, gR),
                       gpu::Vdp1Pack16(quadStep, static_cast<uint32>(texV) & 0xFFFF)};
    const gpu::Vdp1CommandParams &c = ops.commands[batch.cmdOffset + cmdIndex];
    const uint32 slots = ref::SpanSlots(ref::DecodeRasterState(c.rasterFlags, batch.drawFB), ref::DecodeCommand(c),
                                        ref::DecodeSpan(span));
    if (slots == 0) {
        return; // the front half emits no span for a rejected line
    }
    ops.spans.push_back(span);
    ++batch.spanCount;
    batch.fragmentCount += slots;
}

gpu::Vdp1CommandParams HandCommand(uint16 pmod, uint16 ctrl, bool textured, bool aa, uint16 color, uint32 rasterFlags) {
    gpu::Vdp1CommandParams c{};
    c.drawFlags = gpu::Vdp1MakeDrawFlags(pmod, ctrl, textured, aa);
    c.rasterFlags = rasterFlags;
    c.color = color;
    c.sysClip = gpu::Vdp1Pack16(352, 255);
    c.userClip0 = gpu::Vdp1Pack16(20, 10);
    c.userClip1 = gpu::Vdp1Pack16(300, 200);
    return c;
}

void CloseList(gpu::Vdp1OpList &ops) {
    ops.segments = {{0, static_cast<uint32>(ops.ops.size())}};
}

// One batch of `spans` lines from random points to one apex (two half-transparent Gouraud polygon commands, one with
// mesh): one fragment per line on the apex word
gpu::Vdp1OpList ApexList(uint32 rasterFlags, uint32 spans) {
    gpu::Vdp1OpList ops;
    gpu::Vdp1DrawBatch b{};
    ops.commands.push_back(HandCommand(0x00C7, 4, false, rasterFlags == 0, 0x83E0, rasterFlags));
    ops.commands.push_back(HandCommand(0x01C3, 4, false, true, 0xFC1F, rasterFlags)); // mesh
    b.cmdCount = 2;
    const CoordS32 apex{200, 120};
    TestRng rng{99 + rasterFlags};
    for (uint32 n = 0; n < spans; ++n) {
        const CoordS32 start{rng.Range(0, 352), rng.Range(0, 255)};
        AddSpan(ops, b, start, apex, n % 4096, 0, static_cast<uint16>(rng.Next()), static_cast<uint16>(rng.Next()),
                n % 7 == 0 ? 1 : 0);
    }
    ops.ops.push_back({gpu::Vdp1OpKind::Draw, 0});
    ops.batches.push_back(b);
    CloseList(ops);
    return ops;
}

std::unique_ptr<ref::Fbram> ApexInitialFbram() {
    auto initial = std::make_unique<ref::Fbram>();
    for (size_t i = 0; i < initial->main[0].size(); ++i) {
        initial->main[0][i] = static_cast<uint8>(0x80 | (i * 5));
    }
    return initial;
}

} // namespace

// =====================================================================================================================

TEST_CASE("VDP1 raster pass: hand-made span lists equal the reference's fragments and counts", "[gpu][vdp1][passes]") {
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);
    {
        GpuVdp1 g(*device);
        REQUIRE(g.Ok());

        // Two batches in one list (the second's commands and spans at non-zero offsets): untextured polygon lines
        // (Gouraud, antialiased, user clip, mesh, 8 bpp), then textured lines (colour modes 5 and 1, end codes,
        // transparent texels, high-speed shrink, flips, the texel row -1)
        gpu::Vdp1OpList ops;
        TestRng rng{4242};
        {
            gpu::Vdp1DrawBatch b{};
            b.cmdOffset = 0;
            ops.commands.push_back(HandCommand(0x00C4, 4, false, true, 0x83E0, 0));
            ops.commands.push_back(HandCommand(0x05C3, 4, false, true, 0xFC00, 0));              // user clip, half-tr.
            ops.commands.push_back(HandCommand(0x01C0, 4, false, true, 0x801F, 0x80));            // mesh, TM on
            ops.commands.push_back(HandCommand(0x00C0, 6, false, false, 0x00A5, 0x01));           // 8 bpp line
            ops.commands.push_back(HandCommand(0x03C0, 4, false, true, 0x801F, 0x24 | (1u << 8))); // DIE, doubleV
            b.cmdCount = static_cast<uint32>(ops.commands.size());
            for (int n = 0; n < 600; ++n) {
                const CoordS32 a{rng.Range(-60, 400), rng.Range(-60, 300)};
                const CoordS32 e{rng.Chance(80) ? a.x() + rng.Range(-120, 120) : rng.Range(-2000, 2000),
                                 rng.Chance(80) ? a.y() + rng.Range(-120, 120) : rng.Range(-2000, 2000)};
                AddSpan(ops, b, a, e, static_cast<uint32>(n), 0, static_cast<uint16>(rng.Next()),
                        static_cast<uint16>(rng.Next()), static_cast<uint32>(rng.Range(0, b.cmdCount - 1)));
            }
            ops.ops.push_back({gpu::Vdp1OpKind::Draw, 0});
            ops.batches.push_back(b);
        }
        {
            gpu::Vdp1DrawBatch b{};
            b.cmdOffset = static_cast<uint32>(ops.commands.size());
            b.spanOffset = static_cast<uint32>(ops.spans.size());
            b.drawFB = 1;
            // texture: 16 x 16 colour mode 5 at 0x4000 with transparent and end-code texels; a look-up table at 0x6000
            std::vector<uint8> tex = RGBTexture(16, 16, 3);
            for (uint32 i = 0; i < 256; ++i) {
                const uint32 r = static_cast<uint32>(rng.Range(0, 99));
                if (r < 10) {
                    tex[i * 2] = 0x00; // transparent (MSB clear)
                } else if (r < 14) {
                    tex[i * 2] = 0x7F;
                    tex[i * 2 + 1] = 0xFF; // end code
                }
            }
            std::vector<uint8> lut(32);
            for (auto &v : lut) {
                v = static_cast<uint8>(rng.Next() | 0x80);
            }
            std::vector<uint8> tex4(16 * 16 / 2);
            for (auto &v : tex4) {
                v = static_cast<uint8>(rng.Next());
            }
            ops.vramCopies.push_back({0, 0x4000, static_cast<uint32>(tex.size())});
            ops.arena.insert(ops.arena.end(), tex.begin(), tex.end());
            ops.vramCopies.push_back({static_cast<uint32>(ops.arena.size()), 0x6000, 32});
            ops.arena.insert(ops.arena.end(), lut.begin(), lut.end());
            ops.vramCopies.push_back({static_cast<uint32>(ops.arena.size()), 0x5000, static_cast<uint32>(tex4.size())});
            ops.arena.insert(ops.arena.end(), tex4.begin(), tex4.end());
            b.firstCopy = 0;
            b.copyCount = 3;
            auto textured = [&](uint16 pmod, uint16 ctrl, uint32 charAddr, uint32 colorBank, uint32 rasterFlags) {
                gpu::Vdp1CommandParams c = HandCommand(pmod, ctrl, true, true, 0, rasterFlags);
                c.charAddr = charAddr;
                c.colorBank = colorBank;
                c.charSize = gpu::Vdp1Pack16(16, 16);
                return c;
            };
            ops.commands.push_back(textured(0x00EC, 2, 0x4000, 0, 0));          // mode 5, Gouraud, half-lum
            ops.commands.push_back(textured(0x10E8, 0x12, 0x4000, 0, 0x10));    // HSS, flip H, EOS
            ops.commands.push_back(textured(0x0088, 2, 0x5000, 0x6000, 0));     // mode 1 (look-up table), end codes on
            ops.commands.push_back(textured(0x0048, 2, 0x4000, 0, 0x01));       // TPD, 8 bpp
            ops.commands.push_back(textured(0x0208, 2, 0x4000, 0, 0));          // clipping mode 1
            b.cmdCount = 5;
            for (int n = 0; n < 500; ++n) {
                const CoordS32 a{rng.Range(-20, 340), rng.Range(-20, 240)};
                const sint32 len = rng.Chance(30) ? rng.Range(0, 7) : rng.Range(8, 90);
                const CoordS32 e{a.x() + (rng.Chance(50) ? len : -len), a.y() + rng.Range(-20, 20)};
                const sint32 texV = rng.Chance(5) ? -1 : rng.Range(0, 15);
                AddSpan(ops, b, a, e, static_cast<uint32>(n % 4096), texV, static_cast<uint16>(rng.Next()),
                        static_cast<uint16>(rng.Next()), static_cast<uint32>(rng.Range(0, 4)));
            }
            ops.ops.push_back({gpu::Vdp1OpKind::Draw, 1});
            ops.batches.push_back(b);
        }
        CloseList(ops);

        auto initial = std::make_unique<ref::Fbram>();
        for (size_t i = 0; i < initial->main[0].size(); ++i) {
            initial->main[0][i] = static_cast<uint8>(i * 7);
            initial->main[1][i] = static_cast<uint8>(i * 13 + 1);
        }
        SceneRunner runner(g, *initial, true);
        CHECK(runner.Replay(ops) == "");
        CHECK(runner.stats.batches == 2);
        CHECK(runner.stats.fragments > 10000);
        // and the whole list in one go through the reference equals the op-by-op replay
        auto whole = std::make_unique<ref::Fbram>(*initial);
        ref::ReferenceVDP1 reference{*whole};
        reference.Replay(ops);
        CHECK(CompareFbram(*whole, runner.GpuFbram(), "reference", "GPU") == "");
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("VDP1 resolve pass: synthetic fragment sets of up to 50000 per word equal the reference", "[gpu][vdp1][passes]") {
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);
    {
        GpuVdp1 g(*device);
        REQUIRE(g.Ok());
        TestRng rng{777};
        auto randomTarget = [&](uint32 unit) {
            const uint32 kind = static_cast<uint32>(rng.Range(0, 99));
            const bool pixel8 = kind < 15;
            const bool msbOn = kind >= 15 && kind < 20;
            const bool tm = rng.Chance(40);
            const bool mesh = tm && rng.Chance(40);
            return gpu::Vdp1MakeFragmentTarget(unit & gpu::kWordIndexMask, (unit >> 17) != 0, rng.Chance(50),
                                               static_cast<uint32>(rng.Chance(30) ? 0 : rng.Range(1, 3)), mesh, msbOn,
                                               pixel8, tm);
        };
        // Units: per-word counts on both sides of each path boundary (32 / 33, 2048 / 2049) and well beyond (one
        // window and many, keys spread out or packed in runs that make the histogram narrow)
        struct UnitLoad {
            uint32 unit, count;
            bool packed;
        };
        const std::vector<UnitLoad> loads = {
            {5, 1, false},          {6, 32, false},        {7, 33, false},        {0x20000 | 7, 300, true},
            {1000, 2048, false},    {1001, 2049, true},    {1002, 5000, false},   {0x1FFFF, 8932, true},
            {0x20000 | 4096, 20000, false}, {77777, 50000, true}, {123, 1, false},
        };
        uint32 total = 0;
        for (const UnitLoad &l : loads) {
            total += l.count;
        }
        for (const uint32 drawFB : {0u, 1u}) {
            CAPTURE(drawFB);
            const uint32 slotsCount = total * 3 + 1000; // with dead slots between
            std::vector<gpu::Vdp1Fragment> slots(slotsCount, gpu::Vdp1Fragment{0, 0});
            std::vector<uint32> freeSlots(slotsCount);
            for (uint32 i = 0; i < slotsCount; ++i) {
                freeSlots[i] = i;
            }
            for (size_t i = freeSlots.size(); i > 1; --i) {
                std::swap(freeSlots[i - 1], freeSlots[rng.Next() % i]);
            }
            size_t nextFree = 0;
            uint32 packedBase = 0;
            for (const UnitLoad &l : loads) {
                for (uint32 k = 0; k < l.count; ++k) {
                    uint32 slot;
                    if (l.packed && k < l.count * 3 / 4) {
                        // a dense run of slots (an apex stack), the rest spread
                        do {
                            slot = (packedBase + k) % slotsCount;
                            ++packedBase;
                        } while (slots[slot].target != 0);
                    } else {
                        do {
                            slot = freeSlots[nextFree++ % freeSlots.size()];
                        } while (slots[slot].target != 0);
                    }
                    slots[slot] = {randomTarget(l.unit), static_cast<uint32>(rng.Next() & 0xFFFF)};
                }
                packedBase += static_cast<uint32>(rng.Range(1000, 20000));
            }
            auto initial = std::make_unique<ref::Fbram>();
            for (uint32 s = 0; s < 2; ++s) {
                for (size_t i = 0; i < initial->main[s].size(); ++i) {
                    initial->main[s][i] = static_cast<uint8>(rng.Next());
                    initial->alt[s][i] = static_cast<uint8>(rng.Next());
                    initial->mesh[0][s][i] = static_cast<uint8>(rng.Next());
                    initial->mesh[1][s][i] = static_cast<uint8>(rng.Next());
                }
            }
            g.Reset(*initial);
            REQUIRE(g.ResolveFragments(slots, drawFB));

            std::vector<ref::Fragment> fragments;
            for (uint32 i = 0; i < slotsCount; ++i) {
                if (gpu::Vdp1FragLive(slots[i].target)) {
                    fragments.push_back({i, slots[i].target & ~gpu::kFragLiveBit, slots[i].color});
                }
            }
            auto expected = std::make_unique<ref::Fbram>(*initial);
            const ref::ResolveStats rs = ref::Resolve(drawFB, fragments, *expected);
            CHECK(rs.maxPerUnit == 50000);
            auto got = std::make_unique<ref::Fbram>();
            g.GetFbram(*got);
            CHECK(CompareFbram(*expected, *got, "reference", "GPU") == "");
            CHECK(g.CountsZero());
            // ... and in descending order the result differs (the sets are order-sensitive)
            auto reversed = std::make_unique<ref::Fbram>(*initial);
            ref::Resolve(drawFB, fragments, *reversed, ref::ResolveOrder::DescendingForTesting);
            CHECK(CompareFbram(*reversed, *got, "descending", "GPU") != "");
        }
        UNSCOPED_INFO("resolve timings: " << g.Times().Format());
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("VDP1 passes: an apex stack of more than 8000 fragments on one word through the raster pass",
          "[gpu][vdp1][passes]") {
    // 9,000 spans (one half-transparent Gouraud command) all ending at the apex: one fragment per span on its word
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);
    {
        GpuVdp1 g(*device);
        REQUIRE(g.Ok());
        for (const uint32 rasterFlags : {0u, 0x01u, 0x80u}) { // 16 bpp, 8 bpp, transparent meshes
            CAPTURE(rasterFlags);
            const gpu::Vdp1OpList ops = ApexList(rasterFlags, 9000);
            SceneRunner runner(g, *ApexInitialFbram(), true);
            CHECK(runner.Replay(ops) == "");
            CHECK(runner.stats.maxPerUnit > 8000);
        }
        UNSCOPED_INFO("apex timings: " << g.Times().Format());
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("VDP1 passes: hand-made scenes through the front half (apex stacks; modes; erases; CPU writes)",
          "[gpu][vdp1][passes]") {
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);
    {
        GpuVdp1 g(*device);
        REQUIRE(g.Ok());
        SceneStats stats;

        // The reference test's apex variants: every colour calculation with Gouraud, mesh and MSB-on, transparent
        // meshes on and off, 8 bpp, deinterlaced double density (alt field), double interlace
        struct Variant {
            uint16 pmod, tvmr, tvmd, fbcr;
            bool deinterlace, transparentMeshes;
        };
        std::vector<Variant> variants;
        for (const uint16 calc : {uint16{0}, uint16{1}, uint16{2}, uint16{3}}) {
            for (const uint16 extra : {uint16{0x0004}, uint16{0x0104}, uint16{0x8000}}) {
                variants.push_back({static_cast<uint16>(0x00C0 | calc | extra), 0, 0x8000, 0, false, calc % 2 == 1});
            }
        }
        variants.push_back({0x00C7, 1, 0x8000, 0, false, false});
        variants.push_back({0x01C3, 1, 0x8000, 0, false, true});
        variants.push_back({0x80C0, 1, 0x8000, 0, false, true});
        variants.push_back({0x01C7, 0, 0x80C0, 0, true, true});
        variants.push_back({0x00C7, 0, 0x80C0, 0x0008, false, false});
        variants.push_back({0x00C7, 0, 0x8000, 0x0008, false, false});
        for (const Variant &v : variants) {
            for (const bool textured : {false, true}) {
                CAPTURE(v.pmod, v.tvmr, v.tvmd, v.fbcr, v.deinterlace, v.transparentMeshes, textured);
                Scenario s;
                s.tvmd = v.tvmd;
                s.tvmr = v.tvmr;
                s.fbcr = v.fbcr;
                s.deinterlace = v.deinterlace;
                s.transparentMeshes = v.transparentMeshes;
                s.vres = 448;
                s.clip.sysClipH = 352;
                s.clip.sysClipV = 255;
                s.fbFill = {0x80, 0x00, 0xC2, 0x10, 0x9F, 0xFF, 0x00, 0x01, 0xAB, 0xCD};
                const uint16 grda = s.AddGouraud({0x7FFF, 0x0000, 0x0000, 0x7C1F});
                const uint16 srca = s.AddTexture(RGBTexture(16, 64, 5));
                s.Op0(Op::Kind::BeginFrame);
                const uint16 pmod = textured ? static_cast<uint16>(v.pmod | 0x0028) : v.pmod;
                const CoordS32 apex{200, 120};
                if (textured) {
                    s.Distorted(0, pmod, 0, srca, 0x0240, {20, -150}, apex, apex, {30, 400}, grda);
                    s.Distorted(0x0010, pmod, 0, srca, 0x0240, {340, 380}, apex, apex, {320, -120}, grda);
                } else {
                    s.Polygon(pmod, 0xC631, {20, -150}, apex, apex, {30, 400}, grda);
                    s.Polygon(pmod, 0x83E0, {340, 380}, apex, apex, {320, -120}, grda);
                }
                s.Op0(Op::Kind::EndFrame);
                CHECK(RunScene(g, s, true, &stats) == "");
            }
        }
        CHECK(stats.maxPerUnit > 256);

        // Erases: one row, nothing, y1 > y3, the clamps, partial erases mid-row, 8 bpp and every offset shift, the alt
        // mirror and the mesh clear; the display framebuffer first gets main, alt and mesh content
        struct Region {
            uint16 x1, y1, x3, y3;
        };
        const Region regions[] = {{4, 10, 12, 10}, {4, 10, 4, 12}, {4, 13, 12, 10},
                                  {0, 0, 1016, 511}, {8, 100, 600, 300}, {296, 200, 1016, 511}};
        for (const Region &rg : regions) {
            for (const uint64 cycles : {uint64{0}, uint64{37}, uint64{5000}}) {
                for (const uint16 tvmr : {uint16{0}, uint16{1}, uint16{2}, uint16{3}}) {
                    for (const uint16 tvmd : {uint16{0x8000}, uint16{0x80C1}}) {
                        CAPTURE(rg.x1, rg.y1, rg.x3, rg.y3, cycles, tvmr, tvmd);
                        Scenario s;
                        s.tvmd = tvmd;
                        s.tvmr = tvmr;
                        s.deinterlace = true;
                        s.transparentMeshes = true;
                        s.vres = LSMD(tvmd) == 3 ? 480 : 240;
                        s.clip.sysClipH = 511;
                        s.clip.sysClipV = 511;
                        s.fbFill = {0x12, 0x34, 0x56, 0x78, 0x9A};
                        s.Op0(Op::Kind::BeginFrame);
                        s.Polygon(0x01C0, 0x8421, {0, 0}, {40, 0}, {40, 40}, {0, 40});
                        s.Polygon(0x01C0, 0x8421, {280, 8}, {440, 8}, {440, 30}, {280, 30});
                        s.Polygon(0x01C0, 0x8421, {380, 180}, {440, 180}, {440, 520}, {380, 520});
                        s.Polygon(0x00C0, 0xFC00, {100, 230}, {300, 230}, {300, 250}, {100, 250});
                        s.Op0(Op::Kind::EndFrame);
                        s.Op0(Op::Kind::Swap);
                        s.ops.push_back({.kind = Op::Kind::Erase,
                                         .value = 0xBEEF,
                                         .x1 = rg.x1,
                                         .y1 = rg.y1,
                                         .x3 = rg.x3,
                                         .y3 = rg.y3,
                                         .cycles = cycles});
                        CHECK(RunScene(g, s, false, &stats) == "");
                    }
                }
            }
        }
        CHECK(stats.erases > 0);

        // Swaps, CPU writes (mirrored to the alternate field) and erases between draws, a frame boundary in the middle
        {
            Scenario s;
            s.tvmd = 0x80C0;
            s.deinterlace = true;
            s.transparentMeshes = true;
            s.vres = 448;
            s.fbFill = {0x12, 0x34};
            s.Op0(Op::Kind::BeginFrame);
            s.Polygon(0x01C0, 0x83E0, {10, 10}, {60, 10}, {60, 60}, {10, 60});
            for (uint32 k = 0; k < 150; ++k) {
                // more than one 64-record chunk of writes, overlapping words and bytes
                const uint32 word = (20 + (k % 7)) * 512 + 20 + (k % 5);
                if (k % 3 == 0) {
                    s.ops.push_back({.kind = Op::Kind::WriteFB16, .address = word * 2, .value = static_cast<uint16>(k * 0x111)});
                } else {
                    s.ops.push_back({.kind = Op::Kind::WriteFB8,
                                     .address = word * 2 + (k & 1),
                                     .value = static_cast<uint16>(k * 7)});
                }
            }
            s.Polygon(0x00C3, 0x801F, {15, 15}, {50, 15}, {50, 50}, {15, 50});
            s.Op0(Op::Kind::Swap);
            s.Polygon(0x00C0, 0xFC00, {30, 30}, {90, 30}, {90, 90}, {30, 90});
            s.Op0(Op::Kind::FrameBoundary);
            s.ops.push_back({.kind = Op::Kind::Erase, .value = 0x8421, .x1 = 0, .y1 = 0, .x3 = 40, .y3 = 40});
            s.Polygon(0x01C0, 0x801F, {35, 35}, {85, 35}, {85, 85}, {35, 85});
            s.Op0(Op::Kind::EndFrame);
            CHECK(RunScene(g, s, true, &stats) == "");
        }
        CHECK(stats.fbWriteRuns > 0);
        UNSCOPED_INFO("scene timings: " << g.Times().Format());
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("VDP1 passes: CPU framebuffer write runs of overlapping records equal the reference", "[gpu][vdp1][passes]") {
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);
    {
        GpuVdp1 g(*device);
        REQUIRE(g.Ok());
        TestRng rng{31337};
        for (const uint32 count : {1u, 63u, 64u, 65u, 1000u, 20000u, 131072u}) {
            CAPTURE(count);
            gpu::Vdp1OpList ops;
            for (uint32 k = 0; k < count; ++k) {
                // few words (so records collide inside and across chunks), both framebuffers, mirrored or not
                const uint32 word = rng.Chance(70) ? static_cast<uint32>(rng.Range(0, 40)) : static_cast<uint32>(rng.Next() & 0x1FFFF);
                static constexpr uint32 kMasks[] = {0xFF00, 0x00FF, 0xFFFF};
                ops.fbWrites.push_back({gpu::Vdp1MakeFbWriteAddress(word, static_cast<uint32>(rng.Range(0, 1)), rng.Chance(40)),
                                        static_cast<uint32>(rng.Next() & 0xFFFF), rng.Pick(kMasks), 0});
            }
            // two runs: a prefix and the rest (non-zero offset)
            const uint32 split = count / 3;
            if (split != 0) {
                ops.fbWriteRuns.push_back({0, split});
                ops.ops.push_back({gpu::Vdp1OpKind::FbWrites, 0});
            }
            ops.fbWriteRuns.push_back({split, count - split});
            ops.ops.push_back({gpu::Vdp1OpKind::FbWrites, static_cast<uint32>(ops.fbWriteRuns.size() - 1)});
            CloseList(ops);
            auto initial = std::make_unique<ref::Fbram>();
            for (uint32 s = 0; s < 2; ++s) {
                for (size_t i = 0; i < initial->main[s].size(); ++i) {
                    initial->main[s][i] = static_cast<uint8>(rng.Next());
                    initial->alt[s][i] = static_cast<uint8>(rng.Next());
                }
            }
            SceneRunner runner(g, *initial, false);
            CHECK(runner.Replay(ops) == "");
        }
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("VDP1 passes: random command tables through the front half equal the reference", "[gpu][vdp1][passes]") {
    // The reference test's seeded tables (every command type and colour mode, draw modes, Gouraud, meshes with
    // transparent meshes on and off, MSB-on, 8 bpp, double interlace, end codes, HSS, clipping, erases, swaps, register
    // writes between commands, VRAM and CPU writes, batch limits), FBRAM against the reference and at the end against
    // software (BRIMIR_VDP1_GPU_SEEDS=400 for the longer run)
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);
    {
        GpuVdp1 g(*device);
        REQUIRE(g.Ok());
        const uint64 seeds = SeedCount(*device, 40, 10);
        const auto t0 = std::chrono::steady_clock::now();
        SceneStats stats;
        uint64 mismatches = 0;
        Coverage cov;
        for (uint64 seed = 1; seed <= seeds; ++seed) {
            const Scenario s = RandomScenario(seed, cov);
            // every other table: each batch's raster and scatter output and FBRAM after every op; the others FBRAM at
            // the end of each op list
            const std::string d = RunScene(g, s, seed % 2 == 1, &stats);
            if (!d.empty()) {
                if (mismatches++ < 5) {
                    UNSCOPED_INFO("seed " << seed << ": " << d);
                }
            }
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        UNSCOPED_INFO("seeds " << seeds << " in " << seconds << " s: ops " << stats.ops << ", batches " << stats.batches
                               << ", fragment slots " << stats.fragments << ", erases " << stats.erases
                               << ", write runs " << stats.fbWriteRuns << ", max per word " << stats.maxPerUnit);
        UNSCOPED_INFO("timings: " << g.Times().Format());
        CHECK(mismatches == 0);
        CHECK(stats.batches > seeds);
        CHECK(stats.erases > 0);
        CHECK(stats.fbWriteRuns > 0);
        if (std::getenv("BRIMIR_VDP1_GPU_SEEDS") != nullptr) { // an explicit (long) run reports its totals
            std::printf("VDP1 GPU random tables (%s): %llu seeds, %.1f s, %llu batches, %llu slots, max %u per word; %s\n",
                    device->Properties().deviceName, static_cast<unsigned long long>(seeds), seconds,
                    static_cast<unsigned long long>(stats.batches), static_cast<unsigned long long>(stats.fragments),
                    stats.maxPerUnit, g.Times().Format().c_str());
        }
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("VDP1 passes: timings of a polygon-heavy batch and an apex stack", "[gpu][vdp1][passes][timing][.]") {
    // About a million fragment slots in one batch (128 overlapping Gouraud, textured and half-transparent quads, as
    // many as the scenario's command area holds, over a 320 x 224 screen), checked against the reference, with the
    // GPU time per pass printed
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);
    {
        GpuVdp1 g(*device, GpuVdp1::Memory::DeviceLocal);
        REQUIRE(g.Ok());
        Scenario s;
        s.fbFill = {0x80, 0x10, 0x9C, 0xE7};
        TestRng rng{2026};
        const uint16 srca = s.AddTexture(RGBTexture(32, 32, 1));
        s.Op0(Op::Kind::BeginFrame);
        for (int n = 0; n < 800 && s.CanAddCommand(); ++n) {
            const uint16 grda = s.AddGouraud({static_cast<uint16>(rng.Next()), static_cast<uint16>(rng.Next()),
                                              static_cast<uint16>(rng.Next()), static_cast<uint16>(rng.Next())});
            const CoordS32 c{rng.Range(0, 320), rng.Range(0, 224)};
            const sint32 r = rng.Range(20, 70);
            const CoordS32 a{c.x() - r, c.y() - rng.Range(3, r)}, b{c.x() + rng.Range(3, r), c.y() - r};
            const CoordS32 cc{c.x() + r, c.y() + rng.Range(3, r)}, d{c.x() - rng.Range(3, r), c.y() + r};
            static constexpr uint16 kCalc[] = {0, 0, 3, 3, 1, 2};
            const uint16 pmod = static_cast<uint16>(0x00C4 | rng.Pick(kCalc));
            if (rng.Chance(40)) {
                s.Distorted(0, static_cast<uint16>(pmod | 0x0028), 0, srca, 0x0420, a, b, cc, d, grda);
            } else {
                s.Polygon(pmod, static_cast<uint16>(rng.Next() | 0x8000), a, b, cc, d, grda);
            }
        }
        s.Op0(Op::Kind::EndFrame);
        g.Times().Clear();
        SceneStats stats;
        CHECK(RunScene(g, s, false, &stats) == "");
        const Timings heavy = g.Times();
        std::printf("VDP1 GPU heavy batch (%s, %s memory): %llu batches, %llu slots; %s; total %.3f ms\n",
                    device->Properties().deviceName, g.DeviceLocal() ? "device-local" : "host",
                    static_cast<unsigned long long>(stats.batches), static_cast<unsigned long long>(stats.fragments),
                    heavy.Format().c_str(), heavy.Total());
        CHECK(stats.fragments > 500000);

        // 9,000 half-transparent fragments on one word (the large-unit path: windows of 2,048 keys)
        g.Times().Clear();
        SceneRunner runner(g, *ApexInitialFbram(), false);
        CHECK(runner.Replay(ApexList(0, 9000)) == "");
        const Timings apex = g.Times();
        std::printf("VDP1 GPU apex stack (%s): 9000 spans; %s; total %.3f ms\n", device->Properties().deviceName,
                    apex.Format().c_str(), apex.Total());

        // A batch's fixed cost: no fragment (a transparent textured line), one short line, and two fragments at the
        // two ends of the unit space (the cost of covering all 262,144 units, as before the unit range)
        {
            auto timeList = [&](const char *name, const gpu::Vdp1OpList &ops) {
                SceneRunner small(g, *ApexInitialFbram(), false);
                small.Replay(ops); // warm up
                g.Times().Clear();
                for (int rep = 0; rep < 20; ++rep) {
                    CHECK(small.Replay(ops) == "");
                }
                Timings t = g.Times();
                for (uint32 p = 0; p < kPassCount; ++p) {
                    t.ms[p] /= 20;
                    t.dispatches[p] /= 20;
                }
                std::printf("VDP1 GPU %s (%s): per batch %s; total %.4f ms\n", name, device->Properties().deviceName,
                            t.Format().c_str(), t.Total());
            };
            gpu::Vdp1OpList empty;
            {
                gpu::Vdp1DrawBatch b{};
                gpu::Vdp1CommandParams c = HandCommand(0x0028, 2, true, false, 0, 0); // colour mode 5, VRAM zero
                c.charAddr = 0x70000;
                c.charSize = gpu::Vdp1Pack16(16, 16);
                empty.commands.push_back(c);
                b.cmdCount = 1;
                AddSpan(empty, b, {100, 100}, {110, 100}, 0, 0, 0, 0);
                empty.ops.push_back({gpu::Vdp1OpKind::Draw, 0});
                empty.batches.push_back(b);
                CloseList(empty);
            }
            gpu::Vdp1OpList line;
            {
                gpu::Vdp1DrawBatch b{};
                line.commands.push_back(HandCommand(0x00C0, 4, false, false, 0x83E0, 0));
                b.cmdCount = 1;
                AddSpan(line, b, {100, 100}, {110, 100}, 0, 0, 0, 0);
                line.ops.push_back({gpu::Vdp1OpKind::Draw, 0});
                line.batches.push_back(b);
                CloseList(line);
            }
            gpu::Vdp1OpList ends;
            {
                gpu::Vdp1DrawBatch b{};
                // deinterlaced double density: (0, 0) is unit 0 of field 0; (511, 511) unit 0x1FFFF of field 1
                gpu::Vdp1CommandParams c = HandCommand(0x00C0, 4, false, false, 0x83E0, 0x60);
                c.sysClip = gpu::Vdp1Pack16(1023, 1023);
                ends.commands.push_back(c);
                b.cmdCount = 1;
                AddSpan(ends, b, {0, 0}, {0, 0}, 0, 0, 0, 0);
                AddSpan(ends, b, {511, 511}, {511, 511}, 1, 0, 0, 0);
                ends.ops.push_back({gpu::Vdp1OpKind::Draw, 0});
                ends.batches.push_back(b);
                CloseList(ends);
            }
            timeList("batch without fragments", empty);
            timeList("batch of one 11-pixel line", line);
            timeList("batch covering all units", ends);
        }

        // CPU framebuffer write runs: 71,680 words in sequence (a 320 x 224 framebuffer) and 131,072 byte writes
        for (const uint32 count : {71680u, 131072u}) {
            gpu::Vdp1OpList ops;
            for (uint32 k = 0; k < count; ++k) {
                const bool bytes = count == 131072u;
                ops.fbWrites.push_back({gpu::Vdp1MakeFbWriteAddress(bytes ? k / 2 : k, 0, false), k & 0xFFFF,
                                        bytes ? ((k & 1) != 0 ? 0x00FFu : 0xFF00u) : 0xFFFFu, 0});
            }
            ops.fbWriteRuns.push_back({0, count});
            ops.ops.push_back({gpu::Vdp1OpKind::FbWrites, 0});
            CloseList(ops);
            SceneRunner runner(g, *ApexInitialFbram(), false);
            g.Times().Clear();
            CHECK(runner.Replay(ops) == "");
            std::printf("VDP1 GPU CPU writes, %u records (%s): %s; total %.3f ms\n", count,
                        device->Properties().deviceName, g.Times().Format().c_str(), g.Times().Total());
        }

        // One word alone with 9,000 and 50,000 fragments (keys spread over 3 M slots), the bucket to resolve passes
        for (const uint32 perWord : {9000u, 50000u}) {
            std::vector<gpu::Vdp1Fragment> slots(3'000'000, gpu::Vdp1Fragment{0, 0});
            TestRng srng{perWord};
            for (uint32 k = 0; k < perWord;) {
                const uint32 slot = static_cast<uint32>(srng.Next() % slots.size());
                if (slots[slot].target == 0) {
                    slots[slot] = {gpu::Vdp1MakeFragmentTarget(1000, false, false, 3, false, false, false, false),
                                   static_cast<uint32>(srng.Next() & 0xFFFF)};
                    ++k;
                }
            }
            g.Times().Clear();
            REQUIRE(g.ResolveFragments(slots, 0));
            std::printf("VDP1 GPU one word, %u fragments (%s): %s\n", perWord, device->Properties().deviceName,
                        g.Times().Format().c_str());
        }
    }
    CloseAndCheckValidation(device);
}
