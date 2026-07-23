#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app/scope_registry.h"
#include "app/version.h"
#include "core/analysis_worker.h"
#include "core/preferences.h"
#include "imgui.h"
#include "modules/module_registry.h"
#include "platform/graphics.h"

struct GLFWwindow;

/// The pieces the application shell is assembled from at startup. Each is
/// built here and handed back, so init() is the one place that binds them to
/// the shell; nothing in this unit reads or writes App state.
namespace sidescopes {

struct AppCallbackState;

/// Applies the application's dark theme - the base metrics and the color
/// table - to the ImGui style. The interface scale is applied on top of it, so
/// this is what every size change rebuilds from.
void applyTheme();

/// Loads the interface font and its fixed-width companion, returning the
/// monospace font so the picker can align hex codes with it; null when the
/// system had none and the interface font stands in.
[[nodiscard]] ImFont* loadInterfaceFont(GLFWwindow* window);

/// The interface is authored in 100%-scale units. On macOS GLFW window
/// coordinates are already such units - the framebuffer alone carries the
/// Retina factor - so this is 1.0; on Windows the window is sized in
/// physical pixels and the monitor's content scale (1.25, 1.5, ...) says
/// how many of them the interface should treat as one.
[[nodiscard]] float computeUiScale(GLFWwindow* window);

/// The main window and the graphics backend whose hints shaped it; a null
/// window means creation failed and GLFW is already terminated.
struct MainWindow
{
    GLFWwindow* window = nullptr;
    std::unique_ptr<GraphicsBackend> graphics;
};

/// Creates the always-on-top main window: the backend's hints first, then the
/// saved placement clamped onto a visible monitor, and the iconify callback
/// routed through @p callbackState. A development @p version wears itself in
/// the title bar.
[[nodiscard]] MainWindow createMainWindow(const Preferences& startup, const VersionInfo& version,
                                          AppCallbackState& callbackState);

/// Seeds @p analysis from the saved preferences: each scope's parameters by
/// id, and the image sizes the worker starts at before adaptive detail moves
/// them.
void seedAnalysis(AnalysisSettings& analysis, const Preferences& startup);

/// @return One projection instance per module scope in @p registry, keyed by
///         id. The host color picker has no module, so it gets none.
[[nodiscard]] std::map<std::string, ScopeInstance> createProjectionInstances(const ScopeRegistry& registry);

/// One blank texture per module scope, and the pane bookkeeping that runs in
/// registry order beside it.
struct ScopeTextureSet
{
    std::map<std::string, std::unique_ptr<ScopeTexture>> textures;
    std::vector<ImVec2> panePoints;
    std::vector<std::string> paneIds;
    std::vector<std::string> dividerIds;
};

/// Builds a blank texture for every module scope in @p registry, sized from
/// its descriptor, alongside the pane points, pane ids, and divider ids the
/// layout addresses them by.
[[nodiscard]] ScopeTextureSet createScopeTextures(GraphicsBackend& graphics, const ScopeRegistry& registry);

}  // namespace sidescopes
