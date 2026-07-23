#pragma once

#include <optional>
#include <string>

#include "core/frame.h"
#include "imgui.h"

namespace sidescopes {

/// What one frame of pane drawing reads from the host: the values it
/// recomputes every frame, rather than the state the renderer owns or the
/// collaborators it holds. The references stay valid for the single
/// synchronous draw call.
struct PaneRenderInput
{
    /// The interface scale the divider thickness and the smallest pane are
    /// measured in.
    float uiScale;
    /// Whether the region the scopes read already covers the display, which
    /// the reset tool shows as already done.
    bool regionIsFullScreen;
    /// Whether a scope that takes pins is on screen; without one the pin tool
    /// stands down.
    bool pinsAvailable;
    /// The smoothed color under the cursor, per trace, empty until a sample
    /// lands.
    const std::optional<FloatColor>& vectorscopeColor;
    const std::optional<FloatColor>& waveformColor;
    /// The fixed-width companion font the picker aligns hex codes with; null
    /// when the system had none.
    ImFont* monospaceFont;
};

/// A scope a toolbar chip chose, and whether it joins the scopes on screen or
/// replaces them.
struct ScopeChoice
{
    std::string id;
    bool stack = false;
};

/// What one pane-drawing pass decided that the host must apply. The renderer
/// drives the collaborators it holds - the view's weights, the picker's
/// requests, the pin board - itself; only what the host alone can carry out
/// travels here. Several fields can be set at once, so each is applied in turn.
struct PaneRenderOutcome
{
    /// A chip chose a scope: the host shows it, which may wait briefly for the
    /// worker to fill its image.
    std::optional<ScopeChoice> chosenScope;
    /// The reset tool: the host drops every region and attachment.
    bool resetToFullScreen = false;
    /// A scope parameter changed: the host pushes the settings to the worker.
    bool analysisDirty = false;
    /// Interaction happened worth marking: the host stamps its activity clock.
    bool activity = false;
    /// A persisted value changed: the host schedules a preferences save.
    bool preferencesSaveDue = false;
};

}  // namespace sidescopes
