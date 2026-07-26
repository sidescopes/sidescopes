#pragma once

#include <optional>

#include "app/face_lock.h"
#include "core/analysis_worker.h"

namespace sidescopes {

/// How far a region moved to become another, as per cent of the display: the
/// largest of the four edges' displacements.
///
/// The largest rather than the centre's, because the two gestures move a
/// rectangle differently: a region thrown across the picture carries all four
/// edges together, while one being drawn pins two and throws the other two, and
/// its centre then travels at half the speed of the hand. Either way the fastest
/// edge is the hand. A region appearing or disappearing has no travel - there is
/// no previous rectangle to have moved from.
[[nodiscard]] double regionTravelPercent(const std::optional<RegionOfInterest>& from,
                                         const std::optional<RegionOfInterest>& to);

/// Maps a display-percent region onto a frame's pixel grid, where the face lock
/// does its geometry.
[[nodiscard]] LockRect lockRectFromPercent(const RegionOfInterest& region, int frameWidth, int frameHeight);

/// The inverse of @ref lockRectFromPercent: a pixel-grid rectangle back to a
/// display-percent region.
[[nodiscard]] RegionOfInterest percentFromLockRect(const LockRect& rect, int frameWidth, int frameHeight);

}  // namespace sidescopes
