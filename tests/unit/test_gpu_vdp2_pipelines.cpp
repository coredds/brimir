// VDP2 compute shaders ported to the Vulkan 1.2 baseline (src/gpu/shaders/vdp2/cs_vdp2_*.hlsl, generated headers in
// src/gpu/shaders/generated/vdp2_*.spv.h, bindings in src/gpu/vdp2/vdp2_bindings.hpp).
// - The SPIR-V words themselves: only the Shader capability (no Int64), storage images only Rgba8ui/R32ui (sampled
//   images Unknown), no texel buffers, a LocalSize of at most kMaxWorkgroupInvocations (64, the largest workgroup
//   the renderer's shaders use), and exactly the bindings vdp2_bindings.hpp lists, all in set 0, each with its table kind and storage-image format; the check rejects
//   modules altered to break each rule. These run without a Vulkan device ([vdp2]).
// - The device limits check (gpu_version.hpp UnmetLimit) against crafted VkPhysicalDeviceLimits ([vdp2][limits],
//   BRIMIR_VULKAN builds).
// - The three compute pipelines are created on the headless device (1A baseline, no extra features or extensions)
//   from the descriptor set layouts vdp2_bindings.hpp describes and the 48-byte push-constant range, with zero
//   validation errors ([gpu][vdp2]; SKIP without a device).
#include "catch_amalgamated.hpp"

#include <gpu/shaders/generated/cs_frame_output.spv.h>
#include <gpu/shaders/generated/vdp2_compose.spv.h>
#include <gpu/shaders/generated/vdp2_render_bgs.spv.h>
#include <gpu/shaders/generated/vdp2_render_sprite.spv.h>
#include <gpu/vdp2/vdp2_bindings.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
constexpr uint32_t kOpTypePointer = 32;
constexpr uint32_t kOpVariable = 59;
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
    std::map<uint32_t, SpvImage> images;   // OpTypeImage id -> operands
    std::map<uint32_t, uint32_t> pointees; // OpTypePointer id -> pointee type id
    std::map<uint32_t, uint32_t> varTypes; // OpVariable id -> pointer type id
    std::vector<uint32_t> localSize;       // empty if there is no LocalSize execution mode
    std::map<uint32_t, uint32_t> sets;     // id -> DescriptorSet
    std::map<uint32_t, uint32_t> bindings; // id -> Binding
    bool valid = false;

    // The image type a resource variable points to (nullptr for buffers).
    const SpvImage *ImageOf(uint32_t variable) const {
        const auto var = varTypes.find(variable);
        if (var == varTypes.end()) {
            return nullptr;
        }
        const auto pointee = pointees.find(var->second);
        if (pointee == pointees.end()) {
            return nullptr;
        }
        const auto image = images.find(pointee->second);
        return image == images.end() ? nullptr : &image->second;
    }
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
                m.images[w[1]] = {w[3], w[7], w[8]};
            }
            break;
        case kOpTypePointer:
            // %id = OpTypePointer StorageClass %type
            if (count >= 4) {
                m.pointees[w[1]] = w[3];
            }
            break;
        case kOpVariable:
            // %id = OpVariable %pointerType StorageClass [initializer]
            if (count >= 4) {
                m.varTypes[w[2]] = w[1];
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

uint32_t SpvFormat(ImageFormat format) {
    switch (format) {
    case ImageFormat::Rgba8ui: return kImageFormatRgba8ui;
    case ImageFormat::R32ui: return kImageFormatR32ui;
    case ImageFormat::None: break;
    }
    return kImageFormatUnknown;
}

// Every way a module departs from the baseline or from its binding table, one message each (empty: conforms).
std::vector<std::string> CheckModule(std::span<const uint32_t> words, std::span<const Binding> table) {
    std::vector<std::string> errors;
    const auto fail = [&](std::string text) { errors.push_back(std::move(text)); };
    const SpvModule m = ParseSpirv(words);
    if (!m.valid) {
        fail("malformed SPIR-V");
        return errors;
    }

    // Capabilities: exactly Shader (no Int64, no SampledBuffer/ImageBuffer, no storage-image format capabilities).
    if (std::find(m.capabilities.begin(), m.capabilities.end(), kCapabilityShader) == m.capabilities.end()) {
        fail("no Shader capability");
    }
    for (uint32_t cap : m.capabilities) {
        if (cap == kCapabilityInt64) {
            fail("Int64 capability");
        } else if (cap != kCapabilityShader) {
            fail("capability " + std::to_string(cap));
        }
    }

    // Image types: storage images (Sampled = 2) are Rgba8ui or R32ui; sampled images (Sampled = 1) carry no format;
    // no texel buffers.
    for (const auto &[id, image] : m.images) {
        if (image.dim == kDimBuffer) {
            fail("texel buffer type %" + std::to_string(id));
        }
        if (image.sampled == 2) {
            if (image.format != kImageFormatRgba8ui && image.format != kImageFormatR32ui) {
                fail("storage image format " + std::to_string(image.format));
            }
        } else if (image.sampled != 1 || image.format != kImageFormatUnknown) {
            fail("sampled image with Sampled " + std::to_string(image.sampled) + " format " +
                 std::to_string(image.format));
        }
    }

    // Workgroup size: at most kMaxWorkgroupInvocations (the device limit gpu_version.hpp requires), 32 x 1 x 1 as the
    // dispatches in vdp2_bindings.hpp assume.
    if (m.localSize.size() != 3) {
        fail("no LocalSize");
    } else if (m.localSize[0] * m.localSize[1] * m.localSize[2] > kMaxWorkgroupInvocations || m.localSize[0] != kWorkgroupWidth ||
               m.localSize[1] != 1 || m.localSize[2] != 1) {
        fail("LocalSize " + std::to_string(m.localSize[0]) + "x" + std::to_string(m.localSize[1]) + "x" +
             std::to_string(m.localSize[2]));
    }

    // Resources: exactly the table's bindings, all in set 0, each of the table's kind and (images) format.
    std::set<uint32_t> declared;
    for (const auto &[id, binding] : m.bindings) {
        const std::string what = "binding " + std::to_string(binding);
        const auto set = m.sets.find(id);
        if (set == m.sets.end() || set->second != 0) {
            fail(what + " not in set 0");
        }
        if (!declared.insert(binding).second) {
            fail(what + " declared twice");
        }
        const auto entry =
            std::find_if(table.begin(), table.end(), [&](const Binding &b) { return b.binding == binding; });
        if (entry == table.end()) {
            fail(what + " not in the binding table");
            continue;
        }
        const SpvImage *image = m.ImageOf(id);
        switch (entry->kind) {
        case DescriptorKind::StorageImage:
            if (!image || image->sampled != 2) {
                fail(what + " is not a storage image");
            } else if (image->format != SpvFormat(entry->format)) {
                fail(what + " has storage format " + std::to_string(image->format) + ", the table says " +
                     std::to_string(SpvFormat(entry->format)));
            }
            break;
        case DescriptorKind::SampledImage:
            if (!image || image->sampled != 1) {
                fail(what + " is not a sampled image");
            }
            break;
        case DescriptorKind::StorageBuffer:
        case DescriptorKind::StorageBufferDynamic:
            if (image) {
                fail(what + " is an image, the table says buffer");
            }
            break;
        }
    }
    for (const Binding &b : table) {
        if (!declared.contains(b.binding)) {
            fail("binding " + std::to_string(b.binding) + " missing from the module");
        }
    }
    return errors;
}

uint32_t StorageImageVariables(std::span<const uint32_t> words) {
    const SpvModule m = ParseSpirv(words);
    uint32_t count = 0;
    for (const auto &[id, binding] : m.bindings) {
        const SpvImage *image = m.ImageOf(id);
        count += image && image->sampled == 2 ? 1 : 0;
    }
    return count;
}

} // namespace

TEST_CASE("VDP2 shader modules use only the Vulkan 1.2 baseline", "[vdp2][pipelines]") {
    for (const ShaderCase &shader : Shaders()) {
        INFO("shader: " << shader.name);
        for (const std::string &error : CheckModule(shader.spirv, shader.bindings)) {
            FAIL_CHECK(error);
        }
    }
    // Storage images per module: sprite writes the layer and sprite-attribute images, bgs the layer, RBG line colour
    // and colour calculation window images, compose none (its output is a buffer).
    CHECK(StorageImageVariables(sh::kVdp2RenderSpriteSpv) == 2);
    CHECK(StorageImageVariables(sh::kVdp2RenderBgsSpv) == 3);
    CHECK(StorageImageVariables(sh::kVdp2ComposeSpv) == 0);
}

TEST_CASE("The largest workgroup any renderer shader declares is kMaxWorkgroupInvocations", "[vdp2][pipelines]") {
    uint32_t largest = 0;
    for (const ShaderCase &shader : Shaders()) {
        const SpvModule m = ParseSpirv(shader.spirv);
        REQUIRE(m.localSize.size() == 3);
        largest = std::max(largest, m.localSize[0] * m.localSize[1] * m.localSize[2]);
    }
    // The frame output pass (gpu/frame_presenter.cpp) is not a VDP2 shader: 8 x 8 x 1.
    const SpvModule output = ParseSpirv(sh::kCsFrameOutputSpv);
    REQUIRE(output.localSize.size() == 3);
    CHECK(output.localSize[0] == kFrameOutputGroupSize);
    CHECK(output.localSize[1] == kFrameOutputGroupSize);
    CHECK(output.localSize[2] == 1);
    largest = std::max(largest, output.localSize[0] * output.localSize[1] * output.localSize[2]);
    CHECK(largest == kMaxWorkgroupInvocations);
}

TEST_CASE("The VDP2 SPIR-V check rejects a module that departs from its binding table", "[vdp2][pipelines]") {
    const std::vector<uint32_t> bgs(std::begin(sh::kVdp2RenderBgsSpv), std::end(sh::kVdp2RenderBgsSpv));

    SECTION("rgba8ui and r32ui swapped") {
        std::vector<uint32_t> words = bgs;
        uint32_t swapped = 0;
        for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
            if ((words[i] & 0xFFFF) == kOpTypeImage && (words[i] >> 16) >= 9) {
                uint32_t &format = words[i + 8];
                if (format == kImageFormatRgba8ui || format == kImageFormatR32ui) {
                    format = format == kImageFormatRgba8ui ? kImageFormatR32ui : kImageFormatRgba8ui;
                    ++swapped;
                }
            }
        }
        REQUIRE(swapped == 2); // the rgba8ui 2D-array type and the r32ui 2D type
        CHECK_FALSE(CheckModule(words, kRenderBgsBindings).empty());
    }
    SECTION("an Int64 capability") {
        std::vector<uint32_t> words = bgs;
        for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
            if ((words[i] & 0xFFFF) == kOpCapability) {
                words[i + 1] = kCapabilityInt64;
                break;
            }
        }
        CHECK_FALSE(CheckModule(words, kRenderBgsBindings).empty());
    }
    SECTION("another pipeline's binding table") {
        CHECK_FALSE(CheckModule(bgs, kRenderSpriteBindings).empty());
    }
    CHECK(CheckModule(bgs, kRenderBgsBindings).empty());
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

    // The largest per-stage use, which the device limits in gpu_version.hpp must cover (bgs: 6 storage buffers,
    // 3 storage images, 10 descriptors; compose: 4 sampled images).
    STATIC_CHECK(kMaxStageUsage.storageBuffers == 6);
    STATIC_CHECK(kMaxStageUsage.storageImages == 3);
    STATIC_CHECK(kMaxStageUsage.sampledImages == 4);
    STATIC_CHECK(kMaxStageUsage.resources == 10);
}

TEST_CASE("MakeOutputSize packs HRes and VRes", "[vdp2][pipelines]") {
    CHECK(MakeOutputSize(320, 224) == (320u | (224u << 16)));
    CHECK(MakeOutputSize(kMaxResH, kMaxResV) == (704u | (512u << 16)));
}

#if BRIMIR_VULKAN

TEST_CASE("Device limits check: the VDP2 pipelines' per-stage descriptors and push constants", "[vdp2][limits]") {
    namespace gpu = brimir::gpu;
    // A device exactly at the required minimums passes.
    VkPhysicalDeviceLimits limits{};
    limits.maxPerStageDescriptorStorageBuffers = 6;
    limits.maxPerStageDescriptorStorageImages = 3;
    limits.maxPerStageDescriptorSampledImages = 4;
    limits.maxPerStageResources = 10;
    limits.maxComputeWorkGroupInvocations = kMaxWorkgroupInvocations;
    limits.maxPushConstantsSize = 48;
    CHECK(gpu::UnmetLimit(limits).empty());

    // Every entry, one below its minimum, fails with its name and both values.
    REQUIRE(gpu::kRequiredLimits.size() == 6);
    for (const gpu::RequiredLimit &limit : gpu::kRequiredLimits) {
        INFO(limit.name);
        VkPhysicalDeviceLimits low = limits;
        low.*limit.member = limit.minimum - 1;
        CHECK(gpu::UnmetLimit(low) == std::string(limit.name) + " " + std::to_string(limit.minimum - 1) + " < " +
                                          std::to_string(limit.minimum));
    }

    // The Vulkan-guaranteed minimum of 4 storage buffers per stage is not enough (bgs has 6).
    VkPhysicalDeviceLimits spec = limits;
    spec.maxPerStageDescriptorStorageBuffers = 4;
    CHECK(gpu::UnmetLimit(spec) == "maxPerStageDescriptorStorageBuffers 4 < 6");

    STATIC_CHECK(gpu::kRequiredStageStorageBuffers == kMaxStageUsage.storageBuffers);
    STATIC_CHECK(gpu::kRequiredStageResources == kMaxStageUsage.resources);
}

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
