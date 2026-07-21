#include "app/scope_layout.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "app/scope_view.h"

namespace sidescopes {
namespace {

// A collapsed pane out-scores any real misfit, so a direction that starves a
// pane of all room loses to one that does not.
constexpr float DegenerateMisfit = 100.0f;

// One pane's misfit as |log(actual / preferred)|: scale-free, and twice too
// wide reads as bad as twice too tall.
float aspectMisfit(float width, float height, float preferred)
{
    if (width <= 0.0f || height <= 0.0f) {
        return DegenerateMisfit;
    }

    return std::abs(std::log(width / height / preferred));
}

// The summed misfit of laying the weighted panes along mainLength with
// crossLength across, in the given direction.
float splitScore(const std::vector<float>& weights, const std::vector<float>& aspects, float mainLength,
                 float crossLength, bool sideBySide)
{
    const std::vector<float> lengths = paneLengths(weights, mainLength, 0.0f);
    float score = 0.0f;
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        const float width = sideBySide ? lengths[i] : crossLength;
        const float height = sideBySide ? crossLength : lengths[i];
        score += aspectMisfit(width, height, aspects[i]);
    }

    return score;
}

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

float preferredScopeAspect(const std::string& scopeId)
{
    // Dogfood-tuned starting points, not measurements: the wide traces want
    // width far more than height, the vectorscope is square by construction.
    if (scopeId == WaveformScopeId || scopeId == ParadeScopeId) {
        return 3.0f;
    }
    if (scopeId == HistogramScopeId) {
        return 2.0f;
    }
    if (scopeId == VectorscopeScopeId) {
        return 1.0f;
    }

    return 2.0f;
}

SplitDirection resolveSplitDirection(LayoutOrientation orientation, float areaWidth, float areaHeight,
                                     const std::vector<float>& weights, const std::vector<float>& aspects,
                                     float dividerThickness)
{
    if (orientation == LayoutOrientation::Vertical) {
        return SplitDirection::Stacked;
    }
    if (orientation == LayoutOrientation::Horizontal) {
        return SplitDirection::SideBySide;
    }
    // Single panes tie by construction; mismatched metadata must not steer
    // the layout. Both stack - the full-width photographer default.
    if (weights.size() != aspects.size() || weights.size() < 2) {
        return SplitDirection::Stacked;
    }

    const float dividers = dividerThickness * static_cast<float>(weights.size() - 1);
    const float sideScore = splitScore(weights, aspects, areaWidth - dividers, areaHeight, true);
    const float stackScore = splitScore(weights, aspects, areaHeight - dividers, areaWidth, false);

    return stackScore <= sideScore ? SplitDirection::Stacked : SplitDirection::SideBySide;
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
