#include "app/scope_view.h"

#include <algorithm>

namespace sidescopes {

void TraceFlash::show(TraceControl control, double until)
{
    m_control = control;
    m_until = until;
}

bool TraceFlash::showing(TraceControl control, double now) const
{
    return m_control == control && now < m_until;
}

bool ScopeView::shows(ScopeGlyph kind) const
{
    return std::find(m_stack.begin(), m_stack.end(), kind) != m_stack.end();
}

const std::vector<ScopeGlyph>& ScopeView::stack() const
{
    return m_stack;
}

bool ScopeView::toggle(ScopeGlyph kind)
{
    const auto at = std::find(m_stack.begin(), m_stack.end(), kind);
    if (at != m_stack.end()) {
        // The last scope stays: the window never goes empty.
        if (m_stack.size() > 1) {
            m_stack.erase(at);
        }
        return false;
    }
    m_stack.push_back(kind);
    return true;
}

bool ScopeView::choose(ScopeGlyph kind, bool stack)
{
    if (stack) {
        return toggle(kind);
    }
    const bool wasShown = shows(kind);
    m_stack.assign(1, kind);
    return !wasShown;
}

uint32_t ScopeView::enabledMask() const
{
    uint32_t mask = 0;
    for (const ScopeGlyph kind : m_stack) {
        mask |= scopeEnableBit(kind);
    }
    return mask;
}

void ScopeView::restoreStack(const std::string& letters)
{
    m_stack.clear();
    for (const char letter : letters) {
        for (const ScopeGlyph kind : AllScopes) {
            if (scopeLetter(kind) == letter) {
                m_stack.push_back(kind);
            }
        }
    }
    if (m_stack.empty()) {
        m_stack.push_back(ScopeGlyph::Vectorscope);
    }
}

std::string ScopeView::stackLetters() const
{
    std::string letters;
    for (const ScopeGlyph kind : m_stack) {
        letters += scopeLetter(kind);
    }
    return letters;
}

bool ScopeView::graticule() const
{
    return m_graticule;
}

void ScopeView::setGraticule(bool on)
{
    m_graticule = on;
}

bool ScopeView::percentValues() const
{
    return m_percentValues;
}

void ScopeView::setPercentValues(bool on)
{
    m_percentValues = on;
}

int ScopeView::zoom() const
{
    return m_zoom;
}

void ScopeView::setZoom(int level)
{
    m_zoom = level;
}

float ScopeView::intensity(TraceControl control) const
{
    return control == TraceControl::Vectorscope ? m_vectorscopeIntensity : m_waveformIntensity;
}

void ScopeView::setIntensity(TraceControl control, float percent)
{
    if (control == TraceControl::Vectorscope) {
        m_vectorscopeIntensity = percent;
    } else {
        m_waveformIntensity = percent;
    }
}

float ScopeView::smoothing(TraceControl control) const
{
    return control == TraceControl::Vectorscope ? m_vectorscopeSmoothing : m_waveformSmoothing;
}

void ScopeView::setSmoothing(TraceControl control, float milliseconds)
{
    if (control == TraceControl::Vectorscope) {
        m_vectorscopeSmoothing = milliseconds;
    } else {
        m_waveformSmoothing = milliseconds;
    }
}

}  // namespace sidescopes
