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

/// @brief A dot position, before the layer's wrap (the caller masks it to the bitmap size). `transparent`: the layer
/// shows nothing there (an RBG dot whose coefficient is transparent).
struct Dot {
    int64_t u = 0, v = 0;
    bool transparent = false;
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
    // Vertical mosaic block height (progressive modes only; no horizontal mosaic): line Yc repeats line
    // Yc - Yc % mosaicV, vertically native (j = 0); horizontal sub-pixels stay.
    uint32_t mosaicV = 1;
    // Vertical cell scroll (11.8, when not empty): the value of the k-th table read of the line, k the cell columns
    // (8 dots) crossed from the line's start at the sample's own horizontal position (no repeat, no delay):
    //   k = ((fx & 0x7FF) + x * incH + (i * incH) / N) >> 11, added to fy.
    std::vector<int64_t> vcellScroll;
};

/// @brief The NBG dot of N x composite pixel (X, Y).
///
/// Native state of composite row Yc: progressive, line Yc: fy = scrollY + Yc * incV; double density: composite rows are
/// one increment apart (both fields, woven or deinterlaced): fy = scrollY + Yc * incV; single density: both rows of a
/// pair show line Yc / 2: fy = scrollY + (Yc / 2) * incV. Sample (i, j) of native pixel (x, Yc):
///   u = (fx + x * incH + (i * incH) / N) >> 8
///   v = (fy + vcell + (j * incV) / N) >> 8, or in single density at N > 1 (t = (Yc & 1) * N + j):
///   v = (fy + vcell + (t * incV) / 2N) >> 8.
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

enum class CoeffMode { ScaleXY, ScaleX, ScaleY, ViewpointX };

/// @brief A coefficient table entry as the hardware decodes it: the value (8.16 in the scale modes; in viewpoint X
/// mode the data shifted as the hardware does before Xp = value * 4) and the transparency bit. `fetchable` false: the
/// entry is in a VRAM bank not designated for coefficients (per-dot mode), read as transparent.
struct Coefficient {
    int64_t value = 0;
    bool transparent = false;
    bool fetchable = true;
};

struct RBGFrame {
    Geometry geometry;
    RotParams table;
    // From this VDP line on (Xst and Yst re-read at that line, as RPRCTL requests), the table is `reloaded`; with
    // reloadKAst also KA (re-read from reloaded.KAst), else KA keeps accumulating, by the dKAst of each line's table.
    uint32_t reloadLine = UINT32_MAX;
    RotParams reloaded;
    bool reloadKAst = false;
    // Coefficients by entry index (KA >> 10, relative to the table's address offset), when set.
    std::function<Coefficient(uint32_t entry)> coefficient;
    CoeffMode coeffMode = CoeffMode::ScaleXY;
};

/// @brief The RBG dot of N x composite pixel (X, Y).
///
/// RBG dot x = output column (/ 2 in hi-res), sub-position s = i, or (column & 1) * N + i over S = 2 in hi-res; RBG line
/// l = composite row (/ 2 in interlaced modes), sub-line t = j, or (row & 1) * N + j over R = 2 in an interlaced mode,
/// deinterlaced or woven (both fields' rows show line l from the same bases); s = t = 0 at N = 1. Exclusive monitor
/// modes are not modelled. With the table T of line l
/// and its bases Xst(l), Yst(l) (accumulated per line by deltaXst/deltaYst, re-read at the reload line):
///   Xst' = Xst(l) + (t * T.deltaXst) / (R N), Yst' likewise
///   Xsp = (A (Xst' - Px 2^10) + B (Yst' - Py 2^10) + C (Zst - Pz 2^10)) >> 10, Ysp with D, E, F
///   Xp = A (Px - Cx) + B (Py - Cy) + C (Pz - Cz) + Cx 2^10 + Mx, Yp likewise
///   dXh = (A deltaX + B deltaY) >> 10, dYh = (D deltaX + E deltaY) >> 10
///   KA = KA(l) + x dKAx (native x and l; KA(l) accumulated per line by each line's dKAst, re-read on a KAst reload)
///   c0 = the coefficient of entry KA >> 10; a transparent (or unfetchable) c0 makes the dot transparent
///   c = c0, or between lines (stage3-research.md section 4), when t != 0, the mode is a scale mode and the entry
///   c1 of (KA + T.dKAst) >> 10 is fetchable and not transparent and the pair passes the discontinuity guard (same
///   sign, 2 |c1 - c0| <= max(|c0|, |c1|)): c = c0 + ((c1 - c0) t) / (R N)
///   kx, ky = c per the mode (else T.kx/ky); viewpoint X: Xp = c0 * 4 (native)
///   scrX = Xsp + x dXh + (s dXh) / (S N), scrY likewise
///   u = ((kx scrX >> 16) + Xp) >> 10, v likewise.
Dot RBGDotAt(const RBGFrame &frame, uint32_t X, uint32_t Y);

/// @brief The discontinuity guard of the coefficient interpolation: c0 and c1 have the same sign and differ by at
/// most half the larger magnitude.
bool CoefficientsInterpolable(int64_t c0, int64_t c1);

} // namespace brimir::test::subpixel
