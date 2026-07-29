#include "app/preferences_binding.h"

#include <string>
#include <string_view>

#include "app/pin_board.h"
#include "app/scope_layout.h"
#include "app/scope_view.h"
#include "app/shortcut_resolver.h"
#include "core/analysis_worker.h"
#include "core/trace_intensity.h"

namespace sidescopes {
namespace {

// The host's own per-scope smoothing, read straight from the saved map rather
// than through the worker's: it rides the same keys but the worker never sees
// it.
float savedSmoothing(const Preferences& saved, std::string_view id, double fallback)
{
    const auto scope = saved.scopeParams.find(std::string{id});
    if (scope == saved.scopeParams.end()) {
        return static_cast<float>(fallback);
    }
    const auto value = scope->second.find("smoothing_ms");

    return value != scope->second.end() ? static_cast<float>(value->second) : static_cast<float>(fallback);
}

}  // namespace

void restorePreferences(const Preferences& saved, ScopeView& view, PinBoard& pins, ShortcutResolver& shortcuts,
                        AnalysisSettings& analysis)
{
    // The file format caps its pin list at the ring's capacity; the two
    // constants sit in different layers, so the build checks they agree.
    static_assert(MaximumPins == PinBoard::Maximum);

    // The worker is driven entirely by scope id: each scope's saved parameters
    // fan out by key straight from the preference map into the module's
    // declarative vocabulary. Smoothing rides the same map but belongs to the
    // host, so it is filtered out. The parade shares the waveform's gain and
    // stride, so both are re-seeded from the waveform.
    for (const auto& [id, params] : saved.scopeParams) {
        for (const auto& [key, value] : params) {
            if (key != "smoothing_ms") {
                analysis.scopeParams[id][key] = value;
            }
        }
    }
    analysis.scopeParams[ParadeScopeId]["gain"] = analysis.scopeParams[WaveformScopeId]["gain"];
    analysis.scopeParams[ParadeScopeId]["stride"] = analysis.scopeParams[WaveformScopeId]["stride"];

    pins.restore(saved.pins, saved.pinComparator);
    // The order is not restored here: it belongs to the preset slot being
    // resumed, and the controller applies it as it restores the slots.
    view.stack().restore(saved.scopeStack);
    view.setGraticuleStrength(saved.graticuleStrength);
    view.setZoom(saved.vectorscopeZoom);
    view.layout().setOrientation(orientationFromInt(saved.layoutOrientation));
    view.layout().setWeights(saved.layoutWeights);
    // The intensity control is derived from each trace's saved gain, which is
    // the worker's own number and so is read back out of the map above.
    view.traces().setIntensity(
        VectorscopeScopeId,
        intensityFromTraceGain(static_cast<float>(scopeParam(analysis, VectorscopeScopeId, "gain", 3.0)),
                               VectorscopeIntensityShift));
    for (const std::string_view id : {std::string_view{WaveformScopeId}, std::string_view{LumaWaveformScopeId}}) {
        view.traces().setIntensity(id,
                                   intensityFromTraceGain(static_cast<float>(scopeParam(analysis, id, "gain", 0.05))));
        view.traces().setSmoothing(id, savedSmoothing(saved, id, 100.0));
    }
    view.traces().setSmoothing(VectorscopeScopeId, savedSmoothing(saved, VectorscopeScopeId, 75.0));
    shortcuts.restore(saved.shortcuts, saved.scopeShortcuts);
    analysis.enabledScopes = view.stack().enabledScopeIds();
}

Preferences capturePreferences(const ScopeView& view, const PinBoard& pins, const ShortcutResolver& shortcuts,
                               const AnalysisSettings& analysis)
{
    Preferences saved;
    // The worker's parameter map is the persisted state directly; only the
    // host-owned smoothing control is folded back in. The parade is dropped: it
    // mirrors the waveform and re-seeds on load.
    saved.scopeParams = analysis.scopeParams;
    saved.scopeParams.erase(ParadeScopeId);
    saved.scopeParams[VectorscopeScopeId]["smoothing_ms"] = view.traces().smoothing(VectorscopeScopeId);
    saved.scopeParams[WaveformScopeId]["smoothing_ms"] = view.traces().smoothing(WaveformScopeId);
    saved.scopeParams[LumaWaveformScopeId]["smoothing_ms"] = view.traces().smoothing(LumaWaveformScopeId);
    saved.scopeStack = view.stack().tokens();
    saved.graticuleStrength = view.graticuleStrength();
    saved.vectorscopeZoom = view.zoom();
    saved.layoutOrientation = orientationToInt(view.layout().orientation());
    saved.layoutWeights = view.layout().weightsSnapshot();
    saved.shortcuts = shortcuts.bindings();
    saved.scopeShortcuts = shortcuts.scopeOverrides();
    saved.pins = pins.colors();
    saved.pinComparator = pins.comparator();

    return saved;
}

}  // namespace sidescopes
