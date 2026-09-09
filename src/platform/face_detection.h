#pragma once

#include <vector>

#include "core/frame.h"

namespace sidescopes {

enum class FaceDetectionStatus
{
    Completed,
    Failed,
    Unsupported
};

struct FaceDetectionResult
{
    FaceDetectionStatus status = FaceDetectionStatus::Failed;
    std::vector<IntRect> faces;
};

/// Whether this platform ships a built-in face detector. Where it does
/// not, the face-picking action is simply unavailable.
[[nodiscard]] bool supportsFaceDetection();

/// Face rectangles in frame pixels, largest first: the detector's own
/// boxes, unpadded. Faces smaller than a plausible scoping target
/// (thumbnails, filmstrips) are dropped; @p pixelsPerPoint scales that
/// size floor to the frame's density. Runs synchronously and entirely
/// offline, returning at most eight faces. An empty
/// completed result is distinct from a failed or unsupported native call.
[[nodiscard]] FaceDetectionResult detectFaces(const FrameView& frame, float pixelsPerPoint);

}  // namespace sidescopes
