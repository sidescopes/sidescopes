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

/// One scope's trace controls: intensity, sampling stride, whatever
/// continuous setting the scope declares, and marker smoothing. The sliders
/// read their default, headroom and range from the descriptor, and the
/// smoothing slider is host state. @p suffix keeps the widget ids apart
/// between sections.
void drawTraceSettings(const SettingsContext& ctx, std::string_view id, const char* label, const char* suffix)
{
    const SsScopeDescriptor* descriptor = descriptorFor(ctx.registry, id);
    const SsParamInfo* gain = firstParamOfKind(descriptor, SS_PARAM_INTENSITY);
    const SsParamInfo* strideParam = firstParamOfKind(descriptor, SS_PARAM_INT);
    if (gain == nullptr || strideParam == nullptr) {
        return;
    }
    // The parade shares the waveform's controls, so only the waveform is
    // shown and each write reaches both scopes' parameters.
    const std::string_view mirror = id == WaveformScopeId ? std::string_view{ParadeScopeId} : std::string_view{};
    const auto write = [&](const char* key, double value) {
        ctx.analysis.scopeParams[std::string{id}][key] = value;
        if (!mirror.empty()) {
            ctx.analysis.scopeParams[std::string{mirror}][key] = value;
        }
        ctx.analysisDirty = true;
    };

    ImGui::TextDisabled("%s", label);
    float percent = ctx.view.traces().intensity(id);
    if (ImGui::SliderFloat((std::string{"intensity##"} + suffix).c_str(), &percent, 0.0f, 100.0f, "%.0f%%")) {
        ctx.view.traces().setIntensity(id, percent);
        write(gain->key, traceGainFromIntensity(percent, static_cast<float>(gain->intensity_shift)));
    }
    int stride = static_cast<int>(scopeParam(ctx.analysis, id, strideParam->key, strideParam->default_value));
    if (ImGui::SliderInt((std::string{"sampling 1:N##"} + suffix).c_str(), &stride,
                         static_cast<int>(strideParam->min_value), static_cast<int>(strideParam->max_value))) {
        write(strideParam->key, stride);
    }
    // A scope's own continuous setting, drawn only where one is declared: the
    // vectorscope's trace gamma today, anything a module names tomorrow.
    if (const SsParamInfo* scale = firstParamOfKind(descriptor, SS_PARAM_FLOAT); scale != nullptr) {
        float value = static_cast<float>(scopeParam(ctx.analysis, id, scale->key, scale->default_value));
        if (ImGui::SliderFloat((std::string{scale->label} + "##" + suffix).c_str(), &value,
                               static_cast<float>(scale->min_value), static_cast<float>(scale->max_value), "%.2f")) {
            write(scale->key, value);
        }
    }
    float smoothingMs = ctx.view.traces().smoothing(id);
    if (ImGui::SliderFloat((std::string{"smoothing ms##"} + suffix).c_str(), &smoothingMs, 0.0f, 500.0f, "%.0f")) {
        ctx.view.traces().setSmoothing(id, smoothingMs);
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
    drawTraceSettings(ctx, VectorscopeScopeId, "vectorscope", "v");
    drawTraceSettings(ctx, WaveformScopeId, "waveform", "w");
    drawTraceSettings(ctx, LumaWaveformScopeId, "luma waveform", "l");
    ImGui::TextDisabled("modes and toggles: right-click a scope");
    ImGui::TextDisabled("%s", ctx.version.display.c_str());
    ImGui::End();
}

}  // namespace sidescopes
