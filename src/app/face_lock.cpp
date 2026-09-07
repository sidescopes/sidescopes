#include "app/face_lock.h"

namespace sidescopes {
namespace face_lock {
FaceLockState makeLock(const FaceAnchor& anchor, const LockRect& crop)
{
    FaceLockState state;
    state.lastAnchor = anchor;
    rebindCrop(state, crop);

    return state;
}

LockRect mapRegion(const FaceLockState& state, const FaceAnchor& anchor)
{
    const double centerX = anchor.centerX + state.offsetX * anchor.width;
    const double centerY = anchor.centerY + state.offsetY * anchor.width;
    const double halfWidth = state.sizeX * anchor.width / 2.0;
    const double halfHeight = state.sizeY * anchor.width / 2.0;

    return LockRect{centerX - halfWidth, centerY - halfHeight, centerX + halfWidth, centerY + halfHeight};
}

void rebindCrop(FaceLockState& state, const LockRect& crop)
{
    if (state.lastAnchor.width <= 0.0) {
        return;
    }
    const double width = state.lastAnchor.width;
    state.offsetX = ((crop.left + crop.right) / 2.0 - state.lastAnchor.centerX) / width;
    state.offsetY = ((crop.top + crop.bottom) / 2.0 - state.lastAnchor.centerY) / width;
    state.sizeX = (crop.right - crop.left) / width;
    state.sizeY = (crop.bottom - crop.top) / width;
}

void translate(FaceLockState& state, double dxPixels, double dyPixels)
{
    state.lastAnchor.centerX += dxPixels;
    state.lastAnchor.centerY += dyPixels;
}

bool trustworthyBox(const LockRect& box, const LockRect& bounds)
{
    constexpr double FlushClearance = 4.0;
    return box.left - bounds.left >= FlushClearance && bounds.right - box.right >= FlushClearance;
}

}  // namespace face_lock
}  // namespace sidescopes
