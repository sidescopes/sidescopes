#pragma once

#include <cstdint>

#include "core/frame.h"

namespace sidescopes {

/// Content fingerprint of @p region within a frame, used to skip re-analysis
/// when nothing inside the scoped region changed. Pixels inside @p masked are
/// excluded: the application masks its own window there, so its own redraws
/// never re-trigger analysis (without this, a region covering the display
/// turns the app's trace updates into a feedback loop that keeps analysis
/// running on an otherwise idle screen).
///
/// Samples every fourth row, including the trailing pixel of odd-width spans,
/// and mixes those words with the region size and pixel format using FNV's
/// xor-and-multiply step. Changes confined to unsampled rows are invisible.
[[nodiscard]] uint64_t hashRegion(const FrameView& frame, IntRect region, IntRect masked = IntRect{});

}  // namespace sidescopes
