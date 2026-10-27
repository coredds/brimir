#pragma once

// Brimir: quad termination for VDP1 polygons and sprites, shared by the software renderer and the GPU renderer's front
// half (Brimir file, not in upstream Ymir; see src/core/BRIMIR_FORK.md).
//
// Hardware draws every line of a quad. Stopping early is only an optimisation, so it must never skip a line that could
// write a pixel. Upstream stopped after the first line that "plotted" nothing once earlier lines had (two runs for
// QuadStepper::IsDegenerate() quads), where "plotted" depended on user clipping, the clipping mode, texels and end
// codes. That dropped visible lines: a transparent sprite row in clipping mode 1, rotated quads re-entering the user
// clip area, bowtie and concave quads with a second lobe, and lines that graze a corner of the system clip area.
//
// Here the quad stops only when no remaining line can reach the system clip area:
// - Every remaining line runs from the left edge's position to the right edge's position at some later quad step.
//   An edge stepper moves each coordinate by at most one per step and only in one direction (Edge::Step), so its
//   remaining positions lie in the box spanned by its current and its final position. The final positions are found
//   by stepping copies of the edges, so they are exactly what the stepper produces, 13-bit wrapped deltas and the
//   4095 step mask included.
// - A line's pixels (main and antialias) lie in the box of its end points grown by one pixel, as long as the line
//   steppers' 13-bit accumulators do not wrap. That holds for lines shorter than kMaxLineDelta on both axes, so the
//   quad never stops while the remaining end points span that much (tests: "VDP1 line pixels stay in the end point box").

#include <ymir/hw/vdp/renderer/common/vdp1_steppers.hpp>

#include <algorithm>

namespace ymir::vdp {

class VDP1QuadTermination {
public:
    // Largest remaining end-point box (width or height) for which the quad may stop
    static constexpr sint32 kMaxLineDelta = 1023;

    // Call before the quad's first Step(). clipH and clipV are the system clip area's right and bottom coordinates
    // (clipV with the double-density doubling applied).
    template <sint32 t_bits>
    FORCE_INLINE VDP1QuadTermination(const QuadStepper<t_bits> &quad, sint32 clipH, sint32 clipV)
        : m_clipH(clipH)
        , m_clipV(clipV) {
        QuadStepper<t_bits> copy = quad;
        m_finalL = copy.LeftEdge().Coord();
        m_finalR = copy.RightEdge().Coord();
        for (copy.Step(); copy.CanStep(); copy.Step()) {
            m_finalL = copy.LeftEdge().Coord();
            m_finalR = copy.RightEdge().Coord();
        }
    }

    // Returns true if no line from the quad's current step to its last can write a pixel inside the system clip area.
    template <sint32 t_bits>
    FORCE_INLINE bool Done(const QuadStepper<t_bits> &quad) const {
        const CoordS32 l = quad.LeftEdge().Coord();
        const CoordS32 r = quad.RightEdge().Coord();
        const sint32 minX = std::min({l.x(), r.x(), m_finalL.x(), m_finalR.x()});
        const sint32 maxX = std::max({l.x(), r.x(), m_finalL.x(), m_finalR.x()});
        const sint32 minY = std::min({l.y(), r.y(), m_finalL.y(), m_finalR.y()});
        const sint32 maxY = std::max({l.y(), r.y(), m_finalL.y(), m_finalR.y()});
        if (maxX - minX > kMaxLineDelta || maxY - minY > kMaxLineDelta) {
            return false;
        }
        // One pixel of margin for the antialias pixels
        return maxX + 1 < 0 || minX - 1 > m_clipH || maxY + 1 < 0 || minY - 1 > m_clipV;
    }

private:
    CoordS32 m_finalL;
    CoordS32 m_finalR;
    sint32 m_clipH;
    sint32 m_clipV;
};

} // namespace ymir::vdp
