#include "app/pane_layout.h"

namespace sidescopes {

LayoutOrientation PaneLayout::orientation() const
{
    return m_orientation;
}

void PaneLayout::setOrientation(LayoutOrientation orientation)
{
    m_orientation = orientation;
}

float PaneLayout::weight(std::string_view id) const
{
    const auto at = m_weights.find(id);

    return at != m_weights.end() ? at->second : 1.0f;
}

void PaneLayout::setWeight(std::string_view id, float value)
{
    m_weights[std::string{id}] = value;
}

std::vector<float> PaneLayout::stackWeights(const std::vector<std::string>& stack) const
{
    std::vector<float> weights;
    weights.reserve(stack.size());
    for (const std::string& id : stack) {
        weights.push_back(weight(id));
    }

    return weights;
}

void PaneLayout::setWeights(const std::map<std::string, double>& weights)
{
    m_weights.clear();
    for (const auto& [id, value] : weights) {
        m_weights[id] = static_cast<float>(value);
    }
}

std::map<std::string, double> PaneLayout::weightsSnapshot() const
{
    std::map<std::string, double> weights;
    for (const auto& [id, value] : m_weights) {
        weights[id] = value;
    }

    return weights;
}

}  // namespace sidescopes
