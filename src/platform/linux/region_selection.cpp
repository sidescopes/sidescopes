// Region picking on Linux: fullscreen override-redirect X11 sheets under
// XWayland, one per display, dragged on exactly like the macOS and Windows
// overlays - see workbench linux-port-design for the architecture and the
// stacking verification behind it. The picker state machine lives in
// region_picker_x11; this seam converts between its overlay-local pixels and
// the display percentages the application speaks, and pumps the overlay
// events once per poll, on the main thread, like the Windows message loop.
//
// The interactive border is the remaining piece of this seam; until it
// lands, its functions keep their honest no-op shape.

#include "platform/region_selection.h"

#include <optional>

#include "platform/desktop.h"
#include "platform/linux/region_border_x11.h"
#include "platform/linux/region_picker_x11.h"
#include "platform/region_geometry.h"

namespace sidescopes {
namespace {

/// The chip color for the pin cursor, pushed each frame by the application.
/// The X11 picker has no hardware-cursor swatch yet; the value is kept so
/// the seam's contract holds when it grows one.
std::optional<FloatColor> g_pinChipColor;

/// Local pixels to display percentages for one sheet.
RegionOfInterest regionFromSheet(const PickerX11State& picker, const LocalRect& rect)
{
    return regionFromLocalRect(rect, picker.width, picker.height);
}

/// Seeds one sheet's suggestion lists from the display's PickerDisplay entry
/// and arms the mode's active list.
void seedSuggestions(PickerX11State& picker, const PickerDisplay& entry)
{
    for (const SuggestedRegion& suggestion : entry.windows) {
        picker.windows.emplace_back(localRectFromRegion(suggestion.region, picker.width, picker.height),
                                    suggestion.label);
    }
    for (const SuggestedRegion& suggestion : entry.faces) {
        picker.faces.emplace_back(localRectFromRegion(suggestion.region, picker.width, picker.height),
                                  suggestion.label);
    }
    picker.facesScanned = entry.facesScanned;
    if (picker.facesMode) {
        picker.suggestions = picker.faces;
    } else if (!picker.drawMode && !picker.pinMode) {
        picker.suggestions = picker.windows;
    }
}

/// The finishing sheet, if any: the one that confirmed or cancelled.
PickerX11State* finishedSheet()
{
    for (auto& picker : openPickers()) {
        if (picker->finished) {
            return picker.get();
        }
    }
    return nullptr;
}

/// The sheet with a drag or hover preview to report, if any.
PickerX11State* previewSheet()
{
    for (auto& picker : openPickers()) {
        if (picker->dragging || picker->hoveredSuggestion >= 0) {
            return picker.get();
        }
    }
    return nullptr;
}

/// A ready pin from any sheet, collected into the poll and cleared.
void collectPinnedSample(RegionPickPoll& poll)
{
    for (auto& picker : openPickers()) {
        if (!picker->pinnedReady) {
            continue;
        }
        picker->pinnedReady = false;
        poll.displayId = picker->displayId;
        poll.pinnedKeepOpen = picker->pinnedKeepOpen;
        if (picker->pinnedIsPoint) {
            poll.pinnedPoint = DisplayPoint{picker->pinnedPointX / picker->width * 100.0,
                                            picker->pinnedPointY / picker->height * 100.0};
        } else {
            poll.pinnedSample = regionFromSheet(*picker, picker->pinnedSample);
        }
        // A pin that keeps the tool open leaves the sheets up; one that does
        // not finishes the pick without touching the region.
        if (!picker->pinnedKeepOpen) {
            picker->picked = false;
            picker->finished = true;
        }

        return;
    }
}

/// Resolves a finishing sheet into the poll and tears every sheet down.
bool finishRegionPick(RegionPickPoll& poll)
{
    PickerX11State* finishing = finishedSheet();
    if (finishing == nullptr) {
        return false;
    }
    poll.finished = true;
    poll.displayId = finishing->displayId;
    if (finishing->picked) {
        poll.confirmed = regionFromSheet(*finishing, finishing->confirmedRect);
    }
    openPickers().clear();

    return true;
}

/// The live preview: the drag in progress, or the hovered suggestion.
void collectRegionPreview(RegionPickPoll& poll)
{
    PickerX11State* previewing = previewSheet();
    if (previewing == nullptr) {
        return;
    }
    poll.displayId = previewing->displayId;
    if (previewing->dragging) {
        poll.preview =
            regionFromSheet(*previewing, selectionRectFromDrag(previewing->dragStartX, previewing->dragStartY,
                                                               previewing->dragCurrentX, previewing->dragCurrentY));
    } else {
        poll.preview = regionFromSheet(*previewing, previewing->suggestions[previewing->hoveredSuggestion].first);
    }
}

}  // namespace

bool beginRegionPick(const std::vector<PickerDisplay>& displays, RegionPickerMode initialMode)
{
    if (!openPickers().empty()) {
        return false;  // one picker at a time
    }

    // An attach with nothing to attach to anywhere opens as drawing, like the
    // other platforms; the decision is global so every display shows the same
    // mode.
    bool anyWindows = false;
    for (const PickerDisplay& entry : displays) {
        anyWindows |= !entry.windows.empty();
    }
    const bool pin = initialMode == RegionPickerMode::PinColor;
    const bool draw = !pin && (initialMode == RegionPickerMode::DrawGlobal ||
                               (initialMode == RegionPickerMode::AttachWindow && !anyWindows));
    const bool faces = initialMode == RegionPickerMode::AttachFace;

    for (const PickerDisplay& entry : displays) {
        const std::optional<DisplayGeometry> geometry = geometryOfDisplay(entry.displayId);
        if (!geometry) {
            continue;
        }
        auto picker = createPickerSheet(entry.displayId, static_cast<int>(geometry->originX),
                                        static_cast<int>(geometry->originY), static_cast<int>(geometry->widthPoints),
                                        static_cast<int>(geometry->heightPoints), draw, faces, pin);
        if (picker != nullptr) {
            seedSuggestions(*picker, entry);
            paintPicker(*picker);
            openPickers().push_back(std::move(picker));
        }
    }
    if (openPickers().empty()) {
        return false;
    }
    // Esc and the mode letters arrive through the first sheet's grab; the
    // handlers fan the mode out to every sheet.
    openPickers().front()->window.grabKeyboard();

    return true;
}

RegionPickPoll pollRegionPick()
{
    RegionPickPoll poll;
    if (openPickers().empty()) {
        return poll;
    }
    pumpOverlayEvents();
    if (openPickers().empty()) {
        // An event handler may have finished the pick within an earlier pump.
        return poll;
    }
    poll.active = true;
    // Mode flags come first: the finishing poll returns early below, and the
    // caller needs them to know a pin-mode finish never means a region
    // change. The sheets switch modes in lockstep; the front one speaks for
    // all.
    poll.pinMode = openPickers().front()->pinMode;
    poll.attachesToWindow =
        !openPickers().front()->pinMode && !openPickers().front()->drawMode && !openPickers().front()->facesMode;
    collectPinnedSample(poll);
    if (finishRegionPick(poll)) {
        return poll;
    }
    collectRegionPreview(poll);

    return poll;
}

void cancelRegionPick()
{
    if (openPickers().empty()) {
        return;
    }
    cancelAllPickerSheets();
}

void setRegionPickMode(RegionPickerMode mode)
{
    if (openPickers().empty()) {
        return;
    }
    switchPickerMode(mode == RegionPickerMode::DrawGlobal   ? 1
                     : mode == RegionPickerMode::AttachFace ? 2
                     : mode == RegionPickerMode::PinColor   ? 3
                                                            : 0);
}

void updatePickerFaces(uint32_t displayId, const std::vector<SuggestedRegion>& faces)
{
    for (auto& picker : openPickers()) {
        if (picker->displayId != displayId) {
            continue;
        }
        picker->faces.clear();
        for (const SuggestedRegion& suggestion : faces) {
            picker->faces.emplace_back(localRectFromRegion(suggestion.region, picker->width, picker->height),
                                       suggestion.label);
        }
        // The scan is done for this display now: an empty list is the honest
        // "none found", no longer "not yet scanned".
        picker->facesScanned = true;
        if (picker->facesMode) {
            picker->suggestions = picker->faces;
            picker->hoveredSuggestion = -1;
        }
        paintPicker(*picker);

        return;
    }
}

void setRegionPickChipColor(const std::optional<FloatColor>& color)
{
    g_pinChipColor = color;
}

void showRegionBorder(uint32_t displayId, const RegionOfInterest& region, const std::string& label, bool attached)
{
    const std::optional<DisplayGeometry> geometry = geometryOfDisplay(displayId);
    if (!geometry) {
        return;
    }
    const LocalRect regionRoot{geometry->originX + region.leftPercent / 100.0 * geometry->widthPoints,
                               geometry->originY + region.topPercent / 100.0 * geometry->heightPoints,
                               (region.rightPercent - region.leftPercent) / 100.0 * geometry->widthPoints,
                               (region.bottomPercent - region.topPercent) / 100.0 * geometry->heightPoints};
    showBorder(displayId, *geometry, regionRoot, label, attached);
}

void hideRegionBorder()
{
    hideBorder();
}

RegionBorderEdit pollRegionBorderEdit()
{
    // The border's own window delivers its events through the shared pump; the
    // picker's pump does not run while a border is up, so this drives it.
    pumpOverlayEvents();

    RegionBorderEdit edit;
    BorderX11State& state = border();
    if (!state.visible) {
        return edit;
    }
    edit.editing = state.editing;
    edit.dismissed = state.dismissed;
    edit.attachToggled = state.attachToggled;
    state.dismissed = false;
    state.attachToggled = false;
    if (state.edited) {
        state.edited = false;
        // Root pixels back to display percentages, against the border's own
        // display - the same conversion the picker does at its poll boundary.
        const double width = state.displayWidth;
        const double height = state.displayHeight;
        RegionOfInterest region;
        region.leftPercent = (state.editedRegion.x - state.displayOriginX) / width * 100.0;
        region.topPercent = (state.editedRegion.y - state.displayOriginY) / height * 100.0;
        region.rightPercent = (state.editedRegion.x + state.editedRegion.width - state.displayOriginX) / width * 100.0;
        region.bottomPercent =
            (state.editedRegion.y + state.editedRegion.height - state.displayOriginY) / height * 100.0;
        edit.region = region;
    }

    return edit;
}

std::vector<BorderKeyPress> drainBorderKeyPresses()
{
    // The border never takes the keyboard on Linux, as on Windows: it is
    // click-through and override-redirect, so the application keeps focus and
    // its own shortcuts throughout.
    return {};
}

void showAttachedEditDim(uint32_t, const RegionOfInterest&)
{
    // The attached-edit spotlight is a refinement of the border's own dress;
    // the region is already clamped to the window at pick time, so the border
    // ships without the extra veil until the owner asks for it.
}

void hideAttachedEditDim()
{
}

}  // namespace sidescopes
