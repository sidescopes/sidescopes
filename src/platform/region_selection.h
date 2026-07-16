#pragma once

#include <cstdint>
#include <optional>

#include "core/analysis_worker.h"
#include "core/region_suggestions.h"

namespace sidescopes {

/// How the region picker starts out. The mode is chosen up front by the
/// toolbar button (or key) that opened the picker; inside the region
/// modes, A, D, and F switch between window picking, drawing, and face
/// picking. PinColor is its own tool, never switched into or out of: it
/// picks colors rather than regions - undimmed, since judging a color
/// through a dim wash misleads - and the capture region is never
/// touched. Each click decides its own fate: a plain pin closes the
/// tool, a Shift+click (or Shift+drag) pins and keeps it open.
enum class RegionPickerMode
{
    PickWindows,
    Draw,
    PickFaces,
    PinColor
};

/// What the picker offers on one display. Region percentages are always
/// relative to their own display.
struct PickerDisplay
{
    uint32_t displayId = 0;
    std::vector<SuggestedRegion> windows;
    std::vector<SuggestedRegion> faces;
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
    /// Color pinning. @c pinnedArea is the display-percent rectangle to
    /// average and pin - a click delivers a cursor-sized patch, a drag
    /// the dragged area - reported as it happens, without finishing the
    /// pick. @c pinnedKeepOpen carries the click's Shift state: the
    /// user's per-pin choice to keep picking. While @c pinMode is set the
    /// caller must not treat previews or a finish as region changes.
    std::optional<RegionOfInterest> pinnedArea;
    bool pinnedKeepOpen = false;
    bool pinMode = false;
};

/// Screenshot-style selection spanning every display at once: each gets
/// its own dimmed overlay, and a pick anywhere is a pick there. Pick mode
/// highlights the application window under the cursor with the system
/// accent for one-click confirmation, the way the macOS screenshot
/// interface selects windows; draw mode drags a fresh area by hand within
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
/// application polls the edit each frame so the scopes follow. Idempotent:
/// call showRegionBorder again to move it.
void showRegionBorder(uint32_t displayId, const RegionOfInterest& region);
void hideRegionBorder();

/// The border's in-progress or just-finished adjustment, if any.
/// @c dismissed reports the border's own close affordances - the hover
/// close button and a double-click on the band - and means "stop
/// tracking this region"; the application resets to full screen.
struct RegionBorderEdit
{
    bool editing = false;
    bool dismissed = false;
    std::optional<RegionOfInterest> region;
};

RegionBorderEdit pollRegionBorderEdit();

}  // namespace sidescopes
