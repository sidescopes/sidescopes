#pragma once

#include "app/face_lock.h"
#include "core/analysis_worker.h"

namespace sidescopes {

/// Maps a display-percent region onto a frame's pixel grid, where the face lock
/// does its geometry.
[[nodiscard]] LockRect lockRectFromPercent(const RegionOfInterest& region, int frameWidth, int frameHeight);

/// The inverse of @ref lockRectFromPercent: a pixel-grid rectangle back to a
/// display-percent region.
[[nodiscard]] RegionOfInterest percentFromLockRect(const LockRect& rect, int frameWidth, int frameHeight);

}  // namespace sidescopes
