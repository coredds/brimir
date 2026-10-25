// VDP2 compute shaders ported to the Vulkan 1.2 baseline (src/gpu/shaders/vdp2/cs_vdp2_*.hlsl, generated headers in
// src/gpu/shaders/generated/vdp2_*.spv.h, bindings in src/gpu/vdp2/vdp2_bindings.hpp).
// - The SPIR-V words themselves: only the Shader capability (no Int64), storage images only Rgba8ui/R32ui (sampled
//   images Unknown), no texel buffers, a LocalSize of at most 128 invocations, and exactly the bindings
//   vdp2_bindings.hpp lists, all in set 0. These run without a Vulkan device ([vdp2]).
// - The three compute pipelines are created on the headless device (1A baseline, no extra features or extensions)
//   from the descriptor set layouts vdp2_bindings.hpp describes and the 48-byte push-constant range, with zero
//   validation errors ([gpu][vdp2]; SKIP without a device).
#include "catch_amalgamated.hpp"

#include <gpu/shaders/generated/vdp2_compose.spv.h>
#include <gpu/shaders/generated/vdp2_render_bgs.spv.h>
#include <gpu/shaders/generated/vdp2_render_sprite.spv.h>
#include <gpu/vdp2/vdp2_bindings.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <vector>

#if BRIMIR_VULKAN
    #include "gpu_test_device.hpp"

    #include <gpu/gpu_version.hpp>
    #include <gpu/vk_util.hpp>
#endif

using namespace brimir::gpu::vdp2;
namespace sh = brimir::gpu::shaders;

namespace {

struct ShaderCase {
    const char *name;
    std::span<const uint32_t> spirv;
    std::span<const Binding> bindings;
};

const std::vector<ShaderCase> &Shaders() {
    static const std::vector<ShaderCase> shaders{
        {"vdp2_render_sprite", sh::kVdp2RenderSpriteSpv, kRenderSpriteBindings},
        {"vdp2_render_bgs", sh::kVdp2RenderBgsSpv, kRenderBgsBindings},
        {"vdp2_compose", sh::kVdp2ComposeSpv, kComposeBindings},
    };
    return shaders;
}

// SPIR-V opcodes, enumerants and image formats used below (SPIR-V 1.5 specification).
constexpr uint32_t kSpvMagic = 0x07230203;
constexpr uint32_t kOpExecutionMode = 16;
constexpr uint32_t kOpCapability = 17;
constexpr uint32_t kOpTypeImage = 25;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kExecutionModeLocalSize = 17;
constexpr uint32_t kCapabilityShader = 1;
constexpr uint32_t kCapabilityInt64 = 11;
constexpr uint32_t kDecorationBinding = 33;
constexpr uint32_t kDecorationDescriptorSet = 34;
constexpr uint32_t kDimBuffer = 5;
constexpr uint32_t kImageFormatUnknown = 0;
constexpr uint32_t kImageFormatRgba8ui = 32;
constexpr uint32_t kImageFormatR32ui = 33;

struct SpvImage {
    uint32_t dim, sampled, format;
};

struct SpvModule {
    std::vector<uint32_t> capabilities;
    std::vector<SpvImage> images;
    std::vector<uint32_t> localSize;    // empty if there is no LocalSize execution mode
    std::map<uint32_t, uint32_t> sets;     // id -> DescriptorSet
    std::map<uint32_t, uint32_t> bindings; // id -> Binding
    bool valid = false;
};

// Walks the instruction stream (word count in the high half of each instruction's first word).
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
        case kOpTypeImage:
            // %id = OpTypeImage %sampledType Dim Depth Arrayed MS Sampled Format [Access]
            if (count >= 9) {
                m.images.push_back({w[3], w[7], w[8]});
            }
            break;
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

} // namespace

TEST_CASE("VDP2 shader modules use only the Vulkan 1.2 baseline", "[vdp2][pipelines]") {
    for (const ShaderCase &shader : Shaders()) {
        INFO("shader: " << shader.name);
        const SpvModule m = ParseSpirv(shader.spirv);
        REQUIRE(m.valid);

        // Capabilities: Shader only (no Int64, no SampledBuffer/ImageBuffer, no storage-image format capabilities).
        CHECK(std::find(m.capabilities.begin(), m.capabilities.end(), kCapabilityInt64) == m.capabilities.end());
        for (uint32_t cap : m.capabilities) {
            INFO("capability " << cap);
            CHECK(cap == kCapabilityShader);
        }

        // Images: storage images (Sampled = 2) are Rgba8ui or R32ui; sampled images (Sampled = 1) carry no format;
        // no texel buffers.
        for (const SpvImage &image : m.images) {
            INFO("image dim " << image.dim << " sampled " << image.sampled << " format " << image.format);
            CHECK(image.dim != kDimBuffer);
            if (image.sampled == 2) {
                CHECK((image.format == kImageFormatRgba8ui || image.format == kImageFormatR32ui));
            } else {
                CHECK(image.sampled == 1);
                CHECK(image.format == kImageFormatUnknown);
            }
        }

        // Workgroup size: at most 128 invocations (the guaranteed maxComputeWorkGroupInvocations); 32 wide, as the
        // dispatches in vdp2_bindings.hpp assume.
        REQUIRE(m.localSize.size() == 3);
        CHECK(m.localSize[0] * m.localSize[1] * m.localSize[2] <= 128);
        CHECK(m.localSize[0] == kWorkgroupWidth);
        CHECK(m.localSize[1] == 1);
        CHECK(m.localSize[2] == 1);

        // Resources: exactly the bindings vdp2_bindings.hpp lists, all in set 0.
        std::set<uint32_t> declared;
        for (const auto &[id, binding] : m.bindings) {
            const auto set = m.sets.find(id);
            REQUIRE(set != m.sets.end());
            CHECK(set->second == 0);
            CHECK(declared.insert(binding).second);
        }
        std::set<uint32_t> expected;
        for (const Binding &b : shader.bindings) {
            expected.insert(b.binding);
        }
        CHECK(declared == expected);
    }
}

TEST_CASE("VDP2 binding table: read-only bindings 0-15, storage 16-31, parameters dynamic", "[vdp2][pipelines]") {
    for (const ShaderCase &shader : Shaders()) {
        INFO("shader: " << shader.name);
        for (const Binding &b : shader.bindings) {
            INFO("binding " << b.binding);
            CHECK(b.binding < 32);
            const bool storage = b.binding >= 16;
            if (b.binding == kBindingLayerParams || b.binding == kBindingComposeParams) {
                CHECK(b.kind == DescriptorKind::StorageBufferDynamic);
            } else if (storage) {
                CHECK((b.kind == DescriptorKind::StorageImage || b.kind == DescriptorKind::StorageBuffer));
            } else {
                CHECK((b.kind == DescriptorKind::SampledImage || b.kind == DescriptorKind::StorageBuffer));
            }
        }
    }
    STATIC_CHECK(kPushConstantSize == 48);
    STATIC_CHECK(kRotParamBaseCount == 2 * 512);
    STATIC_CHECK(kLnclBackCount == 2 * 512);
}

#if BRIMIR_VULKAN

namespace {

namespace vu = brimir::gpu::vk_util;
using brimir::test::CloseAndCheckValidation;
using brimir::test::OpenDevice;

VkDescriptorType ToVk(DescriptorKind kind) {
    switch (kind) {
    case DescriptorKind::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case DescriptorKind::StorageBufferDynamic: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    case DescriptorKind::SampledImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case DescriptorKind::StorageImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

} // namespace

TEST_CASE("VDP2 compute pipelines build on the baseline device with clean validation", "[gpu][vdp2][pipelines]") {
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);
    const auto &vk = device->Table();

    // The headless device enables exactly the baseline (gpu_version.hpp): no optional core features.
    STATIC_CHECK(brimir::gpu::kRequiredCoreFeatures.empty());
    STATIC_CHECK(brimir::gpu::kRequiredDeviceExtensions.empty());

    const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, kPushConstantSize};
    REQUIRE(pushRange.size <= device->Properties().limits.maxPushConstantsSize);

    for (const ShaderCase &shader : Shaders()) {
        INFO("shader: " << shader.name);

        std::vector<VkDescriptorSetLayoutBinding> bindings;
        uint32_t storageBuffers = 0;
        for (const Binding &b : shader.bindings) {
            bindings.push_back({b.binding, ToVk(b.kind), 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
            if (b.kind == DescriptorKind::StorageBuffer || b.kind == DescriptorKind::StorageBufferDynamic) {
                ++storageBuffers;
            }
        }
        // Beyond the spec minimum of 4 for bgs (6); report the device limit so a failure is explained.
        INFO("storage buffers " << storageBuffers << ", device maxPerStageDescriptorStorageBuffers "
                                << device->Properties().limits.maxPerStageDescriptorStorageBuffers);
        REQUIRE(storageBuffers <= device->Properties().limits.maxPerStageDescriptorStorageBuffers);

        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        CHECK(vu::CreateDescriptorSetLayout(*device, bindings, setLayout) == VK_SUCCESS);
        CHECK(vu::CreatePipelineLayout(*device, {&setLayout, 1}, {&pushRange, 1}, pipelineLayout) == VK_SUCCESS);
        CHECK(vu::CreateComputePipeline(*device, shader.spirv, pipelineLayout, pipeline) == VK_SUCCESS);
        CHECK(pipeline != VK_NULL_HANDLE);

        vk.vkDestroyPipeline(device->Device(), pipeline, nullptr);
        vk.vkDestroyPipelineLayout(device->Device(), pipelineLayout, nullptr);
        vk.vkDestroyDescriptorSetLayout(device->Device(), setLayout, nullptr);
    }
    CloseAndCheckValidation(device);
}

#endif // BRIMIR_VULKAN
