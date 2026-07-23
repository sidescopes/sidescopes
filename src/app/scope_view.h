#pragma once

#include <string>
#include <string_view>

#include "app/pane_layout.h"
#include "app/scope_registry.h"
#include "app/scope_stack.h"
#include "app/trace_params.h"

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

/// @brief What the user has on screen.
///
/// The scopes on screen, how their panes divide the window, each trace's
/// intensity and marker smoothing, and the two toggles that belong to no
/// single scope. It owns the three parts rather than forwarding to them, so
/// everything one set of scopes is drawn from travels as one object.
class ScopeView
{
public:
    explicit ScopeView(const ScopeRegistry& registry);

    /// The scopes on screen, in activation order.
    [[nodiscard]] ScopeStack& stack();
    [[nodiscard]] const ScopeStack& stack() const;

    /// The split direction and the weights the panes divide by.
    [[nodiscard]] PaneLayout& layout();
    [[nodiscard]] const PaneLayout& layout() const;

    /// Each trace's intensity and marker smoothing.
    [[nodiscard]] TraceParams& traces();
    [[nodiscard]] const TraceParams& traces() const;

    [[nodiscard]] bool graticule() const;
    void setGraticule(bool on);

    [[nodiscard]] int zoom() const;
    void setZoom(int level);

private:
    ScopeStack m_stack;
    PaneLayout m_layout;
    TraceParams m_traces;
    bool m_graticule = true;
    int m_zoom = 1;
};

}  // namespace sidescopes
