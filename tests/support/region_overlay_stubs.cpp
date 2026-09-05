#include "region_overlay_stubs.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/frame.h"
#include "core/region_suggestions.h"
#include "platform/region_selection.h"

namespace sidescopes {

namespace {

test::RegionOverlayStubs g_overlays;

}  // namespace

bool beginRegionPick(const std::vector<PickerDisplay>& displays, RegionPickerMode mode)
{
    if (g_overlays.pickActive) {
        return false;
    }
    g_overlays.lastDisplays = displays;
    g_overlays.lastMode = mode;
    g_overlays.pickActive = g_overlays.pickOpens;
    if (g_overlays.pickActive && !g_overlays.poll.finished) {
        g_overlays.poll.active = true;
    }

    return g_overlays.pickOpens;
}

RegionPickPoll pollRegionPick()
{
    ++g_overlays.pickPolls;
    if (!g_overlays.pickActive) {
        return {};
    }
    RegionPickPoll poll = g_overlays.poll;
    if (g_overlays.pickCancelled) {
        poll = {};
        poll.finished = true;
        poll.pinMode = g_overlays.lastMode == RegionPickerMode::PinColor;
    }
    if (poll.finished || !poll.active) {
        g_overlays.pickActive = false;
        g_overlays.pickCancelled = false;
        g_overlays.poll = {};
    }
    return poll;
}

void cancelRegionPick()
{
    ++g_overlays.pickCancels;
    if (g_overlays.pickActive) {
        g_overlays.pickCancelled = true;
    }
}

void setRegionPickMode(RegionPickerMode mode)
{
    g_overlays.lastMode = mode;
}

void setRegionPickChipColor(const std::optional<FloatColor>& color)
{
    g_overlays.chipColor = color;
}

void updatePickerFaces(uint32_t displayId, const std::vector<SuggestedRegion>& faces)
{
    g_overlays.deliveredFaces[displayId] = faces;
}

void showRegionBorder(uint32_t displayId, const RegionOfInterest& region, const std::string& label,
                      RegionBinding binding)
{
    ++g_overlays.borderShows;
    g_overlays.border = test::ShownBorder{displayId, region, label, binding};
}

void hideRegionBorder()
{
    ++g_overlays.borderHides;
    g_overlays.border.reset();
}

RegionBorderEdit pollRegionBorderEdit()
{
    return g_overlays.borderEdit;
}

void showAttachedEditDim(uint32_t, const RegionOfInterest& windowRegion)
{
    g_overlays.editDim = windowRegion;
}

void hideAttachedEditDim()
{
    ++g_overlays.editDimHides;
    g_overlays.editDim.reset();
}

namespace test {

void RegionOverlayStubs::reset()
{
    *this = RegionOverlayStubs{};
}

RegionOverlayStubs& regionOverlayStubs()
{
    return g_overlays;
}

}  // namespace test

}  // namespace sidescopes
