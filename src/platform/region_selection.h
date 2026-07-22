#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/analysis_worker.h"
#include "core/region_suggestions.h"

namespace sidescopes {

/// How the region picker starts out. The mode is chosen up front by the
/// toolbar button (or key) that opened the picker; inside the region
/// modes, A, D, and F switch between attaching to a window, drawing, and
/// attaching to a face. PinColor is its own tool, never switched into or
/// out of: it picks colors rather than regions - undimmed, since judging
/// a color through a dim wash misleads - and the capture region is never
/// touched. Each click decides its own fate: a plain pin closes the
/// tool, a Shift+click (or Shift+drag) pins and keeps it open.
enum class RegionPickerMode
{
    /// An attached region: click the window, or draw a rectangle inside
    /// it. A rectangle drawn where no window sits under it has nothing
    /// to attach to and yields a global region instead.
    AttachWindow,
    /// A global region, drawn by hand and bound to no window.
    DrawGlobal,
    /// An attached region on the window carrying the chosen face.
    AttachFace,
    /// No region at all - the color pinning tool leaves the region be.
    PinColor
};

/// What the picker suggests on one display. Region percentages are always
/// relative to their own display.
struct PickerDisplay
{
    uint32_t displayId = 0;
    std::vector<SuggestedRegion> windows;
    std::vector<SuggestedRegion> faces;
    /// Whether this display's face scan has completed. The streamed display
    /// is scanned when the picker opens; the others are scanned in the
    /// background and reported later through updatePickerFaces. The overlay
    /// tells "no faces here" (scanned, none) from "scanning" (not yet) by
    /// this flag, and only voices the empty verdict once it is set.
    bool facesScanned = false;
};

/// A single point on a display, in that display's percentages - the same
/// space as RegionOfInterest's edges. A plain pin click reports one so the
/// caller can sample it exactly the way the live cursor readout does.
struct DisplayPoint
{
    double xPercent = 0.0;
    double yPercent = 0.0;
};

/// One poll of the asynchronous picker. @c preview is whatever the user is
/// currently indicating - the hovered suggestion or the drag in progress -
/// so the application can feed it to the scopes live; @c displayId says
/// which display it belongs to. On the poll that delivers @c finished,
/// @c confirmed carries the chosen region, or nothing on cancel, and the
/// overlays are gone.
struct RegionPickPoll
{
    bool active = false;
    bool finished = false;
    uint32_t displayId = 0;
    std::optional<RegionOfInterest> preview;
    std::optional<RegionOfInterest> confirmed;
    /// Color pinning. At most one of these carries a ready pin, reported as
    /// it happens without finishing the pick. @c pinnedPoint is a plain
    /// click: a single display-percent point the caller samples the same
    /// way it samples the live cursor readout, so a pin matches the color
    /// the user was reading. @c pinnedArea is a dragged rectangle to
    /// average - the explicit way to ask for a swatch over textured pixels.
    /// @c pinnedKeepOpen carries the click's Shift state: the user's
    /// per-pin choice to keep picking. While @c pinMode is set the caller
    /// must not treat previews or a finish as region changes.
    std::optional<DisplayPoint> pinnedPoint;
    std::optional<RegionOfInterest> pinnedArea;
    bool pinnedKeepOpen = false;
    bool pinMode = false;
    /// Whether the picker is in window mode RIGHT NOW - the overlays switch
    /// modes on their own keys, so the mode that opened the pick can be
    /// stale by the confirm. A rectangle confirmed in window mode binds to
    /// the window under it; one confirmed in draw mode stays global.
    bool windowMode = false;
};

/// Screenshot-style selection spanning every display at once: each gets
/// its own dimmed overlay, and a pick anywhere is a pick there. The attach
/// modes highlight the application window under the cursor with the system
/// accent for one-click confirmation, the way the macOS screenshot
/// interface selects windows; draw mode drags a fresh region by hand within
/// one display - adjusting an existing region happens on the region
/// border itself, not in here. ESC cancels. This application's own
/// windows stay undimmed and clickable through the overlays, so the
/// toolbar keeps working while picking.
///
/// The picker is asynchronous: beginRegionPick shows the overlays and
/// returns, the application keeps running its frame loop (which also pumps
/// the overlays' events), and pollRegionPick is read once per frame so the
/// scopes can preview the selection before it is confirmed.
[[nodiscard]] bool beginRegionPick(const std::vector<PickerDisplay>& displays, RegionPickerMode initialMode);
RegionPickPoll pollRegionPick();

/// Cancels an active pick as if ESC had been pressed on the overlay.
void cancelRegionPick();

/// Switches an active pick between its modes; no-op when none is active.
void setRegionPickMode(RegionPickerMode mode);

/// Delivers a display's face suggestions to an already-open picker, after the
/// background scan of a non-streamed display lands. The call marks that
/// display's face scan complete: an empty @p faces means the scan finished
/// and found nothing, which the overlay reports as "no faces" rather than
/// staying silent. If the picker is in face mode the new suggestions show at
/// once. A no-op when no pick is active or no overlay covers @p displayId
/// (the picker may have closed before the scan finished). Call on the main
/// thread.
void updatePickerFaces(uint32_t displayId, const std::vector<SuggestedRegion>& faces);

/// The live color for the pin modes' cursor chip, pushed by the
/// application each frame: the sample comes from the capture frame, so
/// the chip previews exactly what a click would pin. Ignored outside the
/// pin modes.
void setRegionPickChipColor(const std::optional<FloatColor>& color);

/// Persistent border around the monitored region so users can see what the
/// scopes are reading. Drawn OUTSIDE the region so the border itself never
/// enters the scoped pixels, and its interior stays click-through - the
/// editor underneath keeps working. The border itself is live: edges and
/// corners resize the region, the tab above the top edge moves it, and the
/// application polls the edit each frame so the scopes follow.
/// @p attachedLabel distinguishes the two region kinds: empty is the global
/// region in its plain dress; non-empty is a window-attached region, whose
/// measurement dashes take a warm tint and whose band wears the label (the
/// attached window's application) above the top edge. Idempotent AND cheap to
/// repeat: an unchanged rectangle, label, and visibility is a no-op, so the
/// host may reconcile every frame instead of chasing edges.
/// @p label is always worn on the strip row above the band - the attached
/// window's title for an attached region, the display's name for the
/// global one - with the attach toggle at the label's fixed left end.
/// @p attached picks the toggle's glyph, which shows the STATE: the pin
/// on an attached region, the struck-through pin on the global one.
void showRegionBorder(uint32_t displayId, const RegionOfInterest& region, const std::string& label, bool attached);
void hideRegionBorder();

/// The border's in-progress or just-finished adjustment, if any.
/// @c dismissed reports the border's own close affordances - the hover
/// close button and a double-click on the band - and means "dismiss
/// this region"; the application resets to full screen.
struct RegionBorderEdit
{
    bool editing = false;
    bool dismissed = false;
    /// The border's attach toggle was clicked: an attached region lets go of
    /// its window and becomes the global region in place; a global one
    /// attaches to the frontmost window under it.
    bool attachToggled = false;
    std::optional<RegionOfInterest> region;
};

RegionBorderEdit pollRegionBorderEdit();

/// A key pressed while the region border held the keyboard. On macOS the
/// border panel can take key status WITHOUT activating the application
/// (clicking it focuses the region, Spotlight-style), so its keys are
/// forwarded here for the application to route through its own shortcut
/// map - Escape and the letter shortcuts keep working right after a border
/// interaction. Empty on Windows, whose border never takes the keyboard.
struct BorderKeyPress
{
    std::string key;
    bool shift = false;
    bool escape = false;
};

[[nodiscard]] std::vector<BorderKeyPress> drainBorderKeyPresses();

/// A click-through spotlight while an attached border is being edited:
/// everything on @p displayId outside @p windowRegion (the attached window's
/// rectangle, in display percentages) dims hard, in the attached draw's
/// dress, so the resize limit is visible while dragging. Idempotent and
/// cheap when unchanged; hideAttachedEditDim takes it down.
void showAttachedEditDim(uint32_t displayId, const RegionOfInterest& windowRegion);
void hideAttachedEditDim();

}  // namespace sidescopes
