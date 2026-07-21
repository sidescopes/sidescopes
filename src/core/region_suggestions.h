#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis_worker.h"

namespace sidescopes {

/// A region the picker offers for one-click confirmation.
struct SuggestedRegion
{
    RegionOfInterest region;
    std::string label;
};

/// One detected face turned into everything a confirmed pick needs: the
/// offered crop (as the overlay shows it), the raw detector box the pin
/// anchors on, and the display and frame the box was measured on so a pick
/// on any display maps back to the right pixels.
struct FaceOffer
{
    RegionOfInterest region;
    IntRect box;
    uint32_t displayId = 0;
    int frameWidth = 0;
    int frameHeight = 0;
};

/// A window rectangle in percent of the display, with its owning application.
struct WindowRegion
{
    RegionOfInterest region;
    std::string application;
};

/// The picker's suggestion list: the visible application windows, frontmost
/// first, deduplicated when two practically coincide. Window rectangles come
/// from the operating system and are exact - the picker offers nothing it
/// could be wrong about.
[[nodiscard]] std::vector<SuggestedRegion> buildRegionSuggestions(const std::vector<WindowRegion>& windows);

/// The offered crop for one detected face box: the box shrunk inward enough
/// to shed hair and background at the edges without losing the cheeks.
[[nodiscard]] RegionOfInterest faceSuggestionRegion(const IntRect& face, int frameWidth, int frameHeight);

/// Face rectangles (frame pixels) as picker suggestions. The detector's box
/// is shrunk inward: scoping a face means judging skin, and edge pixels are
/// hair and background that skew the vectorscope's skin cluster.
[[nodiscard]] std::vector<SuggestedRegion> buildFaceSuggestions(const std::vector<IntRect>& faces, int frameWidth,
                                                                int frameHeight);

/// One face offer per detector box, tagged with @p displayId and the frame
/// dimensions, so a confirmed pick on any display recovers its box. Each
/// offer's region is faceSuggestionRegion of the same box, matching the
/// overlay list buildFaceSuggestions produces for the display.
[[nodiscard]] std::vector<FaceOffer> buildFaceOffers(const std::vector<IntRect>& faces, uint32_t displayId,
                                                     int frameWidth, int frameHeight);

}  // namespace sidescopes
