#pragma once

#include <vector>

#include "core/frame.h"

namespace sidescopes {

/// Whether this platform ships a built-in face detector. Where it does
/// not, the face-picking action is simply unavailable.
[[nodiscard]] bool supportsFaceDetection();

/// Loads the platform's face-detection model in the background so the
/// first real detection is instant instead of stalling for the model
/// load. Safe to call once at startup; a no-op where unsupported.
void warmFaceDetection();

/// Face rectangles in frame pixels, largest first: the detector's own
/// boxes, unpadded. Faces smaller than a plausible scoping target
/// (thumbnails, filmstrips) are dropped; @p pixelsPerPoint scales that
/// size floor to the frame's density. Runs synchronously on the caller's
/// thread and entirely offline, returning an owned vector of at most eight
/// faces, largest first.
[[nodiscard]] std::vector<IntRect> detectFaces(const FrameView& frame, float pixelsPerPoint);

}  // namespace sidescopes
