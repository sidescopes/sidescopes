#include "app/scope_layout.h"

#include <algorithm>
#include <cstddef>

namespace sidescopes {
namespace {

// One water-filling round: free panes take their weighted share of the length
// left after the pinned panes claim the floor, any free pane that would fall
// below the floor is pinned, and the current lengths are written. Returns
// whether a new pane was pinned, so the caller repeats until the split settles.
bool distributeFreePanes(const std::vector<float>& weights, float total, float floorLength, std::vector<bool>& pinned,
                         std::vector<float>& lengths)
{
    float freeWeight = 0.0f;
    float freeLength = total;
    std::size_t freeCount = 0;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        if (pinned[i]) {
            freeLength -= floorLength;
        } else {
            freeWeight += std::max(0.0f, weights[i]);
            ++freeCount;
        }
    }

    bool pinnedNew = false;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        if (pinned[i]) {
            lengths[i] = floorLength;
            continue;
        }
        const float share = freeWeight > 0.0f ? freeLength * std::max(0.0f, weights[i]) / freeWeight
                                              : freeLength / static_cast<float>(freeCount);
        lengths[i] = share;
        if (share < floorLength) {
            pinned[i] = true;
            pinnedNew = true;
        }
    }

    return pinnedNew;
}

}  // namespace

SplitDirection resolveSplitDirection(LayoutOrientation orientation, float areaWidth, float areaHeight)
{
    switch (orientation) {
    case LayoutOrientation::Vertical:
        return SplitDirection::Stacked;
    case LayoutOrientation::Horizontal:
        return SplitDirection::SideBySide;
    case LayoutOrientation::Automatic:
    default:
        return areaWidth >= areaHeight ? SplitDirection::SideBySide : SplitDirection::Stacked;
    }
}

std::vector<float> paneLengths(const std::vector<float>& weights, float totalLength, float minLength)
{
    const std::size_t count = weights.size();
    std::vector<float> lengths(count, 0.0f);
    if (count == 0) {
        return lengths;
    }

    const float total = std::max(0.0f, totalLength);
    const float floorLength = std::max(0.0f, minLength);
    // Too little room to honor the minimum everywhere: split evenly and accept
    // sub-minimum panes rather than overflow the area.
    if (floorLength <= 0.0f || total <= floorLength * static_cast<float>(count)) {
        lengths.assign(count, total / static_cast<float>(count));

        return lengths;
    }

    std::vector<bool> pinned(count, false);
    bool unstable = true;
    while (unstable) {
        unstable = distributeFreePanes(weights, total, floorLength, pinned, lengths);
    }

    return lengths;
}

std::pair<float, float> dragDividerWeights(float weightFirst, float weightSecond, float lengthFirst, float lengthSecond,
                                           float deltaPixels, float minLength)
{
    const float combinedLength = lengthFirst + lengthSecond;
    const float combinedWeight = std::max(0.0f, weightFirst) + std::max(0.0f, weightSecond);
    const float floorLength = std::max(0.0f, minLength);
    // Degenerate or too cramped to hold two minimum panes: leave the split be.
    if (combinedLength <= 0.0f || combinedWeight <= 0.0f || combinedLength < 2.0f * floorLength) {
        return {weightFirst, weightSecond};
    }

    const float newFirst = std::clamp(lengthFirst + deltaPixels, floorLength, combinedLength - floorLength);
    const float firstShare = newFirst / combinedLength;

    return {combinedWeight * firstShare, combinedWeight * (1.0f - firstShare)};
}

LayoutOrientation orientationFromInt(int value)
{
    switch (value) {
    case 1:
        return LayoutOrientation::Vertical;
    case 2:
        return LayoutOrientation::Horizontal;
    default:
        return LayoutOrientation::Automatic;
    }
}

int orientationToInt(LayoutOrientation orientation)
{
    return static_cast<int>(orientation);
}

}  // namespace sidescopes
