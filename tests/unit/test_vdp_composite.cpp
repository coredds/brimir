#include "catch_amalgamated.hpp"

#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/util/data_ops.hpp>

#include <algorithm>
#include <array>
#include <memory>

namespace {

using namespace ymir;
using namespace ymir::vdp;

bool Selected(unsigned pattern, uint32 x, uint32 width) {
    switch (pattern) {
    case 0: return true;
    case 1: return false;
    case 2: return (x & 1) == 0;
    case 3: return (x & 7) < 4;
    case 4: return x == 0;
    case 5: return x == width - 1;
    case 6: return x != 0;
    default: return x != width - 1;
    }
}

struct CompositeFrame {
    std::array<uint32, kMaxResH> line{};
    uint32 width = 0;
    uint32 height = 0;

    void Render(SoftwareVDPRenderer &renderer, uint32 requestedWidth) {
        renderer.PostLoadStateSync();
        renderer.SwCallbacks.FrameComplete.Rebind(
            this, [](uint32 *fb, uint32 w, uint32 h, void *context) {
                auto &out = *static_cast<CompositeFrame *>(context);
                std::copy_n(fb, w, out.line.begin());
                out.width = w;
                out.height = h;
            });
        renderer.VDP2SetResolution(requestedWidth, 224, false);
        renderer.VDP2BeginFrame();
        renderer.VDP2RenderLine(0);
        renderer.VDP2EndFrame();
        REQUIRE(width == requestedWidth);
        REQUIRE(height == 224);
    }
};

} // namespace

TEST_CASE("VDP sprite blending and shadow match scalar RGB across lanes and masks", "[vdp][composite][regression]") {
    using namespace ymir;
    using namespace ymir::vdp;

    const uint32 mode = GENERATE(0u, 1u, 2u, 3u);
    constexpr std::array<uint32, 4> widths{320, 352, 640, 704};
    const uint32 width = widths[mode];
    const uint32 scale = mode < 2 ? 1 : 2;
    const unsigned maskPattern = GENERATE(0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u);
    const bool additive = GENERATE(false, true);
    const bool shadow = GENERATE(false, true);
    const uint16 back = GENERATE(uint16{0x51AF}, uint16{0}, uint16{0x7FFF});
    CAPTURE(width, maskPattern, additive, shadow, back);

    auto state = std::make_unique<VDPState>();
    config::VDP2DebugRender debug{};
    config::VDP2AccessPatternsConfig access{};
    auto renderer = std::make_unique<SoftwareVDPRenderer>(*state, debug, access);

    auto &regs = state->regs2;
    regs.WriteTVMD(static_cast<uint16>(0x8000 | mode));
    regs.LatchTVMD();
    regs.WriteBGON(0);
    regs.WriteSPCTL(shadow ? 0x1102 : 0x1100); // Type 2 adds MSB shadow; blend only priority 1.
    regs.WritePRISA(0x0201); // Selector 0 -> priority 1; selector 1 -> priority 2.
    regs.WriteCCCTL(additive ? 0x0140 : 0x0040);

    constexpr std::array<uint8, 8> ratios{0, 1, 4, 8, 16, 24, 30, 31};
    regs.WriteCCRSA((31 - ratios[0]) | ((31 - ratios[1]) << 8));
    regs.WriteCCRSB((31 - ratios[2]) | ((31 - ratios[3]) << 8));
    regs.WriteCCRSC((31 - ratios[4]) | ((31 - ratios[5]) << 8));
    regs.WriteCCRSD((31 - ratios[6]) | ((31 - ratios[7]) << 8));

    constexpr std::array<uint16, 4> palette{0x7FFF, 0x0000, 0x1234, 0x5A63};
    regs.WriteBKTAU(0);
    regs.WriteBKTAL(0x0080);
    state->mem2.WriteVRAM<uint16>(0x100, back);
    for (uint32 i = 0; i < palette.size(); ++i) {
        state->mem2.WriteCRAM<uint16>((i + 1) * 2, palette[i]);
    }

    const auto blendEnabled = [&](uint32 x) { return Selected(maskPattern, x, width / scale); };
    const auto shadowEnabled = [&](uint32 x) { return shadow && Selected(maskPattern, x, width / scale); };
    for (uint32 x = 0; x < width / scale; ++x) {
        const uint16 pixel = static_cast<uint16>((1 + (x % palette.size())) | ((x & 7) << 11) |
                                                 (!blendEnabled(x) << 14) | (shadowEnabled(x) << 15));
        util::WriteBE<uint16>(&state->spriteFB[state->displayFB][2 * x], pixel);
    }
    CompositeFrame frame;
    frame.Render(*renderer, width);

    // Normal scanline widths exercise SIMD blocks and the renderer's scalar tail.
    for (uint32 x = 0; x < width; ++x) {
        CAPTURE(x);
        const uint32 sx = x / scale;
        uint32 expected = 0xFF000000u;
        for (uint32 channel = 0; channel < 3; ++channel) {
            const int top = ((palette[sx % palette.size()] >> (channel * 5)) & 31) * 8;
            const int bottom = ((back >> (channel * 5)) & 31) * 8;
            int value = top;
            if (blendEnabled(sx)) {
                value = additive ? std::min(top + bottom, 255) : bottom + (((top - bottom) * ratios[sx & 7]) >> 5);
            }
            if (shadowEnabled(sx)) {
                value /= 2;
            }
            expected |= static_cast<uint32>(value) << (channel * 8);
        }
        CHECK(frame.line[x] == expected);
    }
}

TEST_CASE("VDP sprite gradation matches scalar selection and averages", "[vdp][composite][gradation][regression]") {
    const uint32 mode = GENERATE(0u, 1u, 2u, 3u);
    constexpr std::array<uint32, 4> widths{320, 352, 640, 704};
    const uint32 width = widths[mode];
    const uint32 scale = mode < 2 ? 1 : 2;
    const unsigned maskPattern = GENERATE(0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u);
    const unsigned colors = GENERATE(0u, 1u, 2u);
    CAPTURE(width, maskPattern, colors);

    auto state = std::make_unique<VDPState>();
    config::VDP2DebugRender debug{};
    config::VDP2AccessPatternsConfig access{};
    auto renderer = std::make_unique<SoftwareVDPRenderer>(*state, debug, access);
    auto &regs = state->regs2;
    regs.WriteTVMD(static_cast<uint16>(0x8000 | mode));
    regs.LatchTVMD();
    regs.WriteBGON(0);
    regs.WriteSPCTL(0x1100);
    regs.WritePRISA(0x0001); // Priority zero removes the sprite from the layer stack, not its source color.
    regs.WriteCCCTL(0x8040); // Sprite gradation and color calculation.
    regs.WriteCCRSA(31); // Ratio zero exposes the gradation (second) screen directly.
    constexpr uint16 back = 0x51AF;
    regs.WriteBKTAU(0);
    regs.WriteBKTAL(0x0080);
    state->mem2.WriteVRAM<uint16>(0x100, back);
    constexpr std::array<uint16, 8> palette{0x7FFF, 0, 0, 0x7FFF, 0x0001, 0x03E0, 0x7C00, 0x1234};
    for (uint32 i = 0; i < palette.size(); ++i) {
        state->mem2.WriteCRAM<uint16>((i + 1) * 2, palette[i]);
    }
    const auto colorIndex = [&](uint32 x) { return colors == 0 ? x % palette.size() : colors - 1; };
    const auto present = [&](uint32 x) { return Selected(maskPattern, x / scale, width / scale); };
    for (uint32 x = 0; x < width / scale; ++x) {
        const uint16 pixel = static_cast<uint16>((1 + colorIndex(x)) | (!present(x * scale) << 14));
        util::WriteBE<uint16>(&state->spriteFB[state->displayFB][2 * x], pixel);
    }
    CompositeFrame frame;
    frame.Render(*renderer, width);
    for (uint32 x = 0; x < width; ++x) {
        CAPTURE(x);
        uint32 expected = 0xFF000000u;
        for (uint32 channel = 0; channel < 3; ++channel) {
            const auto source = [&](uint32 pos) {
                return ((palette[colorIndex(pos / scale)] >> (channel * 5)) & 31) * 8;
            };
            uint32 value = ((back >> (channel * 5)) & 31) * 8;
            if (mode < 2 && present(x)) {
                value = source(x);
                if (x == 1) {
                    value = (source(0) + source(1)) / 2;
                } else if (x >= 2 && present(x - 2)) {
                    // Match the existing caller's unshifted mask and two-pixel destination offset.
                    value = ((source(x - 2) + source(x - 1)) / 2 + source(x)) / 2;
                }
            }
            expected |= value << (channel * 8);
        }
        CHECK(frame.line[x] == expected);
    }
}

TEST_CASE("VDP layered line selection and extended averages match scalar RGB", "[vdp][composite][layers]") {
    const uint32 mode = GENERATE(0u, 1u, 2u, 3u);
    constexpr std::array<uint32, 4> widths{320, 352, 640, 704};
    const uint32 width = widths[mode];
    const uint32 scale = mode < 2 ? 1 : 2;
    const unsigned maskPattern = GENERATE(0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u);
    enum class Operation { SelectLine, AverageSecond, AverageLine };
    const auto operation = GENERATE(Operation::SelectLine, Operation::AverageSecond, Operation::AverageLine);
    const uint16 back = GENERATE(uint16{0}, uint16{0x7FFF}, uint16{0x51AF});
    CAPTURE(width, maskPattern, operation, back);

    auto state = std::make_unique<VDPState>();
    config::VDP2DebugRender debug{};
    config::VDP2AccessPatternsConfig access{};
    auto renderer = std::make_unique<SoftwareVDPRenderer>(*state, debug, access);
    auto &regs = state->regs2;
    regs.WriteTVMD(static_cast<uint16>(0x8000 | mode));
    regs.LatchTVMD();
    regs.WriteBGON(0x0101); // Opaque NBG0 bitmap, with a sprite above or below it.
    regs.WriteCHCTLA(mode < 2 ? 0x004A : 0x003A); // 1024-wide RGB888 (normal) or RGB555 (hi-res).
    regs.WriteMPOFN(1); // Bitmap in bank A1; line/back tables remain in A0.
    regs.WriteCYCA0L(0x4444);
    regs.WriteCYCA0U(0x4444);
    regs.WriteCYCA1L(0x4444);
    regs.WriteCYCA1U(0x4444);
    const bool averageSecond = operation == Operation::AverageSecond;
    regs.WritePRINA(averageSecond ? 3 : 2);
    regs.WritePRISA(averageSecond ? 0x0201 : 0x0003);
    regs.WriteSPCTL(averageSecond ? 0x1100 : 0x1300);
    regs.WriteCCCTL(operation == Operation::SelectLine ? 0x0041 : 0x0461);
    regs.WriteCCRSA(31);
    regs.WriteCCRNA(31); // Ratio zero exposes the selected/averaged second screen.
    regs.WriteLNCLEN(averageSecond ? 0 : 0x0020); // Only sprites insert the line color.
    regs.WriteBKTAU(0);
    regs.WriteBKTAL(0x0080);
    regs.WriteLCTAU(0);
    regs.WriteLCTAL(0x0081);
    state->mem2.WriteVRAM<uint16>(0x100, back);
    state->mem2.WriteVRAM<uint16>(0x102, 16);
    constexpr uint16 line = 0x29D3;
    state->mem2.WriteCRAM<uint16>(32, line);
    constexpr std::array<uint16, 8> palette{0x7FFF, 0, 0x0001, 0x03E0, 0x7C00, 0x1234, 0x5A63, 0x7FFF};
    constexpr std::array<uint32, 8> bitmap{0xFFFFFF, 0, 0x010203, 0xFEFF00, 0xFF0080, 0x7F80FF, 0x123456, 0xFFFFFF};
    for (uint32 i = 0; i < palette.size(); ++i) {
        state->mem2.WriteCRAM<uint16>((i + 1) * 2, palette[i]);
    }
    for (uint32 x = 0; x < width; ++x) {
        if (mode < 2) {
            state->mem2.WriteVRAM<uint32>(0x20000 + 4 * x, 0x80000000u | bitmap[x % bitmap.size()]);
        } else {
            state->mem2.WriteVRAM<uint16>(0x20000 + 2 * x, 0x8000 | palette[x % palette.size()]);
        }
    }
    for (uint32 x = 0; x < width / scale; ++x) {
        const uint16 pixel = static_cast<uint16>((1 + x % palette.size()) |
                                                 (!Selected(maskPattern, x, width / scale) << 14));
        util::WriteBE<uint16>(&state->spriteFB[state->displayFB][2 * x], pixel);
    }
    CompositeFrame frame;
    frame.Render(*renderer, width);
    for (uint32 x = 0; x < width; ++x) {
        CAPTURE(x);
        const bool selected = Selected(maskPattern, x / scale, width / scale);
        uint32 expected = 0xFF000000u;
        for (uint32 channel = 0; channel < 3; ++channel) {
            const uint32 bottom = ((back >> (channel * 5)) & 31) * 8;
            const uint32 lineValue = ((line >> (channel * 5)) & 31) * 8;
            uint32 value = bottom;
            if (averageSecond) {
                value = ((palette[(x / scale) % palette.size()] >> (channel * 5)) & 31) * 8;
                if (mode < 2 && selected) {
                    value = (value + bottom) / 2;
                }
            } else if (selected) {
                value = lineValue;
                if (mode < 2 && operation == Operation::AverageLine) {
                    const uint32 bg = (bitmap[x % bitmap.size()] >> (channel * 8)) & 255;
                    value = ((bg + bottom) / 2 + lineValue) / 2;
                }
            }
            expected |= value << (channel * 8);
        }
        CHECK(frame.line[x] == expected);
    }
}
