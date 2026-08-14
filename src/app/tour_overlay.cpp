// The walk-through's bubble, in the application's own visual language.
//
// The veil, the spotlight and the two-tone outline are the region picker's,
// because a visitor meets that overlay a minute later and the two should
// read as one interface rather than as a tour bolted onto an application.

#include "app/tour_overlay.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {
namespace {

constexpr float BubbleWidth = 268.0f;
constexpr float BubblePad = 14.0f;
constexpr float BubbleGap = 14.0f;  // between the anchor and the bubble
constexpr float Rounding = 8.0f;

/// The warm tone this project gives every TRANSIENT indicator - the picker's
/// live drag wears the same one - for the reason stated where that was
/// decided: neutral grey vanishes against bright content, and only RESTING
/// chrome has to stay neutral beside the sampled pixels. A walk-through is as
/// transient as anything here, and being the one coloured thing on screen is
/// exactly what tells it apart from the application's own borders.
[[nodiscard]] ImU32 accent(float alpha)
{
    return IM_COL32(255, 214, 140, static_cast<int>(std::lround(alpha * 255.0f)));
}

[[nodiscard]] ImU32 grey(float white, float alpha)
{
    const int value = static_cast<int>(std::lround(white * 255.0f));

    return IM_COL32(value, value, value, static_cast<int>(std::lround(alpha * 255.0f)));
}

/// The veil, with a hole cut for the control being named. Four rectangles
/// around the hole rather than one wash with a hole punched in it, because a
/// draw list has no subtractive fill.
void veilAround(ImDrawList* draw, const ImVec2& boundsMin, const ImVec2& boundsMax, const std::optional<ImVec4>& hole,
                float halo)
{
    const ImU32 veil = grey(0.0f, 0.55f);
    if (!hole) {
        draw->AddRectFilled(boundsMin, boundsMax, veil);

        return;
    }
    // Kept INSIDE the bounds, and by a hair more than the stroke is wide, so
    // an anchor that fills its container keeps a visible ring instead of one
    // trimmed away at the corners.
    const float inset = 2.0f;
    const ImVec2 holeMin{std::max(boundsMin.x + inset, hole->x - halo), std::max(boundsMin.y + inset, hole->y - halo)};
    const ImVec2 holeMax{std::min(boundsMax.x - inset, hole->z + halo), std::min(boundsMax.y - inset, hole->w + halo)};
    if (holeMax.x <= holeMin.x || holeMax.y <= holeMin.y) {
        // The anchor lies outside these bounds altogether - the filmstrip sits
        // above the canvas - so there is nothing here to leave bright. The
        // page lights that one itself.
        draw->AddRectFilled(boundsMin, boundsMax, veil);

        return;
    }

    draw->AddRectFilled(boundsMin, ImVec2{boundsMax.x, holeMin.y}, veil);
    draw->AddRectFilled(ImVec2{boundsMin.x, holeMax.y}, boundsMax, veil);
    draw->AddRectFilled(ImVec2{boundsMin.x, holeMin.y}, ImVec2{holeMin.x, holeMax.y}, veil);
    draw->AddRectFilled(ImVec2{holeMax.x, holeMin.y}, ImVec2{boundsMax.x, holeMax.y}, veil);

    // A dark pass under the warm one, as everything else here is drawn, so a
    // tone survives whatever the control is sitting on.
    draw->AddRect(holeMin, holeMax, grey(0.1f, 0.75f), Rounding, 0, 3.0f);
    draw->AddRect(holeMin, holeMax, accent(0.95f), Rounding, 0, 1.5f);
}

/// Where the bubble sits: under the control it names, over it when there is
/// no room beneath, and always inside @p bounds. A narrow window is the case
/// that matters - it is why the row of instructions this replaces was trimmed
/// away rather than wrapped.
/// @p wanted, kept inside [@p low, @p high] for something @p extent long,
/// with a gap at the edge. Never returns a position past the low end, so a
/// bubble too tall for the bounds is cut at the bottom rather than the top -
/// the title and the buttons are what must survive.
[[nodiscard]] float inside(float wanted, float low, float high, float extent)
{
    const float most = std::max(low + BubbleGap, high - extent - BubbleGap);

    return std::clamp(wanted, low + BubbleGap, most);
}

[[nodiscard]] ImVec2 bubbleOrigin(const std::optional<ImVec4>& anchor, const ImVec2& boundsMin, const ImVec2& boundsMax,
                                  float width, float height, float halo)
{
    if (!anchor) {
        return ImVec2{(boundsMin.x + boundsMax.x - width) * 0.5f, (boundsMin.y + boundsMax.y - height) * 0.5f};
    }
    const float below = anchor->w + halo + BubbleGap;
    const float above = anchor->y - halo - BubbleGap - height;
    const float wantedY = below + height <= boundsMax.y ? below : above;
    // Centred on the control, then pushed back inside - BOTH ways. An anchor
    // can sit outside these bounds entirely: the filmstrip is above the
    // canvas, so its stop asks for a bubble at a negative y, and without the
    // clamp the title was simply cut off the top.
    const float wantedX = (anchor->x + anchor->z - width) * 0.5f;

    return ImVec2{inside(wantedX, boundsMin.x, boundsMax.x, width), inside(wantedY, boundsMin.y, boundsMax.y, height)};
}

/// A flat button in the overlay's own palette: Dear ImGui's styled button
/// belongs to the window it is in, and this draws into the foreground.
[[nodiscard]] bool bubbleButton(ImDrawList* draw, const ImVec2& topLeft, const char* label, bool strong)
{
    const ImVec2 text = ImGui::CalcTextSize(label);
    const ImVec2 bottomRight{topLeft.x + text.x + 22.0f, topLeft.y + text.y + 12.0f};
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool over =
        mouse.x >= topLeft.x && mouse.x <= bottomRight.x && mouse.y >= topLeft.y && mouse.y <= bottomRight.y;

    if (strong) {
        draw->AddRectFilled(topLeft, bottomRight, grey(over ? 1.0f : 0.93f, 1.0f), 5.0f);
    } else if (over) {
        draw->AddRectFilled(topLeft, bottomRight, grey(1.0f, 0.14f), 5.0f);
    }
    draw->AddText(ImVec2{topLeft.x + 11.0f, topLeft.y + 6.0f}, strong ? grey(0.08f, 1.0f) : grey(0.88f, 1.0f), label);

    return over && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

}  // namespace

void TourAnchors::note(std::string_view id, const ImVec2& topLeft, const ImVec2& bottomRight)
{
    m_rects[std::string(id)] = ImVec4{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
}

void TourAnchors::clear()
{
    m_rects.clear();
}

std::optional<ImVec4> TourAnchors::find(std::string_view id) const
{
    const auto found = m_rects.find(id);
    if (found == m_rects.end()) {
        return std::nullopt;
    }

    return found->second;
}

TourAction drawTourOverlay(const GuidedTour& tour, const TourAnchors& anchors, const ImVec2& boundsMin,
                           const ImVec2& boundsMax)
{
    const TourStep* step = tour.current();
    if (step == nullptr) {
        return TourAction::None;
    }
    // The foreground list, so the veil covers every window rather than only
    // the one that happened to call this.
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const std::optional<ImVec4> anchor = anchors.find(step->anchor);
    veilAround(draw, boundsMin, boundsMax, anchor, step->halo);

    const float wrap = BubbleWidth - BubblePad * 2.0f;
    const ImVec2 titleSize = ImGui::CalcTextSize(step->title.c_str());
    const ImVec2 bodySize = ImGui::CalcTextSize(step->body.c_str(), nullptr, false, wrap);
    const float buttonRow = ImGui::GetTextLineHeight() + 12.0f;
    const float height = BubblePad * 2.0f + titleSize.y + 6.0f + bodySize.y + 14.0f + buttonRow;
    const ImVec2 origin = bubbleOrigin(anchor, boundsMin, boundsMax, BubbleWidth, height, step->halo);
    const ImVec2 corner{origin.x + BubbleWidth, origin.y + height};

    draw->AddRectFilled(origin, corner, grey(0.08f, 0.96f), Rounding);
    draw->AddRect(origin, corner, grey(0.97f, 0.35f), Rounding, 0, 1.0f);

    float y = origin.y + BubblePad;
    draw->AddText(ImVec2{origin.x + BubblePad, y}, grey(0.98f, 1.0f), step->title.c_str());
    y += titleSize.y + 6.0f;
    draw->AddText(nullptr, 0.0f, ImVec2{origin.x + BubblePad, y}, grey(0.82f, 1.0f), step->body.c_str(), nullptr, wrap);
    y += bodySize.y + 14.0f;

    // "3 of 7", so the visitor knows how much they have let themselves in for.
    const std::string counter = std::to_string(tour.position()) + " of " + std::to_string(tour.count());
    draw->AddText(ImVec2{origin.x + BubblePad, y + 6.0f}, grey(0.55f, 1.0f), counter.c_str());

    const char* onward = tour.onLastStep() ? "Done" : "Next";
    const ImVec2 onwardSize = ImGui::CalcTextSize(onward);
    const ImVec2 onwardAt{corner.x - BubblePad - onwardSize.x - 22.0f, y};
    TourAction action = TourAction::None;
    if (bubbleButton(draw, onwardAt, onward, /*strong=*/true)) {
        action = TourAction::Advance;
    }
    if (!tour.onLastStep()) {
        const ImVec2 skipSize = ImGui::CalcTextSize("Skip");
        if (bubbleButton(draw, ImVec2{onwardAt.x - skipSize.x - 30.0f, y}, "Skip", /*strong=*/false)) {
            action = TourAction::Skip;
        }
    }

    return action;
}

}  // namespace sidescopes
