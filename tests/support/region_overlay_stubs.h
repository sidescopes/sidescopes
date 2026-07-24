#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/analysis_worker.h"
#include "core/region_suggestions.h"
#include "platform/region_selection.h"

namespace sidescopes::test {

// The picker overlay and the region border, scripted in one place: what they
// answer when polled and what they were last told to show. The picker and the
// region coordinator both drive these seams, and a translation unit may define
// each of them only once.

/// The region border as it currently stands on screen.
struct ShownBorder
{
    uint32_t displayId = 0;
    RegionOfInterest region;
    std::string label;
    bool attached = false;
};

/// The scripted overlays. One instance serves the whole test binary, so every
/// suite touching it resets it first.
struct RegionOverlayStubs
{
    /// What the picker overlay answers: whether it can open at all, and the
    /// poll it reports each frame.
    bool pickOpens = true;
    RegionPickPoll poll;
    /// What the border's live edit reports each frame.
    RegionBorderEdit borderEdit;

    /// What the picker overlay was told.
    int pickCancels = 0;
    std::optional<RegionPickerMode> lastMode;
    std::vector<PickerDisplay> lastDisplays;
    std::map<uint32_t, std::vector<SuggestedRegion>> deliveredFaces;

    /// What the border was told: the last showRegionBorder, cleared by
    /// hideRegionBorder, plus the counts so a reconcile that changed nothing
    /// can still be told from one that took the border down.
    std::optional<ShownBorder> border;
    int borderShows = 0;
    int borderHides = 0;

    /// The attached edit's click-through dim, on the same show/hide pattern.
    std::optional<RegionOfInterest> editDim;
    int editDimHides = 0;

    void reset();
};

/// The one scripted set of overlays the stubs answer from.
[[nodiscard]] RegionOverlayStubs& regionOverlayStubs();

}  // namespace sidescopes::test
