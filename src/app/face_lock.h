#pragma once

namespace sidescopes {

/// A rectangle in isotropic display pixels, independent of a capture crop.
struct LockRect
{
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
};

struct FaceAnchor
{
    double centerX = 0.0;
    double centerY = 0.0;
    double width = 0.0;
};

/// The user's crop relative to a face anchor. Width-relative coordinates
/// preserve a forehead or cheek selection as the face moves and changes size.
/// Detection and uncertainty policy live separately in face_tracking.
struct FaceLockState
{
    double offsetX = 0.0;
    double offsetY = 0.0;
    double sizeX = 1.0;
    double sizeY = 1.0;
    FaceAnchor lastAnchor;
};

namespace face_lock {

[[nodiscard]] FaceLockState makeLock(const FaceAnchor& anchor, const LockRect& crop);
[[nodiscard]] LockRect mapRegion(const FaceLockState& state, const FaceAnchor& anchor);
void rebindCrop(FaceLockState& state, const LockRect& crop);
void translate(FaceLockState& state, double dxPixels, double dyPixels);

/// Side clipping corrupts width, the crop's scale reference. Top or bottom
/// clipping preserves that reference and does not alone disqualify a face.
[[nodiscard]] bool trustworthyBox(const LockRect& box, const LockRect& bounds);

}  // namespace face_lock
}  // namespace sidescopes
