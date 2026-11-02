// VDP1 compute shaders on the Vulkan 1.2 baseline (src/gpu/shaders/vdp1/cs_vdp1_*.hlsl, generated headers in
// src/gpu/shaders/generated/vdp1_*.spv.h, bindings and dispatch shapes in src/gpu/vdp1/vdp1_bindings.hpp).
// - The SPIR-V words: only the Shader capability (no Int64), no image types (VDP1 has storage buffers only), the
//   LocalSize the dispatches assume (64 x 1 x 1, the erase pass 8 x 8 x 1; at most kWorkgroupSize invocations), exactly
//   the bindings of the pass's table, all storage buffers in set 0 holding only uint, at most one push-constant block of
//   the pass's struct, and the groupshared bytes vdp1_bindings.hpp states. The check rejects modules altered to break
//   the rules. These run without a Vulkan device ([vdp1]).
// - The device limits check covers the VDP1 maxima ([vdp1][limits], BRIMIR_VULKAN builds).
// - The ten native and seven N x (GPU stage 3) pipelines are created on the headless device from their binding tables and push-constant ranges, with
//   zero validation errors ([gpu][vdp1]; SKIP without a device).
// The passes' results against the CPU reference are in test_gpu_vdp1_passes.cpp.
#include "catch_amalgamated.hpp"

#include <gpu/shaders/generated/vdp1_batch_range.spv.h>
#include <gpu/shaders/generated/vdp1_bucket.spv.h>
#include <gpu/shaders/generated/vdp1_bucket_sums.spv.h>
#include <gpu/shaders/generated/vdp1_bucket_sums_nx.layout.h>
#include <gpu/shaders/generated/vdp1_bucket_sums_nx.spv.h>
#include <gpu/shaders/generated/vdp1_bucket_sums_nx_add.spv.h>
#include <gpu/shaders/generated/vdp1_erase.layout.h>
#include <gpu/shaders/generated/vdp1_erase.spv.h>
#include <gpu/shaders/generated/vdp1_erase_nx.layout.h>
#include <gpu/shaders/generated/vdp1_erase_nx.spv.h>
#include <gpu/shaders/generated/vdp1_fb_write.layout.h>
#include <gpu/shaders/generated/vdp1_fb_write.spv.h>
#include <gpu/shaders/generated/vdp1_fb_write_clear.spv.h>
#include <gpu/shaders/generated/vdp1_fb_write_mark.spv.h>
#include <gpu/shaders/generated/vdp1_fb_write_nx.layout.h>
#include <gpu/shaders/generated/vdp1_fb_write_nx.spv.h>
#include <gpu/shaders/generated/vdp1_filter_nx.spv.h>
#include <gpu/shaders/generated/vdp1_raster.layout.h>
#include <gpu/shaders/generated/vdp1_raster.spv.h>
#include <gpu/shaders/generated/vdp1_raster_nx.layout.h>
#include <gpu/shaders/generated/vdp1_raster_nx.spv.h>
#include <gpu/shaders/generated/vdp1_rebuild_nx.layout.h>
#include <gpu/shaders/generated/vdp1_rebuild_nx.spv.h>
#include <gpu/shaders/generated/vdp1_resolve.layout.h>
#include <gpu/shaders/generated/vdp1_resolve.spv.h>
#include <gpu/shaders/generated/vdp1_resolve_fast_nx.layout.h>
#include <gpu/shaders/generated/vdp1_resolve_fast_nx.spv.h>
#include <gpu/shaders/generated/vdp1_resolve_nx.layout.h>
#include <gpu/shaders/generated/vdp1_resolve_nx.spv.h>
#include <gpu/shaders/generated/vdp1_scatter.layout.h>
#include <gpu/shaders/generated/vdp1_scatter.spv.h>
#include <gpu/vdp1/vdp1_bindings.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#if BRIMIR_VULKAN
    #include "gpu_test_device.hpp"

    #include <gpu/gpu_version.hpp>
    #include <gpu/vk_util.hpp>
#endif

using namespace brimir::gpu::vdp1;
namespace sh = brimir::gpu::shaders;

namespace {

struct ShaderCase {
    const char *name;
    std::span<const uint32_t> spirv;
    std::span<const Binding> bindings;
    uint32_t pushConstantSize; // 0: no push-constant block
    uint32_t localX, localY;
    uint32_t sharedBytes;
};

const std::vector<ShaderCase> &Shaders() {
    static const std::vector<ShaderCase> shaders{
        {"vdp1_raster", sh::kVdp1RasterSpv, kRasterBindings, sizeof(Vdp1BatchPushConstants), 64, 1,
         kRasterSharedMemorySize},
        {"vdp1_batch_range", sh::kVdp1BatchRangeSpv, kBatchRangeBindings, 0, 64, 1, 0},
        {"vdp1_bucket", sh::kVdp1BucketSpv, kBucketBindings, 0, 64, 1, kScanSharedMemorySize},
        {"vdp1_bucket_sums", sh::kVdp1BucketSumsSpv, kBucketSumsBindings, 0, 64, 1, kScanSharedMemorySize},
        {"vdp1_scatter", sh::kVdp1ScatterSpv, kScatterBindings, sizeof(Vdp1BatchPushConstants), 64, 1, 0},
        {"vdp1_resolve", sh::kVdp1ResolveSpv, kResolveBindings, sizeof(Vdp1BatchPushConstants), 64, 1,
         kResolveSharedMemorySize},
        {"vdp1_erase", sh::kVdp1EraseSpv, kEraseBindings, sizeof(Vdp1ErasePushConstants), kEraseGroupSize,
         kEraseGroupSize, 0},
        {"vdp1_fb_write_mark", sh::kVdp1FbWriteMarkSpv, kFbWriteMarkBindings, sizeof(Vdp1FbWritePushConstants), 64, 1,
         0},
        {"vdp1_fb_write", sh::kVdp1FbWriteSpv, kFbWriteBindings, sizeof(Vdp1FbWritePushConstants), 64, 1, 0},
        {"vdp1_fb_write_clear", sh::kVdp1FbWriteClearSpv, kFbWriteClearBindings, sizeof(Vdp1FbWritePushConstants), 64,
         1, 0},
        // N x passes (GPU stage 3), in kNxPipelineBindings order
        {"vdp1_raster_nx", sh::kVdp1RasterNxSpv, kRasterNxBindings, sizeof(Vdp1BatchPushConstants), 64, 1,
         kRasterSharedMemorySize},
        {"vdp1_bucket_sums_nx", sh::kVdp1BucketSumsNxSpv, kBucketSumsNxBindings, sizeof(Vdp1BatchPushConstants), 64, 1,
         kScanSharedMemorySize},
        {"vdp1_bucket_sums_nx_add", sh::kVdp1BucketSumsNxAddSpv, kBucketSumsNxBindings, sizeof(Vdp1BatchPushConstants),
         64, 1, 0},
        {"vdp1_resolve_nx", sh::kVdp1ResolveNxSpv, kResolveNxBindings, sizeof(Vdp1BatchPushConstants), 64, 1,
         kResolveSharedMemorySize},
        {"vdp1_erase_nx", sh::kVdp1EraseNxSpv, kEraseNxBindings, sizeof(Vdp1EraseNxPushConstants), kEraseGroupSize,
         kEraseGroupSize, 0},
        {"vdp1_fb_write_nx", sh::kVdp1FbWriteNxSpv, kFbWriteNxBindings, sizeof(Vdp1FbWriteNxPushConstants), 64, 1, 0},
        {"vdp1_rebuild_nx", sh::kVdp1RebuildNxSpv, kRebuildNxBindings, sizeof(Vdp1RebuildNxPushConstants), 64, 1, 0},
        {"vdp1_resolve_fast_nx", sh::kVdp1ResolveFastNxSpv, kResolveFastNxBindings, sizeof(Vdp1BatchPushConstants), 64,
         1, 0},
        {"vdp1_filter_nx", sh::kVdp1FilterNxSpv, kFilterNxBindings, sizeof(Vdp1BatchPushConstants), 64, 1, 0},
    };
    return shaders;
}

// The native passes come first, in kPipelineBindings order, then the N x passes in kNxPipelineBindings order
constexpr size_t kNativeShaderCount = kPipelineBindings.size();

// The case of the shader with the given name (the cases are looked up by name, not position)
const ShaderCase &ShaderNamed(const char *name) {
    for (const ShaderCase &shader : Shaders()) {
        if (std::string(shader.name) == name) {
            return shader;
        }
    }
    FAIL("no shader case named " << name);
    return Shaders().front();
}

// SPIR-V opcodes and enumerants used below (SPIR-V 1.5 specification)
constexpr uint32_t kSpvMagic = 0x07230203;
constexpr uint32_t kOpExecutionMode = 16;
constexpr uint32_t kOpCapability = 17;
constexpr uint32_t kOpTypeBool = 20;
constexpr uint32_t kOpTypeInt = 21;
constexpr uint32_t kOpTypeFloat = 22;
constexpr uint32_t kOpTypeVector = 23;
constexpr uint32_t kOpTypeImage = 25;
constexpr uint32_t kOpTypeArray = 28;
constexpr uint32_t kOpTypeRuntimeArray = 29;
constexpr uint32_t kOpTypeStruct = 30;
constexpr uint32_t kOpTypePointer = 32;
constexpr uint32_t kOpConstant = 43;
constexpr uint32_t kOpVariable = 59;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kExecutionModeLocalSize = 17;
constexpr uint32_t kCapabilityShader = 1;
constexpr uint32_t kCapabilityInt64 = 11;
constexpr uint32_t kDecorationBinding = 33;
constexpr uint32_t kDecorationDescriptorSet = 34;
constexpr uint32_t kStorageWorkgroup = 4;
constexpr uint32_t kStoragePushConstant = 9;
constexpr uint32_t kStorageStorageBuffer = 12;

struct SpvModule {
    std::vector<uint32_t> capabilities;
    std::map<uint32_t, std::vector<uint32_t>> types;            // id -> the instruction's words from the opcode
    std::map<uint32_t, uint32_t> constants;                      // id -> first literal word
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> pointers;  // id -> (storage class, pointee)
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> variables; // id -> (pointer type, storage class)
    std::map<uint32_t, uint32_t> sets, bindings;
    std::vector<uint32_t> localSize;
    bool hasImages = false;
    bool valid = false;
};

SpvModule ParseSpirv(std::span<const uint32_t> words) {
    SpvModule m;
    if (words.size() < 5 || words[0] != kSpvMagic) {
        return m;
    }
    size_t i = 5;
    while (i < words.size()) {
        const uint32_t count = words[i] >> 16;
        const uint32_t op = words[i] & 0xFFFF;
        if (count == 0 || i + count > words.size()) {
            return m;
        }
        const uint32_t *w = &words[i];
        switch (op) {
        case kOpCapability: m.capabilities.push_back(w[1]); break;
        case kOpTypeImage: m.hasImages = true; break;
        case kOpTypeBool:
        case kOpTypeInt:
        case kOpTypeFloat:
        case kOpTypeVector:
        case kOpTypeArray:
        case kOpTypeRuntimeArray:
        case kOpTypeStruct: m.types[w[1]] = std::vector<uint32_t>(w, w + count); break;
        case kOpConstant:
            if (count >= 4) {
                m.constants[w[2]] = w[3];
            }
            break;
        case kOpTypePointer: m.pointers[w[1]] = {w[2], w[3]}; break;
        case kOpVariable: m.variables[w[2]] = {w[1], w[3]}; break;
        case kOpExecutionMode:
            if (count >= 6 && w[2] == kExecutionModeLocalSize) {
                m.localSize = {w[3], w[4], w[5]};
            }
            break;
        case kOpDecorate:
            if (count >= 4 && w[2] == kDecorationBinding) {
                m.bindings[w[1]] = w[3];
            } else if (count >= 4 && w[2] == kDecorationDescriptorSet) {
                m.sets[w[1]] = w[3];
            }
            break;
        default: break;
        }
        i += count;
    }
    m.valid = i == words.size();
    return m;
}

// uint32, or arrays and structs of them
bool IsUintOnly(const SpvModule &m, uint32_t type) {
    const auto it = m.types.find(type);
    if (it == m.types.end()) {
        return false;
    }
    const std::vector<uint32_t> &w = it->second;
    switch (w[0] & 0xFFFF) {
    case kOpTypeInt: return w[2] == 32 && w[3] == 0;
    case kOpTypeArray:
    case kOpTypeRuntimeArray: return IsUintOnly(m, w[2]);
    case kOpTypeStruct:
        return std::all_of(w.begin() + 2, w.end(), [&](uint32_t member) { return IsUintOnly(m, member); });
    default: return false;
    }
}

// Bytes of the Workgroup variables (tightly packed scalars, vectors, arrays and structs); UINT32_MAX if not understood
uint32_t WorkgroupBytes(const SpvModule &m) {
    std::function<uint64_t(uint32_t)> size = [&](uint32_t type) -> uint64_t {
        const auto t = m.types.find(type);
        if (t == m.types.end()) {
            return UINT32_MAX;
        }
        const std::vector<uint32_t> &w = t->second;
        switch (w[0] & 0xFFFF) {
        case kOpTypeBool: return 4;
        case kOpTypeInt:
        case kOpTypeFloat: return w[2] / 8;
        case kOpTypeVector: return size(w[2]) * w[3];
        case kOpTypeArray: {
            const auto length = m.constants.find(w[3]);
            return length == m.constants.end() ? UINT32_MAX : size(w[2]) * length->second;
        }
        case kOpTypeStruct: {
            uint64_t sum = 0;
            for (size_t k = 2; k < w.size(); ++k) {
                sum += size(w[k]);
            }
            return sum;
        }
        }
        return UINT32_MAX;
    };
    uint64_t total = 0;
    for (const auto &[id, var] : m.variables) {
        if (var.second != kStorageWorkgroup) {
            continue;
        }
        const auto p = m.pointers.find(var.first);
        total += p == m.pointers.end() ? UINT32_MAX : size(p->second.second);
    }
    return static_cast<uint32_t>(std::min<uint64_t>(total, UINT32_MAX));
}

// Every way a module departs from the VDP1 rules or its case, one message each (empty: conforms)
std::vector<std::string> CheckModule(std::span<const uint32_t> words, const ShaderCase &c) {
    std::vector<std::string> errors;
    const auto fail = [&](std::string text) { errors.push_back(std::move(text)); };
    const SpvModule m = ParseSpirv(words);
    if (!m.valid) {
        fail("malformed SPIR-V");
        return errors;
    }
    if (m.capabilities != std::vector<uint32_t>{kCapabilityShader}) {
        for (uint32_t cap : m.capabilities) {
            if (cap != kCapabilityShader) {
                fail(cap == kCapabilityInt64 ? "Int64 capability" : "capability " + std::to_string(cap));
            }
        }
        if (std::find(m.capabilities.begin(), m.capabilities.end(), kCapabilityShader) == m.capabilities.end()) {
            fail("no Shader capability");
        }
    }
    if (m.hasImages) {
        fail("an image type (VDP1 has no images)");
    }
    if (m.localSize.size() != 3) {
        fail("no LocalSize");
    } else if (m.localSize[0] != c.localX || m.localSize[1] != c.localY || m.localSize[2] != 1 ||
               m.localSize[0] * m.localSize[1] * m.localSize[2] > kWorkgroupSize) {
        fail("LocalSize " + std::to_string(m.localSize[0]) + "x" + std::to_string(m.localSize[1]) + "x" +
             std::to_string(m.localSize[2]));
    }

    std::set<uint32_t> declared;
    uint32_t pushConstants = 0;
    for (const auto &[id, var] : m.variables) {
        const auto pointer = m.pointers.find(var.first);
        if (pointer == m.pointers.end()) {
            continue;
        }
        if (var.second == kStoragePushConstant) {
            ++pushConstants;
            if (!IsUintOnly(m, pointer->second.second)) {
                fail("push constants hold something other than uint");
            }
        } else if (var.second == kStorageStorageBuffer) {
            const auto set = m.sets.find(id);
            const auto binding = m.bindings.find(id);
            if (set == m.sets.end() || set->second != 0 || binding == m.bindings.end()) {
                fail("storage buffer outside set 0 or without a binding");
                continue;
            }
            if (!declared.insert(binding->second).second) {
                fail("binding " + std::to_string(binding->second) + " declared twice");
            }
            if (!IsUintOnly(m, pointer->second.second)) {
                fail("storage buffer binding " + std::to_string(binding->second) + " holds something other than uint");
            }
        } else if (m.bindings.count(id) != 0) {
            fail("resource with storage class " + std::to_string(var.second));
        }
    }
    if (pushConstants != (c.pushConstantSize != 0 ? 1u : 0u)) {
        fail(std::to_string(pushConstants) + " push-constant blocks");
    }
    std::set<uint32_t> expected;
    for (const Binding &b : c.bindings) {
        expected.insert(b.binding);
        if (b.kind != DescriptorKind::StorageBuffer) {
            fail("the table has a non-buffer binding");
        }
    }
    if (declared != expected) {
        fail("bindings differ from the table");
    }
    const uint32_t shared = WorkgroupBytes(m);
    if (shared != c.sharedBytes) {
        fail("groupshared " + std::to_string(shared) + " bytes, expected " + std::to_string(c.sharedBytes));
    }
    return errors;
}

} // namespace

TEST_CASE("VDP1 shader modules use only the Vulkan 1.2 baseline and their binding tables", "[vdp1][pipelines]") {
    for (const ShaderCase &shader : Shaders()) {
        INFO("shader: " << shader.name);
        for (const std::string &error : CheckModule(shader.spirv, shader)) {
            FAIL_CHECK(error);
        }
    }
}

TEST_CASE("VDP1 push-constant blocks are the host structs", "[vdp1][pipelines]") {
    STATIC_CHECK(sh::kVdp1Raster_BatchParams_size == sizeof(Vdp1BatchPushConstants));
    STATIC_CHECK(sh::kVdp1Raster_BatchParams_scale == offsetof(Vdp1BatchPushConstants, scale));
    STATIC_CHECK(sh::kVdp1Scatter_BatchParams_size == sizeof(Vdp1BatchPushConstants));
    STATIC_CHECK(sh::kVdp1Scatter_BatchParams_fragmentCount == offsetof(Vdp1BatchPushConstants, fragmentCount));
    STATIC_CHECK(sh::kVdp1Resolve_BatchParams_size == sizeof(Vdp1BatchPushConstants));
    STATIC_CHECK(sh::kVdp1Resolve_BatchParams_drawFB == offsetof(Vdp1BatchPushConstants, drawFB));
    STATIC_CHECK(sh::kVdp1Erase_EraseParams_size == sizeof(Vdp1ErasePushConstants));
    STATIC_CHECK(sh::kVdp1Erase_EraseParams_flags == offsetof(Vdp1ErasePushConstants, flags));
    STATIC_CHECK(sh::kVdp1FbWrite_FbWriteParams_size == sizeof(Vdp1FbWritePushConstants));
    STATIC_CHECK(sh::kVdp1FbWrite_FbWrite_size == sizeof(Vdp1FbWrite));
    STATIC_CHECK(sh::kVdp1Raster_Span_size == sizeof(Vdp1Span));
    STATIC_CHECK(sh::kVdp1Raster_CommandParams_size == sizeof(Vdp1CommandParams));
    STATIC_CHECK(sh::kVdp1Raster_Fragment_size == sizeof(Vdp1Fragment));
    // N x passes
    STATIC_CHECK(sh::kVdp1RasterNx_BatchParams_size == sizeof(Vdp1BatchPushConstants));
    STATIC_CHECK(sh::kVdp1RasterNx_BatchParams_scale == offsetof(Vdp1BatchPushConstants, scale));
    STATIC_CHECK(sh::kVdp1RasterNx_NxSpan_size == sizeof(Vdp1NxSpan));
    STATIC_CHECK(sh::kVdp1RasterNx_NxSpan_flags == offsetof(Vdp1NxSpan, flags));
    STATIC_CHECK(sh::kVdp1RasterNx_NxSpan_slotBase == offsetof(Vdp1NxSpan, slotBase));
    STATIC_CHECK(sh::kVdp1BucketSumsNx_BatchParams_scale == offsetof(Vdp1BatchPushConstants, scale));
    STATIC_CHECK(sh::kVdp1ResolveNx_BatchParams_scale == offsetof(Vdp1BatchPushConstants, scale));
    STATIC_CHECK(sh::kVdp1ResolveNx_BatchParams_drawFB == offsetof(Vdp1BatchPushConstants, drawFB));
    STATIC_CHECK(sh::kVdp1ResolveFastNx_BatchParams_drawFB == offsetof(Vdp1BatchPushConstants, drawFB));
    STATIC_CHECK(sh::kVdp1ResolveFastNx_BatchParams_scale == offsetof(Vdp1BatchPushConstants, scale));
    STATIC_CHECK(sh::kVdp1RasterNx_BatchParams_drawFB == offsetof(Vdp1BatchPushConstants, drawFB));
    STATIC_CHECK(sh::kVdp1EraseNx_EraseNxParams_size == sizeof(Vdp1EraseNxPushConstants));
    STATIC_CHECK(sh::kVdp1EraseNx_EraseNxParams_scale == offsetof(Vdp1EraseNxPushConstants, scale));
    STATIC_CHECK(sh::kVdp1FbWriteNx_FbWriteNxParams_size == sizeof(Vdp1FbWriteNxPushConstants));
    STATIC_CHECK(sh::kVdp1FbWriteNx_FbWriteNxParams_scale == offsetof(Vdp1FbWriteNxPushConstants, scale));
    STATIC_CHECK(sh::kVdp1RebuildNx_RebuildNxParams_size == sizeof(Vdp1RebuildNxPushConstants));
    STATIC_CHECK(sh::kVdp1RebuildNx_RebuildNxParams_scale == offsetof(Vdp1RebuildNxPushConstants, scale));
}

TEST_CASE("VDP1 dispatch shapes and groupshared sizes stay within the baseline", "[vdp1][pipelines]") {
    // Group counts within the guaranteed maxComputeWorkGroupCount (65535) for full batches
    STATIC_CHECK(kMaxSpansPerBatch / kWorkgroupSize == 4096);
    STATIC_CHECK(kMaxFragmentsPerBatch / kScatterSlotsPerGroup == 16384);
    STATIC_CHECK(kResolveGroupCount == 4096);
    STATIC_CHECK(kScanBlockCount == 4096);
    // The erase region is at most 428 x 512 words: 54 x 64 groups
    STATIC_CHECK((428 + kEraseGroupSize - 1) / kEraseGroupSize <= 65535);
    // Groupshared memory: the resolve pass's window, histogram and chunk; all within the guaranteed 16384 and the
    // VDP2 NBG fetch pass's share, so the device limit stays VDP2's
    STATIC_CHECK(kResolveSharedMemorySize == 11536);
    STATIC_CHECK(kScanSharedMemorySize == 256);
    STATIC_CHECK(kRasterSharedMemorySize == 8);
    STATIC_CHECK(kMaxComputeSharedMemorySize == kResolveSharedMemorySize);
    STATIC_CHECK(kMaxComputeSharedMemorySize <= brimir::gpu::vdp2::kMaxComputeSharedMemorySize);
    // Per-stage descriptors: the resolve pass's seven storage buffers, VDP2's seven
    STATIC_CHECK(kMaxStageUsage.storageBuffers == 7);
    STATIC_CHECK(kMaxStageUsage.storageBuffers <= brimir::gpu::vdp2::kMaxStageUsage.storageBuffers);
    STATIC_CHECK(kResolveBindings.size() == 7);
    // The resolve's small path and window
    STATIC_CHECK(kResolveSmallMax == 32);
    STATIC_CHECK(kResolveSortCapacity == 2048);
    STATIC_CHECK(kResolveHistogramBins == 256);
    STATIC_CHECK(kResolveFoldChunk == 256);
    // N x: the bucket groups (capped at 4096) take up to 16 blocks of a 4x range, resolve_nx up to 16 rounds, the two
    // bucket_sums_nx levels N^2 groups, rebuild_nx 16384 x 8 groups at 4x
    STATIC_CHECK(kMaxNxUnitCount == 16 * kUnitCount);
    STATIC_CHECK(Vdp1NxScanBlockCount(kMaxNxScale) / kScanBlockCount == 16);
    STATIC_CHECK(kMaxNxUnitCount / (kWorkgroupSize * kResolveGroupCount) == 16);
    STATIC_CHECK(Vdp1RebuildNxGroups(4) == 16384);
    STATIC_CHECK(kMaxPushConstantSize == sizeof(Vdp1EraseNxPushConstants));
}

TEST_CASE("The VDP1 SPIR-V check rejects modules that depart from the rules", "[vdp1][pipelines]") {
    const ShaderCase &resolve = ShaderNamed("vdp1_resolve");
    const std::vector<uint32_t> words(resolve.spirv.begin(), resolve.spirv.end());
    REQUIRE(CheckModule(words, resolve).empty());

    SECTION("an Int64 capability") {
        std::vector<uint32_t> w = words;
        for (size_t i = 5; i < w.size(); i += w[i] >> 16) {
            if ((w[i] & 0xFFFF) == kOpCapability) {
                w[i + 1] = kCapabilityInt64;
                break;
            }
        }
        CHECK_FALSE(CheckModule(w, resolve).empty());
    }
    SECTION("a signed int in a block") {
        std::vector<uint32_t> w = words;
        for (size_t i = 5; i < w.size(); i += w[i] >> 16) {
            if ((w[i] & 0xFFFF) == kOpTypeInt && w[i + 2] == 32 && w[i + 3] == 0) {
                w[i + 3] = 1;
            }
        }
        CHECK_FALSE(CheckModule(w, resolve).empty());
    }
    SECTION("another pass's binding table") {
        ShaderCase other = resolve;
        other.bindings = kScatterBindings;
        CHECK_FALSE(CheckModule(words, other).empty());
    }
    SECTION("another groupshared size") {
        ShaderCase other = resolve;
        other.sharedBytes = 0;
        CHECK_FALSE(CheckModule(words, other).empty());
    }
    SECTION("another workgroup shape") {
        ShaderCase other = resolve;
        other.localX = 8;
        other.localY = 8;
        CHECK_FALSE(CheckModule(words, other).empty());
    }
}

TEST_CASE("Every VDP1 binding is used by a pass and declared by the layout probe", "[vdp1][pipelines]") {
    const auto uses = [](std::span<const Binding> table, uint32_t binding) {
        return std::any_of(table.begin(), table.end(), [&](const Binding &b) { return b.binding == binding; });
    };
    for (const Binding &b : kLayoutProbeBindings) {
        INFO("binding " << b.binding);
        bool used = false;
        for (const ShaderCase &shader : Shaders()) {
            used = used || uses(shader.bindings, b.binding);
            CHECK(uses(kLayoutProbeBindings, b.binding));
        }
        CHECK(used);
    }
    // The native passes' bindings are the native probe's; the N x passes' are among the native and N x probes'
    for (size_t i = 0; i < Shaders().size(); ++i) {
        const ShaderCase &shader = Shaders()[i];
        INFO("shader: " << shader.name);
        for (const Binding &b : shader.bindings) {
            CHECK((uses(kLayoutProbeBindings, b.binding) ||
                   (i >= kNativeShaderCount && uses(kNxLayoutProbeBindings, b.binding))));
        }
    }
    bool nxSurface = false;
    for (size_t i = kNativeShaderCount; i < Shaders().size(); ++i) {
        nxSurface = nxSurface || uses(Shaders()[i].bindings, kBindingFbramNx);
    }
    CHECK(nxSurface);
    CHECK(uses(kNxLayoutProbeBindings, kBindingFbramNx));
    // The native shader cases are kPipelineBindings and the N x ones kNxPipelineBindings, in order
    for (size_t i = 0; i < kPipelineBindings.size(); ++i) {
        CHECK(Shaders()[i].bindings.data() == kPipelineBindings[i].data());
    }
    REQUIRE(Shaders().size() == kNativeShaderCount + kNxPipelineBindings.size());
    for (size_t i = 0; i < kNxPipelineBindings.size(); ++i) {
        CHECK(Shaders()[kNativeShaderCount + i].bindings.data() == kNxPipelineBindings[i].data());
    }
}

#if BRIMIR_VULKAN

TEST_CASE("Device limits check: the VDP1 maxima are within the required limits", "[vdp1][limits]") {
    namespace gpu = brimir::gpu;
    STATIC_CHECK(gpu::kRequiredSharedMemorySize >= kMaxComputeSharedMemorySize);
    STATIC_CHECK(gpu::kRequiredStageStorageBuffers >= kMaxStageUsage.storageBuffers);
    STATIC_CHECK(gpu::kRequiredStageResources >= kMaxStageUsage.resources);
    STATIC_CHECK(gpu::kRequiredWorkgroupInvocations >= kWorkgroupSize);
    STATIC_CHECK(gpu::kRequiredPushConstantSize >= kMaxPushConstantSize);
    STATIC_CHECK(gpu::kRequiredStorageBufferRange >= kMaxStorageBufferRange);
    // A device with less groupshared memory than the resolve pass needs is refused
    VkPhysicalDeviceLimits limits{};
    for (const gpu::RequiredLimit &limit : gpu::kRequiredLimits) {
        limits.*limit.member = limit.minimum;
    }
    CHECK(gpu::UnmetLimit(limits).empty());
    limits.maxComputeSharedMemorySize = kResolveSharedMemorySize - 1;
    CHECK_FALSE(gpu::UnmetLimit(limits).empty());
}

namespace {

namespace vu = brimir::gpu::vk_util;
using brimir::test::CloseAndCheckValidation;
using brimir::test::OpenDevice;

} // namespace

TEST_CASE("VDP1 compute pipelines build on the baseline device with clean validation", "[gpu][vdp1][pipelines]") {
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);
    const auto &vk = device->Table();
    const VkPhysicalDeviceLimits &limits = device->Properties().limits;
    REQUIRE(kMaxPushConstantSize <= limits.maxPushConstantsSize);
    REQUIRE(kMaxComputeSharedMemorySize <= limits.maxComputeSharedMemorySize);
    REQUIRE(kMaxStageUsage.storageBuffers <= limits.maxPerStageDescriptorStorageBuffers);

    for (const ShaderCase &shader : Shaders()) {
        INFO("shader: " << shader.name);
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const Binding &b : shader.bindings) {
            bindings.push_back({b.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
        const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, shader.pushConstantSize};
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        CHECK(vu::CreateDescriptorSetLayout(*device, bindings, setLayout) == VK_SUCCESS);
        CHECK(vu::CreatePipelineLayout(*device, {&setLayout, 1},
                                       shader.pushConstantSize != 0 ? std::span(&pushRange, 1)
                                                                    : std::span<const VkPushConstantRange>(),
                                       pipelineLayout) == VK_SUCCESS);
        CHECK(vu::CreateComputePipeline(*device, shader.spirv, pipelineLayout, pipeline) == VK_SUCCESS);
        CHECK(pipeline != VK_NULL_HANDLE);
        vk.vkDestroyPipeline(device->Device(), pipeline, nullptr);
        vk.vkDestroyPipelineLayout(device->Device(), pipelineLayout, nullptr);
        vk.vkDestroyDescriptorSetLayout(device->Device(), setLayout, nullptr);
    }
    CloseAndCheckValidation(device);
}

#endif // BRIMIR_VULKAN
