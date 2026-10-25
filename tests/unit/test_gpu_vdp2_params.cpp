// VDP2 GPU parameter structs and 32-bit rotation math (src/gpu/vdp2/vdp2_hw_params.hpp, src/gpu/vdp2/int64_emu.hpp).
// - The host structs match the std430 layouts reflected from the VDP2 shaders that declare them
//   (src/gpu/shaders/generated/vdp2_*.layout.h).
// - RotDotShr10 / RotCoordShr equal the 64-bit expressions of Ymir's cs_vdp2_render_bgs.hlsl (lines 444-445, 502-503)
//   on edge values and 1,000,000 seeded random cases, and the HLSL copy (int64_emu.hlsli, through test_int64_emu.hlsl)
//   equals the C++ mirror on 65,536 cases.
// The CPU cases are tagged [vdp2] only, so they also run in BRIMIR_VULKAN=OFF builds; the GPU case is [gpu] and SKIPs
// without a Vulkan device.
#include "catch_amalgamated.hpp"

#include <gpu/shaders/generated/vdp2_compose.layout.h>
#include <gpu/shaders/generated/vdp2_render_bgs.layout.h>
#include <gpu/shaders/generated/vdp2_render_sprite.layout.h>
#include <gpu/shaders/generated/test_int64_emu.layout.h>
#include <gpu/vdp2/int64_emu.hpp>
#include <gpu/vdp2/vdp2_hw_params.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#if BRIMIR_VULKAN
    #include "gpu_test_device.hpp"

    #include <gpu/shaders/generated/test_int64_emu.spv.h>
    #include <gpu/vk_util.hpp>

    #include <cstring>
#endif

using namespace brimir::gpu::vdp2;
namespace sh = brimir::gpu::shaders;

namespace {

// C++ mirror of test_int64_emu.hlsl's buffers.
struct Int64EmuCase {
    int32_t a0, b0, a1, b1, a2, b2;
    int32_t k, scr, p;
};

struct Int64EmuResult {
    int32_t dot;
    uint32_t coord;
};

// The reference expressions: Ymir cs_vdp2_render_bgs.hlsl at 8cdf0a53, lines 444-445 (Xsp/Ysp) and 502-503.
int32_t RefDotShr10(int32_t a0, int32_t b0, int32_t a1, int32_t b1, int32_t a2, int32_t b2) {
    return static_cast<int32_t>((int64_t(a0) * int64_t(b0) + int64_t(a1) * int64_t(b1) + int64_t(a2) * int64_t(b2)) >>
                                10);
}

uint32_t RefCoordShr(int32_t k, int32_t scr, int32_t p) {
    return static_cast<uint32_t>((((int64_t(k) * scr) >> 16) + p) >> 10);
}

constexpr int32_t kMin32 = std::numeric_limits<int32_t>::min();
constexpr int32_t kMax32 = std::numeric_limits<int32_t>::max();

// Rotation matrix elements are 14-bit signed (SignExtend(..., 14), bgs:328-333).
constexpr std::array<int32_t, 9> kEdgeA{-8192, -8191, -4096, -1, 0, 1, 4096, 8190, 8191};
constexpr std::array<int32_t, 15> kEdgeB{kMin32, kMin32 + 1, -65537, -65536, -65535, -1024, -1, 0,
                                         1,      1023,       65535,  65536,  65537,  kMax32 - 1, kMax32};
// kx/ky: 24-bit signed coefficients, also the full int32 range (the helper is exact for any int32).
constexpr std::array<int32_t, 13> kEdgeK{kMin32, -(1 << 23), -(1 << 23) + 1, -65536, -1, 0, 1,
                                         65536,  (1 << 23) - 1, 1 << 23, kMax32 - 1, kMax32, 0x00FFFFFF};
constexpr std::array<int32_t, 11> kEdgeScrP{kMin32, kMin32 + 1, -(1 << 26), -65536, -1024, -1, 0, 1023, 65535,
                                            kMax32 - 1, kMax32};

// Seeded case generator; uses the raw mt19937 output (std distributions differ between standard libraries).
class CaseGen {
public:
    explicit CaseGen(uint32_t seed) : m_rng(seed) {}

    int32_t A() { return static_cast<int32_t>(m_rng()) >> 18; } // [-8192, 8191]
    int32_t Any() {
        // Mostly uniform int32, sometimes a 24-bit or 16-bit value or an extreme, to cover carries near the edges.
        const uint32_t pick = m_rng() & 7;
        const auto v = static_cast<int32_t>(m_rng());
        switch (pick) {
        case 0: return v >> 8;
        case 1: return v >> 16;
        case 2: return (v & 1) ? kMax32 - (v >> 28 & 7) : kMin32 + (v >> 28 & 7);
        default: return v;
        }
    }
    Int64EmuCase Case() {
        Int64EmuCase c{};
        c.a0 = A();
        c.b0 = Any();
        c.a1 = A();
        c.b1 = Any();
        c.a2 = A();
        c.b2 = Any();
        c.k = (m_rng() & 1) ? (static_cast<int32_t>(m_rng()) >> 8) : Any(); // 24-bit kx/ky or anything
        c.scr = Any();
        c.p = Any();
        return c;
    }

private:
    std::mt19937 m_rng;
};

// Edge cases followed by seeded random ones, `count` in total.
std::vector<Int64EmuCase> MakeCases(size_t count, uint32_t seed) {
    std::vector<Int64EmuCase> cases;
    cases.reserve(count);
    // RotDotShr10: every edge a with every edge b (in all three slots), RotCoordShr: every edge triple.
    size_t i = 0;
    for (int32_t a : kEdgeA) {
        for (int32_t b : kEdgeB) {
            Int64EmuCase c{a, b, a, b, a, b, 0, 0, 0};
            const int32_t k = kEdgeK[i % kEdgeK.size()];
            const int32_t s = kEdgeScrP[i % kEdgeScrP.size()];
            const int32_t p = kEdgeScrP[(i / kEdgeScrP.size()) % kEdgeScrP.size()];
            c.k = k;
            c.scr = s;
            c.p = p;
            cases.push_back(c);
            cases.push_back({a, b, ~a, b, a, ~b, k, ~s, p}); // ~x = -x - 1 without overflow
            ++i;
        }
    }
    for (int32_t k : kEdgeK) {
        for (int32_t s : kEdgeScrP) {
            for (int32_t p : kEdgeScrP) {
                cases.push_back({8191, kMax32, 8191, kMax32, 8191, kMax32, k, s, p});
                cases.push_back({-8192, kMin32, -8192, kMin32, -8192, kMin32, k, s, p});
            }
        }
    }
    CaseGen gen(seed);
    while (cases.size() < count) {
        cases.push_back(gen.Case());
    }
    cases.resize(count);
    return cases;
}

} // namespace

TEST_CASE("RotDotShr10 and RotCoordShr equal the int64_t expressions on edge values", "[vdp2][params]") {
    for (int32_t a0 : kEdgeA) {
        for (int32_t b0 : kEdgeB) {
            for (int32_t a1 : kEdgeA) {
                for (int32_t b1 : kEdgeB) {
                    // Third term from a small rotation of the edges.
                    const int32_t a2 = kEdgeA[(a0 & 7) % kEdgeA.size()];
                    const int32_t b2 = kEdgeB[(static_cast<uint32_t>(b1) >> 3) % kEdgeB.size()];
                    const int32_t got = RotDotShr10(a0, b0, a1, b1, a2, b2);
                    const int32_t want = RefDotShr10(a0, b0, a1, b1, a2, b2);
                    if (got != want) {
                        FAIL("RotDotShr10(" << a0 << ", " << b0 << ", " << a1 << ", " << b1 << ", " << a2 << ", "
                                            << b2 << ") = " << got << ", expected " << want);
                    }
                }
            }
        }
    }
    // All three terms at the same extreme (the largest partial sums).
    for (int32_t a : {-8192, 8191}) {
        for (int32_t b : {kMin32, kMax32}) {
            CHECK(RotDotShr10(a, b, a, b, a, b) == RefDotShr10(a, b, a, b, a, b));
        }
    }
    for (int32_t k : kEdgeK) {
        for (int32_t scr : kEdgeScrP) {
            for (int32_t p : kEdgeScrP) {
                const uint32_t got = RotCoordShr(k, scr, p);
                const uint32_t want = RefCoordShr(k, scr, p);
                if (got != want) {
                    FAIL("RotCoordShr(" << k << ", " << scr << ", " << p << ") = " << got << ", expected " << want);
                }
            }
        }
    }
    // constexpr in C++.
    STATIC_CHECK(RotDotShr10(8191, kMax32, 8191, kMax32, 8191, kMax32) ==
                 static_cast<int32_t>((3 * int64_t(8191) * kMax32) >> 10));
    STATIC_CHECK(RotCoordShr(kMin32, kMin32, kMin32) == static_cast<uint32_t>(((int64_t(1) << 46) + kMin32) >> 10));
}

TEST_CASE("RotDotShr10 and RotCoordShr equal the int64_t expressions on 1,000,000 random cases", "[vdp2][params]") {
    CaseGen gen(0xB817u);
    size_t dotFailures = 0;
    size_t coordFailures = 0;
    for (int i = 0; i < 1'000'000; ++i) {
        const Int64EmuCase c = gen.Case();
        const int32_t dot = RotDotShr10(c.a0, c.b0, c.a1, c.b1, c.a2, c.b2);
        const int32_t wantDot = RefDotShr10(c.a0, c.b0, c.a1, c.b1, c.a2, c.b2);
        if (dot != wantDot && dotFailures++ == 0) {
            UNSCOPED_INFO("RotDotShr10(" << c.a0 << ", " << c.b0 << ", " << c.a1 << ", " << c.b1 << ", " << c.a2
                                         << ", " << c.b2 << ") = " << dot << ", expected " << wantDot);
        }
        const uint32_t coord = RotCoordShr(c.k, c.scr, c.p);
        const uint32_t wantCoord = RefCoordShr(c.k, c.scr, c.p);
        if (coord != wantCoord && coordFailures++ == 0) {
            UNSCOPED_INFO("RotCoordShr(" << c.k << ", " << c.scr << ", " << c.p << ") = " << coord << ", expected "
                                         << wantCoord);
        }
    }
    CHECK(dotFailures == 0);
    CHECK(coordFailures == 0);
}

#define CHECK_OFFSET(type, member, constant) CHECK(offsetof(type, member) == sh::constant)
#define CHECK_SIZE(type, constant) CHECK(sizeof(type) == sh::constant)

// The push-constant block as each of the three VDP2 shaders declares it.
#define CHECK_COMMON_PARAMS(shader)                                                                                    \
    do {                                                                                                               \
        CHECK_OFFSET(VDP2CommonRenderParams, startY, k##shader##_CommonRenderParams_startY);                           \
        CHECK_OFFSET(VDP2CommonRenderParams, displayParams, k##shader##_CommonRenderParams_displayParams);             \
        CHECK_OFFSET(VDP2CommonRenderParams, layerParams, k##shader##_CommonRenderParams_layerParams);                 \
        CHECK_OFFSET(VDP2CommonRenderParams, rotParams, k##shader##_CommonRenderParams_rotParams);                     \
        CHECK_OFFSET(VDP2CommonRenderParams, spriteParams, k##shader##_CommonRenderParams_spriteParams);               \
        CHECK_OFFSET(VDP2CommonRenderParams, borderColorModeLatch,                                                     \
                     k##shader##_CommonRenderParams_borderColorModeLatch);                                             \
        CHECK_OFFSET(VDP2CommonRenderParams, spritePriosRatios, k##shader##_CommonRenderParams_spritePriosRatios);     \
        CHECK_OFFSET(VDP2CommonRenderParams, vcellScroll, k##shader##_CommonRenderParams_vcellScroll);                 \
        CHECK_OFFSET(VDP2CommonRenderParams, windows, k##shader##_CommonRenderParams_windows);                         \
        CHECK_OFFSET(VDP2CommonRenderParams, enhancements, k##shader##_CommonRenderParams_enhancements);               \
        CHECK_OFFSET(VDP2CommonRenderParams, outputSize, k##shader##_CommonRenderParams_outputSize);                   \
        CHECK_SIZE(VDP2CommonRenderParams, k##shader##_CommonRenderParams_size);                                       \
    } while (false)

TEST_CASE("VDP2 host parameter structs match the reflected std430 layouts", "[vdp2][params]") {
    SECTION("VDP2CommonRenderParams (push constants)") {
        CHECK_COMMON_PARAMS(Vdp2RenderSprite);
        CHECK_COMMON_PARAMS(Vdp2RenderBgs);
        CHECK_COMMON_PARAMS(Vdp2Compose);
        STATIC_CHECK(offsetof(VDP2CommonRenderParams, borderColorModeLatch) == 20);
        STATIC_CHECK(offsetof(VDP2CommonRenderParams, spritePriosRatios) == 24);
        STATIC_CHECK(offsetof(VDP2CommonRenderParams, outputSize) == 44);
        CHECK(sizeof(VDP2CommonRenderParams) <= 128); // the push-constant minimum
    }
    SECTION("The sprite shader declares the layer parameters and rotation bases as the background shader does") {
        CHECK(sh::kVdp2RenderSprite_LayerRenderParams_size == sh::kVdp2RenderBgs_LayerRenderParams_size);
        CHECK(sh::kVdp2RenderSprite_LayerRenderParams_nbg == sh::kVdp2RenderBgs_LayerRenderParams_nbg);
        CHECK(sh::kVdp2RenderSprite_LayerRenderParams_rbg == sh::kVdp2RenderBgs_LayerRenderParams_rbg);
        CHECK(sh::kVdp2RenderSprite_LayerRenderParams_windows == sh::kVdp2RenderBgs_LayerRenderParams_windows);
        CHECK(sh::kVdp2RenderSprite_LayerRenderParams_windows_stride ==
              sh::kVdp2RenderBgs_LayerRenderParams_windows_stride);
        CHECK(sh::kVdp2RenderSprite_LayerRenderParams_rotWindows == sh::kVdp2RenderBgs_LayerRenderParams_rotWindows);
        CHECK(sh::kVdp2RenderSprite_LayerRenderParams_lineScreenParams ==
              sh::kVdp2RenderBgs_LayerRenderParams_lineScreenParams);
        CHECK(sh::kVdp2RenderSprite_LayerRenderParams_backScreenParams ==
              sh::kVdp2RenderBgs_LayerRenderParams_backScreenParams);
        CHECK(sh::kVdp2RenderSprite_LayerRenderParams_specialFunctionCodes ==
              sh::kVdp2RenderBgs_LayerRenderParams_specialFunctionCodes);
        CHECK(sh::kVdp2RenderSprite_GlobalWindowParams_size == sh::kVdp2RenderBgs_GlobalWindowParams_size);
        CHECK(sh::kVdp2RenderSprite_NBGParams_size == sh::kVdp2RenderBgs_NBGParams_size);
        CHECK(sh::kVdp2RenderSprite_RBGParams_size == sh::kVdp2RenderBgs_RBGParams_size);
        CHECK(sh::kVdp2RenderSprite_BaseBGParams_size == sh::kVdp2RenderBgs_BaseBGParams_size);
        CHECK(sh::kVdp2RenderSprite_RotParamBase_size == sh::kVdp2RenderBgs_RotParamBase_size);
        CHECK(sh::kVdp2RenderSprite_RotParamBase_KA == sh::kVdp2RenderBgs_RotParamBase_KA);
    }
    SECTION("VDP2ComposeParams") {
        CHECK_OFFSET(VDP2ComposeParams, colorCalcEnable, kVdp2Compose_ComposeParams_colorCalcEnable);
        CHECK_OFFSET(VDP2ComposeParams, colorOffsetEnable, kVdp2Compose_ComposeParams_colorOffsetEnable);
        CHECK_OFFSET(VDP2ComposeParams, colorOffsetSelect, kVdp2Compose_ComposeParams_colorOffsetSelect);
        CHECK_OFFSET(VDP2ComposeParams, lineColorEnable, kVdp2Compose_ComposeParams_lineColorEnable);
        CHECK_OFFSET(VDP2ComposeParams, colorOffsetA, kVdp2Compose_ComposeParams_colorOffsetA);
        CHECK_OFFSET(VDP2ComposeParams, shadowEnable, kVdp2Compose_ComposeParams_shadowEnable);
        CHECK_OFFSET(VDP2ComposeParams, colorOffsetB, kVdp2Compose_ComposeParams_colorOffsetB);
        CHECK_OFFSET(VDP2ComposeParams, bgColorCalcRatios, kVdp2Compose_ComposeParams_bgColorCalcRatios);
        CHECK(sizeof(decltype(VDP2ComposeParams::bgColorCalcRatios)::value_type) ==
              sh::kVdp2Compose_ComposeParams_bgColorCalcRatios_stride);
        CHECK_OFFSET(VDP2ComposeParams, backLineColorCalcRatios,
                     kVdp2Compose_ComposeParams_backLineColorCalcRatios);
        CHECK(sizeof(decltype(VDP2ComposeParams::backLineColorCalcRatios)::value_type) ==
              sh::kVdp2Compose_ComposeParams_backLineColorCalcRatios_stride);
        CHECK_OFFSET(VDP2ComposeParams, pad, kVdp2Compose_ComposeParams_pad);
        CHECK_SIZE(VDP2ComposeParams, kVdp2Compose_ComposeParams_size);
        // The inventory's std430 offsets (section 5.2).
        STATIC_CHECK(offsetof(VDP2ComposeParams, colorOffsetB) == 32);
        STATIC_CHECK(offsetof(VDP2ComposeParams, bgColorCalcRatios) == 44);
        STATIC_CHECK(offsetof(VDP2ComposeParams, backLineColorCalcRatios) == 64);
        STATIC_CHECK(sizeof(VDP2ComposeParams) == 80);
    }
    SECTION("VDP2RotParamBase") {
        CHECK_OFFSET(VDP2RotParamBase, tableAddress, kVdp2RenderBgs_RotParamBase_tableAddress);
        CHECK_OFFSET(VDP2RotParamBase, Xst, kVdp2RenderBgs_RotParamBase_Xst);
        CHECK_OFFSET(VDP2RotParamBase, Yst, kVdp2RenderBgs_RotParamBase_Yst);
        CHECK_OFFSET(VDP2RotParamBase, KA, kVdp2RenderBgs_RotParamBase_KA);
        CHECK_SIZE(VDP2RotParamBase, kVdp2RenderBgs_RotParamBase_size);
    }
    SECTION("VDP2NBGLine") {
        CHECK_OFFSET(VDP2NBGLine, fracScrollX, kVdp2RenderBgs_NBGLine_fracScrollX);
        CHECK_OFFSET(VDP2NBGLine, fracScrollY, kVdp2RenderBgs_NBGLine_fracScrollY);
        CHECK_OFFSET(VDP2NBGLine, scrollIncH, kVdp2RenderBgs_NBGLine_scrollIncH);
        CHECK_OFFSET(VDP2NBGLine, mosaicSrcLine, kVdp2RenderBgs_NBGLine_mosaicSrcLine);
        CHECK_OFFSET(VDP2NBGLine, prevLastVCellScroll, kVdp2RenderBgs_NBGLine_prevLastVCellScroll);
        CHECK_OFFSET(VDP2NBGLine, prevLineEndScrollX, kVdp2RenderBgs_NBGLine_prevLineEndScrollX);
        CHECK_OFFSET(VDP2NBGLine, prevLineEndScrollY, kVdp2RenderBgs_NBGLine_prevLineEndScrollY);
        CHECK_OFFSET(VDP2NBGLine, pad, kVdp2RenderBgs_NBGLine_pad);
        CHECK_SIZE(VDP2NBGLine, kVdp2RenderBgs_NBGLine_size);
        STATIC_CHECK(sizeof(VDP2NBGLine) == 32);
    }
    SECTION("VDP2LayerRenderParams") {
        CHECK_OFFSET(VDP2LayerRenderParams, nbg, kVdp2RenderBgs_LayerRenderParams_nbg);
        CHECK(sizeof(NBGParams) == sh::kVdp2RenderBgs_LayerRenderParams_nbg_stride);
        CHECK_OFFSET(VDP2LayerRenderParams, rbg, kVdp2RenderBgs_LayerRenderParams_rbg);
        CHECK(sizeof(RBGParams) == sh::kVdp2RenderBgs_LayerRenderParams_rbg_stride);
        CHECK_OFFSET(VDP2LayerRenderParams, windows, kVdp2RenderBgs_LayerRenderParams_windows);
        CHECK(sizeof(VDP2GlobalWindowParams) == sh::kVdp2RenderBgs_LayerRenderParams_windows_stride);
        CHECK_OFFSET(VDP2LayerRenderParams, rotWindows, kVdp2RenderBgs_LayerRenderParams_rotWindows);
        CHECK_OFFSET(VDP2LayerRenderParams, lineScreenParams, kVdp2RenderBgs_LayerRenderParams_lineScreenParams);
        CHECK_OFFSET(VDP2LayerRenderParams, backScreenParams, kVdp2RenderBgs_LayerRenderParams_backScreenParams);
        CHECK_OFFSET(VDP2LayerRenderParams, specialFunctionCodes,
                     kVdp2RenderBgs_LayerRenderParams_specialFunctionCodes);
        CHECK_SIZE(VDP2LayerRenderParams, kVdp2RenderBgs_LayerRenderParams_size);
        STATIC_CHECK(sizeof(VDP2LayerRenderParams) == 1464);
    }
    SECTION("NBGParams") {
        CHECK_OFFSET(NBGParams, base, kVdp2RenderBgs_NBGParams_base);
        CHECK_OFFSET(NBGParams, scrollAmount, kVdp2RenderBgs_NBGParams_scrollAmount);
        CHECK_OFFSET(NBGParams, scrollInc, kVdp2RenderBgs_NBGParams_scrollInc);
        CHECK_OFFSET(NBGParams, pageBaseAddresses, kVdp2RenderBgs_NBGParams_pageBaseAddresses);
        CHECK(sizeof(decltype(NBGParams::pageBaseAddresses)::value_type) ==
              sh::kVdp2RenderBgs_NBGParams_pageBaseAddresses_stride);
        CHECK_OFFSET(NBGParams, vcellScrollEnable, kVdp2RenderBgs_NBGParams_vcellScrollEnable);
        CHECK_OFFSET(NBGParams, lineScrollXEnable, kVdp2RenderBgs_NBGParams_lineScrollXEnable);
        CHECK_OFFSET(NBGParams, lineScrollYEnable, kVdp2RenderBgs_NBGParams_lineScrollYEnable);
        CHECK_OFFSET(NBGParams, lineZoomEnable, kVdp2RenderBgs_NBGParams_lineZoomEnable);
        CHECK_OFFSET(NBGParams, lineScrollInterval, kVdp2RenderBgs_NBGParams_lineScrollInterval);
        CHECK_OFFSET(NBGParams, lineScrollTableAddress, kVdp2RenderBgs_NBGParams_lineScrollTableAddress);
        CHECK_OFFSET(NBGParams, vcellScrollOffset, kVdp2RenderBgs_NBGParams_vcellScrollOffset);
        CHECK_OFFSET(NBGParams, vcellScrollDelay, kVdp2RenderBgs_NBGParams_vcellScrollDelay);
        CHECK_OFFSET(NBGParams, vcellScrollRepeat, kVdp2RenderBgs_NBGParams_vcellScrollRepeat);
        CHECK_OFFSET(NBGParams, scrollYIncBase, kVdp2RenderBgs_NBGParams_scrollYIncBase);
        CHECK_SIZE(NBGParams, kVdp2RenderBgs_NBGParams_size);
    }
    SECTION("RBGParams") {
        CHECK_OFFSET(RBGParams, base, kVdp2RenderBgs_RBGParams_base);
        CHECK_OFFSET(RBGParams, screenOverProcess, kVdp2RenderBgs_RBGParams_screenOverProcess);
        CHECK_OFFSET(RBGParams, screenOverPatternName, kVdp2RenderBgs_RBGParams_screenOverPatternName);
        CHECK_OFFSET(RBGParams, pageBaseAddresses, kVdp2RenderBgs_RBGParams_pageBaseAddresses);
        CHECK(sizeof(decltype(RBGParams::pageBaseAddresses)::value_type) ==
              sh::kVdp2RenderBgs_RBGParams_pageBaseAddresses_stride);
        CHECK_SIZE(RBGParams, kVdp2RenderBgs_RBGParams_size);
    }
    SECTION("VDP2BaseBGParams") {
        CHECK_OFFSET(VDP2BaseBGParams, enabled, kVdp2RenderBgs_BaseBGParams_enabled);
        CHECK_OFFSET(VDP2BaseBGParams, enableTransparency, kVdp2RenderBgs_BaseBGParams_enableTransparency);
        CHECK_OFFSET(VDP2BaseBGParams, bitmap, kVdp2RenderBgs_BaseBGParams_bitmap);
        CHECK_OFFSET(VDP2BaseBGParams, priorityNumber, kVdp2RenderBgs_BaseBGParams_priorityNumber);
        CHECK_OFFSET(VDP2BaseBGParams, priorityMode, kVdp2RenderBgs_BaseBGParams_priorityMode);
        CHECK_OFFSET(VDP2BaseBGParams, specialFunctionSelect, kVdp2RenderBgs_BaseBGParams_specialFunctionSelect);
        CHECK_OFFSET(VDP2BaseBGParams, cellSizeShift, kVdp2RenderBgs_BaseBGParams_cellSizeShift);
        CHECK_OFFSET(VDP2BaseBGParams, colorFormat, kVdp2RenderBgs_BaseBGParams_colorFormat);
        CHECK_OFFSET(VDP2BaseBGParams, cramOffset, kVdp2RenderBgs_BaseBGParams_cramOffset);
        CHECK_OFFSET(VDP2BaseBGParams, supplScrollCharNum, kVdp2RenderBgs_BaseBGParams_supplScrollCharNum);
        CHECK_OFFSET(VDP2BaseBGParams, supplPalNum, kVdp2RenderBgs_BaseBGParams_supplPalNum);
        CHECK_OFFSET(VDP2BaseBGParams, supplSpecialColorCalc, kVdp2RenderBgs_BaseBGParams_supplSpecialColorCalc);
        CHECK_OFFSET(VDP2BaseBGParams, supplSpecialPriority, kVdp2RenderBgs_BaseBGParams_supplSpecialPriority);
        CHECK_OFFSET(VDP2BaseBGParams, mosaicEnable, kVdp2RenderBgs_BaseBGParams_mosaicEnable);
        CHECK_OFFSET(VDP2BaseBGParams, colorCalcEnable, kVdp2RenderBgs_BaseBGParams_colorCalcEnable);
        CHECK_OFFSET(VDP2BaseBGParams, extChar, kVdp2RenderBgs_BaseBGParams_extChar);
        CHECK_OFFSET(VDP2BaseBGParams, twoWordChar, kVdp2RenderBgs_BaseBGParams_twoWordChar);
        CHECK_OFFSET(VDP2BaseBGParams, patNameAccess, kVdp2RenderBgs_BaseBGParams_patNameAccess);
        CHECK_OFFSET(VDP2BaseBGParams, charPatAccess, kVdp2RenderBgs_BaseBGParams_charPatAccess);
        CHECK_OFFSET(VDP2BaseBGParams, charPatDelay, kVdp2RenderBgs_BaseBGParams_charPatDelay);
        CHECK_OFFSET(VDP2BaseBGParams, vramDataOffset, kVdp2RenderBgs_BaseBGParams_vramDataOffset);
        CHECK_OFFSET(VDP2BaseBGParams, specialColorCalcMode, kVdp2RenderBgs_BaseBGParams_specialColorCalcMode);
        CHECK_OFFSET(VDP2BaseBGParams, pageShift, kVdp2RenderBgs_BaseBGParams_pageShift);
        CHECK_OFFSET(VDP2BaseBGParams, bitmapSize, kVdp2RenderBgs_BaseBGParams_bitmapSize);
        CHECK_OFFSET(VDP2BaseBGParams, bitmapBaseAddress, kVdp2RenderBgs_BaseBGParams_bitmapBaseAddress);
        CHECK_OFFSET(VDP2BaseBGParams, windowParams, kVdp2RenderBgs_BaseBGParams_windowParams);
        CHECK_SIZE(VDP2BaseBGParams, kVdp2RenderBgs_BaseBGParams_size);
        STATIC_CHECK(offsetof(VDP2BaseBGParams, pageShift) == 88);
    }
    SECTION("Window and line/back screen params") {
        CHECK_OFFSET(VDP2GlobalWindowParams, start, kVdp2RenderBgs_GlobalWindowParams_start);
        CHECK_OFFSET(VDP2GlobalWindowParams, end, kVdp2RenderBgs_GlobalWindowParams_end);
        CHECK_OFFSET(VDP2GlobalWindowParams, lineWindowTableAddress,
                     kVdp2RenderBgs_GlobalWindowParams_lineWindowTableAddress);
        CHECK_OFFSET(VDP2GlobalWindowParams, lineWindowTableEnable,
                     kVdp2RenderBgs_GlobalWindowParams_lineWindowTableEnable);
        CHECK_SIZE(VDP2GlobalWindowParams, kVdp2RenderBgs_GlobalWindowParams_size);

        CHECK_OFFSET(VDP2LayerWindowParams, windowLogicAnd, kVdp2RenderBgs_LayerWindowParams_windowLogicAnd);
        CHECK_OFFSET(VDP2LayerWindowParams, window0Enable, kVdp2RenderBgs_LayerWindowParams_window0Enable);
        CHECK_OFFSET(VDP2LayerWindowParams, window0Invert, kVdp2RenderBgs_LayerWindowParams_window0Invert);
        CHECK_OFFSET(VDP2LayerWindowParams, window1Enable, kVdp2RenderBgs_LayerWindowParams_window1Enable);
        CHECK_OFFSET(VDP2LayerWindowParams, window1Invert, kVdp2RenderBgs_LayerWindowParams_window1Invert);
        CHECK_SIZE(VDP2LayerWindowParams, kVdp2RenderBgs_LayerWindowParams_size);

        CHECK_OFFSET(VDP2LayerWindowParamsS, base, kVdp2RenderBgs_LayerWindowParamsS_base);
        CHECK_OFFSET(VDP2LayerWindowParamsS, spriteWindowEnable,
                     kVdp2RenderBgs_LayerWindowParamsS_spriteWindowEnable);
        CHECK_OFFSET(VDP2LayerWindowParamsS, spriteWindowInvert,
                     kVdp2RenderBgs_LayerWindowParamsS_spriteWindowInvert);
        CHECK_SIZE(VDP2LayerWindowParamsS, kVdp2RenderBgs_LayerWindowParamsS_size);

        CHECK_OFFSET(VDP2LineBackScreenParams, baseAddress, kVdp2RenderBgs_LineBackScreenParams_baseAddress);
        CHECK_OFFSET(VDP2LineBackScreenParams, perLine, kVdp2RenderBgs_LineBackScreenParams_perLine);
        CHECK_SIZE(VDP2LineBackScreenParams, kVdp2RenderBgs_LineBackScreenParams_size);
    }
    SECTION("test_int64_emu buffers") {
        CHECK_OFFSET(Int64EmuCase, a0, kTestInt64Emu_Int64EmuCase_a0);
        CHECK_OFFSET(Int64EmuCase, b0, kTestInt64Emu_Int64EmuCase_b0);
        CHECK_OFFSET(Int64EmuCase, a1, kTestInt64Emu_Int64EmuCase_a1);
        CHECK_OFFSET(Int64EmuCase, b1, kTestInt64Emu_Int64EmuCase_b1);
        CHECK_OFFSET(Int64EmuCase, a2, kTestInt64Emu_Int64EmuCase_a2);
        CHECK_OFFSET(Int64EmuCase, b2, kTestInt64Emu_Int64EmuCase_b2);
        CHECK_OFFSET(Int64EmuCase, k, kTestInt64Emu_Int64EmuCase_k);
        CHECK_OFFSET(Int64EmuCase, scr, kTestInt64Emu_Int64EmuCase_scr);
        CHECK_OFFSET(Int64EmuCase, p, kTestInt64Emu_Int64EmuCase_p);
        CHECK_SIZE(Int64EmuCase, kTestInt64Emu_Int64EmuCase_size);
        CHECK_OFFSET(Int64EmuResult, dot, kTestInt64Emu_Int64EmuResult_dot);
        CHECK_OFFSET(Int64EmuResult, coord, kTestInt64Emu_Int64EmuResult_coord);
        CHECK_SIZE(Int64EmuResult, kTestInt64Emu_Int64EmuResult_size);
    }
}

TEST_CASE("VDP2 GPU limits", "[vdp2][params]") {
    STATIC_CHECK(kMaxResH == 704);
    STATIC_CHECK(kMaxResV == 512);
    STATIC_CHECK(sizeof(ColorR8G8B8A8) == 4);
    STATIC_CHECK(sizeof(CRAMColorCache) == kVDP2CRAMColorBufferSize);
    STATIC_CHECK(kVDP2CRAMColorBufferSize == 8192);
    STATIC_CHECK(kVDP2CRAMRotCoeffBufferSize == 2048);
}

#if BRIMIR_VULKAN

namespace {

namespace vu = brimir::gpu::vk_util;
using brimir::test::CloseAndCheckValidation;
using brimir::test::OpenDevice;

// Runs test_int64_emu over `cases` and returns the results.
std::vector<Int64EmuResult> RunInt64Emu(brimir::gpu::VulkanDevice &device, const std::vector<Int64EmuCase> &cases) {
    const auto &vk = device.Table();
    const auto count = static_cast<uint32_t>(cases.size());
    std::vector<Int64EmuResult> results;

    vu::Buffer input;
    vu::Buffer output;
    constexpr VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    bool ok = vu::CreateBuffer(device, count * sizeof(Int64EmuCase), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kHost,
                               input) == VK_SUCCESS;
    ok = ok && vu::CreateBuffer(device, count * sizeof(Int64EmuResult), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kHost,
                                output) == VK_SUCCESS;
    ok = ok && input.mapped != nullptr && output.mapped != nullptr;
    if (ok) {
        std::memcpy(input.mapped, cases.data(), count * sizeof(Int64EmuCase));
        std::memset(output.mapped, 0xCD, count * sizeof(Int64EmuResult));
    }

    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    ok = ok && vu::CreateDescriptorSetLayout(device, bindings, setLayout) == VK_SUCCESS;
    ok = ok && vu::CreateDescriptorPool(device, {&poolSize, 1}, 1, pool) == VK_SUCCESS;
    ok = ok && vu::AllocateDescriptorSet(device, pool, setLayout, set) == VK_SUCCESS;
    ok = ok && vu::CreatePipelineLayout(device, {&setLayout, 1}, {}, pipelineLayout) == VK_SUCCESS;
    ok = ok && vu::CreateComputePipeline(device, sh::kTestInt64EmuSpv, pipelineLayout, pipeline) == VK_SUCCESS;
    if (ok) {
        vu::WriteStorageBuffer(device, set, 0, input);
        vu::WriteStorageBuffer(device, set, 16, output);
        ok = vu::SubmitOneShot(device, [&](VkCommandBuffer cmd) {
                 vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                 vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0,
                                            nullptr);
                 vk.vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
                 vu::GlobalBarrier(device, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
             }) == VK_SUCCESS;
    }
    if (ok) {
        const auto *words = static_cast<const Int64EmuResult *>(output.mapped);
        results.assign(words, words + count);
    }

    vk.vkDestroyPipeline(device.Device(), pipeline, nullptr);
    vk.vkDestroyPipelineLayout(device.Device(), pipelineLayout, nullptr);
    vk.vkDestroyDescriptorPool(device.Device(), pool, nullptr);
    vk.vkDestroyDescriptorSetLayout(device.Device(), setLayout, nullptr);
    vu::DestroyBuffer(device, output);
    vu::DestroyBuffer(device, input);
    REQUIRE(ok);
    return results;
}

} // namespace

TEST_CASE("int64_emu.hlsli equals the C++ mirror on 65,536 cases", "[gpu][vdp2][params]") {
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);

    const std::vector<Int64EmuCase> cases = MakeCases(65536, 0x1B0Bu);
    const std::vector<Int64EmuResult> results = RunInt64Emu(*device, cases);
    REQUIRE(results.size() == cases.size());

    size_t dotFailures = 0;
    size_t coordFailures = 0;
    for (size_t i = 0; i < cases.size(); ++i) {
        const Int64EmuCase &c = cases[i];
        const int32_t dot = RotDotShr10(c.a0, c.b0, c.a1, c.b1, c.a2, c.b2);
        const uint32_t coord = RotCoordShr(c.k, c.scr, c.p);
        // The mirror is itself checked against int64_t above; check it here too so a GPU pass means "exact".
        REQUIRE(dot == RefDotShr10(c.a0, c.b0, c.a1, c.b1, c.a2, c.b2));
        REQUIRE(coord == RefCoordShr(c.k, c.scr, c.p));
        if (results[i].dot != dot && dotFailures++ == 0) {
            UNSCOPED_INFO("case " << i << ": GPU RotDotShr10(" << c.a0 << ", " << c.b0 << ", " << c.a1 << ", " << c.b1
                                  << ", " << c.a2 << ", " << c.b2 << ") = " << results[i].dot << ", expected " << dot);
        }
        if (results[i].coord != coord && coordFailures++ == 0) {
            UNSCOPED_INFO("case " << i << ": GPU RotCoordShr(" << c.k << ", " << c.scr << ", " << c.p
                                  << ") = " << results[i].coord << ", expected " << coord);
        }
    }
    CHECK(dotFailures == 0);
    CHECK(coordFailures == 0);
    CloseAndCheckValidation(device);
}

#endif // BRIMIR_VULKAN
