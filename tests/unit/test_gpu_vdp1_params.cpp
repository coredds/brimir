// VDP1 GPU structs, accessors and bindings (src/gpu/vdp1/vdp1_hw_params.hpp, vdp1_bindings.hpp,
// src/gpu/shaders/vdp1/include/*.hlsli, the layout probe cs_vdp1_layout_probe.hlsl).
// - Every offset, array stride and size of the host structs equals the layout reflected from the probe shader
//   (generated/vdp1_layout_probe.layout.h). The probe's SPIR-V is checked, so the comparison is not vacuous: it names
//   every struct, declares exactly the bindings of the binding table in set 0, has only uint (and arrays of uint, and
//   the push-constant structs) in the blocks reachable from its buffers and push constants, and needs only the Shader
//   capability.
// - The flag bit positions equal the hardware's (CMDPMOD, CMDCTRL) and Task 1's reference constants, and the structs
//   hold every field of the reference's command parameters, spans and fragments over random commands.
// - On a device, the probe evaluates every HLSL accessor over random words and the results equal the C++ copies.
// The CPU cases are tagged [vdp1] only, so they also run in BRIMIR_VULKAN=OFF builds; the GPU case is [gpu] and SKIPs
// without a Vulkan device.
#include "catch_amalgamated.hpp"

#include "vdp1_raster_reference.hpp"

#include <gpu/shaders/generated/vdp1_layout_probe.layout.h>
#include <gpu/shaders/generated/vdp1_layout_probe.spv.h>
#include <gpu/vdp1/vdp1_bindings.hpp>
#include <gpu/vdp1/vdp1_hw_params.hpp>

#include <ymir/hw/vdp/vdp1_defs.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#if BRIMIR_VULKAN
    #include "gpu_test_device.hpp"

    #include <gpu/gpu_version.hpp>
    #include <gpu/vk_util.hpp>
#endif

using namespace brimir::gpu::vdp1;
namespace sh = brimir::gpu::shaders;
namespace ref = brimir::test::vdp1ref;
using ymir::vdp::VDP1Command;

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// SPIR-V of the probe (SPIR-V 1.5 specification)

constexpr uint32_t kSpvMagic = 0x07230203;
constexpr uint32_t kOpName = 5;
constexpr uint32_t kOpExecutionMode = 16;
constexpr uint32_t kOpCapability = 17;
constexpr uint32_t kOpTypeInt = 21;
constexpr uint32_t kOpTypeFloat = 22;
constexpr uint32_t kOpTypeImage = 25;
constexpr uint32_t kOpTypeArray = 28;
constexpr uint32_t kOpTypeRuntimeArray = 29;
constexpr uint32_t kOpTypeStruct = 30;
constexpr uint32_t kOpTypePointer = 32;
constexpr uint32_t kOpVariable = 59;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kExecutionModeLocalSize = 17;
constexpr uint32_t kCapabilityShader = 1;
constexpr uint32_t kDecorationBinding = 33;
constexpr uint32_t kDecorationDescriptorSet = 34;
constexpr uint32_t kStoragePushConstant = 9;
constexpr uint32_t kStorageStorageBuffer = 12;

struct SpvType {
    uint32_t op = 0;
    uint32_t width = 0, signedness = 0; // OpTypeInt
    std::vector<uint32_t> members;      // OpTypeStruct members, or the element of an array
};

struct SpvModule {
    std::vector<uint32_t> capabilities;
    std::map<uint32_t, SpvType> types;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> pointers; // OpTypePointer id -> (storage class, pointee)
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> variables; // OpVariable id -> (pointer type, storage class)
    std::map<uint32_t, uint32_t> sets, bindings;                 // id -> decoration value
    std::set<std::string> names;
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
        case kOpName: {
            // OpName %target "literal string" (little-endian packed, NUL terminated)
            std::string name;
            for (uint32_t j = 2; j < count; ++j) {
                for (uint32_t b = 0; b < 4; ++b) {
                    const char ch = static_cast<char>((w[j] >> (8 * b)) & 0xFF);
                    if (ch == 0) {
                        j = count;
                        break;
                    }
                    name.push_back(ch);
                }
            }
            m.names.insert(name);
            break;
        }
        case kOpTypeInt: m.types[w[1]] = {op, w[2], w[3], {}}; break;
        case kOpTypeImage: m.hasImages = true; break;
        case kOpTypeArray:
        case kOpTypeRuntimeArray: m.types[w[1]] = {op, 0, 0, {w[2]}}; break;
        case kOpTypeStruct: m.types[w[1]] = {op, 0, 0, std::vector<uint32_t>(w + 2, w + count)}; break;
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
        default:
            if (op == kOpTypeFloat) {
                m.types[w[1]] = {op, w[2], 0, {}};
            }
            break;
        }
        i += count;
    }
    m.valid = i == words.size();
    return m;
}

// Whether `type` is a uint32, an array of them, or a struct of such members (recursively).
bool IsUintOnly(const SpvModule &m, uint32_t type) {
    const auto it = m.types.find(type);
    if (it == m.types.end()) {
        return false;
    }
    const SpvType &t = it->second;
    switch (t.op) {
    case kOpTypeInt: return t.width == 32 && t.signedness == 0;
    case kOpTypeArray:
    case kOpTypeRuntimeArray: return IsUintOnly(m, t.members[0]);
    case kOpTypeStruct:
        return std::all_of(t.members.begin(), t.members.end(), [&](uint32_t member) { return IsUintOnly(m, member); });
    default: return false;
    }
}

// Every way the probe module departs from the VDP1 rules, one message each (empty: conforms).
std::vector<std::string> CheckProbe(std::span<const uint32_t> words, std::span<const Binding> table) {
    std::vector<std::string> errors;
    const auto fail = [&](std::string text) { errors.push_back(std::move(text)); };
    const SpvModule m = ParseSpirv(words);
    if (!m.valid) {
        fail("malformed SPIR-V");
        return errors;
    }
    if (m.capabilities != std::vector<uint32_t>{kCapabilityShader}) {
        fail("capabilities other than Shader (no Int64, images, texel buffers or storage formats)");
    }
    if (m.hasImages) {
        fail("an image type (VDP1 has no images)");
    }
    if (m.localSize.size() != 3 || m.localSize[0] * m.localSize[1] * m.localSize[2] > kWorkgroupSize) {
        fail("workgroup larger than kWorkgroupSize");
    }

    // The resource variables are the bindings of the table, all in set 0, all storage buffers; their blocks are
    // uint-only. The push constants are the one PushConstant variable.
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
            declared.insert(binding->second);
            if (!IsUintOnly(m, pointer->second.second)) {
                fail("storage buffer binding " + std::to_string(binding->second) + " holds something other than uint");
            }
        } else if (m.bindings.count(id) != 0) {
            fail("resource with storage class " + std::to_string(var.second));
        }
    }
    if (pushConstants != 1) {
        fail("not exactly one push-constant block");
    }
    std::set<uint32_t> expected;
    for (const Binding &b : table) {
        expected.insert(b.binding);
        if (b.kind != DescriptorKind::StorageBuffer) {
            fail("the table has a non-buffer binding");
        }
    }
    if (declared != expected) {
        fail("bindings differ from the table");
    }
    return errors;
}

uint32_t Rand32(std::mt19937 &rng) {
    return rng();
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Layouts

#define CHECK_OFFSET(type, member, constant) CHECK(offsetof(type, member) == sh::constant)
#define CHECK_SIZE(type, constant) CHECK(sizeof(type) == sh::constant)

namespace {

// The probe's push-constant block: the three push-constant structs one after the other.
struct ProbePush {
    Vdp1BatchPushConstants batch;
    Vdp1ErasePushConstants erase;
    Vdp1FbWritePushConstants fbWrite;
};

} // namespace

TEST_CASE("VDP1 host structs match the reflected std430 layouts", "[vdp1][params]") {
    SECTION("CommandParams") {
        CHECK_OFFSET(Vdp1CommandParams, vertex, kVdp1LayoutProbe_CommandParams_vertex);
        CHECK(sizeof(Vdp1CommandParams::vertex[0]) == sh::kVdp1LayoutProbe_CommandParams_vertex_stride);
        CHECK(sizeof(Vdp1CommandParams::vertex) == 8 * sh::kVdp1LayoutProbe_CommandParams_vertex_stride);
        CHECK_OFFSET(Vdp1CommandParams, gouraud, kVdp1LayoutProbe_CommandParams_gouraud);
        CHECK(sizeof(Vdp1CommandParams::gouraud[0]) == sh::kVdp1LayoutProbe_CommandParams_gouraud_stride);
        CHECK(sizeof(Vdp1CommandParams::gouraud) == 2 * sh::kVdp1LayoutProbe_CommandParams_gouraud_stride);
        CHECK_OFFSET(Vdp1CommandParams, drawFlags, kVdp1LayoutProbe_CommandParams_drawFlags);
        CHECK_OFFSET(Vdp1CommandParams, rasterFlags, kVdp1LayoutProbe_CommandParams_rasterFlags);
        CHECK_OFFSET(Vdp1CommandParams, color, kVdp1LayoutProbe_CommandParams_color);
        CHECK_OFFSET(Vdp1CommandParams, colorBank, kVdp1LayoutProbe_CommandParams_colorBank);
        CHECK_OFFSET(Vdp1CommandParams, charAddr, kVdp1LayoutProbe_CommandParams_charAddr);
        CHECK_OFFSET(Vdp1CommandParams, charSize, kVdp1LayoutProbe_CommandParams_charSize);
        CHECK_OFFSET(Vdp1CommandParams, sysClip, kVdp1LayoutProbe_CommandParams_sysClip);
        CHECK_OFFSET(Vdp1CommandParams, userClip0, kVdp1LayoutProbe_CommandParams_userClip0);
        CHECK_OFFSET(Vdp1CommandParams, userClip1, kVdp1LayoutProbe_CommandParams_userClip1);
        CHECK_OFFSET(Vdp1CommandParams, cmdAddress, kVdp1LayoutProbe_CommandParams_cmdAddress);
        CHECK_SIZE(Vdp1CommandParams, kVdp1LayoutProbe_CommandParams_size);
    }
    SECTION("Span") {
        CHECK_OFFSET(Vdp1Span, x0, kVdp1LayoutProbe_Span_x0);
        CHECK_OFFSET(Vdp1Span, y0, kVdp1LayoutProbe_Span_y0);
        CHECK_OFFSET(Vdp1Span, x1, kVdp1LayoutProbe_Span_x1);
        CHECK_OFFSET(Vdp1Span, y1, kVdp1LayoutProbe_Span_y1);
        CHECK_OFFSET(Vdp1Span, cmdIndex, kVdp1LayoutProbe_Span_cmdIndex);
        CHECK_OFFSET(Vdp1Span, fragBase, kVdp1LayoutProbe_Span_fragBase);
        CHECK_OFFSET(Vdp1Span, gouraudLR, kVdp1LayoutProbe_Span_gouraudLR);
        CHECK_OFFSET(Vdp1Span, stepTexV, kVdp1LayoutProbe_Span_stepTexV);
        CHECK_SIZE(Vdp1Span, kVdp1LayoutProbe_Span_size);
        STATIC_CHECK(sizeof(Vdp1Span) == 32);
    }
    SECTION("Fragment") {
        CHECK_OFFSET(Vdp1Fragment, target, kVdp1LayoutProbe_Fragment_target);
        CHECK_OFFSET(Vdp1Fragment, color, kVdp1LayoutProbe_Fragment_color);
        CHECK_SIZE(Vdp1Fragment, kVdp1LayoutProbe_Fragment_size);
    }
    SECTION("FbWrite") {
        CHECK_OFFSET(Vdp1FbWrite, address, kVdp1LayoutProbe_FbWrite_address);
        CHECK_OFFSET(Vdp1FbWrite, value, kVdp1LayoutProbe_FbWrite_value);
        CHECK_OFFSET(Vdp1FbWrite, mask, kVdp1LayoutProbe_FbWrite_mask);
        CHECK_OFFSET(Vdp1FbWrite, pad, kVdp1LayoutProbe_FbWrite_pad);
        CHECK_SIZE(Vdp1FbWrite, kVdp1LayoutProbe_FbWrite_size);
    }
    SECTION("Push constants") {
        CHECK_OFFSET(Vdp1BatchPushConstants, spanOffset, kVdp1LayoutProbe_BatchParams_spanOffset);
        CHECK_OFFSET(Vdp1BatchPushConstants, spanCount, kVdp1LayoutProbe_BatchParams_spanCount);
        CHECK_OFFSET(Vdp1BatchPushConstants, cmdOffset, kVdp1LayoutProbe_BatchParams_cmdOffset);
        CHECK_OFFSET(Vdp1BatchPushConstants, fragmentCount, kVdp1LayoutProbe_BatchParams_fragmentCount);
        CHECK_OFFSET(Vdp1BatchPushConstants, drawFB, kVdp1LayoutProbe_BatchParams_drawFB);
        CHECK_OFFSET(Vdp1BatchPushConstants, scale, kVdp1LayoutProbe_BatchParams_scale);
        CHECK_SIZE(Vdp1BatchPushConstants, kVdp1LayoutProbe_BatchParams_size);

        CHECK_OFFSET(Vdp1ErasePushConstants, x1, kVdp1LayoutProbe_EraseParams_x1);
        CHECK_OFFSET(Vdp1ErasePushConstants, x3, kVdp1LayoutProbe_EraseParams_x3);
        CHECK_OFFSET(Vdp1ErasePushConstants, y1, kVdp1LayoutProbe_EraseParams_y1);
        CHECK_OFFSET(Vdp1ErasePushConstants, y3, kVdp1LayoutProbe_EraseParams_y3);
        CHECK_OFFSET(Vdp1ErasePushConstants, value, kVdp1LayoutProbe_EraseParams_value);
        CHECK_OFFSET(Vdp1ErasePushConstants, offsetShift, kVdp1LayoutProbe_EraseParams_offsetShift);
        CHECK_OFFSET(Vdp1ErasePushConstants, writeLimit, kVdp1LayoutProbe_EraseParams_writeLimit);
        CHECK_OFFSET(Vdp1ErasePushConstants, flags, kVdp1LayoutProbe_EraseParams_flags);
        CHECK_SIZE(Vdp1ErasePushConstants, kVdp1LayoutProbe_EraseParams_size);

        CHECK_OFFSET(Vdp1FbWritePushConstants, offset, kVdp1LayoutProbe_FbWriteParams_offset);
        CHECK_OFFSET(Vdp1FbWritePushConstants, count, kVdp1LayoutProbe_FbWriteParams_count);
        CHECK_SIZE(Vdp1FbWritePushConstants, kVdp1LayoutProbe_FbWriteParams_size);

        // The probe's own block: the three structs in a row, the largest a real shader declares is one of them.
        CHECK_OFFSET(ProbePush, batch, kVdp1LayoutProbe_ProbePush_batch);
        CHECK_OFFSET(ProbePush, erase, kVdp1LayoutProbe_ProbePush_erase);
        CHECK_OFFSET(ProbePush, fbWrite, kVdp1LayoutProbe_ProbePush_fbWrite);
        CHECK_SIZE(ProbePush, kVdp1LayoutProbe_ProbePush_size);
        CHECK(sizeof(Vdp1ErasePushConstants) <= 128); // the push-constant minimum
    }
    SECTION("Uint-only, 16-byte multiples (the fragment and push-constant blocks excepted)") {
        STATIC_CHECK(std::is_standard_layout_v<Vdp1CommandParams> && std::is_trivially_copyable_v<Vdp1CommandParams>);
        STATIC_CHECK(std::is_standard_layout_v<Vdp1Span> && std::is_trivially_copyable_v<Vdp1Span>);
        STATIC_CHECK(std::is_standard_layout_v<Vdp1Fragment> && std::is_trivially_copyable_v<Vdp1Fragment>);
        STATIC_CHECK(std::is_standard_layout_v<Vdp1FbWrite> && std::is_trivially_copyable_v<Vdp1FbWrite>);
        // No padding anywhere: the size is the member count times 4 (HLSLuint is the only member type).
        STATIC_CHECK(sizeof(Vdp1CommandParams) == 20 * sizeof(HLSLuint));
        STATIC_CHECK(sizeof(Vdp1Span) == 8 * sizeof(HLSLuint));
        STATIC_CHECK(sizeof(Vdp1Fragment) == 2 * sizeof(HLSLuint));
        STATIC_CHECK(sizeof(Vdp1FbWrite) == 4 * sizeof(HLSLuint));
        STATIC_CHECK(sizeof(Vdp1BatchPushConstants) == 6 * sizeof(HLSLuint));
        STATIC_CHECK(sizeof(Vdp1ErasePushConstants) == 8 * sizeof(HLSLuint));
        STATIC_CHECK(sizeof(Vdp1FbWritePushConstants) == 2 * sizeof(HLSLuint));
        STATIC_CHECK(sizeof(Vdp1CommandParams) % 16 == 0);
        STATIC_CHECK(sizeof(Vdp1Span) % 16 == 0);
        STATIC_CHECK(sizeof(Vdp1FbWrite) % 16 == 0);
        STATIC_CHECK(std::is_same_v<HLSLuint, uint32_t>);
    }
}

TEST_CASE("The layout probe declares every VDP1 struct, binding and push-constant block", "[vdp1][params]") {
    const std::span<const uint32_t> probe(sh::kVdp1LayoutProbeSpv);
    const std::vector<std::string> errors = CheckProbe(probe, kLayoutProbeBindings);
    for (const std::string &error : errors) {
        UNSCOPED_INFO(error);
    }
    CHECK(errors.empty());

    // The reflected constants come from structs the module names (a reflection of nothing would pass vacuously).
    const SpvModule m = ParseSpirv(probe);
    REQUIRE(m.valid);
    for (const char *name : {"CommandParams", "Span", "Fragment", "FbWrite", "BatchParams", "EraseParams",
                             "FbWriteParams", "type.PushConstant.ProbePush"}) {
        INFO(name);
        CHECK(m.names.count(name) == 1);
    }
    CHECK(m.localSize == std::vector<uint32_t>{64, 1, 1});
    STATIC_CHECK(kWorkgroupSize == 64);

    // The binding table is the nine resources of the shaders' pipelines, in the t (0-15) and u (16+) ranges.
    STATIC_CHECK(kLayoutProbeBindings.size() == 9);
    STATIC_CHECK(kBindingCommands < 16 && kBindingSpans < 16 && kBindingVRAM < 16 && kBindingFbWrites < 16);
    STATIC_CHECK(kBindingFragments >= 16 && kBindingUnitCounts >= 16 && kBindingBucket >= 16 &&
                 kBindingFbram >= 16 && kBindingScanSums >= 16);
    const auto uses = [](std::span<const Binding> table, uint32_t binding) {
        return std::any_of(table.begin(), table.end(), [&](const Binding &b) { return b.binding == binding; });
    };
    // Every pipeline's bindings are among the probe's; each probe binding is used by some pipeline.
    for (const std::span<const Binding> table :
         {std::span<const Binding>(kRasterBindings), std::span<const Binding>(kBucketBindings),
          std::span<const Binding>(kScatterBindings), std::span<const Binding>(kResolveBindings),
          std::span<const Binding>(kEraseBindings), std::span<const Binding>(kFbWriteBindings)}) {
        for (const Binding &b : table) {
            CHECK(uses(kLayoutProbeBindings, b.binding));
            CHECK(b.kind == DescriptorKind::StorageBuffer);
        }
    }
    for (const Binding &b : kLayoutProbeBindings) {
        INFO("binding " << b.binding);
        CHECK((uses(kRasterBindings, b.binding) || uses(kBucketBindings, b.binding) ||
               uses(kScatterBindings, b.binding) || uses(kResolveBindings, b.binding) ||
               uses(kEraseBindings, b.binding) || uses(kFbWriteBindings, b.binding)));
    }
}

TEST_CASE("The VDP1 probe check rejects modules that depart from the rules", "[vdp1][params]") {
    const std::vector<uint32_t> probe(std::begin(sh::kVdp1LayoutProbeSpv), std::end(sh::kVdp1LayoutProbeSpv));
    REQUIRE(CheckProbe(probe, kLayoutProbeBindings).empty());

    SECTION("a signed int in a block") {
        // OpTypeInt 32 0 -> OpTypeInt 32 1 turns every uint member into an int one
        std::vector<uint32_t> words = probe;
        uint32_t changed = 0;
        for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
            if ((words[i] & 0xFFFF) == kOpTypeInt && words[i + 2] == 32 && words[i + 3] == 0) {
                words[i + 3] = 1;
                ++changed;
            }
        }
        REQUIRE(changed >= 1);
        CHECK_FALSE(CheckProbe(words, kLayoutProbeBindings).empty());
    }
    SECTION("a float in a block") {
        std::vector<uint32_t> words = probe;
        uint32_t changed = 0;
        for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
            if ((words[i] & 0xFFFF) == kOpTypeInt && words[i + 2] == 32 && words[i + 3] == 0) {
                // OpTypeFloat %id 32 has one operand fewer: turn the int into a float and the signedness word into a
                // no-op (OpNop, 1 word)
                words[i] = (3u << 16) | kOpTypeFloat;
                words[i + 3] = 1u << 16; // OpNop
                ++changed;
            }
        }
        REQUIRE(changed >= 1);
        CHECK_FALSE(CheckProbe(words, kLayoutProbeBindings).empty());
    }
    SECTION("a binding missing from the table") {
        const std::array<Binding, 8> fewer{{{kBindingCommands, DescriptorKind::StorageBuffer},
                                            {kBindingSpans, DescriptorKind::StorageBuffer},
                                            {kBindingVRAM, DescriptorKind::StorageBuffer},
                                            {kBindingFbWrites, DescriptorKind::StorageBuffer},
                                            {kBindingFragments, DescriptorKind::StorageBuffer},
                                            {kBindingUnitCounts, DescriptorKind::StorageBuffer},
                                            {kBindingBucket, DescriptorKind::StorageBuffer},
                                            {kBindingFbram, DescriptorKind::StorageBuffer}}};
        CHECK_FALSE(CheckProbe(probe, fewer).empty());
    }
    SECTION("an Int64 capability") {
        std::vector<uint32_t> words = probe;
        for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
            if ((words[i] & 0xFFFF) == kOpCapability) {
                words[i + 1] = 11; // Int64
                break;
            }
        }
        CHECK_FALSE(CheckProbe(words, kLayoutProbeBindings).empty());
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Bindings and limits

TEST_CASE("VDP1 bindings stay within the VDP2 maxima the device limits are set from", "[vdp1][params][limits]") {
    STATIC_CHECK(kMaxStageUsage.storageBuffers == 5);
    STATIC_CHECK(kMaxStageUsage.storageImages == 0);
    STATIC_CHECK(kMaxStageUsage.sampledImages == 0);
    STATIC_CHECK(kMaxStageUsage.resources == 5);
    // VDP2's maxima (7 storage buffers, 3 storage images, 5 sampled images, 11 resources) cover them
    STATIC_CHECK(kMaxStageUsage.storageBuffers <= brimir::gpu::vdp2::kMaxStageUsage.storageBuffers);
    STATIC_CHECK(kMaxStageUsage.resources <= brimir::gpu::vdp2::kMaxStageUsage.resources);
    STATIC_CHECK(kMaxPushConstantSize == 32);
    STATIC_CHECK(kMaxPushConstantSize <= brimir::gpu::vdp2::kMaxPushConstantSize);
    STATIC_CHECK(kWorkgroupSize <= brimir::gpu::vdp2::kMaxWorkgroupInvocations);
    STATIC_CHECK(kMaxComputeSharedMemorySize == 0);

    // Buffer sizes: the fragment buffer is the largest range, within the guaranteed maxStorageBufferRange (2^27)
    STATIC_CHECK(kFragmentBufferSize == 32u * 1024 * 1024);
    STATIC_CHECK(kMaxStorageBufferRange == kFragmentBufferSize);
    STATIC_CHECK(kMaxStorageBufferRange <= (1u << 27));
    STATIC_CHECK(kBucketBufferSize == 16u * 1024 * 1024);
    STATIC_CHECK(kUnitCount == 262144);
    STATIC_CHECK(kUnitCountsBufferSize == 1024u * 1024);
    STATIC_CHECK(kScanBlockCount * kScanBlockSize == kUnitCount);
    STATIC_CHECK(kFbramBufferSize == 2u * 1024 * 1024);
    STATIC_CHECK(kFbramBufferSize == brimir::gpu::vdp2::kSpriteFBBufferSize);
    STATIC_CHECK(kVRAMBufferSize == 512u * 1024);
    // A span (2 * (4096 + 1) slots at most) always fits a fresh batch
    STATIC_CHECK(2u * 4097u < kMaxFragmentsPerBatch);
}

TEST_CASE("The FBRAM slice layout is the VDP2 sprite buffer's", "[vdp1][params]") {
    // vdp2_vulkan_backend.cpp kSpriteSliceBase: main, alt, mesh 0, mesh 1, each display framebuffer 0/1
    STATIC_CHECK(Vdp1MainSlice(0) == 0);
    STATIC_CHECK(Vdp1MainSlice(1) == 1);
    STATIC_CHECK(Vdp1AltSlice(0) == 2);
    STATIC_CHECK(Vdp1AltSlice(1) == 3);
    STATIC_CHECK(Vdp1MeshSlice(0, 0) == 4);
    STATIC_CHECK(Vdp1MeshSlice(0, 1) == 5);
    STATIC_CHECK(Vdp1MeshSlice(1, 0) == 6);
    STATIC_CHECK(Vdp1MeshSlice(1, 1) == 7);
    // Test-local mirror of the base table (kSpriteSliceBase = {0, 2, 4, 6} slices + fb)
    for (uint32_t fb = 0; fb < 2; ++fb) {
        CHECK(Vdp1MainSlice(fb) == 0 + fb);
        CHECK(Vdp1AltSlice(fb) == 2 + fb);
        CHECK(Vdp1MeshSlice(0, fb) == 4 + fb);
        CHECK(Vdp1MeshSlice(1, fb) == 6 + fb);
    }
    CHECK(Vdp1FbramByteAddress(3, 0x1FFFF) == 3 * 256 * 1024 + 0x3FFFE);
    CHECK(Vdp1FbramByteAddress(7, 0x20000) == 7 * 256 * 1024); // the word index wraps within the slice
    CHECK(Vdp1FbWordShift(0) == 0);
    CHECK(Vdp1FbWordShift(1) == 16);
    CHECK(Vdp1FbWordShift(0x1FFFF) == 16);
}

// ---------------------------------------------------------------------------------------------------------------------
// Flag bit positions

TEST_CASE("drawFlags keep CMDPMOD and CMDCTRL in the hardware's bit positions", "[vdp1][params]") {
    STATIC_CHECK(kCmdpmodMask == 0x9FFF);

    // Each DrawMode field alone, through the core's bitfield union
    const auto mode = [](auto set) {
        VDP1Command::DrawMode m{.u16 = 0};
        set(m);
        return Vdp1MakeDrawFlags(m.u16, 0, false, false);
    };
    CHECK(Vdp1CmdColorCalc(mode([](auto &m) { m.colorCalcBits = 3; })) == 3);
    CHECK(Vdp1CmdColorCalc(mode([](auto &m) { m.colorCalcBits = 2; })) == 2);
    CHECK(Vdp1CmdGouraud(mode([](auto &m) { m.gouraudEnable = 1; })));
    CHECK(Vdp1CmdColorMode(mode([](auto &m) { m.colorMode = 5; })) == 5);
    CHECK(Vdp1CmdColorMode(mode([](auto &m) { m.colorMode = 7; })) == 7);
    CHECK(Vdp1CmdTransparentPixelDisable(mode([](auto &m) { m.transparentPixelDisable = 1; })));
    CHECK(Vdp1CmdEndCodeDisable(mode([](auto &m) { m.endCodeDisable = 1; })));
    CHECK(Vdp1CmdMesh(mode([](auto &m) { m.meshEnable = 1; })));
    CHECK(Vdp1CmdClippingMode(mode([](auto &m) { m.clippingMode = 1; })));
    CHECK(Vdp1CmdUserClip(mode([](auto &m) { m.userClippingEnable = 1; })));
    CHECK(Vdp1CmdHighSpeedShrink(mode([](auto &m) { m.highSpeedShrink = 1; })));
    CHECK(Vdp1CmdMsbOn(mode([](auto &m) { m.msbOn = 1; })));
    // ... and nothing else: setting one field sets only its own accessor
    {
        VDP1Command::DrawMode m{.u16 = 0};
        m.meshEnable = 1;
        const uint32_t f = Vdp1MakeDrawFlags(m.u16, 0, false, false);
        CHECK(Vdp1CmdMesh(f));
        CHECK_FALSE(Vdp1CmdGouraud(f));
        CHECK_FALSE(Vdp1CmdClippingMode(f));
        CHECK_FALSE(Vdp1CmdUserClip(f));
        CHECK_FALSE(Vdp1CmdMsbOn(f));
        CHECK(Vdp1CmdColorMode(f) == 0);
        CHECK(Vdp1CmdColorCalc(f) == 0);
        CHECK((f & 0xFFFF) == m.u16);
    }
    // The pre-clipping-disable bit is carried (unused by the shaders); bits 13-14 are dropped
    CHECK(Vdp1MakeDrawFlags(0xFFFF, 0, false, false) == 0x9FFF);
    CHECK(Vdp1MakeDrawFlags(1u << 11, 0, false, false) == (1u << 11));

    // CMDCTRL through the core's union
    for (uint32_t command = 0; command < 16; ++command) {
        for (uint32_t flip = 0; flip < 4; ++flip) {
            VDP1Command::Control c{.u16 = 0};
            c.command = static_cast<VDP1Command::CommandType>(command);
            c.flipH = flip & 1;
            c.flipV = (flip >> 1) & 1;
            c.zoomPoint = 0xF; // not carried
            c.end = 1;
            const uint32_t f = Vdp1MakeDrawFlags(0, c.u16, true, false);
            CHECK(Vdp1CmdType(f) == command);
            CHECK(Vdp1CmdFlipH(f) == ((flip & 1) != 0));
            CHECK(Vdp1CmdFlipV(f) == (((flip >> 1) & 1) != 0));
            CHECK(Vdp1CmdTextured(f));
            CHECK_FALSE(Vdp1CmdAntiAlias(f));
            CHECK((f & 0xFFFF) == 0);
        }
    }
    CHECK(Vdp1CmdAntiAlias(Vdp1MakeDrawFlags(0, 0, false, true)));
    CHECK_FALSE(Vdp1CmdTextured(Vdp1MakeDrawFlags(0, 0, false, true)));
}

TEST_CASE("rasterFlags, fragment targets, FbWrite addresses and erase flags round-trip", "[vdp1][params]") {
    // rasterFlags: every field alone
    CHECK(Vdp1RasPixel8(Vdp1MakeRasterFlags(true, 512, false, false, false, false, false, false, 0)));
    CHECK(Vdp1RasFbSizeH(Vdp1MakeRasterFlags(false, 1024, false, false, false, false, false, false, 0)) == 1024);
    CHECK(Vdp1RasFbSizeH(Vdp1MakeRasterFlags(false, 512, false, false, false, false, false, false, 0)) == 512);
    CHECK(Vdp1RasDblInterlace(Vdp1MakeRasterFlags(false, 512, true, false, false, false, false, false, 0)));
    CHECK(Vdp1RasDblInterlaceDrawLine(Vdp1MakeRasterFlags(false, 512, false, true, false, false, false, false, 0)));
    CHECK(Vdp1RasEvenOddCoordSelect(Vdp1MakeRasterFlags(false, 512, false, false, true, false, false, false, 0)));
    CHECK(Vdp1RasDoubleDensity(Vdp1MakeRasterFlags(false, 512, false, false, false, true, false, false, 0)));
    CHECK(Vdp1RasDeinterlace(Vdp1MakeRasterFlags(false, 512, false, false, false, false, true, false, 0)));
    CHECK(Vdp1RasTransparentMeshes(Vdp1MakeRasterFlags(false, 512, false, false, false, false, false, true, 0)));
    CHECK(Vdp1RasDoubleV(Vdp1MakeRasterFlags(false, 512, false, false, false, false, false, false, 1)) == 1);
    CHECK(Vdp1MakeRasterFlags(false, 512, false, false, false, false, false, false, 0) == 0);

    // fragment targets: the reference's constants (Task 1) and its encoding
    STATIC_CHECK(ref::kTargetWordMask == kWordIndexMask);
    STATIC_CHECK(ref::kTargetAltBit == kFragAltBit);
    STATIC_CHECK(ref::kTargetOddBit == 1u << 18);
    STATIC_CHECK(ref::kTargetCalcShift == 19);
    STATIC_CHECK(ref::kTargetMeshBit == 1u << 21);
    STATIC_CHECK(ref::kTargetMsbOnBit == 1u << 22);
    STATIC_CHECK(ref::kTargetUnitMask == kFragUnitMask);
    {
        const uint32_t t = Vdp1MakeFragmentTarget(0x1ABCD, true, true, 3, true, true, true, true);
        CHECK(Vdp1FragWord(t) == 0x1ABCD);
        CHECK(Vdp1FragAlt(t));
        CHECK(Vdp1FragOddByte(t));
        CHECK(Vdp1FragColorCalc(t) == 3);
        CHECK(Vdp1FragMesh(t));
        CHECK(Vdp1FragMsbOn(t));
        CHECK(Vdp1FragPixel8(t));
        CHECK(Vdp1FragTransparentMeshes(t));
        CHECK(Vdp1FragLive(t));
        CHECK(Vdp1FragUnit(t) == (0x1ABCD | kFragAltBit));
        CHECK(Vdp1FragLive(Vdp1MakeFragmentTarget(0, false, false, 0, false, false, false, false)));
        CHECK_FALSE(Vdp1FragLive(0)); // an empty slot
        // One bit each, nothing overlapping
        uint32_t seen = 0;
        for (uint32_t bit : {kFragAltBit, 1u << 18, 1u << 19, 1u << 21, 1u << 22, 1u << 23, 1u << 24, kFragLiveBit}) {
            CHECK((seen & bit) == 0);
            seen |= bit;
        }
        CHECK((seen & kWordIndexMask) == 0);
    }
    {
        const uint32_t a = Vdp1MakeFbWriteAddress(0x1F00F, 1, true);
        CHECK(Vdp1FbWriteWord(a) == 0x1F00F);
        CHECK(Vdp1FbWriteFb(a) == 1);
        CHECK(Vdp1FbWriteMirrorAlt(a));
        CHECK(Vdp1FbWriteFb(Vdp1MakeFbWriteAddress(5, 0, false)) == 0);
        CHECK_FALSE(Vdp1FbWriteMirrorAlt(Vdp1MakeFbWriteAddress(5, 0, false)));
    }
    {
        for (uint32_t fb = 0; fb < 2; ++fb) {
            for (uint32_t bits = 0; bits < 4; ++bits) {
                const uint32_t f = Vdp1MakeEraseFlags(fb, bits & 1, bits & 2);
                CHECK(Vdp1EraseFb(f) == fb);
                CHECK(Vdp1EraseMirrorAlt(f) == ((bits & 1) != 0));
                CHECK(Vdp1EraseClearMesh(f) == ((bits & 2) != 0));
            }
        }
    }
    CHECK(Vdp1SpanQuadStep(Vdp1Pack16(4097, 255)) == 4097);
    CHECK(Vdp1SpanTexV(Vdp1Pack16(4097, 255)) == 255);
}

// ---------------------------------------------------------------------------------------------------------------------
// The structs hold everything the reference's commands, spans and fragments do

namespace {

uint32_t Pack(const ymir::vdp::Color555 &lo, const ymir::vdp::Color555 &hi) {
    return Vdp1Pack16(lo.u16, hi.u16);
}

uint32_t AsU(int32_t v) {
    return static_cast<uint32_t>(v);
}

// What the front half does with a reference command (the real packer, Task 3, does the same from its own state).
Vdp1CommandParams PackCommand(const ref::CommandParams &p, const ref::RasterState &s) {
    Vdp1CommandParams c{};
    for (size_t i = 0; i < 4; ++i) {
        c.vertex[2 * i] = AsU(p.vertices[i].x());
        c.vertex[2 * i + 1] = AsU(p.vertices[i].y());
    }
    c.gouraud[0] = Pack(p.gouraud[0], p.gouraud[1]);
    c.gouraud[1] = Pack(p.gouraud[2], p.gouraud[3]);
    c.drawFlags = Vdp1MakeDrawFlags(p.mode.u16, p.control.u16, p.textured, p.antiAlias);
    c.rasterFlags = Vdp1MakeRasterFlags(s.pixel8Bits, s.fbSizeH, s.dblInterlaceEnable, s.dblInterlaceDrawLine,
                                        s.evenOddCoordSelect, s.doubleDensity, s.deinterlace, s.transparentMeshes,
                                        s.doubleV);
    c.color = p.color;
    c.colorBank = p.colorBank;
    c.charAddr = p.charAddr;
    c.charSize = Vdp1Pack16(p.charSizeH, p.charSizeV);
    c.sysClip = Vdp1Pack16(p.clip.sysClipH, p.clip.sysClipV);
    c.userClip0 = Vdp1Pack16(p.clip.userClipX0, p.clip.userClipY0);
    c.userClip1 = Vdp1Pack16(p.clip.userClipX1, p.clip.userClipY1);
    c.cmdAddress = p.address;
    return c;
}

Vdp1Span PackSpan(const ref::Span &s) {
    return {AsU(s.a.x()), AsU(s.a.y()), AsU(s.b.x()), AsU(s.b.y()), s.cmdIndex, s.fragBase,
            Pack(s.gouraudL, s.gouraudR), Vdp1Pack16(s.quadStep, s.texV)};
}

Vdp1Fragment PackFragment(const ref::Fragment &f, const ref::RasterState &s) {
    const uint32_t t = f.target;
    return {Vdp1MakeFragmentTarget(t & ref::kTargetWordMask, (t & ref::kTargetAltBit) != 0,
                                   (t & ref::kTargetOddBit) != 0, (t >> ref::kTargetCalcShift) & 3,
                                   (t & ref::kTargetMeshBit) != 0, (t & ref::kTargetMsbOnBit) != 0, s.pixel8Bits,
                                   s.transparentMeshes),
            f.color};
}

} // namespace

TEST_CASE("The VDP1 structs hold every field of the reference's commands, spans and fragments", "[vdp1][params]") {
    std::mt19937 rng(0x7D51u);
    std::vector<uint8_t> vram(0x80000);
    uint64_t commands = 0, spans = 0, fragments = 0, textured = 0, gouraud = 0, mesh = 0, alt = 0;
    constexpr uint32_t kCommandTableBase = 0x1000;
    for (uint32_t iteration = 0; iteration < 2500; ++iteration) {
        // Random VRAM, with plausible command tables: a command at a random address with sizes in range
        for (auto &b : vram) {
            b = static_cast<uint8_t>(rng());
        }
        ref::RasterState state;
        state.fbSizeH = (rng() & 1) ? 1024 : 512;
        state.pixel8Bits = (rng() & 3) == 0;
        state.dblInterlaceEnable = (rng() & 1) != 0;
        state.dblInterlaceDrawLine = (rng() & 1) != 0;
        state.evenOddCoordSelect = (rng() & 1) != 0;
        state.doubleDensity = (rng() & 1) != 0;
        state.deinterlace = (rng() & 1) != 0;
        state.transparentMeshes = (rng() & 1) != 0;
        state.doubleV = rng() & 1;
        ref::ClipContext ctx;
        ctx.sysClipH = static_cast<uint16_t>(rng() % 700);
        ctx.sysClipV = static_cast<uint16_t>(rng() % 520);
        ctx.userClipX0 = static_cast<uint16_t>(rng() & 0xFFFF);
        ctx.userClipY0 = static_cast<uint16_t>(rng() & 0xFFFF);
        ctx.userClipX1 = static_cast<uint16_t>(rng() & 0xFFFF);
        ctx.userClipY1 = static_cast<uint16_t>(rng() & 0xFFFF);
        ctx.localX = static_cast<int32_t>(rng() % 700) - 100;
        ctx.localY = static_cast<int32_t>(rng() % 500) - 100;

        const uint32_t address = kCommandTableBase + (iteration % 64) * 0x20;
        // CMDCTRL: draw commands 0-7 (6 is the line, 5 and 7 polylines) with random flips
        vram[address] = static_cast<uint8_t>(rng() & 0x03); // jump/skip/end bits clear (the top byte)
        vram[address + 1] = static_cast<uint8_t>((rng() % 8) | ((rng() & 3) << 4));
        // vertices within a few hundred pixels of the clip rectangle so most quads draw
        for (uint32_t v = 0; v < 4; ++v) {
            const uint16_t x = static_cast<uint16_t>(rng() % 500);
            const uint16_t y = static_cast<uint16_t>(rng() % 400);
            vram[address + 0x0C + v * 4] = static_cast<uint8_t>(x >> 8);
            vram[address + 0x0D + v * 4] = static_cast<uint8_t>(x);
            vram[address + 0x0E + v * 4] = static_cast<uint8_t>(y >> 8);
            vram[address + 0x0F + v * 4] = static_cast<uint8_t>(y);
        }
        // CMDSIZE: width in 8-pixel units 1-6, height 1-200; CMDSRCA / CMDGRDA inside VRAM
        vram[address + 0x0A] = static_cast<uint8_t>(1 + rng() % 6);
        vram[address + 0x0B] = static_cast<uint8_t>(1 + rng() % 200);

        ref::CommandSpans cs = ref::BuildCommandSpans(vram.data(), address, ctx, state);
        ++commands;
        if (!cs.draws) {
            continue;
        }
        const Vdp1CommandParams gpu = PackCommand(cs.params, state);
        const uint32_t df = gpu.drawFlags;
        const uint32_t rf = gpu.rasterFlags;
        textured += cs.params.textured;
        gouraud += cs.params.mode.gouraudEnable;

        // The command round-trips through the accessors
        CHECK(Vdp1CmdColorCalc(df) == cs.params.mode.colorCalcBits);
        CHECK(Vdp1CmdGouraud(df) == (cs.params.mode.gouraudEnable != 0));
        CHECK(Vdp1CmdColorMode(df) == cs.params.mode.colorMode);
        CHECK(Vdp1CmdTransparentPixelDisable(df) == (cs.params.mode.transparentPixelDisable != 0));
        CHECK(Vdp1CmdEndCodeDisable(df) == (cs.params.mode.endCodeDisable != 0));
        CHECK(Vdp1CmdMesh(df) == (cs.params.mode.meshEnable != 0));
        CHECK(Vdp1CmdClippingMode(df) == (cs.params.mode.clippingMode != 0));
        CHECK(Vdp1CmdUserClip(df) == (cs.params.mode.userClippingEnable != 0));
        CHECK(Vdp1CmdHighSpeedShrink(df) == (cs.params.mode.highSpeedShrink != 0));
        CHECK(Vdp1CmdMsbOn(df) == (cs.params.mode.msbOn != 0));
        CHECK(Vdp1CmdTextured(df) == cs.params.textured);
        CHECK(Vdp1CmdAntiAlias(df) == cs.params.antiAlias);
        CHECK(Vdp1CmdFlipH(df) == (cs.params.control.flipH != 0));
        CHECK(Vdp1CmdFlipV(df) == (cs.params.control.flipV != 0));
        CHECK(Vdp1CmdType(df) == static_cast<uint32_t>(cs.params.control.command));
        CHECK(Vdp1RasPixel8(rf) == state.pixel8Bits);
        CHECK(Vdp1RasFbSizeH(rf) == state.fbSizeH);
        CHECK(Vdp1RasDblInterlace(rf) == state.dblInterlaceEnable);
        CHECK(Vdp1RasDblInterlaceDrawLine(rf) == state.dblInterlaceDrawLine);
        CHECK(Vdp1RasEvenOddCoordSelect(rf) == state.evenOddCoordSelect);
        CHECK(Vdp1RasDoubleDensity(rf) == state.doubleDensity);
        CHECK(Vdp1RasDeinterlace(rf) == state.deinterlace);
        CHECK(Vdp1RasTransparentMeshes(rf) == state.transparentMeshes);
        CHECK(Vdp1RasDoubleV(rf) == state.doubleV);
        CHECK(gpu.color == cs.params.color);
        CHECK(gpu.colorBank == cs.params.colorBank);
        CHECK(gpu.charAddr == cs.params.charAddr);
        CHECK(Vdp1Lo16(gpu.charSize) == cs.params.charSizeH);
        CHECK(Vdp1Hi16(gpu.charSize) == cs.params.charSizeV);
        CHECK(Vdp1Lo16(gpu.sysClip) == cs.params.clip.sysClipH);
        CHECK(Vdp1Hi16(gpu.sysClip) == cs.params.clip.sysClipV);
        CHECK(Vdp1Lo16(gpu.userClip0) == cs.params.clip.userClipX0);
        CHECK(Vdp1Hi16(gpu.userClip0) == cs.params.clip.userClipY0);
        CHECK(Vdp1Lo16(gpu.userClip1) == cs.params.clip.userClipX1);
        CHECK(Vdp1Hi16(gpu.userClip1) == cs.params.clip.userClipY1);
        CHECK(gpu.cmdAddress == cs.params.address);
        for (size_t v = 0; v < 4; ++v) {
            CHECK(static_cast<int32_t>(gpu.vertex[2 * v]) == cs.params.vertices[v].x());
            CHECK(static_cast<int32_t>(gpu.vertex[2 * v + 1]) == cs.params.vertices[v].y());
        }
        for (size_t g = 0; g < 4; ++g) {
            const uint32_t word = gpu.gouraud[g >> 1];
            CHECK(((g & 1) ? Vdp1Hi16(word) : Vdp1Lo16(word)) == cs.params.gouraud[g].u16);
        }

        // Spans and their fragments
        // Every span gets its slots; about a dozen per command are rasterized and checked, and eight of each one's
        // fragments (the loop is long: a quad has hundreds of lines of hundreds of pixels)
        uint32_t fragBase = 0;
        std::vector<ref::Fragment> frags;
        const size_t spanStride = std::max<size_t>(1, cs.spans.size() / 12);
        size_t spanIndex = 0;
        for (ref::Span &span : cs.spans) {
            const uint32_t slots = ref::SpanSlots(state, cs.params, span);
            if (slots == 0) {
                continue;
            }
            span.cmdIndex = iteration & 0xFF;
            span.fragBase = fragBase;
            fragBase += slots;
            if (spanIndex++ % spanStride != 0) {
                continue;
            }
            ++spans;
            const Vdp1Span gs = PackSpan(span);
            CHECK(static_cast<int32_t>(gs.x0) == span.a.x());
            CHECK(static_cast<int32_t>(gs.y0) == span.a.y());
            CHECK(static_cast<int32_t>(gs.x1) == span.b.x());
            CHECK(static_cast<int32_t>(gs.y1) == span.b.y());
            CHECK(gs.cmdIndex == span.cmdIndex);
            CHECK(gs.fragBase == span.fragBase);
            CHECK(Vdp1SpanQuadStep(gs.stepTexV) == span.quadStep);
            CHECK(Vdp1SpanTexV(gs.stepTexV) == span.texV);
            CHECK(Vdp1Lo16(gs.gouraudLR) == span.gouraudL.u16);
            CHECK(Vdp1Hi16(gs.gouraudLR) == span.gouraudR.u16);

            frags.clear();
            ref::RasterSpan(state, cs.params, span, vram.data(), {}, frags);
            const size_t fragmentStride = std::max<size_t>(1, frags.size() / 8);
            for (size_t fragmentIndex = 0; fragmentIndex < frags.size(); fragmentIndex += fragmentStride) {
                const ref::Fragment &f = frags[fragmentIndex];
                const Vdp1Fragment gf = PackFragment(f, state);
                ++fragments;
                mesh += (f.target & ref::kTargetMeshBit) != 0;
                alt += (f.target & ref::kTargetAltBit) != 0;
                // The slot index is the key
                REQUIRE(f.key >= span.fragBase);
                REQUIRE(f.key < span.fragBase + slots);
                CHECK(Vdp1FragLive(gf.target));
                CHECK(Vdp1FragWord(gf.target) == (f.target & ref::kTargetWordMask));
                CHECK(Vdp1FragAlt(gf.target) == ((f.target & ref::kTargetAltBit) != 0));
                CHECK(Vdp1FragOddByte(gf.target) == ((f.target & ref::kTargetOddBit) != 0));
                CHECK(Vdp1FragColorCalc(gf.target) == ((f.target >> ref::kTargetCalcShift) & 3));
                CHECK(Vdp1FragMesh(gf.target) == ((f.target & ref::kTargetMeshBit) != 0));
                CHECK(Vdp1FragMsbOn(gf.target) == ((f.target & ref::kTargetMsbOnBit) != 0));
                CHECK(Vdp1FragPixel8(gf.target) == state.pixel8Bits);
                CHECK(Vdp1FragTransparentMeshes(gf.target) == state.transparentMeshes);
                CHECK(Vdp1FragUnit(gf.target) == (f.target & ref::kTargetUnitMask));
                // The reference's target is the fragment's target without the two bits that moved here
                CHECK((gf.target & 0x7FFFFF) == (f.target & 0x7FFFFF));
                CHECK(gf.color == f.color);
                // The word is inside the framebuffer's slice
                CHECK(Vdp1FbramByteAddress(0, Vdp1FragWord(gf.target)) < kFbramSliceSize);
            }
        }
    }
    // The random commands covered what the structs must carry
    INFO("commands " << commands << " spans " << spans << " fragments " << fragments << " textured " << textured
                     << " gouraud " << gouraud << " mesh " << mesh << " alt " << alt);
    CHECK(spans > 3000);
    CHECK(fragments > 10000);
    CHECK(textured > 100);
    CHECK(gouraud > 100);
    CHECK(mesh > 100);
    CHECK(alt > 100);
}

// ---------------------------------------------------------------------------------------------------------------------
// The HLSL accessors on a device

namespace {

constexpr uint32_t kProbeWords = 94;

// What cs_vdp1_layout_probe.hlsl writes for one case: the same accessors, in the shader's order.
std::array<uint32_t, kProbeWords> ProbeExpected(const Vdp1CommandParams &c, const Vdp1Span &s, const Vdp1Fragment &f,
                                                const Vdp1FbWrite &w, const ProbePush &p, uint32_t vramWord,
                                                uint32_t bucket, uint32_t fbramWord, uint32_t scanSum) {
    std::array<uint32_t, kProbeWords> out{};
    size_t k = 0;
    const auto emit = [&](uint32_t value) { out[k++] = value; };
    const uint32_t df = c.drawFlags, rf = c.rasterFlags, t = f.target;
    for (uint32_t v : c.vertex) {
        emit(v);
    }
    emit(c.gouraud[0]);
    emit(c.gouraud[1]);
    emit(Vdp1CmdColorCalc(df));
    emit(Vdp1CmdGouraud(df));
    emit(Vdp1CmdColorMode(df));
    emit(Vdp1CmdTransparentPixelDisable(df));
    emit(Vdp1CmdEndCodeDisable(df));
    emit(Vdp1CmdMesh(df));
    emit(Vdp1CmdClippingMode(df));
    emit(Vdp1CmdUserClip(df));
    emit(Vdp1CmdHighSpeedShrink(df));
    emit(Vdp1CmdMsbOn(df));
    emit(Vdp1CmdTextured(df));
    emit(Vdp1CmdAntiAlias(df));
    emit(Vdp1CmdFlipH(df));
    emit(Vdp1CmdFlipV(df));
    emit(Vdp1CmdType(df));
    emit(Vdp1RasPixel8(rf));
    emit(Vdp1RasFbSizeH(rf));
    emit(Vdp1RasDblInterlace(rf));
    emit(Vdp1RasDblInterlaceDrawLine(rf));
    emit(Vdp1RasEvenOddCoordSelect(rf));
    emit(Vdp1RasDoubleDensity(rf));
    emit(Vdp1RasDeinterlace(rf));
    emit(Vdp1RasTransparentMeshes(rf));
    emit(Vdp1RasDoubleV(rf));
    emit(Vdp1Lo16(c.charSize));
    emit(Vdp1Hi16(c.charSize));
    emit(Vdp1Lo16(c.sysClip));
    emit(Vdp1Hi16(c.sysClip));
    emit(Vdp1Lo16(c.userClip0));
    emit(Vdp1Hi16(c.userClip0));
    emit(Vdp1Lo16(c.userClip1));
    emit(Vdp1Hi16(c.userClip1));
    emit(c.color);
    emit(c.colorBank);
    emit(c.charAddr);
    emit(c.cmdAddress);
    emit(s.x0);
    emit(s.y0);
    emit(s.x1);
    emit(s.y1);
    emit(s.cmdIndex);
    emit(s.fragBase);
    emit(Vdp1SpanQuadStep(s.stepTexV));
    emit(Vdp1SpanTexV(s.stepTexV));
    emit(Vdp1Lo16(s.gouraudLR));
    emit(Vdp1Hi16(s.gouraudLR));
    emit(Vdp1FragWord(t));
    emit(Vdp1FragAlt(t));
    emit(Vdp1FragOddByte(t));
    emit(Vdp1FragColorCalc(t));
    emit(Vdp1FragMesh(t));
    emit(Vdp1FragMsbOn(t));
    emit(Vdp1FragPixel8(t));
    emit(Vdp1FragTransparentMeshes(t));
    emit(Vdp1FragLive(t));
    emit(Vdp1FragUnit(t));
    emit(f.color);
    emit(Vdp1MakeFragmentTarget(Vdp1FragWord(t), Vdp1FragAlt(t), Vdp1FragOddByte(t), Vdp1FragColorCalc(t),
                                Vdp1FragMesh(t), Vdp1FragMsbOn(t), Vdp1FragPixel8(t), Vdp1FragTransparentMeshes(t)));
    emit(Vdp1FbWriteWord(w.address));
    emit(Vdp1FbWriteFb(w.address));
    emit(Vdp1FbWriteMirrorAlt(w.address));
    emit(w.value);
    emit(w.mask);
    emit(Vdp1EraseFb(p.erase.flags));
    emit(Vdp1EraseMirrorAlt(p.erase.flags));
    emit(Vdp1EraseClearMesh(p.erase.flags));
    const uint32_t fb = Vdp1FbWriteFb(w.address);
    emit(Vdp1FbramByteAddress(c.cmdAddress, Vdp1FragWord(t)));
    emit(Vdp1FbWordShift(Vdp1FragWord(t)));
    emit(Vdp1MainSlice(fb));
    emit(Vdp1AltSlice(fb));
    emit(Vdp1MeshSlice(Vdp1FragAlt(t), fb));
    emit(vramWord);
    emit(bucket);
    emit(fbramWord);
    emit(scanSum);
    emit(Vdp1MakeDrawFlags(df & kCmdpmodMask,
                           Vdp1CmdType(df) | (Vdp1CmdFlipH(df) ? 16u : 0u) | (Vdp1CmdFlipV(df) ? 32u : 0u),
                           Vdp1CmdTextured(df), Vdp1CmdAntiAlias(df)));
    emit(Vdp1MakeRasterFlags(Vdp1RasPixel8(rf), Vdp1RasFbSizeH(rf), Vdp1RasDblInterlace(rf),
                             Vdp1RasDblInterlaceDrawLine(rf), Vdp1RasEvenOddCoordSelect(rf), Vdp1RasDoubleDensity(rf),
                             Vdp1RasDeinterlace(rf), Vdp1RasTransparentMeshes(rf), Vdp1RasDoubleV(rf)));
    emit(Vdp1MakeFbWriteAddress(Vdp1FbWriteWord(w.address), Vdp1FbWriteFb(w.address), Vdp1FbWriteMirrorAlt(w.address)));
    emit(Vdp1MakeEraseFlags(Vdp1EraseFb(p.erase.flags), Vdp1EraseMirrorAlt(p.erase.flags),
                            Vdp1EraseClearMesh(p.erase.flags)));
    emit(Vdp1Pack16(s.gouraudLR, s.stepTexV));
    emit(p.erase.x1 + p.erase.x3 + p.erase.y1 + p.erase.y3);
    emit(p.erase.value ^ p.erase.offsetShift ^ p.erase.writeLimit);
    emit(p.batch.spanOffset + p.batch.cmdOffset + p.batch.fragmentCount + p.batch.drawFB + p.batch.scale);
    emit(p.fbWrite.offset + p.fbWrite.count + w.pad);
    REQUIRE(k == kProbeWords);
    return out;
}

} // namespace

TEST_CASE("The C++ accessor copies are what the probe's HLSL evaluates (host side of the case table)",
          "[vdp1][params]") {
    // ProbeExpected on all-ones and all-zero inputs: a fixed expectation catches an accessor changed on one side only
    Vdp1CommandParams ones{};
    std::memset(&ones, 0xFF, sizeof(ones));
    Vdp1Span onesSpan;
    std::memset(&onesSpan, 0xFF, sizeof(onesSpan));
    Vdp1Fragment onesFrag;
    std::memset(&onesFrag, 0xFF, sizeof(onesFrag));
    Vdp1FbWrite onesWrite;
    std::memset(&onesWrite, 0xFF, sizeof(onesWrite));
    ProbePush push;
    std::memset(&push, 0xFF, sizeof(push));
    const auto e = ProbeExpected(ones, onesSpan, onesFrag, onesWrite, push, 0, 0, 0, 0);
    CHECK(e[10] == 3);     // colour calculation
    CHECK(e[12] == 7);     // colour mode
    CHECK(e[24] == 15);    // command
    CHECK(e[26] == 1024);  // framebuffer width
    CHECK(e[33] == 1);     // doubleV
    CHECK(e[34] == 0xFFFF);
    CHECK(e[35] == 0xFFFF);
    CHECK(e[56] == 0x1FFFF); // fragment word
    CHECK(e[59] == 3);     // fragment colour calculation
    CHECK(e[65] == (0x1FFFFu | kFragAltBit)); // fragment unit
    CHECK(e[67] == (0x1FFFFu | kFragAltBit | (1u << 18) | (3u << 19) | (1u << 21) | (1u << 22) | (1u << 23) |
                    (1u << 24) | kFragLiveBit));
    CHECK(e[68] == 0x1FFFF);
    CHECK(e[76] == 7 * 256 * 1024 + 0x3FFFE);
    CHECK(e[77] == 16);
    CHECK(e[85] == ((0x9FFFu) | (0x3Fu << 16) | (1u << 22) | (1u << 23)));
}

#if BRIMIR_VULKAN

namespace {

namespace vu = brimir::gpu::vk_util;
using brimir::test::CloseAndCheckValidation;
using brimir::test::OpenDevice;

template <typename T>
void FillRandom(std::vector<T> &items, std::mt19937 &rng) {
    static_assert(std::is_trivially_copyable_v<T>);
    auto *words = reinterpret_cast<uint32_t *>(items.data());
    for (size_t i = 0; i < items.size() * sizeof(T) / sizeof(uint32_t); ++i) {
        // Mostly uniform words; some all-ones and zero words for the extremes
        const uint32_t pick = rng() & 15;
        words[i] = pick == 0 ? 0u : pick == 1 ? 0xFFFFFFFFu : Rand32(rng);
    }
}

} // namespace

TEST_CASE("The probe's HLSL accessors equal the C++ copies on random words", "[gpu][vdp1][params]") {
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);
    const auto &vk = device->Table();

    constexpr uint32_t kCases = 4096;
    std::mt19937 rng(0x7D52u);
    std::vector<Vdp1CommandParams> commands(kCases);
    std::vector<Vdp1Span> spans(kCases);
    std::vector<Vdp1Fragment> fragments(kCases);
    std::vector<Vdp1FbWrite> fbWrites(kCases);
    std::vector<uint32_t> vram(kVRAMBufferSize / 4), bucket(kCases), fbram(kFbramBufferSize / 4), scanSums(kCases);
    FillRandom(commands, rng);
    FillRandom(spans, rng);
    FillRandom(fragments, rng);
    FillRandom(fbWrites, rng);
    FillRandom(vram, rng);
    FillRandom(bucket, rng);
    FillRandom(fbram, rng);
    FillRandom(scanSums, rng);
    ProbePush push{};
    push.batch = {Rand32(rng), kCases, Rand32(rng), Rand32(rng), Rand32(rng), Rand32(rng)};
    push.erase = {Rand32(rng), Rand32(rng), Rand32(rng), Rand32(rng), Rand32(rng), Rand32(rng), Rand32(rng),
                  Rand32(rng)};
    push.fbWrite = {Rand32(rng), Rand32(rng)};

    // Buffers in the binding table's order
    struct Resource {
        uint32_t binding;
        const void *data;
        VkDeviceSize size;
        vu::Buffer buffer;
    };
    std::vector<uint32_t> results(static_cast<size_t>(kCases) * kProbeWords, 0xCDCDCDCDu);
    std::array<Resource, 9> resources{{
        {kBindingCommands, commands.data(), commands.size() * sizeof(Vdp1CommandParams), {}},
        {kBindingSpans, spans.data(), spans.size() * sizeof(Vdp1Span), {}},
        {kBindingVRAM, vram.data(), vram.size() * 4, {}},
        {kBindingFbWrites, fbWrites.data(), fbWrites.size() * sizeof(Vdp1FbWrite), {}},
        {kBindingFragments, fragments.data(), fragments.size() * sizeof(Vdp1Fragment), {}},
        {kBindingUnitCounts, results.data(), results.size() * 4, {}},
        {kBindingBucket, bucket.data(), bucket.size() * 4, {}},
        {kBindingFbram, fbram.data(), fbram.size() * 4, {}},
        {kBindingScanSums, scanSums.data(), scanSums.size() * 4, {}},
    }};

    constexpr VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    bool ok = true;
    for (Resource &r : resources) {
        ok = ok && vu::CreateBuffer(*device, r.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kHost, r.buffer) ==
                       VK_SUCCESS;
        ok = ok && r.buffer.mapped != nullptr;
        if (ok) {
            std::memcpy(r.buffer.mapped, r.data, r.size);
        }
    }

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (const Binding &b : kLayoutProbeBindings) {
        bindings.push_back({b.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<uint32_t>(bindings.size())};
    const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ProbePush)};
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    ok = ok && vu::CreateDescriptorSetLayout(*device, bindings, setLayout) == VK_SUCCESS;
    ok = ok && vu::CreateDescriptorPool(*device, {&poolSize, 1}, 1, pool) == VK_SUCCESS;
    ok = ok && vu::AllocateDescriptorSet(*device, pool, setLayout, set) == VK_SUCCESS;
    ok = ok && vu::CreatePipelineLayout(*device, {&setLayout, 1}, {&pushRange, 1}, pipelineLayout) == VK_SUCCESS;
    ok = ok && vu::CreateComputePipeline(*device, sh::kVdp1LayoutProbeSpv, pipelineLayout, pipeline) == VK_SUCCESS;
    if (ok) {
        for (const Resource &r : resources) {
            vu::WriteStorageBuffer(*device, set, r.binding, r.buffer);
        }
        ok = vu::SubmitOneShot(*device, [&](VkCommandBuffer cmd) {
                 vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                 vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0,
                                            nullptr);
                 vk.vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
                 vk.vkCmdDispatch(cmd, (kCases + kWorkgroupSize - 1) / kWorkgroupSize, 1, 1);
                 vu::GlobalBarrier(*device, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
             }) == VK_SUCCESS;
    }
    if (ok) {
        std::memcpy(results.data(), resources[5].buffer.mapped, results.size() * 4);
    }

    vk.vkDestroyPipeline(device->Device(), pipeline, nullptr);
    vk.vkDestroyPipelineLayout(device->Device(), pipelineLayout, nullptr);
    vk.vkDestroyDescriptorPool(device->Device(), pool, nullptr);
    vk.vkDestroyDescriptorSetLayout(device->Device(), setLayout, nullptr);
    for (Resource &r : resources) {
        vu::DestroyBuffer(*device, r.buffer);
    }
    REQUIRE(ok);

    size_t failures = 0;
    for (uint32_t i = 0; i < kCases; ++i) {
        const auto expected = ProbeExpected(commands[i], spans[i], fragments[i], fbWrites[i], push,
                                            vram[i & (kVRAMBufferSize / 4 - 1)], bucket[i],
                                            fbram[i & (kFbramBufferSize / 4 - 1)], scanSums[i]);
        for (uint32_t word = 0; word < kProbeWords; ++word) {
            const uint32_t got = results[static_cast<size_t>(i) * kProbeWords + word];
            if (got != expected[word] && failures++ < 10) {
                UNSCOPED_INFO("case " << i << " word " << word << ": GPU " << got << ", expected " << expected[word]);
            }
        }
    }
    CHECK(failures == 0);
    CloseAndCheckValidation(device);
}

#endif // BRIMIR_VULKAN
