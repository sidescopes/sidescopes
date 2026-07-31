#pragma once

#include <X11/Xlib.h>

#include <optional>

#include "core/frame.h"

namespace sidescopes {

/// The pin mode's crosshair, with the sampled colour riding beside it, as a
/// CURSOR rather than as something the sheet paints.
///
/// The macOS and Windows pin cursors carry the same note and it is the reason
/// this is not overlay drawing: a swatch painted into the overlay always
/// trails the pointer by a composition frame, because the cursor image rides
/// its own plane. Only a cursor keeps up with the hand.
///
/// @param color The colour under the pointer, or nothing before the first
/// sample - then the crosshair is drawn alone.
/// @return A cursor owned by this unit, reused across calls and rebuilt only
/// when the rounded colour changes, or None where the server cannot make one.
[[nodiscard]] ::Cursor pinCursor(const std::optional<FloatColor>& color);

}  // namespace sidescopes
