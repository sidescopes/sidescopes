#include "app/scope_view.h"

#include <algorithm>

#include "app/scope_registry.h"

namespace sidescopes {
namespace {

/// The control-owner id for @p id: the parade shares the waveform's intensity
/// and smoothing, so both resolve to one control.
std::string_view controlKey(std::string_view id)
{
    return id == ParadeScopeId ? std::string_view{WaveformScopeId} : id;
}

}  // namespace

void TraceFlash::show(std::string_view control, double until)
{
    m_control = control;
    m_until = until;
}

bool TraceFlash::showing(std::string_view control, double now) const
{
    return m_control == control && now < m_until;
}

ScopeView::ScopeView(const ScopeRegistry& registry)
    : m_registry(registry)
{
}

bool ScopeView::shows(std::string_view id) const
{
    return std::find(m_stack.begin(), m_stack.end(), id) != m_stack.end();
}

const std::vector<std::string>& ScopeView::stack() const
{
    return m_stack;
}

bool ScopeView::toggle(std::string_view id)
{
    const auto at = std::find(m_stack.begin(), m_stack.end(), id);
    if (at != m_stack.end()) {
        // The last scope stays: the window never goes empty.
        if (m_stack.size() > 1) {
            m_stack.erase(at);
        }
        return false;
    }
    m_stack.emplace_back(id);

    return true;
}

bool ScopeView::choose(std::string_view id, bool stack)
{
    if (stack) {
        return toggle(id);
    }
    const bool wasShown = shows(id);
    m_stack.assign(1, std::string{id});

    return !wasShown;
}

std::vector<std::string> ScopeView::enabledScopeIds() const
{
    std::vector<std::string> ids;
    for (const std::string& id : m_stack) {
        const HostScope* scope = m_registry.byId(id);
        if (scope && !scope->host) {
            ids.push_back(id);
        }
    }

    return ids;
}

void ScopeView::restoreStack(const std::string& tokens)
{
    m_stack.clear();
    for (std::size_t at = 0; at < tokens.size();) {
        const HostScope* scope = nullptr;
        if (tokens[at] == '[') {
            const auto close = tokens.find(']', at);
            if (close == std::string::npos) {
                break;
            }
            scope = m_registry.byId(tokens.substr(at + 1, close - at - 1));
            at = close + 1;
        } else {
            scope = m_registry.byLetter(tokens[at]);
            ++at;
        }
        if (scope != nullptr && !shows(scope->id)) {
            m_stack.push_back(scope->id);
        }
    }
    if (m_stack.empty()) {
        m_stack.emplace_back(VectorscopeScopeId);
    }
}

std::string ScopeView::stackTokens() const
{
    std::string tokens;
    for (const std::string& id : m_stack) {
        const HostScope* scope = m_registry.byId(id);
        if (scope != nullptr && scope->letter != 0) {
            tokens += scope->letter;
        } else {
            tokens += '[';
            tokens += id;
            tokens += ']';
        }
    }

    return tokens;
}

bool ScopeView::graticule() const
{
    return m_graticule;
}

void ScopeView::setGraticule(bool on)
{
    m_graticule = on;
}

int ScopeView::zoom() const
{
    return m_zoom;
}

void ScopeView::setZoom(int level)
{
    m_zoom = level;
}

LayoutOrientation ScopeView::orientation() const
{
    return m_orientation;
}

void ScopeView::setOrientation(LayoutOrientation orientation)
{
    m_orientation = orientation;
}

float ScopeView::weight(std::string_view id) const
{
    const auto at = m_weights.find(id);

    return at != m_weights.end() ? at->second : 1.0f;
}

void ScopeView::setWeight(std::string_view id, float value)
{
    m_weights[std::string{id}] = value;
}

std::vector<float> ScopeView::stackWeights() const
{
    std::vector<float> weights;
    weights.reserve(m_stack.size());
    for (const std::string& id : m_stack) {
        weights.push_back(weight(id));
    }

    return weights;
}

void ScopeView::setWeights(const std::map<std::string, double>& weights)
{
    m_weights.clear();
    for (const auto& [id, value] : weights) {
        m_weights[id] = static_cast<float>(value);
    }
}

std::map<std::string, double> ScopeView::weightsSnapshot() const
{
    std::map<std::string, double> weights;
    for (const auto& [id, value] : m_weights) {
        weights[id] = value;
    }

    return weights;
}

float ScopeView::intensity(std::string_view id) const
{
    const auto at = m_intensity.find(controlKey(id));

    return at != m_intensity.end() ? at->second : 0.0f;
}

void ScopeView::setIntensity(std::string_view id, float percent)
{
    m_intensity[std::string{controlKey(id)}] = percent;
}

float ScopeView::smoothing(std::string_view id) const
{
    const auto at = m_smoothing.find(controlKey(id));

    return at != m_smoothing.end() ? at->second : 0.0f;
}

void ScopeView::setSmoothing(std::string_view id, float milliseconds)
{
    m_smoothing[std::string{controlKey(id)}] = milliseconds;
}

}  // namespace sidescopes
