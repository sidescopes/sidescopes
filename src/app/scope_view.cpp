#include "app/scope_view.h"

namespace sidescopes {

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
    : m_stack(registry)
{
}

ScopeStack& ScopeView::stack()
{
    return m_stack;
}

const ScopeStack& ScopeView::stack() const
{
    return m_stack;
}

PaneLayout& ScopeView::layout()
{
    return m_layout;
}

const PaneLayout& ScopeView::layout() const
{
    return m_layout;
}

TraceParams& ScopeView::traces()
{
    return m_traces;
}

const TraceParams& ScopeView::traces() const
{
    return m_traces;
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

}  // namespace sidescopes
