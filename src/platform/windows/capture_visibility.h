#pragma once

namespace sidescopes {

/// Whether this application's windows should currently be visible to
/// screen captures: the user's session toggle, or the environment's
/// blanket SIDESCOPES_NO_CAPTURE_EXCLUSION. Overlay windows consult it
/// when they are created; setCaptureVisibility retunes the ones already
/// alive.
[[nodiscard]] bool captureWindowsVisible();

}  // namespace sidescopes
