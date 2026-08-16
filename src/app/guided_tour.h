#pragma once

#include <string>
#include <vector>

namespace sidescopes {

/// One stop on the tour: what it points at, and what it says about it.
struct TourStep
{
    /// The anchor's id. Whatever draws that control registers a rectangle
    /// under this id during the frame; a step whose anchor is not registered
    /// speaks from the middle of the window rather than pointing at nothing,
    /// so a control that is not on screen this session costs a stop rather
    /// than breaking the tour.
    std::string anchor;
    std::string title;
    std::string body;
    /// How far the bright area stands off the control, in points.
    ///
    /// Per stop rather than one number for all of them, because the things a
    /// tour points at differ by more than any single value can cover: an icon
    /// is a couple of dozen points across and wants a snug ring, a region
    /// already carries a band and a close badge OUTSIDE itself that the ring
    /// has to clear, and something filling its container wants none at all or
    /// the ring falls off the edge.
    float halo = 8.0f;
};

/// @brief A walk-through of the interface, one control at a time.
///
/// Deliberately free of any toolkit: this is the ORDER of the stops and the
/// state of the walk, which is what wants testing and what a second host
/// would otherwise reimplement. The bubble that draws it lives beside this,
/// and the anchors come from whoever draws the controls.
///
/// It carries no text of its own either. The steps are handed in, because a
/// tour of the browser lab names things a desktop tour would not, and a
/// class that knew both would have to choose.
class GuidedTour
{
public:
    explicit GuidedTour(std::vector<TourStep> steps);

    /// From the first step. Starts again even when it has been settled
    /// before - that is what a "take the tour" button is for - and does
    /// nothing at all when there are no steps.
    void start();

    /// On to the next stop, finishing after the last. Finishing settles it.
    void advance();

    /// Out, at once, and settled: someone who waved it away should not be
    /// met with it again on the next visit.
    void skip();

    [[nodiscard]] bool running() const
    {
        return m_at >= 0;
    }

    /// The stop being shown, or nothing when the tour is not running.
    [[nodiscard]] const TourStep* current() const;

    /// One-based, for "3 of 7". Zero when not running.
    [[nodiscard]] int position() const
    {
        return m_at + 1;
    }

    [[nodiscard]] int count() const
    {
        return static_cast<int>(m_steps.size());
    }

    /// Whether this stop is the last one, so the button can say so.
    [[nodiscard]] bool onLastStep() const;

    /// Whether the visitor has seen it through or waved it away. THIS is what
    /// is remembered between visits - not which step they reached, because
    /// resuming a walk-through half way through is more confusing than
    /// starting it again.
    [[nodiscard]] bool settled() const
    {
        return m_settled;
    }

    /// Restores what was remembered. A tour that has not been settled starts
    /// itself, which is the whole point of remembering.
    void restoreSettled(bool settled);

private:
    std::vector<TourStep> m_steps;
    /// The step being shown, or -1 when the tour is not running.
    int m_at = -1;
    bool m_settled = false;
};

}  // namespace sidescopes
