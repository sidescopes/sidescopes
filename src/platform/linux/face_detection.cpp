// Linux ships no system face detector, so the face-picking action is simply
// unavailable; the platform contract makes that a first-class answer.

#include "platform/face_detection.h"

namespace sidescopes {

bool supportsFaceDetection()
{
    return false;
}

void warmFaceDetection()
{
}

std::vector<IntRect> detectFaces(const FrameView&, float)
{
    return {};
}

}  // namespace sidescopes
