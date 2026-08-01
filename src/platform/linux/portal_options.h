#pragma once

#include <cstdint>

namespace sidescopes {

/// What the session captures: a whole monitor, or a single window the
/// compositor's own picker chooses. A window stream IS that window and follows
/// it, which is how a native Wayland window - one with no queryable screen
/// rectangle - is attached to.
enum class PortalSourceKind
{
    Monitor,
    Window
};

/// The portal's `types` bitmask for a source kind: MONITOR (1) or WINDOW (2),
/// from the ScreenCast interface.
[[nodiscard]] uint32_t portalSourceTypeMask(PortalSourceKind kind);

/// The `cursor_mode` to ask SelectSources for, given the portal's
/// AvailableCursorModes bitmask.
[[nodiscard]] uint32_t portalCursorMode(uint32_t availableModes);

}  // namespace sidescopes
