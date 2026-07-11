#pragma once

#include <vector>

#include "core/frame.h"

namespace sidescopes {

// Face rectangles in frame pixels, largest first. Detection runs on the
// platform's built-in face detector (Vision on macOS), entirely offline.
// Rectangles are padded beyond the detector's eyebrows-to-chin box so the
// surrounding skin joins the sample - the point of scoping a face is
// judging skin, not features - and faces smaller than a plausible scoping
// target (thumbnails, filmstrips) are dropped. `pixels_per_point` scales
// that size floor to the frame's density.
std::vector<IntRect> DetectFaces(const FrameView& frame, float pixels_per_point);

}  // namespace sidescopes
