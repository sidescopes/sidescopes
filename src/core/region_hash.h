#pragma once

#include <cstdint>

#include "core/frame.h"

namespace sidescopes {

/// Content fingerprint of @p region within a frame, used to skip re-analysis
/// when nothing inside the scoped region changed. Pixels inside @p masked are
/// excluded: the application masks its own window there, so its own redraws
/// never re-trigger analysis (without this, a full-screen region turns the
/// app's trace updates into a feedback loop that keeps analysis running on an
/// otherwise idle screen).
///
/// The hash samples every fourth row in 8-byte (two-pixel) chunks, dropping a
/// trailing odd pixel, which detects any real content change at roughly a
/// quarter of the cost of reading every pixel. FNV-1a over the sampled bytes.
[[nodiscard]] uint64_t hashRegion(const FrameView& frame, IntRect region, IntRect masked = IntRect{});

}  // namespace sidescopes
