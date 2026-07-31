#pragma once

#include <string>

#include "platform/desktop.h"  // DisplayGeometry
#include "platform/linux/x11_overlay.h"
#include "platform/region_geometry.h"

namespace sidescopes {

// The border window extends this far beyond the region on every side: the grab
// band, wide enough to hit with a cursor, slim enough to stay unobtrusive.
// The matching constants on the other platforms; kept identical so the region
// feels the same to grab everywhere.
inline constexpr double BorderPad = 12.0;
inline constexpr double EdgeRing = 1.0;
inline constexpr double HandleRadius = 3.5;
inline constexpr double HandleMargin = HandleRadius + 2.0;
inline constexpr double WindowPad = BorderPad + HandleMargin;
inline constexpr double MinimumRegionSize = 24.0;
inline constexpr double LabelBand = 20.0;
inline constexpr double CloseRadius = 6.5;
inline constexpr double CloseHitRadius = 11.0;
inline constexpr double CloseCornerInset = 2.0;
inline constexpr double TabAttachZone = 18.0;
inline constexpr double MinimumWidthForClose = 48.0;

/// The interactive region border: an override-redirect X11 window ringing the
/// region, drawn OUTSIDE it so the border never enters the scoped pixels, with
/// a click-through interior so the editor beneath keeps working. The band
/// resizes on its edges and corners, moves from the tab above the top edge,
/// toggles attach and dismisses from its badges. The Linux sibling of the
/// Windows BorderState.
struct BorderX11State
{
    OverlayWindow window;
    bool visible = false;
    uint32_t displayId = 0;
    double displayOriginX = 0.0;
    double displayOriginY = 0.0;
    double displayWidth = 0.0;
    double displayHeight = 0.0;
    /// The region in root pixels - the rectangle the scopes read, without the
    /// band's padding.
    LocalRect region{};
    /// The window's own top-left in root pixels (region minus the padding, and
    /// the label band when a label is worn).
    int windowX = 0;
    int windowY = 0;
    bool attachedRegion = false;
    std::string label;
    /// The live drag: which zone was grabbed, and the region and pointer it
    /// began from, so a delta can be applied against the start rather than
    /// accumulated frame to frame.
    unsigned dragZone = ZoneNone;
    LocalRect dragStartRegion{};
    int dragStartRootX = 0;
    int dragStartRootY = 0;
    bool dragging = false;
    /// Edits collected for the next poll: a resize/move, an attach toggle, a
    /// dismissal.
    bool editing = false;
    bool edited = false;
    bool dismissed = false;
    bool attachToggled = false;
    LocalRect editedRegion{};
    /// Where the pointer sits over the band, for the resize cursor and the
    /// hover-revealed close badge.
    unsigned hoverZone = ZoneNone;
};

/// The one border, created lazily. Owned by region_selection.cpp.
[[nodiscard]] BorderX11State& border();

/// Repaints the band, handles, label tab, attach toggle and close badge for
/// the border's current geometry and hover state.
void paintBorder(BorderX11State& state);

/// Whether the border needs the whole label band above the region (an
/// attached region, or any region wearing a label).
[[nodiscard]] bool borderWearsLabel(const BorderX11State& state);

/// Creates or repositions the border around @p regionRoot (root pixels) on
/// @p displayId, wearing @p label with the attached dress when @p attached.
/// Reconciled every frame by the seam; cheap when the geometry is unchanged.
void showBorder(uint32_t displayId, const DisplayGeometry& geometry, const LocalRect& regionRoot,
                const std::string& label, bool attached);

/// Tears the border down.
void hideBorder();

}  // namespace sidescopes
