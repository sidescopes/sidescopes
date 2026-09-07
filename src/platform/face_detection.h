#pragma once

#include <memory>
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

/// A synchronous detector owned, called and destroyed by one analysis thread.
/// Keeping the native model alive avoids recreating it for each video frame.
/// An empty successful result remains distinct from a failed native call.
class FaceDetectionSession
{
public:
    virtual ~FaceDetectionSession() = default;
    [[nodiscard]] virtual FaceDetectionResult detect(const FrameView& frame, double minimumPixels) = 0;
};

[[nodiscard]] std::unique_ptr<FaceDetectionSession> createFaceDetectionSession();

/// Whether this platform ships a built-in face detector. Where it does
/// not, the face-picking action is simply unavailable.
[[nodiscard]] bool supportsFaceDetection();

/// Face rectangles in frame pixels, largest first: the detector's own
/// boxes, unpadded. Faces smaller than a plausible scoping target
/// (thumbnails, filmstrips) are dropped; @p pixelsPerPoint scales that
/// size floor to the frame's density. Runs synchronously on the caller's
/// thread and entirely offline, returning an owned vector of at most eight
/// faces, largest first.
[[nodiscard]] std::vector<IntRect> detectFaces(const FrameView& frame, float pixelsPerPoint);

}  // namespace sidescopes
