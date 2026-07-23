#include "app/settings_window.h"

#include <string>
#include <string_view>

#include "app/param_menu.h"
#include "core/trace_intensity.h"
#include "imgui.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

const SsScopeDescriptor* descriptorFor(const ScopeRegistry& registry, std::string_view id)
{
    const HostScope* hostScope = registry.byId(id);

    return hostScope != nullptr ? hostScope->descriptor : nullptr;
}

double scopeParam(const AnalysisSettings& analysis, std::string_view id, std::string_view key, double fallback)
{
    const auto scope = analysis.scopeParams.find(std::string{id});
    if (scope == analysis.scopeParams.end()) {
        return fallback;
    }
    const auto value = scope->second.find(std::string{key});

    return value != scope->second.end() ? value->second : fallback;
}

void drawVectorscopeSettings(const SettingsContext& ctx)
{
    // The intensity and stride sliders read their default, headroom, and range
    // from the descriptor; the smoothing slider is host state.
    const SsParamInfo* gain = firstParamOfKind(descriptorFor(ctx.registry, VectorscopeScopeId), SS_PARAM_INTENSITY);
    const SsParamInfo* strideParam = firstParamOfKind(descriptorFor(ctx.registry, VectorscopeScopeId), SS_PARAM_INT);
    ImGui::TextDisabled("vectorscope");
    float percent = ctx.view.traces().intensity(VectorscopeScopeId);
    if (ImGui::SliderFloat("intensity##v", &percent, 0.0f, 100.0f, "%.0f%%")) {
        ctx.view.traces().setIntensity(VectorscopeScopeId, percent);
        ctx.analysis.scopeParams[VectorscopeScopeId][gain->key] =
            traceGainFromIntensity(percent, static_cast<float>(gain->intensity_shift));
        ctx.analysisDirty = true;
    }
    int stride =
        static_cast<int>(scopeParam(ctx.analysis, VectorscopeScopeId, strideParam->key, strideParam->default_value));
    if (ImGui::SliderInt("sampling 1:N##v", &stride, static_cast<int>(strideParam->min_value),
                         static_cast<int>(strideParam->max_value))) {
        ctx.analysis.scopeParams[VectorscopeScopeId][strideParam->key] = stride;
        ctx.analysisDirty = true;
    }
    float smoothingMs = ctx.view.traces().smoothing(VectorscopeScopeId);
    if (ImGui::SliderFloat("smoothing ms##v", &smoothingMs, 0.0f, 500.0f, "%.0f")) {
        ctx.view.traces().setSmoothing(VectorscopeScopeId, smoothingMs);
    }
}

void drawWaveformSettings(const SettingsContext& ctx)
{
    // The waveform and its parade share one control, so only the waveform is
    // shown; each write reaches both scopes' parameters.
    const SsParamInfo* gain = firstParamOfKind(descriptorFor(ctx.registry, WaveformScopeId), SS_PARAM_INTENSITY);
    const SsParamInfo* strideParam = firstParamOfKind(descriptorFor(ctx.registry, WaveformScopeId), SS_PARAM_INT);
    ImGui::TextDisabled("waveform");
    float percent = ctx.view.traces().intensity(WaveformScopeId);
    if (ImGui::SliderFloat("intensity##w", &percent, 0.0f, 100.0f, "%.0f%%")) {
        ctx.view.traces().setIntensity(WaveformScopeId, percent);
        const double value = traceGainFromIntensity(percent, static_cast<float>(gain->intensity_shift));
        ctx.analysis.scopeParams[WaveformScopeId]["gain"] = value;
        ctx.analysis.scopeParams[ParadeScopeId]["gain"] = value;
        ctx.analysisDirty = true;
    }
    int stride =
        static_cast<int>(scopeParam(ctx.analysis, WaveformScopeId, strideParam->key, strideParam->default_value));
    if (ImGui::SliderInt("sampling 1:N##w", &stride, static_cast<int>(strideParam->min_value),
                         static_cast<int>(strideParam->max_value))) {
        ctx.analysis.scopeParams[WaveformScopeId]["stride"] = stride;
        ctx.analysis.scopeParams[ParadeScopeId]["stride"] = stride;
        ctx.analysisDirty = true;
    }
    float smoothingMs = ctx.view.traces().smoothing(WaveformScopeId);
    if (ImGui::SliderFloat("smoothing ms##w", &smoothingMs, 0.0f, 500.0f, "%.0f")) {
        ctx.view.traces().setSmoothing(WaveformScopeId, smoothingMs);
    }
}

}  // namespace

void drawSettingsWindow(const SettingsContext& ctx)
{
    if (!ctx.showSettings) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Settings", &ctx.showSettings, ImGuiWindowFlags_NoCollapse);
    ImGui::TextWrapped("capture: %s", ctx.captureStatus.c_str());
    ImGui::Text("analysis %.2f ms | frames %llu | ui %.0f fps", ctx.output.accumulateMilliseconds,
                static_cast<unsigned long long>(ctx.output.framesProcessed), static_cast<double>(io.Framerate));
    ImGui::Separator();
    drawVectorscopeSettings(ctx);
    drawWaveformSettings(ctx);
    ImGui::TextDisabled("modes and toggles: right-click a scope");
    ImGui::TextDisabled("%s", ctx.version.display.c_str());
    ImGui::End();
}

}  // namespace sidescopes
