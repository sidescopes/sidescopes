#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "app/scope_layout.h"

namespace sidescopes {

/// @brief How the scopes on screen divide the window.
///
/// The split direction and a relative weight per scope id. It holds no pane
/// geometry of its own: scope_layout turns these into lengths for whatever
/// area it is handed.
class PaneLayout
{
public:
    /// How the enabled scopes divide the window; Automatic by default, which
    /// splits the longer axis exactly as the app always has.
    [[nodiscard]] LayoutOrientation orientation() const;
    void setOrientation(LayoutOrientation orientation);

    /// The relative pane weight for @p id, defaulting to 1 for any scope never
    /// resized. A scope keeps its weight while toggled off and reuses it when
    /// shown again.
    [[nodiscard]] float weight(std::string_view id) const;
    void setWeight(std::string_view id, float value);

    /// The weights of the scopes in @p stack, in stacking order; feeds the
    /// layout split directly.
    [[nodiscard]] std::vector<float> stackWeights(const std::vector<std::string>& stack) const;

    /// Replaces every stored weight with @p weights, keyed by scope id; scopes
    /// absent from the map fall back to the default weight of 1. Used to apply a
    /// saved layout preset.
    void setWeights(const std::map<std::string, double>& weights);

    /// Every stored weight as a scope-id to value map, for persistence.
    [[nodiscard]] std::map<std::string, double> weightsSnapshot() const;

private:
    LayoutOrientation m_orientation = LayoutOrientation::Automatic;
    std::map<std::string, float, std::less<>> m_weights;
};

}  // namespace sidescopes
