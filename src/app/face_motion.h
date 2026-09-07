#pragma once

#include <optional>

#include "app/face_lock.h"

namespace sidescopes {

/// Stabilizes accepted face geometry without changing identity association.
/// One gain for center and width preserves the crop's affine mapping and
/// keeps its edges between the previous and current valid crops.
/// Inputs are validated accepted anchors and monotonic source timestamps.
class FaceMotion
{
public:
    void reset(FaceLockState crop);
    void pause();
    [[nodiscard]] FaceAnchor follow(FaceAnchor target, double sourceSeconds);
    [[nodiscard]] FaceAnchor current() const;

private:
    FaceLockState m_crop;
    std::optional<double> m_sourceSeconds;
};

}  // namespace sidescopes
