#include "vdp2_subpixel_reference.hpp"

namespace brimir::test::subpixel {

int64_t SubOffset(int64_t sub, int64_t step, int64_t span) {
    return (sub * step) / span;
}

Dot NBGDotAt(const NBGFrame &frame, uint32_t X, uint32_t Y) {
    const Geometry &g = frame.geometry;
    const int64_t n = g.n;
    const uint32_t x = X / g.n, i = X % g.n;
    const uint32_t row = Y / g.n, j = Y % g.n;

    // The VDP line and the vertical position of the composite row's native pixel
    int64_t fy;
    uint32_t line;
    switch (g.interlace) {
    case Interlace::DoubleDensity:
        line = row / 2;
        fy = frame.scrollY + int64_t{row} * frame.incV;
        break;
    case Interlace::SingleDensity:
        line = row / 2;
        fy = frame.scrollY + int64_t{line} * frame.incV;
        break;
    default:
        line = row;
        fy = frame.scrollY + int64_t{row} * frame.incV;
        break;
    }
    const int64_t incH = line < frame.lineIncH.size() ? frame.lineIncH[line] : frame.incH;
    const int64_t fx = (line < frame.lineScrollX.size() ? frame.lineScrollX[line] : 0) + frame.scrollX;

    Dot dot;
    dot.u = (fx + int64_t{x} * incH + SubOffset(i, incH, n)) >> 8;
    if (g.interlace == Interlace::SingleDensity && n > 1) {
        const int64_t t = int64_t{row & 1} * n + j;
        dot.v = (fy + SubOffset(t, frame.incV, 2 * n)) >> 8;
    } else {
        dot.v = (fy + SubOffset(j, frame.incV, n)) >> 8;
    }
    return dot;
}

Dot RBGDotAt(const RBGFrame &frame, uint32_t X, uint32_t Y) {
    const Geometry &g = frame.geometry;
    const int64_t n = g.n;
    const bool interlaced = g.interlace != Interlace::None;
    const uint32_t column = X / g.n, i = X % g.n;
    const uint32_t row = Y / g.n, j = Y % g.n;

    // RBG dot and line, and the sample's position within them
    const int64_t x = g.hiRes ? column / 2 : column;
    const uint32_t line = interlaced ? row / 2 : row;
    int64_t s = 0, spanS = n, t = 0, spanT = n;
    if (n > 1) {
        s = i;
        t = j;
        if (g.hiRes) {
            s += int64_t{column & 1} * n;
            spanS = 2 * n;
        }
        if (interlaced) {
            spanT = 2 * n;
            if (g.deinterlace) {
                t += int64_t{row & 1} * n;
            }
        }
    }

    // The line's table and bases
    const bool reloaded = line >= frame.reloadLine;
    const RotParams &T = reloaded ? frame.reloaded : frame.table;
    const int64_t lines = reloaded ? line - frame.reloadLine : line;
    const int64_t baseXst = T.Xst + lines * T.deltaXst;
    const int64_t baseYst = T.Yst + lines * T.deltaYst;
    // KA is never re-read: it accumulates from line 0 (both tables have the same dKAst)
    const int64_t KA = frame.table.KAst + int64_t{line} * frame.table.dKAst + x * T.dKAx;

    const int64_t Xst = baseXst + SubOffset(t, T.deltaXst, spanT);
    const int64_t Yst = baseYst + SubOffset(t, T.deltaYst, spanT);
    const int64_t Tx = Xst - (T.Px << 10);
    const int64_t Ty = Yst - (T.Py << 10);
    const int64_t Tz = T.Zst - (T.Pz << 10);
    const int64_t Xsp = (T.A * Tx + T.B * Ty + T.C * Tz) >> 10;
    const int64_t Ysp = (T.D * Tx + T.E * Ty + T.F * Tz) >> 10;
    const int64_t Xp = T.A * (T.Px - T.Cx) + T.B * (T.Py - T.Cy) + T.C * (T.Pz - T.Cz) + (T.Cx << 10) + T.Mx;
    const int64_t Yp = T.D * (T.Px - T.Cx) + T.E * (T.Py - T.Cy) + T.F * (T.Pz - T.Cz) + (T.Cy << 10) + T.My;
    const int64_t dXh = (T.A * T.deltaX + T.B * T.deltaY) >> 10;
    const int64_t dYh = (T.D * T.deltaX + T.E * T.deltaY) >> 10;

    int64_t kx = T.kx, ky = T.ky;
    if (frame.coefficient) {
        kx = ky = frame.coefficient(static_cast<uint32_t>(KA >> 10));
    }

    const int64_t scrX = Xsp + x * dXh + SubOffset(s, dXh, spanS);
    const int64_t scrY = Ysp + x * dYh + SubOffset(s, dYh, spanS);
    return {((kx * scrX >> 16) + Xp) >> 10, ((ky * scrY >> 16) + Yp) >> 10};
}

} // namespace brimir::test::subpixel
