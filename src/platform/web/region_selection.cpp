// Region selection on the desktop, answered for a browser: none of it can
// exist here, and every seam says so.
//
// The desktop draws the picker and the region border as WINDOWS ON THE
// DESKTOP - a full-screen overlay to drag in, a frameless border that sits
// over the editor being measured. Neither is available to a page: a page
// paints inside its own canvas and nowhere else, and no browser API will
// ever change that.
//
// So the demo does the equivalent INSIDE its canvas, over the picture
// standing in for the screen, in src/web/region_editor.cpp. That is a
// different surface rather than an implementation of this one, which is why
// these answer "nothing" instead of forwarding.
//
// They are written out rather than left undefined DELIBERATELY. macOS and
// Windows each answer every seam in this header, and a platform that answers
// only the ones its current host happens to call is not a platform - it is a
// host with a private arrangement. Answering all of them is what lets the
// application be built against this layer unchanged.

#include "platform/region_selection.h"

#include <optional>
#include <string>
#include <vector>

#include "core/frame.h"

namespace sidescopes {

// --- the picker --------------------------------------------------------

bool beginRegionPick(const std::vector<PickerDisplay>&, RegionPickerMode)
{
    // Refused, which the caller already handles: the picker reports that it
    // could not open and the tool stands down. Nothing is left half-armed.
    return false;
}

RegionPickPoll pollRegionPick()
{
    // Nothing was begun, so there is nothing in flight to report.
    return {};
}

void cancelRegionPick()
{
}

void setRegionPickMode(RegionPickerMode)
{
}

void updatePickerFaces(uint32_t, const std::vector<SuggestedRegion>&)
{
    // No picker to show them on, and no detector to find them either -
    // supportsFaceDetection() is false, so nothing calls this.
}

void setRegionPickChipColor(const std::optional<FloatColor>&)
{
    // The pin swatch rides the CURSOR here, which the desktop layers also
    // do; web/demo_shell.cpp builds it. This seam is the picker window's
    // copy of the same idea and has no window to sit in.
}

// --- the region border -------------------------------------------------

void showRegionBorder(uint32_t, const RegionOfInterest&, const std::string&, bool)
{
}

void hideRegionBorder()
{
}

RegionBorderEdit pollRegionBorderEdit()
{
    return {};
}

std::vector<BorderKeyPress> drainBorderKeyPresses()
{
    // The desktop's border window can hold the keyboard, so it forwards what
    // it receives back to the application. A page's keys reach the canvas
    // directly and are read there, so nothing ever queues here.
    return {};
}

void showAttachedEditDim(uint32_t, const RegionOfInterest&)
{
}

void hideAttachedEditDim()
{
}

}  // namespace sidescopes
