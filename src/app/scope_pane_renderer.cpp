#include "app/scope_pane_renderer.h"

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

PaneRenderOutcome ScopePaneRenderer::drawScopeToggles(bool stackModifier)
{
    return m_toolbar.drawScopeToggles(stackModifier);
}

PaneRenderOutcome ScopePaneRenderer::drawRegionToolIcons(const PaneRenderInput& input)
{
    return m_toolbar.drawRegionToolIcons(input.regionIsFullScreen);
}

PaneRenderOutcome ScopePaneRenderer::drawScopePanes(const PaneRenderInput& input)
{
    return m_panes.draw(input);
}

void ScopePaneRenderer::drawStatusBar(const PaneRenderInput& input)
{
    m_statusBar.draw(input.pinsAvailable, input.vectorscopeColor);
}

void ScopePaneRenderer::configureProjections()
{
    m_panes.configureProjections();
}

void ScopePaneRenderer::uploadVisibleScopes()
{
    m_panes.uploadVisibleScopes();
}

bool ScopePaneRenderer::hasTexture(std::string_view id) const
{
    return m_panes.hasTexture(id);
}

const ScopeImage& ScopePaneRenderer::imageFor(std::string_view id) const
{
    return m_panes.imageFor(id);
}

ImVec2 ScopePaneRenderer::paneSizePoints(std::string_view id) const
{
    return m_panes.paneSizePoints(id);
}

int ScopePaneRenderer::paneAt(const ImVec2& point) const
{
    return m_panes.paneAt(point);
}

void ScopePaneRenderer::setStatus(std::string message)
{
    m_statusBar.setStatus(std::move(message));
}

void ScopePaneRenderer::showAttachNotice(std::string message)
{
    m_toolbar.showAttachNotice(std::move(message));
}

}  // namespace sidescopes
