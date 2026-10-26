#pragma once

// A CPU reference of the VDP2 sub-pixel rules at internal resolution N (stage1c-upscaling-research.md sections 1.2-1.4,
// the stage 1C Task 4 brief), written from the hardware's fixed-point definitions with 64-bit arithmetic, independently
// of the shaders and of the front half: given a frame's registers and rotation parameter tables as the hardware
// integers, it computes which background dot (u, v) every N x composite pixel shows. The coordinate-bitmap scenes
// (test_gpu_vdp2_scenes.cpp) draw a bitmap whose texels encode their own (u, v) and compare the decoded N x composite
// with this, pixel by pixel. At N = 1 it is the hardware's native evaluation (the scenes check it against software).
//
// The rule: a quantity that is affine in the screen position within one native line is evaluated at a sample as
// `native value + (sub * step) / span`, with C++ integer division (truncation toward zero, as HLSL int division);
// sub = 0 gives the native value exactly. Native line state (scroll, line zoom, rotation bases) is per native line.

#include <cstdint>
#include <functional>
#include <vector>

namespace brimir::test::subpixel {

enum class Interlace { None, SingleDensity, DoubleDensity };

struct Geometry {
    uint32_t n = 1;      // internal resolution N
    bool hiRes = false;  // 640/704 dots wide
    Interlace interlace = Interlace::None;
    bool deinterlace = false; // both fields drawn every frame (else each field writes its own composite rows)
};

/// @brief value(sub) = (sub * step) / span, truncated toward zero.
int64_t SubOffset(int64_t sub, int64_t step, int64_t span);

/// @brief A dot position, before the layer's wrap (the caller masks it to the bitmap size).
struct Dot {
    int64_t u = 0, v = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// NBG: scroll (11.8), zoom (3.8), line scroll X and line zoom per VDP line, vertical zoom.

struct NBGFrame {
    Geometry geometry;
    int64_t scrollX = 0, scrollY = 0;    // SCXIN/SCXDN, SCYIN/SCYDN (11.8)
    int64_t incH = 0x100, incV = 0x100;  // ZMXIN/ZMXDN, ZMYIN/ZMYDN (3.8)
    // Per VDP line (progressive modes only), when not empty: the line scroll X table value (11.8, replacing the start
    // of the line; the register scroll is added to it) and the horizontal increment (line zoom, or the register value
    // of that line).
    std::vector<int64_t> lineScrollX;
    std::vector<int64_t> lineIncH;
};

/// @brief The NBG dot of N x composite pixel (X, Y).
///
/// Native state of composite row Yc: progressive, line Yc: fy = scrollY + Yc * incV; double density: composite rows are
/// one increment apart (both fields, woven or deinterlaced): fy = scrollY + Yc * incV; single density: both rows of a
/// pair show line Yc / 2: fy = scrollY + (Yc / 2) * incV. Sample (i, j) of native pixel (x, Yc):
///   u = (fx + x * incH + (i * incH) / N) >> 8
///   v = (fy + (j * incV) / N) >> 8, or in single density at N > 1 (t = (Yc & 1) * N + j):
///   v = (fy + (t * incV) / 2N) >> 8.
Dot NBGDotAt(const NBGFrame &frame, uint32_t X, uint32_t Y);

// ---------------------------------------------------------------------------------------------------------------------
// RBG: rotation parameter table values as the hardware integers.

struct RotParams {
    int64_t Xst = 0, Yst = 0, Zst = 0;       // 13.10
    int64_t deltaXst = 0, deltaYst = 0;      // 3.10
    int64_t deltaX = 0, deltaY = 0;          // 3.10
    int64_t A = 0, B = 0, C = 0, D = 0, E = 0, F = 0; // 4.10
    int64_t Px = 0, Py = 0, Pz = 0, Cx = 0, Cy = 0, Cz = 0; // integers
    int64_t Mx = 0, My = 0;                  // 14.10
    int64_t kx = 0x10000, ky = 0x10000;      // 8.16
    int64_t KAst = 0, dKAst = 0, dKAx = 0;   // 16.10, 10.10, 10.10 (relative to the table's address offset)
};

struct RBGFrame {
    Geometry geometry;
    RotParams table;
    // From this VDP line on (Xst and Yst re-read at that line, as RPRCTL requests), the table is `reloaded`.
    uint32_t reloadLine = UINT32_MAX;
    RotParams reloaded;
    // Coefficients (mode 0: kx = ky = the coefficient, 8.16) by entry index (KA >> 10), when set.
    std::function<int64_t(uint32_t entry)> coefficient;
};

/// @brief The RBG dot of N x composite pixel (X, Y).
///
/// RBG dot x = output column (/ 2 in hi-res), sub-position s = i, or (column & 1) * N + i over S = 2 in hi-res; RBG line
/// l = composite row (/ 2 in interlaced modes), sub-line t = j, or (row & 1) * N + j over R = 2 when deinterlacing an
/// interlaced mode (R = 2, t = j when each field writes its own rows); s = t = 0 at N = 1. With the table T of line l
/// and its bases Xst(l), Yst(l) (accumulated per line by deltaXst/deltaYst, re-read at the reload line):
///   Xst' = Xst(l) + (t * T.deltaXst) / (R N), Yst' likewise
///   Xsp = (A (Xst' - Px 2^10) + B (Yst' - Py 2^10) + C (Zst - Pz 2^10)) >> 10, Ysp with D, E, F
///   Xp = A (Px - Cx) + B (Py - Cy) + C (Pz - Cz) + Cx 2^10 + Mx, Yp likewise
///   dXh = (A deltaX + B deltaY) >> 10, dYh = (D deltaX + E deltaY) >> 10
///   k = the coefficient of entry (KAst + l dKAst + x dKAx) >> 10 (native x and l), else kx/ky
///   scrX = Xsp + x dXh + (s dXh) / (S N), scrY likewise
///   u = ((kx scrX >> 16) + Xp) >> 10, v likewise.
Dot RBGDotAt(const RBGFrame &frame, uint32_t X, uint32_t Y);

} // namespace brimir::test::subpixel
