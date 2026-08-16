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

double TraceFlash::redrawDueSeconds() const
{
    return m_until;
}

ScopeView::ScopeView(const ScopeRegistry& registry)
    : m_order(registry),
      m_stack(registry, m_order)
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

ScopeOrder& ScopeView::order()
{
    return m_order;
}

const ScopeOrder& ScopeView::order() const
{
    return m_order;
}

bool ScopeView::reorderScopes(int from, int gap)
{
    if (!m_order.move(from, gap)) {
        return false;
    }
    m_stack.applyOrder();

    return true;
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

float ScopeView::graticuleStrength() const
{
    return m_graticuleStrength;
}

void ScopeView::setGraticuleStrength(float strength)
{
    m_graticuleStrength = cleanedGraticuleStrength(strength);
}

int ScopeView::zoom() const
{
    return m_zoom;
}

void ScopeView::setZoom(int level)
{
    m_zoom = level;
}

bool ScopeView::cursorMarkersVisible() const
{
    return m_cursorMarkersVisible;
}

void ScopeView::setCursorMarkersVisible(bool visible)
{
    m_cursorMarkersVisible = visible;
}

}  // namespace sidescopes
