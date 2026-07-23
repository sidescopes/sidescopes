#pragma once

#include <chrono>

namespace sidescopes {

class GraphicsBackend;

/// Presents each built frame and times it for the perf diagnostics channel.
/// The body clock is stamped only while the channel records, so a mid-frame
/// record toggle never times against a stale start; with the channel off this
/// is a plain present through the graphics backend.
class FrameTimer
{
public:
    /// @p graphics presents the built frame; it must outlive the timer.
    explicit FrameTimer(GraphicsBackend& graphics);

    /// Stamps the frame-body clock for the perf channel, after the event
    /// wait so an idle wait never counts as frame work. A no-op when perf
    /// diagnostics are off.
    void markFrameBodyStart();

    /// Presents the built frame and, for the perf channel, logs the frame
    /// body and the present/vsync wait as one line. A no-op present wrapper
    /// when perf diagnostics are off.
    void presentFrame();

private:
    GraphicsBackend& m_graphics;

    // Perf-channel frame timing: the frame body's start, stamped only while
    // the channel records; the flag marks the stamp fresh for this frame so
    // a mid-frame record toggle never times against a stale start.
    std::chrono::steady_clock::time_point m_frameBodyStart;
    bool m_frameBodyStamped = false;
};

}  // namespace sidescopes
