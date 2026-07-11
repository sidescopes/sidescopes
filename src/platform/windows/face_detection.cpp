#include "platform/face_detection.h"

namespace sidescopes {

// Windows has WinRT's FaceDetector as a dependency-free future path; until
// the runtime work lands the face-picking action is simply not offered.
bool SupportsFaceDetection() {
    return false;
}

void WarmFaceDetection() {}

std::vector<IntRect> DetectFaces(const FrameView&, float) {
    return {};
}

}  // namespace sidescopes
