#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "app/scope_layout.h"
#include "app/scope_registry.h"

namespace sidescopes {

/// Which trace's intensity readout is flashing, and until when. The control is
/// named by the scope id that owns it; the waveform and its parade share one.
class TraceFlash
{
public:
    /// Shows @p control's readout until @p until, a glfwGetTime stamp.
    void show(std::string_view control, double until);

    /// @return Whether @p control's readout is still on screen at @p now.
    [[nodiscard]] bool showing(std::string_view control, double now) const;

private:
    std::string m_control;
    double m_until = 0.0;
};

/// What the user has on screen: the activation-ordered stack of scopes, keyed
/// by scope id, the view toggles, and each trace's intensity and marker
/// smoothing. Letters, order, and mask membership are resolved through the
/// registry it is constructed with.
class ScopeView
{
public:
    explicit ScopeView(const ScopeRegistry& registry);

    /// @return Whether @p id is on screen.
    [[nodiscard]] bool shows(std::string_view id) const;

    /// @return The scope ids on screen, in activation order.
    [[nodiscard]] const std::vector<std::string>& stack() const;

    /// Adds @p id, or removes it when already shown. The last scope stays,
    /// so the window is never empty.
    /// @return Whether @p id became newly visible, so the caller can refresh
    ///         its image.
    bool toggle(std::string_view id);

    /// Stacks @p id onto the current scopes when @p stack, otherwise solos it.
    /// @return Whether @p id became newly visible.
    bool choose(std::string_view id, bool stack);

    /// @return The scope ids the worker should compute for what is on screen:
    ///         the visible scopes minus the host-only ones (the color picker
    ///         reads the sampled cursor color, so it asks nothing of the
    ///         worker), in activation order.
    [[nodiscard]] std::vector<std::string> enabledScopeIds() const;

    /// Restores the stack from a preference token string, in the format
    /// parseStackTokens reads.
    void restoreStack(const std::string& tokens);

    /// @return The stack as a preference token string, in the format
    ///         formatStackTokens writes.
    [[nodiscard]] std::string stackTokens() const;

    [[nodiscard]] bool graticule() const;
    void setGraticule(bool on);

    [[nodiscard]] int zoom() const;
    void setZoom(int level);

    /// How the enabled scopes divide the window; Automatic by default, which
    /// splits the longer axis exactly as the app always has.
    [[nodiscard]] LayoutOrientation orientation() const;
    void setOrientation(LayoutOrientation orientation);

    /// The relative pane weight for @p id, defaulting to 1 for any scope never
    /// resized. A scope keeps its weight while toggled off and reuses it when
    /// shown again.
    [[nodiscard]] float weight(std::string_view id) const;
    void setWeight(std::string_view id, float value);

    /// The weights of the scopes on screen, in stacking order; feeds the layout
    /// split directly.
    [[nodiscard]] std::vector<float> stackWeights() const;

    /// Replaces every stored weight with @p weights, keyed by scope id; scopes
    /// absent from the map fall back to the default weight of 1. Used to apply a
    /// saved layout preset.
    void setWeights(const std::map<std::string, double>& weights);

    /// Every stored weight as a scope-id to value map, for persistence.
    [[nodiscard]] std::map<std::string, double> weightsSnapshot() const;

    /// Trace intensity and marker smoothing are tracked per control-owner id;
    /// the parade shares the waveform's, resolved here.
    [[nodiscard]] float intensity(std::string_view id) const;
    void setIntensity(std::string_view id, float percent);

    [[nodiscard]] float smoothing(std::string_view id) const;
    void setSmoothing(std::string_view id, float milliseconds);

private:
    const ScopeRegistry& m_registry;
    std::vector<std::string> m_stack{VectorscopeScopeId};
    bool m_graticule = true;
    int m_zoom = 1;
    LayoutOrientation m_orientation = LayoutOrientation::Automatic;
    std::map<std::string, float, std::less<>> m_weights;
    std::map<std::string, float, std::less<>> m_intensity;
    std::map<std::string, float, std::less<>> m_smoothing;
};

}  // namespace sidescopes
