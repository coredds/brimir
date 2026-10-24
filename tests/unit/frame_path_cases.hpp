// Brimir - shared frame path cases for the bridge characterization tests and the GPU output pass tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0
//
// FramePathCase describes one native frame size plus the bridge settings applied to it.
// CpuFramePath() runs a PatternXBGR frame through the REAL bridge code (CoreWrapper::OnFrameComplete, reached
// through the BRIMIR_BUILD_TESTS seam TestInjectFrame) and returns the XRGB8888 frame the libretro layer would
// hand to video_cb. A GPU output pass must reproduce it (alpha aside).
//
// `deinterlace` has no effect on the bridge: the bridge never post-processes fields, the setting only reaches
// Ymir's VDP enhancement flag (which decides how the native frame is drawn, upstream of OnFrameComplete).
// CpuFramePath therefore ignores it. It is carried for GPU tests, where the output pass must be exercised with
// the setting on and off and give the same pixels.

#pragma once

#include <brimir/core_wrapper.hpp>

#include <cstdint>
#include <vector>

namespace brimir::test {

struct FramePathCase {
    uint32_t w, h;          // native frame size
    uint32_t cropH, cropV;  // total overscan crop in pixels per axis, as passed to SetOverscanCrop
    uint32_t rotation;      // 0, 90, 180, 270
    bool deinterlace;       // see header comment
};

/// @brief Deterministic native pixel (Ymir XBGR8888, 0xAABBGGRR, opaque alpha). The 24 colour bits are a bijection
/// of the pixel index, so every pixel of a frame up to 2^24 pixels is distinct and R differs from B.
inline uint32_t PatternXBGR(uint32_t x, uint32_t y, uint32_t w, uint32_t /*h*/) {
    const uint32_t index = y * w + x;
    const uint32_t colour = (index * 0x9E3779u + 0x123457u) & 0x00FFFFFFu;
    return 0xFF000000u | colour;
}

inline std::vector<uint32_t> MakePatternFrame(uint32_t w, uint32_t h) {
    std::vector<uint32_t> frame(static_cast<size_t>(w) * h);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            frame[static_cast<size_t>(y) * w + x] = PatternXBGR(x, y, w, h);
        }
    }
    return frame;
}

/// @brief The bridge's XRGB8888 output (0x00RRGGBB, tightly packed) for a PatternXBGR frame of the case.
/// @param outW receives the output width, if not null
/// @param outH receives the output height, if not null
inline std::vector<uint32_t> CpuFramePath(const FramePathCase& c, uint32_t* outW = nullptr,
                                          uint32_t* outH = nullptr) {
    CoreWrapper core;
    core.SetRotation(static_cast<int>(c.rotation));
    core.SetOverscanCrop(static_cast<int>(c.cropH), static_cast<int>(c.cropV));

    const std::vector<uint32_t> frame = MakePatternFrame(c.w, c.h);
    core.TestInjectFrame(frame.data(), c.w, c.h);

    const uint32_t w = core.GetFramebufferWidth();
    const uint32_t h = core.GetFramebufferHeight();
    const uint32_t* pixels = static_cast<const uint32_t*>(core.GetFramebuffer());
    if (outW) {
        *outW = w;
    }
    if (outH) {
        *outH = h;
    }
    return std::vector<uint32_t>(pixels, pixels + static_cast<size_t>(w) * h);
}

/// @brief Every combination of native size (320/352/640/704 x 224/240/448/480/512), overscan option (0/16/32/48
/// px per axis, the brimir_overscan values none/small/medium/large) and rotation. `deinterlace` is false; GPU
/// tests repeat each case with it on.
inline const std::vector<FramePathCase>& FramePathCases() {
    static const std::vector<FramePathCase> cases = [] {
        std::vector<FramePathCase> list;
        const uint32_t widths[] = {320, 352, 640, 704};
        const uint32_t heights[] = {224, 240, 448, 480, 512};
        const uint32_t crops[] = {0, 16, 32, 48};
        const uint32_t rotations[] = {0, 90, 180, 270};
        for (uint32_t w : widths) {
            for (uint32_t h : heights) {
                for (uint32_t crop : crops) {
                    for (uint32_t rotation : rotations) {
                        list.push_back({w, h, crop, crop, rotation, false});
                    }
                }
            }
        }
        return list;
    }();
    return cases;
}

/// @brief One case per rotation with a non-square crop (even and odd totals, so the left/top rounding is
/// pinned), plus cases where the bridge's crop guard (frame must exceed the crop by more than 32 px on both
/// axes) rejects the crop and passes the frame through uncropped.
inline const std::vector<FramePathCase>& FramePathExtraCases() {
    static const std::vector<FramePathCase> cases = {
        {352, 240, 48, 16, 0, false},   {352, 240, 48, 16, 90, false},
        {352, 240, 48, 16, 180, false}, {352, 240, 48, 16, 270, false},
        {320, 224, 21, 9, 0, false},    {320, 224, 21, 9, 90, false},
        {320, 224, 21, 9, 180, false},  {320, 224, 21, 9, 270, false},
        {704, 480, 17, 49, 90, true},   {704, 480, 17, 49, 270, true},
        // guard rejects: width 320 is not > 300 + 32; height 224 is not > 200 + 32
        {320, 224, 300, 0, 0, false},   {320, 224, 300, 0, 90, false},
        {320, 224, 0, 200, 180, false}, {320, 224, 16, 200, 270, false},
    };
    return cases;
}

} // namespace brimir::test
