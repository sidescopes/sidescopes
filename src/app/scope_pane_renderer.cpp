#include "app/scope_pane_renderer.h"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace sidescopes {

ScopePaneRenderer::ScopePaneRenderer(const ScopePaneContext& context, std::map<std::string, ScopeInstance> projections,
                                     ScopeTextureSet textures)
    : m_icons(context.graphics),
      m_toolbar(context.registry, context.view, context.shortcuts, context.regionPicker, m_icons),
      m_panes(PaneAreaContext{context.graphics, context.view, context.registry, context.analysis, context.output,
                              context.capture, context.pins},
              std::move(projections), std::move(textures)),
      m_statusBar(context.shortcuts, context.regionPicker, m_icons)
{
}

IconTextures& ScopePaneRenderer::icons()
{
    return m_icons;
}

PaneRenderOutcome ScopePaneRenderer::drawScopeToggles(bool stackModifier)
{
    return m_toolbar.drawScopeToggles(stackModifier);
}

PaneRenderOutcome ScopePaneRenderer::drawRegionToolIcons(const PaneRenderInput& input)
{
    return m_toolbar.drawRegionToolIcons(input.regionSelected);
}

PaneRenderOutcome ScopePaneRenderer::drawScopePanes(const PaneRenderInput& input)
{
    return m_panes.draw(input);
}

void ScopePaneRenderer::drawStatusBar(const PaneRenderInput& input)
{
    m_statusBar.draw(input.regionSelected, input.pinsAvailable, input.readoutColor);
}

void ScopePaneRenderer::configureProjections()
{
    m_panes.configureProjections();
}

void ScopePaneRenderer::uploadVisibleScopes(bool traceLive)
{
    m_panes.uploadVisibleScopes(traceLive);
}

void ScopePaneRenderer::releaseTraces()
{
    m_panes.releaseTraces();
}

bool ScopePaneRenderer::hasTexture(std::string_view id) const
{
    return m_panes.hasTexture(id);
}

const ScopeImage& ScopePaneRenderer::imageFor(std::string_view id) const
{
    return m_panes.imageFor(id);
}

ScopePaneSizes ScopePaneRenderer::paneSizes() const
{
    // The largest pane the family got, because its scopes share one image: a
    // small parade beside a wide waveform must not cost the waveform its
    // columns, and neither may be given a size of its own.
    const auto largestOf = [this](std::initializer_list<std::string_view> family) {
        PaneSize largest;
        for (const std::string_view id : family) {
            const ImVec2 points = m_panes.paneSizePoints(id);
            largest.width = std::max(largest.width, points.x);
            largest.height = std::max(largest.height, points.y);
        }

        return largest;
    };

    return ScopePaneSizes{largestOf({WaveformScopeId, ParadeScopeId}),
                          largestOf({HistogramScopeId, CombinedHistogramScopeId}), largestOf({VectorscopeScopeId})};
}

int ScopePaneRenderer::paneAt(const ImVec2& point) const
{
    return m_panes.paneAt(point);
}

void ScopePaneRenderer::setStatus(std::string message)
{
    m_statusBar.setStatus(std::move(message));
}

double ScopePaneRenderer::redrawDueSeconds() const
{
    return std::max(m_panes.redrawDueSeconds(), m_statusBar.redrawDueSeconds());
}

}  // namespace sidescopes
