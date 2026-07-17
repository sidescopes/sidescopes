#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>
#include <string>

#include "core/preferences.h"
#include "temp_file.h"

namespace sidescopes {

using namespace test;

namespace {

constexpr char VectorscopeId[] = "org.sidescopes.vectorscope";
constexpr char WaveformId[] = "org.sidescopes.waveform";
constexpr char ParadeId[] = "org.sidescopes.parade";
constexpr char HistogramId[] = "org.sidescopes.histogram";

// The value of one scope parameter, or a sentinel when the scope or key is
// unset, so a test reads the map without spelling out both lookups.
double param(const Preferences& preferences, const char* id, const char* key, double missing = -1.0)
{
    const auto scope = preferences.scopeParams.find(id);
    if (scope == preferences.scopeParams.end()) {
        return missing;
    }
    const auto value = scope->second.find(key);

    return value != scope->second.end() ? value->second : missing;
}

}  // namespace

TEST_CASE("Preferences round-trip through a file")
{
    Preferences saved;
    saved.scopeParams[VectorscopeId]["gain"] = 4.5;
    saved.scopeParams[WaveformId]["gain"] = 0.12;
    saved.scopeParams[WaveformId]["stride"] = 2.0;
    saved.scopeParams[VectorscopeId]["smoothing_ms"] = 60.0;
    saved.scopeParams[VectorscopeId]["matrix"] = 1.0;
    saved.scopeParams[VectorscopeId]["response"] = 1.0;
    saved.vectorscopeZoom = 2;
    saved.scopeStack = "HWV";  // stacking order is part of the setting
    saved.showGraticule = false;
    saved.windowX = 120;
    saved.windowWidth = 640;

    const TempFile file("roundtrip.txt");
    REQUIRE(savePreferences(saved, file.path()));

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, VectorscopeId, "gain") == 4.5);
    CHECK(param(loaded, WaveformId, "gain") == 0.12);
    CHECK(param(loaded, WaveformId, "stride") == 2.0);
    CHECK(param(loaded, VectorscopeId, "smoothing_ms") == 60.0);
    CHECK(param(loaded, VectorscopeId, "matrix") == 1.0);
    CHECK(param(loaded, VectorscopeId, "response") == 1.0);
    CHECK(loaded.vectorscopeZoom == 2);
    CHECK(loaded.scopeStack == "HWV");
    CHECK_FALSE(loaded.showGraticule);
    CHECK(loaded.windowX == 120);
    CHECK(loaded.windowWidth == 640);
}

TEST_CASE("Preferences default when the file is missing")
{
    const TempFile file("does-not-exist.txt");
    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, VectorscopeId, "gain") == 3.0);
    CHECK(param(loaded, WaveformId, "gain") == 0.05);
    CHECK(loaded.showGraticule);
}

TEST_CASE("Preferences read a legacy per-scope gain")
{
    // A file from before the generic key scheme: the old vectorscope_gain key
    // still lands as the vectorscope's gain parameter.
    const TempFile file("legacy-gain.txt");
    file.write("vectorscope_gain=4.5\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, VectorscopeId, "gain") == 4.5);
}

TEST_CASE("Preferences read the legacy chroma matrix and trace response")
{
    const TempFile file("legacy-enum.txt");
    file.write("matrix=0\ntrace_response=1\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, VectorscopeId, "matrix") == 0.0);    // BT.601
    CHECK(param(loaded, VectorscopeId, "response") == 1.0);  // Linear
}

TEST_CASE("Preferences invert the legacy per-channel histogram flag")
{
    // The retired bool is stored 1 for per-channel, which is style choice 0;
    // and 0 for combined, which is style choice 1.
    const TempFile perChannel("legacy-hist-on.txt");
    perChannel.write("histogram_per_channel=1\n");
    CHECK(param(loadPreferences(perChannel.path()), HistogramId, "style") == 0.0);

    const TempFile combined("legacy-hist-off.txt");
    combined.write("histogram_per_channel=0\n");
    CHECK(param(loadPreferences(combined.path()), HistogramId, "style") == 1.0);
}

TEST_CASE("Preferences map the legacy waveform mode to a style choice")
{
    // The retired enum stored Luma as 0 and ColoredLuma as 4; every other
    // value read as RGB (choice 0).
    const TempFile luma("legacy-mode-luma.txt");
    luma.write("waveform_mode=0\n");
    CHECK(param(loadPreferences(luma.path()), WaveformId, "mode") == 1.0);

    const TempFile colored("legacy-mode-colored.txt");
    colored.write("waveform_mode=4\n");
    CHECK(param(loadPreferences(colored.path()), WaveformId, "mode") == 2.0);
}

TEST_CASE("Preferences seed the parade from the waveform")
{
    // The parade persists nothing of its own; its gain and stride mirror the
    // waveform, whether the waveform came from a legacy or a generic key.
    const TempFile file("parade-seed.txt");
    file.write("waveform_gain=4.5\nwaveform_stride=3\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, ParadeId, "gain") == 4.5);
    CHECK(param(loaded, ParadeId, "stride") == 3.0);
}

TEST_CASE("Preferences let a generic key win over its legacy key")
{
    // Both a legacy and a new key name the same parameter; the new
    // scopeId.paramKey form supersedes the legacy one.
    const TempFile file("supersede.txt");
    file.write("vectorscope_gain=2.0\norg.sidescopes.vectorscope.gain=7.0\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, VectorscopeId, "gain") == 7.0);
}

TEST_CASE("Preferences never write the parade to file")
{
    // The saver drops the parade so it can never drift from the waveform.
    Preferences saved;
    saved.scopeParams[ParadeId]["gain"] = 9.0;

    const TempFile file("no-parade.txt");
    REQUIRE(savePreferences(saved, file.path()));

    // Read the raw text: no parade key survives the save.
    std::ifstream text(file.path());
    std::string contents((std::istreambuf_iterator<char>(text)), std::istreambuf_iterator<char>());
    CHECK(contents.find("org.sidescopes.parade") == std::string::npos);
}

TEST_CASE("Preferences migrate the legacy single view mode")
{
    const TempFile file("legacy-view-mode.txt");
    file.write("view_mode=2\n");  // the old vectorscope-and-waveform pair

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.scopeStack == "VW");
}

TEST_CASE("Preferences migrate the scope bit set and waveform style")
{
    // The RGB+Luma composite folds into the one waveform scope, whose
    // style now lives in the context menu.
    const TempFile file("legacy-bit-set.txt");
    file.write("visible_scopes=6\nwaveform_mode=2\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.scopeStack == "WH");
}

TEST_CASE("Preferences fold the retired luma scope into the waveform style")
{
    // A stack saved with the short-lived separate luma waveform: the
    // letter becomes W, the style becomes Luma, the parade stays.
    const TempFile file("legacy-luma-letter.txt");
    file.write("scope_stack=LR\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.scopeStack == "WR");
    CHECK(param(loaded, WaveformId, "mode") == 1.0);  // Luma
}

TEST_CASE("Preferences never load an empty scope set")
{
    const TempFile file("empty-scopes.txt");
    file.write("scope_stack=XYZ\n");  // no known scope letters

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.scopeStack == "V");
}

TEST_CASE("Preferences deduplicate scope letters")
{
    const TempFile file("dup-scopes.txt");
    file.write("scope_stack=RWRxW\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.scopeStack == "RW");
}

TEST_CASE("Preferences keep the color picker in the stack")
{
    const TempFile file("picker-scope.txt");
    file.write("scope_stack=CV\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.scopeStack == "CV");
}

TEST_CASE("Preferences carry bracketed id tokens through the stack")
{
    // A letterless scope rides the stack as [id]; the file cleaner passes the
    // token through untouched for the registry to resolve, deduplicating whole
    // tokens and mixing freely with legacy letters.
    const TempFile file("id-token-stack.txt");
    file.write("scope_stack=V[org.sidescopes.test.custom]V[org.sidescopes.test.custom]H\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.scopeStack == "V[org.sidescopes.test.custom]H");
}

TEST_CASE("Preferences tolerate unknown keys and malformed lines")
{
    const TempFile file("forward-compat.txt");
    file.write(
        "future_feature=42\n"
        "no separator here\n"
        "waveform_gain=0.2\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, WaveformId, "gain") == 0.2);
    CHECK(param(loaded, VectorscopeId, "gain") == 3.0);
}

TEST_CASE("Preferences load a whole legacy file to the same live state")
{
    // A realistic file written by the last typed build: legacy per-scope keys,
    // a letter stack, the inverted per-channel flag, a per-name shortcut. Every
    // value must land exactly where the new build reads it, or the owner loses
    // settings silently on the first launch.
    const TempFile file("legacy-whole.txt");
    file.write(
        "vectorscope_gain=5.0\n"
        "vectorscope_stride=2\n"
        "vectorscope_smoothing_ms=90\n"
        "waveform_gain=0.08\n"
        "waveform_stride=1\n"
        "waveform_smoothing_ms=110\n"
        "histogram_stride=3\n"
        "matrix=0\n"
        "trace_response=1\n"
        "waveform_mode=4\n"
        "histogram_per_channel=1\n"
        "scope_stack=VWH\n"
        "show_graticule=0\n"
        "vectorscope_zoom=2\n"
        "window_x=100\n"
        "window_width=500\n"
        "shortcut_waveform=X\n");

    const Preferences loaded = loadPreferences(file.path());

    CHECK(param(loaded, VectorscopeId, "gain") == 5.0);
    CHECK(param(loaded, VectorscopeId, "stride") == 2.0);
    CHECK(param(loaded, VectorscopeId, "smoothing_ms") == 90.0);
    CHECK(param(loaded, VectorscopeId, "matrix") == 0.0);    // BT.601
    CHECK(param(loaded, VectorscopeId, "response") == 1.0);  // Linear
    CHECK(param(loaded, WaveformId, "gain") == 0.08);
    CHECK(param(loaded, WaveformId, "stride") == 1.0);
    CHECK(param(loaded, WaveformId, "smoothing_ms") == 110.0);
    CHECK(param(loaded, WaveformId, "mode") == 2.0);  // ColoredLuma
    CHECK(param(loaded, ParadeId, "gain") == 0.08);   // mirrors the waveform
    CHECK(param(loaded, ParadeId, "stride") == 1.0);
    CHECK(param(loaded, HistogramId, "stride") == 3.0);
    CHECK(param(loaded, HistogramId, "style") == 0.0);  // per-channel inverts to choice 0
    CHECK(loaded.scopeStack == "VWH");
    CHECK_FALSE(loaded.showGraticule);
    CHECK(loaded.vectorscopeZoom == 2);
    CHECK(loaded.windowX == 100);
    CHECK(loaded.windowWidth == 500);
    const auto binding = loaded.scopeShortcuts.find(WaveformId);
    REQUIRE(binding != loaded.scopeShortcuts.end());
    CHECK(binding->second == "X");
}

TEST_CASE("Preferences reject a malformed action shortcut binding")
{
    // A binding is one letter A-Z or "Escape"; anything else keeps the
    // default rather than storing an unusable chord.
    const TempFile file("bad-shortcut.txt");
    file.write("shortcut_draw_region=ab\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.shortcuts.drawRegion == "D");
}

TEST_CASE("Preferences rekey a legacy scope shortcut to its id")
{
    // The retired shortcut_<name> key folds onto the scope id, so a hand-set
    // binding survives the move to id-keyed bindings.
    const TempFile file("legacy-shortcut.txt");
    file.write("shortcut_waveform=X\n");

    const Preferences loaded = loadPreferences(file.path());
    const auto binding = loaded.scopeShortcuts.find(WaveformId);
    REQUIRE(binding != loaded.scopeShortcuts.end());
    CHECK(binding->second == "X");
    // A malformed legacy binding is rejected, leaving the scope unbound so the
    // host falls back to its letter.
    CHECK(loaded.scopeShortcuts.find(VectorscopeId) == loaded.scopeShortcuts.end());
}

TEST_CASE("Preferences let a new scope shortcut win over its legacy name")
{
    // Both keys name the waveform's binding; the id-keyed key supersedes.
    const TempFile file("shortcut-supersede.txt");
    file.write("shortcut_waveform=X\nshortcut_org.sidescopes.waveform=Y\n");

    const Preferences loaded = loadPreferences(file.path());
    const auto binding = loaded.scopeShortcuts.find(WaveformId);
    REQUIRE(binding != loaded.scopeShortcuts.end());
    CHECK(binding->second == "Y");
}

TEST_CASE("Preferences write scope shortcuts under their id")
{
    // A saved override round-trips, and the file names it by id, not by the
    // retired per-name key.
    Preferences saved;
    saved.scopeShortcuts[WaveformId] = "Y";

    const TempFile file("shortcut-roundtrip.txt");
    REQUIRE(savePreferences(saved, file.path()));

    std::ifstream text(file.path());
    std::string contents((std::istreambuf_iterator<char>(text)), std::istreambuf_iterator<char>());
    CHECK(contents.find("shortcut_org.sidescopes.waveform=Y") != std::string::npos);
    CHECK(contents.find("shortcut_waveform=") == std::string::npos);

    const Preferences loaded = loadPreferences(file.path());
    const auto binding = loaded.scopeShortcuts.find(WaveformId);
    REQUIRE(binding != loaded.scopeShortcuts.end());
    CHECK(binding->second == "Y");
}

TEST_CASE("Preferences clamp an out-of-range vectorscope zoom")
{
    // Only 1, 2, and 4 are valid magnify factors; anything else falls to 1.
    const TempFile file("bad-zoom.txt");
    file.write("vectorscope_zoom=3\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.vectorscopeZoom == 1);
}

TEST_CASE("Preferences fall back to BT.709 for an unknown legacy matrix")
{
    // The legacy matrix was 0 for BT.601 and 1 for BT.709; any other value
    // read as 709, the default, which is style choice 1.
    const TempFile file("bad-matrix.txt");
    file.write("matrix=9\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, VectorscopeId, "matrix") == 1.0);
}

TEST_CASE("Preferences round-trip the colored-luma waveform mode")
{
    Preferences saved;
    saved.scopeParams[WaveformId]["mode"] = 2.0;

    const TempFile file("colored-luma.txt");
    REQUIRE(savePreferences(saved, file.path()));

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, WaveformId, "mode") == 2.0);
}

TEST_CASE("Preferences round-trip a non-round gain within serializer precision")
{
    // The serializer writes at the stream's default six significant digits;
    // 3.14159 already fits that, so it round-trips to the same double.
    Preferences saved;
    saved.scopeParams[VectorscopeId]["gain"] = 3.14159;

    const TempFile file("odd-gain.txt");
    REQUIRE(savePreferences(saved, file.path()));

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, VectorscopeId, "gain") == 3.14159);
}

TEST_CASE("Preferences read a non-numeric gain as zero")
{
    // The loader does not validate numeric fields: strtod yields 0 on text
    // it cannot parse, and that is the value that lands in the setting.
    const TempFile file("junk-gain.txt");
    file.write("waveform_gain=abc\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, WaveformId, "gain") == 0.0);
}

TEST_CASE("Preferences round-trip the live layout orientation and weights")
{
    Preferences saved;
    saved.layoutOrientation = 2;  // horizontal
    saved.layoutWeights[VectorscopeId] = 2.5;
    saved.layoutWeights[HistogramId] = 0.5;

    const TempFile file("layout-live.txt");
    REQUIRE(savePreferences(saved, file.path()));

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.layoutOrientation == 2);
    CHECK(loaded.layoutWeights.at(VectorscopeId) == 2.5);
    CHECK(loaded.layoutWeights.at(HistogramId) == 0.5);
}

TEST_CASE("Preferences default the layout to automatic with no weights")
{
    const TempFile file("layout-missing.txt");
    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.layoutOrientation == 0);  // automatic: the historical split
    CHECK(loaded.layoutWeights.empty());
    for (const LayoutPreset& preset : loaded.layoutPresets) {
        CHECK(preset.stack.empty());  // every slot starts unused
    }
}

TEST_CASE("Preferences clamp an out-of-range layout orientation")
{
    const TempFile file("layout-bad-orientation.txt");
    file.write("layout_orientation=7\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.layoutOrientation == 0);
}

TEST_CASE("Preferences round-trip a saved layout preset")
{
    Preferences saved;
    saved.layoutPresets[0].stack = "VWH";
    saved.layoutPresets[0].orientation = 1;  // vertical
    saved.layoutPresets[0].weights[VectorscopeId] = 3.0;
    saved.layoutPresets[0].weights[HistogramId] = 0.75;
    saved.layoutPresets[4].stack = "C";  // slot 5, another used slot

    const TempFile file("layout-presets.txt");
    REQUIRE(savePreferences(saved, file.path()));

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.layoutPresets[0].stack == "VWH");
    CHECK(loaded.layoutPresets[0].orientation == 1);
    CHECK(loaded.layoutPresets[0].weights.at(VectorscopeId) == 3.0);
    CHECK(loaded.layoutPresets[0].weights.at(HistogramId) == 0.75);
    CHECK(loaded.layoutPresets[4].stack == "C");
    // Unused slots write nothing and reload empty.
    CHECK(loaded.layoutPresets[1].stack.empty());
    CHECK(loaded.layoutPresets[8].stack.empty());
}

TEST_CASE("Preferences skip empty preset slots in the file")
{
    Preferences saved;
    saved.layoutPresets[2].stack = "VH";  // only slot 3 is used

    const TempFile file("layout-sparse.txt");
    REQUIRE(savePreferences(saved, file.path()));

    std::ifstream text(file.path());
    std::string contents((std::istreambuf_iterator<char>(text)), std::istreambuf_iterator<char>());
    CHECK(contents.find("layout.preset3.stack=VH") != std::string::npos);
    CHECK(contents.find("layout.preset1.stack") == std::string::npos);
    CHECK(contents.find("layout.preset2.stack") == std::string::npos);
}

TEST_CASE("Preferences drop malformed preset weight pairs")
{
    // The weights list is id:weight pairs; a pair without a colon, and any
    // non-positive weight, is discarded while the valid pairs survive.
    const TempFile file("layout-bad-weights.txt");
    file.write(
        "layout.preset1.stack=VW\n"
        "layout.preset1.weights=org.sidescopes.vectorscope:2,garbage,org.sidescopes.waveform:-1\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.layoutPresets[0].stack == "VW");
    CHECK(loaded.layoutPresets[0].weights.size() == 1);
    CHECK(loaded.layoutPresets[0].weights.at(VectorscopeId) == 2.0);
}

}  // namespace sidescopes
