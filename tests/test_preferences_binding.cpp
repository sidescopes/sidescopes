#include <catch2/catch_test_macros.hpp>
#include <string>

#include "app/pin_board.h"
#include "app/preferences_binding.h"
#include "app/scope_layout.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/shortcut_resolver.h"
#include "core/analysis_worker.h"
#include "core/preferences.h"
#include "modules/module_registry.h"

namespace sidescopes {
namespace {

const ScopeRegistry& registry()
{
    static const ScopeRegistry instance{builtinModules()};

    return instance;
}

// The objects a saved session is restored into, held together so a case can
// hand the same four to both directions.
struct LiveSession
{
    ScopeView view{registry()};
    PinBoard pins;
    ShortcutResolver shortcuts{registry()};
    AnalysisSettings analysis;
};

// A session that differs from the shipped defaults in every field the binding
// carries, so a field left behind shows up as a difference rather than as a
// coincidence.
Preferences savedSession()
{
    Preferences saved;
    saved.scopeParams["org.sidescopes.vectorscope"]["gain"] = 5.0;
    saved.scopeParams["org.sidescopes.vectorscope"]["stride"] = 2.0;
    saved.scopeParams["org.sidescopes.vectorscope"]["smoothing_ms"] = 40.0;
    saved.scopeParams["org.sidescopes.waveform"]["gain"] = 0.2;
    saved.scopeParams["org.sidescopes.waveform"]["stride"] = 3.0;
    saved.scopeParams["org.sidescopes.waveform"]["smoothing_ms"] = 160.0;
    saved.scopeParams["org.sidescopes.histogram"]["style"] = 1.0;
    saved.scopeStack = "WH";
    saved.graticuleStrength = 0.5f;
    saved.vectorscopeZoom = 2;
    saved.layoutOrientation = 2;
    saved.layoutWeights["org.sidescopes.waveform"] = 2.0;
    saved.shortcuts.drawRegion = "G";
    saved.shortcuts.clearRegion = "Escape";
    saved.scopeShortcuts["org.sidescopes.histogram"] = "K";
    saved.pins.clear();
    saved.pins.push_back(FloatColor{0.1f, 0.2f, 0.3f});
    saved.pins.push_back(FloatColor{0.4f, 0.5f, 0.6f});
    saved.pinComparator = 1;

    return saved;
}

}  // namespace

TEST_CASE("A saved session round-trips through the live objects")
{
    const Preferences saved = savedSession();
    LiveSession live;
    restorePreferences(saved, live.view, live.pins, live.shortcuts, live.analysis);

    const Preferences written = capturePreferences(live.view, live.pins, live.shortcuts, live.analysis);

    CHECK(written.scopeStack == saved.scopeStack);
    CHECK(written.graticuleStrength == saved.graticuleStrength);
    CHECK(written.vectorscopeZoom == saved.vectorscopeZoom);
    CHECK(written.layoutOrientation == saved.layoutOrientation);
    CHECK(written.layoutWeights == saved.layoutWeights);
    CHECK(written.shortcuts.drawRegion == saved.shortcuts.drawRegion);
    CHECK(written.scopeShortcuts == saved.scopeShortcuts);
    CHECK(written.pins.size() == saved.pins.size());
    CHECK(written.pins[1].g == saved.pins[1].g);
    CHECK(written.pinComparator == saved.pinComparator);
    // The parade is dropped on the way out and re-seeded on the way in, so it
    // is the one scope the two maps are allowed to disagree about.
    CHECK(written.scopeParams.find("org.sidescopes.parade") == written.scopeParams.end());
    AnalysisSettings readBack;
    readBack.scopeParams = written.scopeParams;
    for (const auto& [id, params] : saved.scopeParams) {
        for (const auto& [key, value] : params) {
            INFO(id << "." << key);
            CHECK(scopeParam(readBack, id, key, -1.0) == value);
        }
    }
}

TEST_CASE("The worker never sees the host's own smoothing")
{
    LiveSession live;
    restorePreferences(savedSession(), live.view, live.pins, live.shortcuts, live.analysis);

    CHECK(scopeParam(live.analysis, "org.sidescopes.vectorscope", "smoothing_ms", -1.0) == -1.0);
    CHECK(scopeParam(live.analysis, "org.sidescopes.waveform", "smoothing_ms", -1.0) == -1.0);
    // It reaches the view instead, which is what draws the marker it smooths.
    CHECK(live.view.traces().smoothing("org.sidescopes.vectorscope") == 40.0f);
    CHECK(live.view.traces().smoothing("org.sidescopes.waveform") == 160.0f);
}

TEST_CASE("A missing smoothing key falls back rather than reading zero")
{
    Preferences saved = savedSession();
    saved.scopeParams["org.sidescopes.vectorscope"].erase("smoothing_ms");
    saved.scopeParams.erase("org.sidescopes.waveform");
    LiveSession live;
    restorePreferences(saved, live.view, live.pins, live.shortcuts, live.analysis);

    CHECK(live.view.traces().smoothing("org.sidescopes.vectorscope") == 75.0f);
    CHECK(live.view.traces().smoothing("org.sidescopes.waveform") == 100.0f);
}

TEST_CASE("The parade is seeded from the waveform it mirrors")
{
    LiveSession live;
    restorePreferences(savedSession(), live.view, live.pins, live.shortcuts, live.analysis);

    CHECK(scopeParam(live.analysis, "org.sidescopes.parade", "gain", -1.0) == 0.2);
    CHECK(scopeParam(live.analysis, "org.sidescopes.parade", "stride", -1.0) == 3.0);
}

TEST_CASE("Restoring states the enabled scopes the worker computes")
{
    LiveSession live;
    restorePreferences(savedSession(), live.view, live.pins, live.shortcuts, live.analysis);

    CHECK(live.analysis.enabledScopes == live.view.stack().enabledScopeIds());
    CHECK_FALSE(live.analysis.enabledScopes.empty());
}

TEST_CASE("The trace intensity is derived from the gain it was saved as")
{
    Preferences saved = savedSession();
    saved.scopeParams["org.sidescopes.waveform"]["gain"] = 0.05;
    LiveSession quiet;
    restorePreferences(saved, quiet.view, quiet.pins, quiet.shortcuts, quiet.analysis);

    saved.scopeParams["org.sidescopes.waveform"]["gain"] = 0.5;
    LiveSession loud;
    restorePreferences(saved, loud.view, loud.pins, loud.shortcuts, loud.analysis);

    CHECK(loud.view.traces().intensity("org.sidescopes.waveform") >
          quiet.view.traces().intensity("org.sidescopes.waveform"));
}

}  // namespace sidescopes
