// Face detection, answered for a browser: there is none.
//
// The desktop builds use Vision and WinRT respectively. Neither has a browser
// counterpart worth pretending to, and a detector that guessed would break the
// picker's own rule that it suggests only exact information.
//
// This is a refusal rather than a stub. `supportsFaceDetection()` returning
// false is what removes the face tool from the toolbar, the context menu and
// the keyboard, so nothing offers a control that cannot work.

#include "platform/face_detection.h"

#include <vector>

#include "core/frame.h"

namespace sidescopes {

std::vector<IntRect> detectFaces(const FrameView&, float)
{
    return {};
}

bool supportsFaceDetection()
{
    return false;
}

void warmFaceDetection()
{
    // Nothing to warm. The Windows layer's own body is empty too, for a
    // different reason - it measured the warm-up as pure cost - and both are
    // answers rather than gaps.
}

}  // namespace sidescopes
