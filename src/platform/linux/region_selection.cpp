// Region selection on Linux is not built yet. The design (workbench
// linux-port-design) is two-tier: an in-application picker over the portal
// stream everywhere, and layer-shell overlays where the compositor offers
// them. Until either exists the picker declines to open - beginRegionPick's
// false is the contract for exactly that - and the border draws nothing.

#include "platform/region_selection.h"

namespace sidescopes {

bool beginRegionPick(const std::vector<PickerDisplay>&, RegionPickerMode)
{
    return false;
}

RegionPickPoll pollRegionPick()
{
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
}

void setRegionPickChipColor(const std::optional<FloatColor>&)
{
}

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
    return {};
}

void showAttachedEditDim(uint32_t, const RegionOfInterest&)
{
}

void hideAttachedEditDim()
{
}

}  // namespace sidescopes
