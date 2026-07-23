#include "app/adaptive_detail.h"

#include <algorithm>
#include <string>

#include "app/scope_view.h"
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
                          scaled(panes.histogram, density), scaled(panes.vectorscope, density)};
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
        wantColumns = wfWidth >= 1400.0f ? 2048 : wfWidth >= 500.0f ? 1024 : 512;
        if (regionWidth > 0) {
            wantColumns = std::min(wantColumns, regionWidth >= 2048 ? 2048 : regionWidth >= 1024 ? 1024 : 512);
        }
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
        wantHistWidth = scopePane.width >= 1400.0f ? 2048 : scopePane.width >= 500.0f ? 1024 : 512;
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
        // nothing extra.
        const PaneSize scopePane = panePixels.vectorscope;
        const float extent = std::min(scopePane.width, scopePane.height);
        wantVectorscope = extent >= 480.0f ? 512 : 256;
    }

    return wantVectorscope;
}

std::optional<DetailSizes> AdaptiveDetail::update(const ScopePaneSizes& panes, float density,
                                                  std::optional<AnalysisWorker::FrameSize> frameSize, double now)
{
    // Resolution follows the pane a scope actually gets, and never exceeds what
    // the region can populate; desired resolutions are debounced so a live
    // resize does not thrash engine reallocation.
    int regionWidth = 0;
    if (frameSize) {
        regionWidth = m_analysis.region.toPixels(frameSize->width, frameSize->height).width;
    }
    const ScopePaneSizes panePixels = inPixels(panes, density);

    const std::pair<int, int> waveSize = currentSize(WaveformScopeId);
    const std::pair<int, int> histSize = currentSize(HistogramScopeId);
    const std::pair<int, int> vecSize = currentSize(VectorscopeScopeId);
    const auto [wantColumns, wantHeight] = desiredWaveformSize(panePixels, regionWidth);
    const auto [wantHistWidth, wantHistHeight] = desiredHistogramSize(panePixels);
    const int wantVectorscope = desiredVectorscopeSize(panePixels);

    const bool differs = wantColumns != waveSize.first || wantHeight != waveSize.second ||
                         wantVectorscope != vecSize.second || wantHistWidth != histSize.first ||
                         wantHistHeight != histSize.second;
    if (!differs) {
        m_pendingColumns = 0;

        return std::nullopt;
    }
    if (m_pendingColumns != wantColumns || m_pendingImageHeight != wantHeight ||
        m_pendingVectorscope != wantVectorscope || m_pendingHistWidth != wantHistWidth ||
        m_pendingHistHeight != wantHistHeight) {
        m_pendingColumns = wantColumns;
        m_pendingImageHeight = wantHeight;
        m_pendingVectorscope = wantVectorscope;
        m_pendingHistWidth = wantHistWidth;
        m_pendingHistHeight = wantHistHeight;
        m_pendingSince = now;

        return std::nullopt;
    }
    if (now - m_pendingSince <= DetailSettleSeconds) {
        return std::nullopt;
    }

    return DetailSizes{{wantColumns, wantHeight}, {wantHistWidth, wantHistHeight}, wantVectorscope};
}

}  // namespace sidescopes
