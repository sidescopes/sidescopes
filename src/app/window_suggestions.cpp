// The picker's window-suggestion logic, kept out of the platform glue in
// main.cpp so it can be reasoned about and unit-tested on its own: given a
// display's on-screen windows and geometry, it decides which windows become
// one-click pick suggestions and in what order.

#include "app/window_suggestions.h"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "platform/region_geometry.h"

namespace sidescopes {
namespace {

// The share of `inner` that lies within `outer`, as a fraction of `inner`'s
// area. Zero when the two do not meet.
double containedFraction(const DesktopWindow& inner, const DesktopWindow& outer)
{
    const double left = std::max(inner.x, outer.x);
    const double top = std::max(inner.y, outer.y);
    const double right = std::min(inner.x + inner.width, outer.x + outer.width);
    const double bottom = std::min(inner.y + inner.height, outer.y + outer.height);
    if (right <= left || bottom <= top) {
        return 0.0;
    }

    return (right - left) * (bottom - top) / (inner.width * inner.height);
}

// Whether `candidate` is auxiliary chrome: a window living mostly inside a
// bigger window of the same application. It is judged against every window, not
// only the ones already kept, so window order does not matter here.
bool isAuxiliary(const DesktopWindow& candidate, const std::vector<DesktopWindow>& windows)
{
    // How much of the candidate must sit inside the bigger same-app window
    // before it reads as one of its panels rather than a window of its own.
    constexpr double AuxiliaryContainment = 0.9;
    for (const DesktopWindow& other : windows) {
        if (&other == &candidate || other.application != candidate.application) {
            continue;
        }
        if (other.width * other.height <= candidate.width * candidate.height) {
            continue;
        }
        if (containedFraction(candidate, other) > AuxiliaryContainment) {
            return true;
        }
    }

    return false;
}

}  // namespace

std::vector<SuggestedRegion> buildWindowSuggestions(const std::vector<DesktopWindow>& windows,
                                                    const DisplayGeometry& geometry, int maxSuggestions)
{
    // Drop the auxiliary chrome up front so the occlusion math never sees it: a
    // window's own panels must neither be suggested nor bury the window behind.
    std::vector<DesktopWindow> eligible;
    std::vector<LocalRect> rects;
    for (const DesktopWindow& candidate : windows) {
        if (isAuxiliary(candidate, windows)) {
            continue;
        }

        rects.push_back(LocalRect{candidate.x, candidate.y, candidate.width, candidate.height});
        eligible.push_back(candidate);
    }

    // The windows still meaningfully visible under the union of everything in
    // front of them, frontmost first, capped at the suggestion limit.
    std::vector<WindowRegion> windowRegions;
    for (const std::size_t index : meaningfulPickCandidates(rects)) {
        if (static_cast<int>(windowRegions.size()) >= maxSuggestions) {
            break;
        }

        const DesktopWindow& window = eligible[index];
        WindowRegion region;
        region.region.leftPercent =
            std::clamp((window.x - geometry.originX) / geometry.widthPoints * 100.0, 0.0, 100.0);
        region.region.topPercent =
            std::clamp((window.y - geometry.originY) / geometry.heightPoints * 100.0, 0.0, 100.0);
        region.region.rightPercent =
            std::clamp((window.x + window.width - geometry.originX) / geometry.widthPoints * 100.0, 0.0, 100.0);
        region.region.bottomPercent =
            std::clamp((window.y + window.height - geometry.originY) / geometry.heightPoints * 100.0, 0.0, 100.0);
        region.application = window.application;
        windowRegions.push_back(std::move(region));
    }

    return buildRegionSuggestions(windowRegions);
}

RegionOfInterest displayPercentRect(const WindowGeometry& windowGeom, const DisplayGeometry& display)
{
    RegionOfInterest region;
    region.leftPercent = std::clamp((windowGeom.x - display.originX) / display.widthPoints * 100.0, 0.0, 100.0);
    region.topPercent = std::clamp((windowGeom.y - display.originY) / display.heightPoints * 100.0, 0.0, 100.0);
    region.rightPercent =
        std::clamp((windowGeom.x + windowGeom.width - display.originX) / display.widthPoints * 100.0, 0.0, 100.0);
    region.bottomPercent =
        std::clamp((windowGeom.y + windowGeom.height - display.originY) / display.heightPoints * 100.0, 0.0, 100.0);

    return region;
}

}  // namespace sidescopes
