#pragma once

#include <optional>
#include <string>
#include <vector>

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
    /// Whether a region has been selected at all. Without one the scopes read
    /// nothing, so no trace is drawn and the clear tool stands down.
    bool regionSelected;
    /// Whether a scope that takes pins is on screen; without one the pin tool
    /// stands down.
    bool pinsAvailable;
    /// The smoothed color each trace marks, empty until a sample lands and
    /// whenever the pointer is outside the region the scopes read.
    const std::optional<FloatColor>& vectorscopeColor;
    const std::optional<FloatColor>& waveformColor;
    /// The smoothed color under the cursor wherever it is, which the readout
    /// and the color picker show; empty until a sample lands.
    const std::optional<FloatColor>& readoutColor;
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
    /// A drag in the selector reordered the scopes on screen: the host applies
    /// the new sequence, which reflows the panes. Set only when the order
    /// actually changed.
    std::optional<std::vector<std::string>> reorderedStack;
    /// The clear tool: the host drops every region and attachment.
    bool clearRegion = false;
    /// A scope parameter changed: the host pushes the settings to the worker.
    bool analysisDirty = false;
    /// Interaction happened worth marking: the host stamps its activity clock.
    bool activity = false;
    /// A persisted value changed: the host schedules a preferences save.
    bool preferencesSaveDue = false;
};

}  // namespace sidescopes
