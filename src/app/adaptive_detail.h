#pragma once

#include <optional>
#include <string_view>
#include <utility>

#include "app/quality.h"
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

/// The panes the measured scopes last drew at, one size per scope FAMILY: the
/// scopes of a family share one image, so the largest pane any of them got is
/// what the ladder reads. Stated per family rather than per scope because a
/// member given an image size of its own draws correctly while reallocating
/// megabytes at every scope of every frame - working code with the cost hidden
/// inside it - and a struct with no room for that size cannot express it.
struct ScopePaneSizes
{
    PaneSize waveform;
    PaneSize histogram;
    PaneSize vectorscope;
};

/// The scope image resolutions one step settled on, for the host to put in
/// force.
struct DetailSizes
{
    /// The waveform image, columns by levels; every waveform-family scope
    /// shares it.
    std::pair<int, int> waveform;
    /// The histogram image, bins by height; both histograms share it.
    std::pair<int, int> histogram;
    /// The vectorscope image, square.
    int vectorscope = 0;

    [[nodiscard]] bool operator==(const DetailSizes&) const = default;
};

/// How long the desired resolutions must sit still before they are applied. A
/// live resize walks through every size on the way, and each one put in force
/// costs the engines a reallocation.
inline constexpr double DetailSettleSeconds = 0.4;

/// What each scope image's sides are divided by while the user drags the
/// region. Dragging a region across a picture is scanning it - for a blown
/// highlight, a colour cast, the next face - and a pass costs roughly what its
/// image covers, so a coarser image is most of the pass.
///
/// Measured against full detail on a photograph, magnifying the coarse image
/// back the way a texture is stretched over a pane, over the region sizes a
/// scan actually uses: the vectorscope moves by a mean of 0.03 to 0.13 of 255
/// and the histogram by 0.12 to 0.49 - both are upsamples of a grid that does
/// not change.
inline constexpr int DraggedDetailDivisor = 2;

/// The smallest side a dragged pass is asked for, so a small pane's image stays
/// a picture rather than a handful of cells.
inline constexpr int DraggedDetailFloor = 128;

/// What the samples per bin are further divided by while the region moves, for
/// the scopes that offer the thinning extension. It multiplies the level's own
/// thinning rather than replacing it, so a level that already samples thinly
/// still gives something up under the hand. It is the waveform family's
/// answer to the coarser image the others take: its columns are places in the
/// region and are left alone, so what it gives up instead is how densely each
/// column is filled. Measured at 1024 columns over a whole display, halving
/// them takes the pass to 64% of full for a mean of 1.3 of 255 - a better
/// trade per millisecond than any image axis on any scope, and three times
/// the saving. It also does nothing at all below about four megapixels, where
/// the budget already exceeds the region.
inline constexpr int DraggedSampleDivisor = 2;

/// @p settings with every scope image it names computed at a fraction of its
/// resolution: what to ask of the worker while the user drags the region.
///
/// The waveform's COLUMNS are the exception and are left alone. A column is a
/// place in the region - the same reason the sampling doctrine thins rows and
/// never columns - and scanning for a blown highlight or a skin tone is exactly
/// when that scope matters most. Measured on a photograph at the sizes the
/// application asks for, halving them moves the waveform by a mean of 5.5 to
/// 10.6 of 255 against 0.29 to 0.42 for the halved height, and the parade by
/// 2.1 to 4.1 against 0.10 to 0.14: nearly all of the cost of coarsening, on
/// the one axis that carries data rather than resolution. Per millisecond
/// saved the columns are the worst knob on any scope by a factor of five.
[[nodiscard]] AnalysisSettings coarsenedForDrag(AnalysisSettings settings);

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

    /// The waveform family's desired image, columns by levels, for the panes in
    /// @p panePixels. @p regionWidth caps the columns at what the region can
    /// populate; 0 means no frame yet and no cap. Stays at the resolution in
    /// force while no member of the family is on screen.
    [[nodiscard]] std::pair<int, int> desiredWaveformSize(const ScopePaneSizes& panePixels, int regionWidth) const;

    /// The histogram family's desired image, bins by height, for the panes in
    /// @p panePixels. Stays at the resolution in force while neither is on
    /// screen.
    [[nodiscard]] std::pair<int, int> desiredHistogramSize(const ScopePaneSizes& panePixels) const;

    /// The vectorscope's desired image edge for the pane in @p panePixels.
    /// Stays at the resolution in force while it is off screen.
    [[nodiscard]] int desiredVectorscopeSize(const ScopePaneSizes& panePixels) const;

    /// Reads the ladders above at @p level's numbers from here on. Only the
    /// resolutions move with it; how thinly the region is sampled and how often
    /// it is read are the host's to apply.
    void setQuality(QualityLevel level);

private:
    /// The numbers the level in force asks for.
    [[nodiscard]] const QualityProfile& profile() const;

    /// @return The image size @p id is computed at right now, {0, 0} for a
    ///         scope left at its module's default.
    [[nodiscard]] std::pair<int, int> currentSize(std::string_view id) const;

    /// The resolutions in force, as one value to compare a desired set against.
    [[nodiscard]] DetailSizes inForce() const;

    const ScopeView& m_view;
    const AnalysisSettings& m_analysis;
    QualityLevel m_quality = QualityLevel::Standard;

    // The resolutions waiting out the settle time, and when they were first
    // asked for.
    std::optional<DetailSizes> m_pending;
    double m_pendingSince = 0.0;
};

}  // namespace sidescopes
