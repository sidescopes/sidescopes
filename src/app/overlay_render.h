#pragma once

#include <optional>
#include <vector>

#include "app/overlay_style.h"
#include "imgui.h"
#include "sidescopes/module.h"

namespace sidescopes {

/// A scope image drawn into a pane: where it landed, how big it is, and the
/// magnify factor the overlays share with the trace.
struct DrawnScope
{
    ImVec2 origin;
    ImVec2 size;
    float zoom = 1.0f;
};

/// Maps a normalized [0,1] scope coordinate to the screen, applying the
/// magnify: overlays crop and scale around the center with the trace, which is
/// what keeps every mark glued to the cloud.
[[nodiscard]] ImVec2 at(const DrawnScope& scope, float nx, float ny);

/// Draws a scope's graticule from the module's declarative primitives. @p style
/// carries the host stroke weights the ABI leaves out; the roomy rule follows
/// the pane height.
void drawGraticule(const DrawnScope& scope, const std::vector<SsGraticulePrimitive>& primitives, GraticuleStyle style);

/// Draws a scope's cursor markers from the module's declarative markers, merging
/// and coloring them host-side. @p overrideColor forces one color for every
/// marker (the vectorscope's pinned references).
void drawMarkers(const DrawnScope& scope, const std::vector<SsMarker>& markers,
                 std::optional<uint32_t> overrideColor = std::nullopt);

}  // namespace sidescopes
