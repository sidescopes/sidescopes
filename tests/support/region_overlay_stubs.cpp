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
    g_overlays.lastDisplays = displays;
    g_overlays.lastMode = mode;

    return g_overlays.pickOpens;
}

RegionPickPoll pollRegionPick()
{
    return g_overlays.poll;
}

void cancelRegionPick()
{
    ++g_overlays.pickCancels;
}

void setRegionPickMode(RegionPickerMode mode)
{
    g_overlays.lastMode = mode;
}

void setRegionPickChipColor(const std::optional<FloatColor>&)
{
}

void updatePickerFaces(uint32_t displayId, const std::vector<SuggestedRegion>& faces)
{
    g_overlays.deliveredFaces[displayId] = faces;
}

void showRegionBorder(uint32_t displayId, const RegionOfInterest& region, const std::string& label, bool attached)
{
    ++g_overlays.borderShows;
    g_overlays.border = test::ShownBorder{displayId, region, label, attached};
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
