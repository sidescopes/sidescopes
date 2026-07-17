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

TEST_CASE("Preferences reject a malformed shortcut binding")
{
    // A binding is one letter A-Z or "Escape"; anything else keeps the
    // default rather than storing an unusable chord.
    const TempFile file("bad-shortcut.txt");
    file.write("shortcut_waveform=ab\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.shortcuts.waveform == "W");
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

}  // namespace sidescopes
