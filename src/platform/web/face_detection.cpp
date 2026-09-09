#include "platform/face_detection.h"

namespace sidescopes {

FaceDetectionResult detectFaces(const FrameView&, float)
{
    return {FaceDetectionStatus::Unsupported, {}};
}

bool supportsFaceDetection()
{
    return false;
}

}  // namespace sidescopes
