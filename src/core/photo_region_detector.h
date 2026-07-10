#pragma once

#include <vector>

#include "core/frame.h"

namespace sidescopes {

// A candidate rectangle for the scoped region.
struct RegionCandidate {
    IntRect rect;
    // 0..1: how much of the rectangle's perimeter is a real contrast edge.
    float confidence = 0.0f;
};

// Finds distinctive rectangles on screen: regions bounded by long, straight,
// axis-aligned contrast edges. Photographs displayed by any application are
// exactly that - whatever their content (smooth skies, black-and-white,
// downscaled to flatness), their boundary against the surroundings stays a
// sharp rectangle - and so are windows and panels.
//
// The design deliberately favors recall over precision: candidates feed
// hover-and-confirm suggestions, where an extra rectangle costs one glance
// but a missed photograph is a failure. Nested rectangles (a window and the
// photo inside it) are both reported; the picker highlights the smallest one
// under the cursor.
//
// `masked_regions` are areas whose pixels prove nothing about the desktop
// underneath - chiefly this application's own always-on-top window, which
// habitually floats over the very photo being scoped. Masked pixels count
// neither for nor against a rectangle: borders may pass beneath them, and
// content inside them (scope traces, graticule lines) yields no candidates.
//
// `pixels_per_point` is the frame's pixel density (2.0 on Retina displays).
// Geometric tolerances are defined in points - window chrome, corner radii,
// and photo sizes are laid out in points - and must grow with the density,
// or a rounded corner that a search absorbs at 1x pushes a border out of
// reach at 2x.
//
// Returns up to `max_candidates`, largest first. Deterministic.
std::vector<RegionCandidate> DetectPhotoRegions(const FrameView& frame,
                                                const std::vector<IntRect>& masked_regions = {},
                                                float pixels_per_point = 1.0f,
                                                int max_candidates = 8);

}  // namespace sidescopes
