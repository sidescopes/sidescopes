#pragma once

#include <optional>
#include <utility>

#include "imgui.h"
#include "platform/region_geometry.h"
#include "sidescopes/module.h"

namespace sidescopes {

/// @brief A draggable region over the lab's picture.
///
/// The desktop draws its region border as a native window on the desktop,
/// per platform, so none of that code can run in a page. The GEOMETRY and
/// the LOOK are taken from it rather than invented — the constants below
/// are src/platform/macos/region_border_view.h's, and the drawing follows
/// what that view draws — so the lab's border reads as the application's.
///
/// The gesture is the documented one, and it is not the obvious one:
///
///   * the BAND outside the region moves it — the region's interior is the
///     picture, and on the desktop the border window has a hole there;
///   * the EIGHT handle dots resize, corners taking two edges and edge
///     midpoints taking one;
///   * nothing else does anything.
///
/// The rectangle is kept in IMAGE PIXELS, so it survives the picture being
/// fitted to whatever space the pane leaves. The border itself is drawn at a
/// constant SCREEN size, as it is on a desktop, where it does not grow with
/// what is under it.
class RegionEditor
{
public:
    /// Where the picture landed on screen, and how many screen pixels one
    /// image pixel became.
    struct Placement
    {
        ImVec2 origin;
        float scale = 1.0f;
    };

    /// Centres a region covering @p fraction of the picture, which is what a
    /// visitor should find already in place rather than an empty scope and
    /// an instruction to draw one.
    void reset(int imageWidth, int imageHeight, float fraction = 0.55f);

    /// Keeps the region where it is ON SCREEN across a change of picture.
    ///
    /// The rectangle is kept in image pixels, so a new picture of a different
    /// size or shape would otherwise move it and take its proportions from
    /// the new image - a landscape region becoming a portrait one because the
    /// photograph did. On a desktop nothing of the sort happens: the region
    /// is a rectangle on the DISPLAY, and changing what is displayed beneath
    /// it leaves it exactly where it was.
    ///
    /// Call this with the placement the OUTGOING picture had. The new
    /// picture's placement is not known until it has been laid out, so the
    /// screen rectangle is remembered and put back on the next update.
    /// Does nothing when there is no region to hold.
    void holdOnScreen(const Placement& placement);

    /// Drops the region entirely. The scopes then read nothing, which is the
    /// desktop's own answer to Escape: an empty scope is a state, not a
    /// failure.
    void clear();

    /// Arms the draw gesture, as the desktop's picker does: the next drag on
    /// the picture lays down a new region instead of moving the old one.
    void armDraw();

    [[nodiscard]] bool armed() const
    {
        return m_arming;
    }

    [[nodiscard]] bool hasRegion() const
    {
        return m_rect.width > 0 && m_rect.height > 0;
    }

    /// Runs one frame of interaction inside @p placement and draws the
    /// border. @return Whether the region changed and the scopes are due a
    /// new pass.
    [[nodiscard]] bool update(const Placement& placement, int imageWidth, int imageHeight);

    /// Whether the close badge was clicked since the last ask, and clears
    /// the flag. The host does the dismissing, because clearing a region is
    /// more than the rectangle: the traces are released with it.
    [[nodiscard]] bool takeDismissed();

    [[nodiscard]] SsRect rect() const
    {
        return m_rect;
    }

private:
    /// The eight handle centres, in screen coordinates, in the order the
    /// desktop draws them. Drawing only: the zone a press lands in comes
    /// from platform/region_geometry, so the dots cannot say one thing while
    /// the gesture does another.
    struct Handles
    {
        ImVec2 point[8];
    };

    [[nodiscard]] static Handles handlesFor(const ImVec2& topLeft, const ImVec2& bottomRight);
    [[nodiscard]] unsigned grabAt(const ImVec2& point, const Placement& placement) const;
    void applyDrag(const ImVec2& delta, int imageWidth, int imageHeight);
    /// The armed gesture: lay down a new region. @return Whether it changed.
    [[nodiscard]] bool updateDrawing(const Placement& placement, int imageWidth, int imageHeight);
    /// The ordinary gesture: move by the band, resize by a handle.
    [[nodiscard]] bool updateEditing(const Placement& placement, int imageWidth, int imageHeight);
    /// Says what a press would do, as the desktop border's zones do.
    void announceCursor(const Placement& placement) const;
    void restoreHeldPosition(const Placement& placement, int imageWidth, int imageHeight);
    void drawBorder(const Placement& placement, int imageWidth, int imageHeight) const;
    /// The hover-revealed close badge's centre, on the band's outer top
    /// corner at forty-five degrees off the corner handle.
    /// The region's rectangle on screen, SNAPPED to whole points.
    ///
    /// Everything the border draws is built from this - the measured ring, the
    /// dashes, the band's inner edge and its clip - and the clip has to be on
    /// whole pixels or its quarters seam. Snapping only the clip left the band
    /// standing up to half a point clear of the ring, which showed as a
    /// hairline of picture between the dashes and the stripes. Snapping HERE
    /// puts all of them on the same number.
    ///
    /// The hit tests read it too, so what is pointed at is what is drawn. What
    /// the region MEASURES is untouched: that is m_rect, in image pixels.
    [[nodiscard]] std::pair<ImVec2, ImVec2> screenRect(const Placement& placement) const;

    [[nodiscard]] static ImVec2 closeCentre(const ImVec2& topLeft, const ImVec2& bottomRight);
    /// @return Whether the badge is offered at all: a region too narrow
    ///         yields the corner to its resize zones instead.
    [[nodiscard]] bool closeOffered(const Placement& placement) const;
    [[nodiscard]] bool closeVisible(const Placement& placement) const;
    void drawCloseBadge(const ImVec2& centre) const;
    /// @return Whether the pointer is on the badge, so the gesture below it
    ///         is left alone this frame.
    [[nodiscard]] bool updateClose(const Placement& placement);
    /// The picker's dimmed workspace and its instruction banner.
    static void dashedRect(ImDrawList* draw, const ImVec2& topLeft, const ImVec2& bottomRight);
    void drawPickerOverlay(const Placement& placement, int imageWidth, int imageHeight) const;

    SsRect m_rect{0, 0, 0, 0};
    SsRect m_pressed{0, 0, 0, 0};
    unsigned m_grab = ZoneNone;
    /// Waiting for the drag that lays down a new region.
    bool m_arming = false;
    /// That drag, in progress.
    bool m_drawing = false;
    ImVec2 m_drawFrom{0.0f, 0.0f};
    bool m_dismissed = false;
    /// Where the region sat on screen when the picture changed, waiting for
    /// the new picture's placement so it can be put back there.
    std::optional<ImVec4> m_heldOnScreen;
};

}  // namespace sidescopes
