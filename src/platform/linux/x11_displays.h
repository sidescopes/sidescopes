#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "platform/desktop.h"

namespace sidescopes {

/// A connected output as X11/XRandR reports it; the id is the RROutput
/// identifier, stable while the output stays connected. Shared by the desktop
/// services and the capture backend so both speak the same display identity.
struct LinuxDisplay
{
    uint32_t id = 0;
    DisplayGeometry geometry;
    std::string name;
};

/// The connected outputs with an active mode, or the whole screen as one
/// display where RandR is absent (a bare Xvfb), or nothing at all on a
/// pure-Wayland session without XWayland.
[[nodiscard]] std::vector<LinuxDisplay> connectedDisplays();

}  // namespace sidescopes
