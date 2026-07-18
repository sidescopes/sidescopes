#include "app/face_pin.h"

#include <cmath>
#include <cstddef>

namespace sidescopes {
namespace face_pin {
namespace {

// Gates, all relative to the last adopted anchor's width. A candidate must
// pass every gate; anything else holds the region where it is.
constexpr double ProximityRadius = 1.5;    ///< max centre distance, in anchor widths
constexpr double SizeRatioMin = 0.6;       ///< min candidate width vs anchor
constexpr double SizeRatioMax = 1.7;       ///< max candidate width vs anchor
constexpr double WideSizeRatioMin = 0.2;   ///< size gate while searching wide
constexpr double WideSizeRatioMax = 5.0;   ///< size gate while searching wide
constexpr double SameCenterSlack = 0.15;   ///< "same candidate" centre tolerance
constexpr double SameSizeSlack = 0.10;     ///< "same candidate" width tolerance
constexpr double MicroCenterSlack = 0.08;  ///< adoption threshold: centre motion
constexpr double MicroSizeSlack = 0.04;    ///< adoption threshold: width change
constexpr int StableProbes = 2;            ///< consecutive agreeing sightings to adopt
constexpr int RecoveryMisses = 3;          ///< empty probes before the search widens
constexpr int GiveUpMisses = 16;           ///< probes without the face before the pin gives up
constexpr double FlushClearance = 4.0;     ///< pixels: a box this close to a side edge was clipped by it

double centerDistance(const FaceAnchor& a, const FaceAnchor& b)
{
    return std::hypot(a.centerX - b.centerX, a.centerY - b.centerY);
}

bool passesGates(const FaceAnchor& candidate, const FaceAnchor& anchor, bool wide)
{
    if (anchor.width <= 0.0 || candidate.width <= 0.0) {
        return false;
    }
    if (!wide && centerDistance(candidate, anchor) > ProximityRadius * anchor.width) {
        return false;
    }
    const double ratio = candidate.width / anchor.width;

    return ratio >= (wide ? WideSizeRatioMin : SizeRatioMin) && ratio <= (wide ? WideSizeRatioMax : SizeRatioMax);
}

bool withinSlack(const FaceAnchor& a, const FaceAnchor& b, double centerSlack, double sizeSlack)
{
    if (b.width <= 0.0) {
        return false;
    }
    if (centerDistance(a, b) > centerSlack * b.width) {
        return false;
    }

    return std::abs(a.width - b.width) <= sizeSlack * b.width;
}

}  // namespace

FacePinState makePin(const FaceAnchor& anchor, const PinRect& crop)
{
    FacePinState state;
    state.lastAnchor = anchor;
    rebindCrop(state, crop);
    // The pick itself is the first sighting; one agreeing probe confirms.
    state.pendingAnchor = anchor;
    state.pendingCount = 1;

    return state;
}

PinRect mapRegion(const FacePinState& state, const FaceAnchor& anchor)
{
    const double centerX = anchor.centerX + state.offsetX * anchor.width;
    const double centerY = anchor.centerY + state.offsetY * anchor.width;
    const double halfWidth = state.sizeX * anchor.width / 2.0;
    const double halfHeight = state.sizeY * anchor.width / 2.0;

    return PinRect{centerX - halfWidth, centerY - halfHeight, centerX + halfWidth, centerY + halfHeight};
}

void rebindCrop(FacePinState& state, const PinRect& crop)
{
    if (state.lastAnchor.width <= 0.0) {
        return;
    }
    const double width = state.lastAnchor.width;
    state.offsetX = ((crop.left + crop.right) / 2.0 - state.lastAnchor.centerX) / width;
    state.offsetY = ((crop.top + crop.bottom) / 2.0 - state.lastAnchor.centerY) / width;
    state.sizeX = (crop.right - crop.left) / width;
    state.sizeY = (crop.bottom - crop.top) / width;
}

void translate(FacePinState& state, double dxPixels, double dyPixels)
{
    state.lastAnchor.centerX += dxPixels;
    state.lastAnchor.centerY += dyPixels;
    state.pendingAnchor.centerX += dxPixels;
    state.pendingAnchor.centerY += dyPixels;
}

bool searchingWide(const FacePinState& state)
{
    return state.missCount >= RecoveryMisses;
}

bool givenUp(const FacePinState& state)
{
    return state.missCount >= GiveUpMisses;
}

bool trustworthyBox(const PinRect& box, const PinRect& bounds)
{
    return box.left - bounds.left >= FlushClearance && bounds.right - box.right >= FlushClearance;
}

FacePinDecision decide(FacePinState& state, const std::vector<FaceAnchor>& candidates)
{
    const bool wide = searchingWide(state);
    const FaceAnchor* winner = nullptr;
    std::size_t passing = 0;
    for (const FaceAnchor& candidate : candidates) {
        if (!passesGates(candidate, state.lastAnchor, wide)) {
            continue;
        }
        ++passing;
        if (winner == nullptr ||
            centerDistance(candidate, state.lastAnchor) < centerDistance(*winner, state.lastAnchor)) {
            winner = &candidate;
        }
    }
    if (passing == 0) {
        ++state.missCount;
        state.pendingCount = 0;

        return FacePinDecision{
            false, true,
            candidates.empty() ? "no faces" : "none near (" + std::to_string(candidates.size()) + " seen)"};
    }
    if (passing > 1) {
        // Rivals mean the face's position is not certain: the region
        // freezes, the border hides, and the give-up clock keeps running -
        // persistent rivalry ends the pin instead of guessing.
        state.pendingCount = 0;
        ++state.missCount;

        return FacePinDecision{false, true, "ambiguous (" + std::to_string(passing) + " rivals)"};
    }
    const bool agrees = withinSlack(*winner, state.pendingAnchor, SameCenterSlack, SameSizeSlack);
    state.pendingCount = agrees ? state.pendingCount + 1 : 1;
    state.pendingAnchor = *winner;
    if (state.pendingCount < StableProbes) {
        return FacePinDecision{false, true, "unsettled"};
    }
    // A stable candidate is a found face, wherever the verdict lands next:
    // the search narrows back to the patch around it.
    state.missCount = 0;
    if (withinSlack(*winner, state.lastAnchor, MicroCenterSlack, MicroSizeSlack)) {
        return FacePinDecision{false, false, "micro-move"};
    }
    state.lastAnchor = *winner;

    return FacePinDecision{true, false, wide ? "reacquired" : "settled"};
}

}  // namespace face_pin
}  // namespace sidescopes
