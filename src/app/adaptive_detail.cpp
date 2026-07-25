#include "app/adaptive_detail.h"

#include <algorithm>
#include <initializer_list>
#include <string>

#include "app/scope_view.h"
#include "core/scopes/neutral.h"
#include "core/scopes/waveform.h"

namespace sidescopes {

namespace {

// The thresholds below are pixel counts, so a pane measured in points becomes
// what it covers on the display first.
PaneSize scaled(const PaneSize& pane, float density)
{
    return PaneSize{pane.width * density, pane.height * density};
}

ScopePaneSizes inPixels(const ScopePaneSizes& panes, float density)
{
    return ScopePaneSizes{scaled(panes.waveform, density), scaled(panes.parade, density),
                          scaled(panes.histogram, density), scaled(panes.vectorscope, density),
                          scaled(panes.neutral, density)};
}

// How much the display may magnify a scope texture before the softness shows.
// A trace is smooth, so a little magnification is invisible - and letting a
// small scope keep a small image is what keeps a small scope cheap. Past this
// factor the texture is visibly stretched, which is what a scope filling a
// second monitor looked like while these ladders stopped part way up.
constexpr float MagnificationTolerance = 1.4f;

// The smallest offered resolution that covers the pane to within @p tolerance.
// Rounding up rather than down also buys free supersampling, which is what the
// hand-written thresholds this replaces were already doing.
int resolutionCovering(float paneExtentPixels, std::initializer_list<int> ladder)
{
    const float wanted = paneExtentPixels / MagnificationTolerance;
    for (const int step : ladder) {
        if (static_cast<float>(step) >= wanted) {
            return step;
        }
    }

    return *(ladder.end() - 1);
}

// The largest offered resolution the region can actually populate: there is no
// point resolving more columns than the region has pixels to put in them.
int resolutionWithin(int regionWidth, std::initializer_list<int> ladder)
{
    int chosen = *ladder.begin();
    for (const int step : ladder) {
        if (step <= regionWidth) {
            chosen = step;
        }
    }

    return chosen;
}

}  // namespace

AdaptiveDetail::AdaptiveDetail(const ScopeView& view, const AnalysisSettings& analysis)
    : m_view(view),
      m_analysis(analysis)
{
}

std::pair<int, int> AdaptiveDetail::currentSize(std::string_view id) const
{
    const auto at = m_analysis.imageSizes.find(std::string{id});

    return at != m_analysis.imageSizes.end() ? at->second : std::pair<int, int>{0, 0};
}

std::pair<int, int> AdaptiveDetail::desiredWaveformSize(const ScopePaneSizes& panePixels, int regionWidth) const
{
    const std::pair<int, int> waveSize = currentSize(WaveformScopeId);
    int wantColumns = waveSize.first;
    int wantHeight = waveSize.second;
    if (m_view.stack().shows(WaveformScopeId) || m_view.stack().shows(ParadeScopeId)) {
        const float wfWidth = std::max(panePixels.waveform.width, panePixels.parade.width);
        const float wfHeight = std::max(panePixels.waveform.height, panePixels.parade.height);
        wantColumns = resolutionCovering(wfWidth, {512, 1024, 2048, MaximumWaveformColumns});
        if (regionWidth > 0) {
            wantColumns =
                std::min(wantColumns, resolutionWithin(regionWidth, {512, 1024, 2048, MaximumWaveformColumns}));
        }
        // Height only draws a finer spline through levels that eight-bit input
        // fixes at 256, and it is priced like real data: doubling it cost 14 ms
        // a pass over a whole display for 2.5% more resolved detail, against
        // 4.5% for free from the columns. So this threshold stays where
        // measurement put it while the columns follow the pane.
        wantHeight = wfHeight >= 560.0f ? 512 : WaveformLevels;
    }

    return {wantColumns, wantHeight};
}

std::pair<int, int> AdaptiveDetail::desiredHistogramSize(const ScopePaneSizes& panePixels) const
{
    const std::pair<int, int> histSize = currentSize(HistogramScopeId);
    int wantHistWidth = histSize.first;
    int wantHistHeight = histSize.second;
    if (m_view.stack().shows(HistogramScopeId)) {
        // Near one texture pixel per screen pixel keeps the outline's width even
        // on flats and steep slopes alike.
        const PaneSize scopePane = panePixels.histogram;
        wantHistWidth = resolutionCovering(scopePane.width, {512, 1024, 2048, 3072, 4096});
        // The bright outline is stroked by the interface at display resolution,
        // so the texture carries only the dim fill and its height is the least
        // visible of these; left where it was.
        wantHistHeight = scopePane.height >= 560.0f ? 768 : 384;
    }

    return {wantHistWidth, wantHistHeight};
}

int AdaptiveDetail::desiredVectorscopeSize(const ScopePaneSizes& panePixels) const
{
    int wantVectorscope = currentSize(VectorscopeScopeId).second;
    if (m_view.stack().shows(VectorscopeScopeId)) {
        // Purely a display resolution: accumulation stays on the 256-code grid
        // and a finer image is interpolated from it, so a sparse region costs
        // nothing extra. This does NOT follow the pane past its step, because
        // measurement says a finer image resolves nothing the reconstruction
        // has not already spread: on a photograph 256, 512 and 1024 magnify to
        // the same detail, and on saturated content 1024 reads softer than 512
        // while costing five milliseconds a pass more. A vectorscope that looks
        // soft on a large pane is limited by its code grid, not by this.
        const PaneSize scopePane = panePixels.vectorscope;
        const float extent = std::min(scopePane.width, scopePane.height);
        wantVectorscope = extent >= 480.0f ? 512 : 256;
    }

    return wantVectorscope;
}

int AdaptiveDetail::desiredNeutralSize(const ScopePaneSizes& panePixels) const
{
    int wantNeutral = currentSize(NeutralScopeId).second;
    if (m_view.stack().shows(NeutralScopeId)) {
        // The cloud is accumulated at this resolution, so this is real detail
        // and not interpolation. It stops at a thousand because every sample is
        // splatted over three pixels, and past that the plane would resolve
        // structure the splat has already spread.
        const PaneSize scopePane = panePixels.neutral;
        wantNeutral = resolutionCovering(std::min(scopePane.width, scopePane.height), {256, 512, MaximumNeutralSize});
    }

    return wantNeutral;
}

DetailSizes AdaptiveDetail::inForce() const
{
    return DetailSizes{currentSize(WaveformScopeId), currentSize(HistogramScopeId),
                       currentSize(VectorscopeScopeId).second, currentSize(NeutralScopeId).second};
}

std::optional<DetailSizes> AdaptiveDetail::update(const ScopePaneSizes& panes, float density,
                                                  std::optional<AnalysisWorker::FrameSize> frameSize, double now)
{
    // Resolution follows the pane a scope actually gets, and never exceeds what
    // the region can populate; desired resolutions are debounced so a live
    // resize does not thrash engine reallocation.
    int regionWidth = 0;
    if (frameSize) {
        // The region is a share of the DISPLAY, so it is resolved against the
        // display's extents whether or not the capture covers all of it.
        regionWidth = m_analysis.region.toPixels(frameSize->displayWidth, frameSize->displayHeight).width;
    }
    const ScopePaneSizes panePixels = inPixels(panes, density);
    const DetailSizes wanted{desiredWaveformSize(panePixels, regionWidth), desiredHistogramSize(panePixels),
                             desiredVectorscopeSize(panePixels), desiredNeutralSize(panePixels)};

    if (wanted == inForce()) {
        m_pending.reset();

        return std::nullopt;
    }
    if (!m_pending || *m_pending != wanted) {
        m_pending = wanted;
        m_pendingSince = now;

        return std::nullopt;
    }
    if (now - m_pendingSince <= DetailSettleSeconds) {
        return std::nullopt;
    }

    return wanted;
}

AnalysisSettings coarsenedForMotion(AnalysisSettings settings)
{
    for (auto& [id, size] : settings.imageSizes) {
        size.first = std::max(size.first / MovingDetailDivisor, std::min(size.first, MovingDetailFloor));
        size.second = std::max(size.second / MovingDetailDivisor, std::min(size.second, MovingDetailFloor));
    }

    return settings;
}

}  // namespace sidescopes
