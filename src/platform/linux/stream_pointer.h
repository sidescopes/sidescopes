#pragma once

#include <optional>

#include "platform/desktop.h"

namespace sidescopes {

/// Where a capture stream's frames sit on the desktop, and how large those
/// frames are. A cursor position arrives from the stream in FRAME pixels and
/// has to leave as a desktop point, and the two spaces differ whenever the
/// compositor scales an output - so both are carried rather than assumed
/// equal.
///
/// widthPoints/heightPoints are what the portal says the stream covers. A
/// portal that states neither leaves them zero, which reads as "the frame is
/// its own space" - true of every unscaled output and the only honest guess
/// when nothing was said.
struct StreamPlacement
{
    double originX = 0.0;  ///< The stream's top-left in global desktop points.
    double originY = 0.0;
    double widthPoints = 0.0;
    double heightPoints = 0.0;
    int frameWidth = 0;  ///< The negotiated video size, in pixels.
    int frameHeight = 0;
};

/// The widest cursor bitmap, per side in pixels, the stream declares it can
/// accept metadata for.
///
/// MEASURED, and the whole reason the live probe was dead on Wayland for a
/// day: a cursor metadata block is negotiated by SIZE RANGE, and GNOME's
/// compositor asks for 589872 bytes - a 384x384 bitmap. A consumer whose range
/// stops below that does not get a smaller block, or a warning, or an error.
/// The ranges simply fail to intersect, the metadata is dropped from the
/// buffer, and the only symptom is a pointer position that never arrives. This
/// stood at 256 and cost an afternoon of looking everywhere else.
///
/// So: never lower it, and only raise it. The bitmap itself is never read -
/// only the position beside it - so the size buys nothing but the intersection.
inline constexpr int CursorBitmapSide = 384;

/// The desktop point a cursor sitting at @p frameX, @p frameY in @p placement's
/// frame stands on, or nothing when the position cannot mean anything: no
/// frame to scale from, or a point outside the frame.
///
/// Outside the frame is not an error to clamp - a compositor reports the
/// cursor's position only while it is over the captured output, so a point
/// past the edge means the pointer has left, and answering with the nearest
/// edge would pin a live probe to the border instead of letting it fall back.
[[nodiscard]] std::optional<DesktopPoint> streamPointToDesktop(const StreamPlacement& placement, int frameX,
                                                               int frameY);

/// Publishes where the capture stream last saw the pointer, in global desktop
/// points; empty clears it. Called from the PipeWire thread as buffers arrive,
/// and on stream stop - a position that outlives its stream describes a
/// pointer that has been free to move unobserved ever since.
void publishStreamPointer(const std::optional<DesktopPoint>& pointer);

/// The freshest published position, or nothing when no stream is reporting
/// one. Read from the main thread once a frame.
///
/// It does not go stale on its own: a compositor sends a frame for cursor
/// motion as well as for damage, so silence means the pointer stopped and the
/// last position is still exactly where it is. Only stream stop clears it.
[[nodiscard]] std::optional<DesktopPoint> streamPointer();

}  // namespace sidescopes
