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

/// What a module calls one of its parameters, or its key when it calls it
/// nothing.
///
/// The label is the module's to give and the host has no business inventing
/// one - a module the host was never compiled against could not be guessed at
/// anyway. But a module that declares nothing must not leave a slider with a
/// blank where its name goes, so the key stands in: it is required to exist,
/// it is unique within the scope, and it is at least a word the module chose.
[[nodiscard]] const char* paramLabel(const SsParamInfo& info)
{
    return info.label != nullptr && info.label[0] != '\0' ? info.label : info.key;
}

/// What a module calls itself, or its id when it calls itself nothing. Same
/// rule, same reason.
[[nodiscard]] const char* scopeLabel(const SsScopeDescriptor& descriptor)
{
    return descriptor.name != nullptr && descriptor.name[0] != '\0' ? descriptor.name : descriptor.id;
}

/// One scope's trace controls: intensity, sampling stride, whatever
/// continuous setting the scope declares, and marker smoothing.
///
/// EVERY WORD NAMING THE MODULE COMES FROM THE MODULE - the section's heading
/// and each slider's label as well as its default, headroom and range. A
/// module can be loaded at runtime, so a label the host held would be a guess
/// that happens to be right for the ones shipped here and wrong for anything
/// else. Only the smoothing slider is the host's to name, because the value
/// behind it is the host's and no descriptor declares it. @p suffix keeps the
/// widget ids apart between sections.
void drawTraceSettings(const SettingsContext& ctx, std::string_view id, const char* suffix)
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

    ImGui::TextDisabled("%s", scopeLabel(*descriptor));
    float percent = ctx.view.traces().intensity(id);
    if (ImGui::SliderFloat((std::string{paramLabel(*gain)} + "##" + suffix).c_str(), &percent, 0.0f, 100.0f,
                           "%.0f%%")) {
        ctx.view.traces().setIntensity(id, percent);
        write(gain->key, traceGainFromIntensity(percent, static_cast<float>(gain->intensity_shift)));
    }
    int stride = static_cast<int>(scopeParam(ctx.analysis, id, strideParam->key, strideParam->default_value));
    if (ImGui::SliderInt((std::string{paramLabel(*strideParam)} + "##" + suffix).c_str(), &stride,
                         static_cast<int>(strideParam->min_value), static_cast<int>(strideParam->max_value))) {
        write(strideParam->key, stride);
    }
    // A scope's own continuous setting, drawn only where one is declared: the
    // vectorscope's trace gamma today, anything a module names tomorrow.
    if (const SsParamInfo* scale = firstParamOfKind(descriptor, SS_PARAM_FLOAT); scale != nullptr) {
        float value = static_cast<float>(scopeParam(ctx.analysis, id, scale->key, scale->default_value));
        if (ImGui::SliderFloat((std::string{paramLabel(*scale)} + "##" + suffix).c_str(), &value,
                               static_cast<float>(scale->min_value), static_cast<float>(scale->max_value), "%.2f")) {
            write(scale->key, value);
        }
    }
    float smoothingMs = ctx.view.traces().smoothing(id);
    if (ImGui::SliderFloat((std::string{"Smoothing ms##"} + suffix).c_str(), &smoothingMs, 0.0f, 500.0f, "%.0f")) {
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
    ImGui::TextWrapped("Capture: %s", ctx.captureStatus.c_str());
    ImGui::Text("Analysis %.2f ms | Frames %llu | UI %.0f fps", ctx.output.accumulateMilliseconds,
                static_cast<unsigned long long>(ctx.output.framesProcessed), static_cast<double>(io.Framerate));
    ImGui::Separator();
    drawTraceSettings(ctx, VectorscopeScopeId, "v");
    drawTraceSettings(ctx, WaveformScopeId, "w");
    drawTraceSettings(ctx, LumaWaveformScopeId, "l");
    ImGui::TextDisabled("Modes and toggles: right-click a scope");
    ImGui::TextDisabled("%s", ctx.version.display.c_str());
    ImGui::End();
}

}  // namespace sidescopes
