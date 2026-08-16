#pragma once

#include <string>
#include <string_view>

#include "app/overlay_style.h"
#include "app/pane_layout.h"
#include "app/scope_order.h"
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

    /// @return When the readout leaves the screen by itself, or zero while
    ///         none has ever been shown. The frame loop draws a frame then,
    ///         since nothing else will take it away.
    [[nodiscard]] double redrawDueSeconds() const;

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

    /// The scopes on screen, in the preferred order.
    [[nodiscard]] ScopeStack& stack();
    [[nodiscard]] const ScopeStack& stack() const;

    /// The order the user keeps every scope in, on screen or not.
    [[nodiscard]] ScopeOrder& order();
    [[nodiscard]] const ScopeOrder& order() const;

    /// Moves the scope at @p from to the @p gap slot of the preferred order,
    /// bringing the panes with it - the two are one gesture, so nothing else
    /// has to remember to re-seat them.
    /// @return Whether the order changed.
    bool reorderScopes(int from, int gap);

    /// The split direction and the weights the panes divide by.
    [[nodiscard]] PaneLayout& layout();
    [[nodiscard]] const PaneLayout& layout() const;

    /// Each trace's intensity and marker smoothing.
    [[nodiscard]] TraceParams& traces();
    [[nodiscard]] const TraceParams& traces() const;

    /// How strongly the graticule is drawn over every scope. Bounded below
    /// rather than switchable off: it is what makes a trace readable, and with
    /// no region selected it is the whole instrument.
    [[nodiscard]] float graticuleStrength() const;

    /// Sets the graticule's strength, snapped to an offered step.
    void setGraticuleStrength(float strength);

    [[nodiscard]] int zoom() const;
    void setZoom(int level);

    /// Whether the live color under the pointer is marked on compatible
    /// scopes. The numeric readout and pinned references are independent.
    [[nodiscard]] bool cursorMarkersVisible() const;
    void setCursorMarkersVisible(bool visible);

private:
    // Before the stack, which reads it as it seats a scope.
    ScopeOrder m_order;
    ScopeStack m_stack;
    PaneLayout m_layout;
    TraceParams m_traces;
    float m_graticuleStrength = DefaultGraticuleStrength;
    int m_zoom = 1;
    bool m_cursorMarkersVisible = true;
};

}  // namespace sidescopes
