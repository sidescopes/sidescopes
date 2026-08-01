#pragma once

#include <vector>

#include "core/region_suggestions.h"
#include "platform/desktop.h"

namespace sidescopes {

/// The pick suggestions for one display, built from its on-screen windows.
/// @p windows is that display's windows frontmost first (as the window server
/// reports them), in global desktop points; @p geometry places the display in
/// the same space so the rectangles become display-relative percentages;
/// @p maxSuggestions caps how many are suggested.
///
/// A window living mostly inside a larger window of the same application is
/// auxiliary chrome - an editor draws its panels and info overlays as
/// borderless windows over the main one - so it is never suggested and never
/// occludes: a panel over the photo cannot disqualify the photo. Of the rest,
/// only the windows still at least MinimumVisibleFraction visible under the
/// union of the windows in front of them are kept, so a window buried behind
/// the ones on top drops out. Survivors stay frontmost first, the topmost
/// suggested first.
[[nodiscard]] std::vector<SuggestedRegion> buildWindowSuggestions(const std::vector<DesktopWindow>& windows,
                                                                  const DisplayGeometry& geometry, int maxSuggestions);

/// How a request to pick a window is answered on this session.
enum class WindowPickRoute
{
    /// Draw the picker over the desktop and offer the windows enumerated on
    /// it: the whole desktop is known, so the sheet can show all of it, and a
    /// chosen window is ATTACHED - the region follows it, and a rectangle may
    /// be drawn inside it.
    Suggestions,
    /// Hand the choice to the compositor's own picker. It is the only thing
    /// that knows what windows exist, and what comes back is a stream of that
    /// window rather than a rectangle on a display - so the whole window is
    /// scoped and nothing follows it, because nothing has to.
    CompositorPicker
};

/// Which route a window pick takes, given whether this session can enumerate
/// foreign windows and whether a compositor picker is available at all.
///
/// The pairing that matters is the third: a session that can enumerate nothing
/// AND has no compositor picker still opens the suggestion sheet, because a
/// picker showing an honest handful beats refusing to open. That is a real
/// state - a bare Wayland compositor with no ScreenCast portal - and silently
/// doing nothing there would read as the shortcut being broken.
[[nodiscard]] WindowPickRoute windowPickRoute(bool windowsEnumerable, bool compositorPickerAvailable);

/// A window rectangle as its display's percentages - the shape the
/// window-candidate list and the edit-time veil speak. Clamped to the
/// display, so a window straddling an edge reports the visible part.
[[nodiscard]] RegionOfInterest displayPercentRect(const WindowGeometry& windowGeom, const DisplayGeometry& display);

}  // namespace sidescopes
