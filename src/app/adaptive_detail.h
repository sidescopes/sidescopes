#pragma once

#include <optional>
#include <string_view>
#include <utility>

#include "core/analysis_worker.h"

namespace sidescopes {

class ScopeView;

/// One scope pane's extent - in interface points as the panes are measured, or
/// in framebuffer pixels once the display density is folded in.
struct PaneSize
{
    float width = 0.0f;
    float height = 0.0f;
};

/// The panes the measured scopes last drew at. The waveform and the parade
/// share one image, so both are measured and the larger decides.
struct ScopePaneSizes
{
    PaneSize waveform;
    PaneSize parade;
    PaneSize histogram;
    PaneSize vectorscope;
    PaneSize neutral;
};

/// The scope image resolutions one step settled on, for the host to put in
/// force.
struct DetailSizes
{
    /// The waveform image, columns by levels; the parade shares it.
    std::pair<int, int> waveform;
    /// The histogram image, bins by height.
    std::pair<int, int> histogram;
    /// The vectorscope image, square.
    int vectorscope = 0;
    /// The neutral plane, square.
    int neutral = 0;

    [[nodiscard]] bool operator==(const DetailSizes&) const = default;
};

/// How long the desired resolutions must sit still before they are applied. A
/// live resize walks through every size on the way, and each one put in force
/// costs the engines a reallocation.
inline constexpr double DetailSettleSeconds = 0.4;

/// What each scope image's sides are divided by while the region is moving.
/// Dragging a region across a picture is scanning it - for a blown highlight, a
/// colour cast, the next face - and none of that is read off the finest detail
/// of a trace. A pass costs roughly what its image covers, so halving both
/// sides is most of the pass.
///
/// Measured against full detail on a photograph, magnifying the coarse image
/// back the way a texture is stretched over a pane: the vectorscope moves by a
/// mean of 0.02 of 255 and the histogram by 0.10 - both are upsamples of a grid
/// that does not change - the neutral plane by 1.13 with its cast reading
/// unmoved, and the waveform by 3.8, all of it from the halved columns. Its
/// halved height costs 0.18, the levels being fixed at 256 by 8-bit input.
inline constexpr int MovingDetailDivisor = 2;

/// The smallest side a moving pass is asked for, so a small pane's image stays
/// a picture rather than a handful of cells.
inline constexpr int MovingDetailFloor = 128;

/// How long the region must sit still before the scopes go back to full
/// detail. Longer than the gap between two steps of a drag, so one pass is not
/// computed at full detail in the middle of one, and short enough that letting
/// go is followed by the sharp trace.
inline constexpr double RegionSettleSeconds = 0.25;

/// @p settings with every scope image it names computed at a fraction of its
/// resolution: what to ask of the worker while the region is moving.
[[nodiscard]] AnalysisSettings coarsenedForMotion(AnalysisSettings settings);

/// Decides what resolution each scope's image is computed at from the pane the
/// scope actually gets, and holds a change back until the sizes have sat still.
/// It reads the view and the settings it is constructed with and returns the
/// settled resolutions; putting them in force - writing the settings and
/// pushing them to the worker - stays with the host.
class AdaptiveDetail
{
public:
    /// @p view answers which scopes are on screen; @p analysis carries the
    /// resolutions in force and the region they are capped against. Both must
    /// outlive this.
    AdaptiveDetail(const ScopeView& view, const AnalysisSettings& analysis);

    /// One per-frame step. @p panes are the panes the scopes last drew at in
    /// interface points, @p density the framebuffer pixels one point covers,
    /// @p frameSize the captured frame the region is measured in (empty before
    /// the first frame lands), and @p now the frame clock.
    /// @return The resolutions to put in force, on the one step the debounce
    ///         elapses on; empty on every other.
    [[nodiscard]] std::optional<DetailSizes> update(const ScopePaneSizes& panes, float density,
                                                    std::optional<AnalysisWorker::FrameSize> frameSize, double now);

    /// The waveform's desired image, columns by levels, for the panes in
    /// @p panePixels. @p regionWidth caps the columns at what the region can
    /// populate; 0 means no frame yet and no cap. Stays at the resolution in
    /// force while neither the waveform nor the parade is on screen.
    [[nodiscard]] std::pair<int, int> desiredWaveformSize(const ScopePaneSizes& panePixels, int regionWidth) const;

    /// The histogram's desired image, bins by height, for the pane in
    /// @p panePixels. Stays at the resolution in force while it is off screen.
    [[nodiscard]] std::pair<int, int> desiredHistogramSize(const ScopePaneSizes& panePixels) const;

    /// The vectorscope's desired image edge for the pane in @p panePixels.
    /// Stays at the resolution in force while it is off screen.
    [[nodiscard]] int desiredVectorscopeSize(const ScopePaneSizes& panePixels) const;

    /// The neutral plane's desired edge for the pane in @p panePixels. Unlike
    /// the vectorscope's, this is not only a display resolution: the
    /// near-neutral cloud is accumulated at it, so a larger plane resolves the
    /// cloud more finely rather than interpolating it. Stays at the resolution
    /// in force while it is off screen.
    [[nodiscard]] int desiredNeutralSize(const ScopePaneSizes& panePixels) const;

private:
    /// @return The image size @p id is computed at right now, {0, 0} for a
    ///         scope left at its module's default.
    [[nodiscard]] std::pair<int, int> currentSize(std::string_view id) const;

    /// The resolutions in force, as one value to compare a desired set against.
    [[nodiscard]] DetailSizes inForce() const;

    const ScopeView& m_view;
    const AnalysisSettings& m_analysis;

    // The resolutions waiting out the settle time, and when they were first
    // asked for.
    std::optional<DetailSizes> m_pending;
    double m_pendingSince = 0.0;
};

}  // namespace sidescopes
