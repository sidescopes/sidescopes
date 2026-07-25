#pragma once

#include <optional>

#include "core/frame.h"

namespace sidescopes {

/// How long a region must hold still before the capture is narrowed to it.
/// Narrowing asks the compositor to reconfigure a running stream, so a region
/// still being dragged, or an attached window still being moved, would pay for
/// one reconfiguration per frame and get nothing for it.
inline constexpr double CropSettleSeconds = 0.4;

/// The smallest region worth narrowing to, as a share of the display's pixels.
/// Below the gain is real but the region is already cheap, and above it the
/// reconfiguration costs more than it saves - a region covering nearly the whole
/// display would be narrowed, un-narrowed and narrowed again for a percent.
inline constexpr double CropWorthwhileShare = 0.8;

/// What the capture is being asked to deliver, and why. Everything the decision
/// needs, so the policy stays a pure function of observable state.
struct CropInputs
{
    /// The region to analyse, in display pixels; empty asks for no narrowing.
    IntRect region;
    /// The display's own pixel extents.
    int displayWidth = 0;
    int displayHeight = 0;
    /// The region picker is open: its display scans and its dragged-pin colour
    /// sample both read pixels anywhere on the display.
    bool pickerActive = false;
    /// A face lock is live: its content-stability grid and its probe crop both
    /// read the active window's rectangle, which is not the analysis region.
    bool faceLockActive = false;
    /// When the region last changed, and the current time, on the frame clock.
    double regionChangedAt = 0.0;
    double now = 0.0;
};

/// The sub-rectangle the capture should deliver, in display pixels, or nothing
/// at all when it should deliver the whole display.
///
/// Narrowing is only safe while the analysis worker is the sole reader of a
/// frame. Everything else that reads one - the picker's scans, a dragged pin's
/// average, the face probe - needs pixels the region does not contain, so any of
/// those being live keeps the whole display coming. They are not starved by the
/// wait: a frame says whether it is narrowed, so a reader that needs the whole
/// display can skip frames until an un-narrowed one arrives.
[[nodiscard]] std::optional<IntRect> cropFor(const CropInputs& inputs);

/// Asks cropFor on the caller's behalf, keeping the one piece of state the pure
/// policy cannot: when the region last changed. The region is written from three
/// different places - a picked region, an attached window's, a preset's - so
/// noticing the change here rather than at each of them keeps the settle rule in
/// one place and off the paths that merely set a region.
class CropTracker
{
public:
    /// The sub-rectangle the capture should deliver now, given the region in
    /// display pixels and who else is reading frames.
    [[nodiscard]] std::optional<IntRect> decide(IntRect regionPixels, int displayWidth, int displayHeight,
                                                bool pickerActive, bool faceLockActive, double now);

private:
    IntRect m_region;
    double m_changedAt = 0.0;
    bool m_seenRegion = false;
};

/// Whether @p view carries what a reader of the whole display needs. False for a
/// narrowed frame, whatever it contains, because the pixels that reader wants may
/// simply not be present.
[[nodiscard]] inline bool coversWholeDisplay(const FrameView& view)
{
    return !view.cropped();
}

}  // namespace sidescopes
