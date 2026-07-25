#include "app/capture_crop.h"

namespace sidescopes {
namespace {

// A region the display can actually deliver: inside the display, and never
// empty, so a reconfiguration is never asked for a rectangle the compositor
// would reject.
std::optional<IntRect> deliverable(IntRect region, int displayWidth, int displayHeight)
{
    const IntRect clamped = region.clampedTo(displayWidth, displayHeight);
    if (clamped.empty()) {
        return std::nullopt;
    }

    return clamped;
}

}  // namespace

std::optional<IntRect> cropFor(const CropInputs& inputs)
{
    if (inputs.displayWidth <= 0 || inputs.displayHeight <= 0) {
        return std::nullopt;
    }
    // Anything that reads outside the region keeps the whole display coming.
    if (inputs.pickerActive || inputs.faceLockActive) {
        return std::nullopt;
    }
    if (inputs.now - inputs.regionChangedAt < CropSettleSeconds) {
        return std::nullopt;
    }

    const std::optional<IntRect> region = deliverable(inputs.region, inputs.displayWidth, inputs.displayHeight);
    if (!region) {
        return std::nullopt;
    }

    const double displayPixels = static_cast<double>(inputs.displayWidth) * inputs.displayHeight;
    const double regionPixels = static_cast<double>(region->width) * region->height;
    if (regionPixels > displayPixels * CropWorthwhileShare) {
        return std::nullopt;
    }

    return region;
}

std::optional<IntRect> CropTracker::decide(IntRect regionPixels, int displayWidth, int displayHeight, bool pickerActive,
                                           bool faceLockActive, double now)
{
    if (!m_seenRegion || !(m_region == regionPixels)) {
        m_region = regionPixels;
        m_changedAt = now;
        m_seenRegion = true;
    }

    const CropInputs inputs{regionPixels, displayWidth, displayHeight, pickerActive, faceLockActive, m_changedAt, now};

    return cropFor(inputs);
}

}  // namespace sidescopes
