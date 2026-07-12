#pragma once

#include <cstdint>
#include <optional>

#include "core/analysis_worker.h"
#include "core/region_suggestions.h"

namespace sidescopes {

// How the region picker starts out. The mode is chosen up front by the
// toolbar button (or key) that opened the picker; inside it, A, D, and F
// switch between window picking, drawing, and face picking, and P (with
// Shift for the repeating flavor) switches to color pinning. The two pin
// modes pick colors rather than regions: undimmed, since judging a color
// through a dim wash misleads, and the capture region is never touched.
// PinColor closes after one pin; PinColors stays until ESC.
enum class RegionPickerMode { PickWindows, Draw, PickFaces, PinColor, PinColors };

// What the picker offers on one display. Region percentages are always
// relative to their own display.
struct PickerDisplay {
    uint32_t display_id = 0;
    std::vector<SuggestedRegion> windows;
    std::vector<SuggestedRegion> faces;
};

// One poll of the asynchronous picker. `preview` is whatever the user is
// currently indicating - the hovered suggestion or the drag in progress -
// so the application can feed it to the scopes live; `display_id` says
// which display it belongs to. On the poll that delivers `finished`,
// `confirmed` carries the chosen region, or nothing on cancel, and the
// overlays are gone.
struct RegionPickPoll {
    bool active = false;
    bool finished = false;
    uint32_t display_id = 0;
    std::optional<RegionOfInterest> preview;
    std::optional<RegionOfInterest> confirmed;
    // Color pinning. `pinned_area` is the display-percent rectangle to
    // average and pin - a click delivers a cursor-sized patch, a drag
    // the dragged area - reported as it happens, without finishing the
    // pick. The mode flags describe the pickers' CURRENT state (overlay
    // keys switch modes mid-pick): while `pin_mode` is set the caller
    // must not treat previews or a finish as region changes.
    std::optional<RegionOfInterest> pinned_area;
    bool pin_mode = false;
    bool pin_single = false;
};

// Screenshot-style selection spanning every display at once: each gets
// its own dimmed overlay, and a pick anywhere is a pick there. Pick mode
// highlights the application window under the cursor with the system
// accent for one-click confirmation, the way the macOS screenshot
// interface selects windows; draw mode drags a fresh area by hand within
// one display - adjusting an existing region happens on the region
// border itself, not in here. ESC cancels. This application's own
// windows stay undimmed and clickable through the overlays, so the
// toolbar keeps working while picking.
//
// The picker is asynchronous: Begin shows the overlays and returns, the
// application keeps running its frame loop (which also pumps the
// overlays' events), and PollRegionPick is read once per frame so the
// scopes can preview the selection before it is confirmed.
bool BeginRegionPick(const std::vector<PickerDisplay>& displays, RegionPickerMode initial_mode);
RegionPickPoll PollRegionPick();

// Cancels an active pick as if ESC had been pressed on the overlay.
void CancelRegionPick();

// Switches an active pick between its modes; no-op when none is active.
void SetRegionPickMode(RegionPickerMode mode);

// The live color for the pin modes' cursor chip, pushed by the
// application each frame: the sample comes from the capture frame, so
// the chip previews exactly what a click would pin. Ignored outside the
// pin modes.
void SetRegionPickChipColor(const std::optional<FloatColor>& color);

// Persistent border around the monitored region so users can see what the
// scopes are reading. Drawn OUTSIDE the region so the border itself never
// enters the scoped pixels, and its interior stays click-through - the
// editor underneath keeps working. The border itself is live: edges and
// corners resize the region, the tab above the top edge moves it, and the
// application polls the edit each frame so the scopes follow. Idempotent:
// call Show again to move it.
void ShowRegionBorder(uint32_t display_id, const RegionOfInterest& region);
void HideRegionBorder();

// The border's in-progress or just-finished adjustment, if any.
// `dismissed` reports the border's own close affordances - the hover
// close button and a double-click on the band - and means "stop
// tracking this region"; the application resets to full screen.
struct RegionBorderEdit {
    bool editing = false;
    bool dismissed = false;
    std::optional<RegionOfInterest> region;
};
RegionBorderEdit PollRegionBorderEdit();

}  // namespace sidescopes
