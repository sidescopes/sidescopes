// The option values the SelectSources call carries. They are pure, and kept
// apart from the handshake that sends them, because each one decides something
// total and silent: the wrong source type asks the compositor for the wrong
// thing, and the wrong cursor mode either fails the handshake outright or
// leaves the live probe with no pointer to follow. A unit with no D-Bus in it
// is a unit a test can hold to those values.

#include "platform/linux/portal_options.h"

namespace sidescopes {
namespace {

// SelectSources option values, from the portal's ScreenCast interface.
constexpr uint32_t SourceTypeMonitor = 1;
constexpr uint32_t SourceTypeWindow = 2;
constexpr uint32_t CursorModeHidden = 1;
constexpr uint32_t CursorModeMetadata = 4;

}  // namespace

uint32_t portalSourceTypeMask(PortalSourceKind kind)
{
    return kind == PortalSourceKind::Window ? SourceTypeWindow : SourceTypeMonitor;
}

uint32_t portalCursorMode(uint32_t availableModes)
{
    // Metadata states where the pointer is beside the pixels without drawing it
    // into them: the position the live probe needs on a session where X cannot
    // see the pointer, and no cursor in the scopes' own analysis. It is the
    // ONLY route to that position - a Wayland client is never told where the
    // pointer is outside its own surfaces.
    //
    // Hidden otherwise: a mode the portal does not advertise is an ERROR that
    // fails the whole handshake, not a request quietly downgraded. Better a
    // probe that cannot follow the pointer than no capture at all.
    return (availableModes & CursorModeMetadata) != 0 ? CursorModeMetadata : CursorModeHidden;
}

}  // namespace sidescopes
