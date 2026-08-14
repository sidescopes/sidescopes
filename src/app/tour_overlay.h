#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "app/guided_tour.h"
#include "imgui.h"

namespace sidescopes {

/// Where each named control landed this frame.
///
/// Whatever draws a control notes its rectangle here, so the tour points at
/// what is actually on screen rather than at coordinates written down once
/// and left to rot. A control that did not draw simply has no entry, and its
/// stop speaks from the middle instead of pointing somewhere wrong.
class TourAnchors
{
public:
    void note(std::string_view id, const ImVec2& topLeft, const ImVec2& bottomRight);

    /// Everything from the previous frame, dropped. Called at the top of the
    /// frame so a control that stops drawing stops being pointed at.
    void clear();

    [[nodiscard]] std::optional<ImVec4> find(std::string_view id) const;

private:
    std::map<std::string, ImVec4, std::less<>> m_rects;
};

/// What the visitor pressed, for the caller to apply. Returned rather than
/// applied here so the tour's state changes in one place, the way every other
/// outcome in this application is handled.
enum class TourAction
{
    None,
    Advance,
    Skip,
};

/// Draws the current stop: the rest of the interface veiled, the control it
/// names left bright, and a bubble beside it with a way on and a way out.
///
/// Draws nothing and answers None when the tour is not running.
///
/// @param bounds the area to veil and to keep the bubble inside - the whole
///        window. The bubble is placed below its anchor, above it when there
///        is no room, and pushed back inside on a narrow one, which is the
///        case the strip of instructions this replaces could not handle.
[[nodiscard]] TourAction drawTourOverlay(const GuidedTour& tour, const TourAnchors& anchors, const ImVec2& boundsMin,
                                         const ImVec2& boundsMax);

}  // namespace sidescopes
