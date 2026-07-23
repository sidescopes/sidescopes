#pragma once

#include <string>

#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/version.h"
#include "core/analysis_worker.h"

namespace sidescopes {

/// The collaborators the Settings window reads and writes: the view and
/// analysis its sliders tune, the registry that supplies each slider's range
/// from the scope descriptors, and the status lines it displays. References
/// stay valid for the single synchronous draw call.
struct SettingsContext
{
    bool& showSettings;
    ScopeView& view;
    AnalysisSettings& analysis;
    bool& analysisDirty;
    const ScopeRegistry& registry;
    const AnalysisWorker::Output& output;
    const VersionInfo& version;
    std::string captureStatus;
};

/// Draws the Settings window when open: the capture and frame status, then the
/// vectorscope and waveform intensity, sampling, and smoothing controls.
void drawSettingsWindow(const SettingsContext& ctx);

}  // namespace sidescopes
