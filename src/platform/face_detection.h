#pragma once

#include <vector>

#include "core/frame.h"

namespace sidescopes {

/// Whether this platform ships a built-in face detector. Where it does
/// not, the face-picking action is simply unavailable.
[[nodiscard]] bool supportsFaceDetection();

/// Called once at startup, where a platform wants its face-detection model
/// loaded ahead of the first detection. Neither platform does: both measure
/// the warm-up as memory charged to every session against a saving most
/// never collect, so the model is loaded by the first real detection.
void warmFaceDetection();

/// Face rectangles in frame pixels, largest first: the detector's own
/// boxes, unpadded. Faces smaller than a plausible scoping target
/// (thumbnails, filmstrips) are dropped; @p pixelsPerPoint scales that
/// size floor to the frame's density. Runs synchronously on the caller's
/// thread and entirely offline, returning an owned vector of at most eight
/// faces, largest first.
[[nodiscard]] std::vector<IntRect> detectFaces(const FrameView& frame, float pixelsPerPoint);

}  // namespace sidescopes
