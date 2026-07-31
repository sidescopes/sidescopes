#pragma once

#include <memory>

#include "platform/screen_capture.h"

namespace sidescopes {

/// The X11 screen-capture backend: MIT-SHM reads of the root window, the
/// direct path a pure X11 session offers - no desktop portal, no PipeWire, no
/// consent dialog, the same shape macOS and Windows capture takes. Chosen by
/// createScreenCaptureSource only on an X11 session; a Wayland session falls
/// to the portal, whose stream is the only way to see native Wayland pixels.
[[nodiscard]] std::unique_ptr<ScreenCaptureSource> createX11ShmScreenCaptureSource();

}  // namespace sidescopes
