#pragma once

#include <array>

namespace sidescopes {

/// An axis-aligned rectangle in points.
///
/// Deliberately not an ImGui type. This unit is arithmetic that decides
/// whether the region border draws correctly, and keeping it free of Dear
/// ImGui and of Emscripten is what lets it be tested on any machine rather
/// than only inside a browser - the same trade the lab's image adjustments
/// already make.
struct BandRect
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    [[nodiscard]] bool empty() const;
    [[nodiscard]] float area() const;
    [[nodiscard]] bool operator==(const BandRect&) const = default;
};

/// The hazard band's four quarters, as CLIP rectangles: the ring between
/// @p outer and @p hole, cut into a top, a bottom and two sides.
///
/// Every edge is rounded to a whole point, and that is the point of the
/// function rather than a detail of it. A clip becomes a scissor box, which is
/// integers, and the renderer truncates when it converts - so a boundary on a
/// fractional coordinate can land INSIDE both of the quarters that share it.
/// Each quarter draws the whole ruling, so that row then takes the stripe ink
/// twice and a light line appears across the band, exactly where two quarters
/// meet. Rounding first hands both the same number and the scissors abut.
///
/// The three properties worth holding this to, all of which have been broken
/// in practice: adjacent quarters share EQUAL edges, no two quarters overlap,
/// and together they cover the ring exactly - no gap, which would show as a
/// hairline of picture between the band and the region's own edge.
[[nodiscard]] std::array<BandRect, 4> bandQuarters(const BandRect& outer, const BandRect& hole);

}  // namespace sidescopes
