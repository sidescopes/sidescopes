#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>
#include <locale>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>

#include <cerrno>
#endif

#include "core/preferences.h"
#include "support/scope_tokens.h"
#include "temp_file.h"

namespace sidescopes {

using namespace test;

namespace {

constexpr char VectorscopeId[] = "org.sidescopes.vectorscope";
constexpr char WaveformId[] = "org.sidescopes.waveform";
constexpr char LumaWaveformId[] = "org.sidescopes.waveform.luma";
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

std::string contentsOf(const std::filesystem::path& file)
{
    std::ifstream input(file);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class CommaDecimal : public std::numpunct<char>
{
protected:
    char do_decimal_point() const override
    {
        return ',';
    }
};

class GlobalLocale
{
public:
    explicit GlobalLocale(const std::locale& locale)
        : m_previous(std::locale::global(locale))
    {
    }

    ~GlobalLocale()
    {
        std::locale::global(m_previous);
    }

    GlobalLocale(const GlobalLocale&) = delete;
    GlobalLocale& operator=(const GlobalLocale&) = delete;

private:
    std::locale m_previous;
};

#if defined(__unix__) || defined(__APPLE__)
// A real buffered write failure, confined to this test process and restored
// before Catch writes any result. The hard limit never changes.
class FileSizeLimit
{
public:
    explicit FileSizeLimit(rlim_t bytes)
    {
        if (getrlimit(RLIMIT_FSIZE, &m_previousLimit) != 0) {
            throw std::system_error(errno, std::generic_category());
        }

        struct sigaction ignored
        {
        };

        ignored.sa_handler = SIG_IGN;
        sigemptyset(&ignored.sa_mask);
        if (sigaction(SIGXFSZ, &ignored, &m_previousSignal) != 0) {
            throw std::system_error(errno, std::generic_category());
        }
        const rlimit limit{bytes, m_previousLimit.rlim_max};
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            const int error = errno;
            sigaction(SIGXFSZ, &m_previousSignal, nullptr);
            throw std::system_error(error, std::generic_category());
        }
    }

    ~FileSizeLimit()
    {
        setrlimit(RLIMIT_FSIZE, &m_previousLimit);
        sigaction(SIGXFSZ, &m_previousSignal, nullptr);
    }

    FileSizeLimit(const FileSizeLimit&) = delete;
    FileSizeLimit& operator=(const FileSizeLimit&) = delete;

private:
    rlimit m_previousLimit{};

    struct sigaction m_previousSignal
    {
    };
};
#endif

}  // namespace

TEST_CASE("Repeated carriage returns are removed in one preferences load", "[preferences]")
{
    const TempFile input("repeated-carriage-returns.conf");
    const TempFile first("normalized-preferences.conf");
    const TempFile second("reloaded-preferences.conf");
    std::ofstream(input.path(), std::ios::binary)
        << "layout.preset1.stack=\r\r\nquality=high\r\r\nwindow_width=640\r\r\n";
    const auto loaded = loadPreferences(input.path());
    CHECK(loaded.layoutPresets[0].stack.empty());
    CHECK(loaded.quality == "high");
    CHECK(loaded.windowWidth == 640);
    REQUIRE(savePreferences(loaded, first.path()));
    REQUIRE(savePreferences(loadPreferences(first.path()), second.path()));
    CHECK(contentsOf(first.path()) == contentsOf(second.path()));
}

TEST_CASE("Preferences round-trip through a file")
{
    Preferences saved;
    saved.scopeParams[VectorscopeId]["gain"] = 4.5;
    saved.scopeParams[WaveformId]["gain"] = 0.12;
    saved.scopeParams[WaveformId]["stride"] = 2.0;
    saved.scopeParams[VectorscopeId]["smoothing_ms"] = 60.0;
    saved.scopeParams[VectorscopeId]["gamma"] = 0.9;
    saved.vectorscopeZoom = 2;
    saved.showCursorMarkers = false;
    saved.tourSettled = 1;
    saved.scopeStack = testing::idTokens("HWV");  // stacking order is part of the setting
    saved.graticuleStrength = 0.5f;
    saved.windowPosition = WindowPosition{120, -50};
    saved.windowWidth = 640;
    saved.quality = "high";

    const TempFile file("roundtrip.txt");
    REQUIRE(savePreferences(saved, file.path()));

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, VectorscopeId, "gain") == 4.5);
    CHECK(param(loaded, WaveformId, "gain") == 0.12);
    CHECK(param(loaded, WaveformId, "stride") == 2.0);
    CHECK(param(loaded, VectorscopeId, "smoothing_ms") == 60.0);
    CHECK(param(loaded, VectorscopeId, "gamma") == 0.9);
    CHECK(loaded.vectorscopeZoom == 2);
    CHECK_FALSE(loaded.showCursorMarkers);
    // Writing what you read is the feature. Without this the walk-through
    // would greet a returning visitor all over again, which is the failure
    // the flag exists to prevent.
    CHECK(loaded.tourSettled == 1);
    CHECK(loaded.scopeStack == testing::idTokens("HWV"));
    CHECK(loaded.graticuleStrength == 0.5f);
    REQUIRE(loaded.windowPosition);
    CHECK(loaded.windowPosition->x == 120);
    CHECK(loaded.windowPosition->y == -50);
    CHECK(loaded.windowWidth == 640);
    CHECK(loaded.quality == "high");
}

TEST_CASE("Temporary files with the same name coexist independently")
{
    const TempFile first("shared-name.txt");
    first.write("first");
    std::filesystem::path secondPath;
    {
        const TempFile second("shared-name.txt");
        secondPath = second.path();
        REQUIRE(first.path() != second.path());
        second.write("second");
        CHECK(contentsOf(first.path()) == "first");
        CHECK(contentsOf(second.path()) == "second");
    }
    CHECK_FALSE(std::filesystem::exists(secondPath));
    CHECK(contentsOf(first.path()) == "first");
}

TEST_CASE("Preferences create missing parents and replace complete files")
{
    const TempDir directory("preferences-replacement");
    const auto file = directory.file("nested/settings.conf");
    Preferences saved;
    saved.quality = "high";
    REQUIRE(savePreferences(saved, file));
    CHECK(loadPreferences(file).quality == "high");

    saved.quality = "standard";
    REQUIRE(savePreferences(saved, file));
    CHECK(loadPreferences(file).quality == "standard");
    CHECK(std::distance(std::filesystem::directory_iterator(file.parent_path()),
                        std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("Preferences leave the destination intact when replacement fails")
{
    const TempDir directory("preferences-failed-replacement");
    const auto destination = directory.file("settings.conf");
    REQUIRE(std::filesystem::create_directory(destination));
    std::ofstream(destination / "existing") << "preserved";

    CHECK_FALSE(savePreferences(Preferences{}, destination));
    CHECK(contentsOf(destination / "existing") == "preserved");
    CHECK(std::distance(std::filesystem::directory_iterator(directory.path()), std::filesystem::directory_iterator{}) ==
          1);
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("Preferences preserve the previous file after a buffered write fails")
{
    const TempFile file("preferences-write-failure.conf");
    const std::string previous = "quality=high\n";
    file.write(previous);
    bool saved = true;
    {
        const FileSizeLimit limit(64);
        saved = savePreferences(Preferences{}, file.path());
    }
    CHECK_FALSE(saved);
    CHECK(contentsOf(file.path()) == previous);
    CHECK(std::distance(std::filesystem::directory_iterator(file.path().parent_path()),
                        std::filesystem::directory_iterator{}) == 1);
}
#endif

TEST_CASE("Preferences round-trip module settings from arbitrary reverse-DNS domains")
{
    Preferences saved;
    for (const char* id : {"com.example.scope", "net.example.custom.scope", "dev.example.another"}) {
        saved.scopeParams[id]["gain"] = 2.5;
        saved.scopeShortcuts[id] = "K";
    }
    const TempFile file("third-party-domains.conf");
    REQUIRE(savePreferences(saved, file.path()));
    const Preferences loaded = loadPreferences(file.path());
    for (const char* id : {"com.example.scope", "net.example.custom.scope", "dev.example.another"}) {
        CHECK(param(loaded, id, "gain") == 2.5);
        CHECK(loaded.scopeShortcuts.at(id) == "K");
    }
}

TEST_CASE("Generic module parameters do not consume reserved or malformed keys")
{
    const TempFile file("scope-param-namespaces.conf");
    file.write(
        "layout.preset1.orientation=2\n"
        "shortcut_com.example.scope=7\n"
        "com..example.scope.gain=3\n"
        ".com.example.scope.gain=3\n"
        "com.example.scope.=3\n"
        "scope.gain=3\n"
        "shortcut_com..example.scope=K\n");
    const Preferences loaded = loadPreferences(file.path());
    Preferences defaults;
    defaults.scopeParams[ParadeId] = {{"gain", 0.05}, {"stride", 1.0}};
    CHECK(loaded.scopeParams == defaults.scopeParams);
    CHECK(loaded.scopeShortcuts.empty());
    CHECK(loaded.layoutPresets[0].orientation == 2);
}

TEST_CASE("Preferences default when the file is missing")
{
    const TempFile file("does-not-exist.conf");
    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, VectorscopeId, "gain") == 3.0);
    CHECK(param(loaded, WaveformId, "gain") == 0.05);
    CHECK(loaded.graticuleStrength == 1.0f);
    CHECK(loaded.showCursorMarkers);
    CHECK(loaded.quality == "standard");
    CHECK(loaded.windowWidth == 340);
    CHECK(loaded.windowHeight == 500);
}

TEST_CASE("Pointer marker visibility accepts only explicit boolean values")
{
    const TempFile hidden("markers-hidden.conf");
    hidden.write("show_cursor_markers=0\n");
    CHECK_FALSE(loadPreferences(hidden.path()).showCursorMarkers);

    const TempFile visible("markers-visible.conf");
    visible.write("show_cursor_markers=1\n");
    CHECK(loadPreferences(visible.path()).showCursorMarkers);

    const TempFile invalid("markers-invalid.conf");
    invalid.write("show_cursor_markers=false\n");
    CHECK(loadPreferences(invalid.path()).showCursorMarkers);
}

TEST_CASE("Preferences ignore retired per-scope keys")
{
    const TempFile file("retired-scope-keys.txt");
    file.write(
        "vectorscope_gain=4.5\nvectorscope_stride=3\nvectorscope_smoothing_ms=90\n"
        "waveform_gain=4.5\nwaveform_stride=3\nwaveform_smoothing_ms=90\n"
        "waveform_mode=2\nhistogram_stride=3\nhistogram_per_channel=0\n"
        "matrix=0\ntrace_response=1\n");

    const Preferences loaded = loadPreferences(file.path());
    const Preferences defaults;
    for (const char* id : {VectorscopeId, WaveformId, HistogramId}) {
        CHECK(loaded.scopeParams.at(id) == defaults.scopeParams.at(id));
    }
}

TEST_CASE("Preferences keep a generic retired key without acting on it")
{
    // A file written by a build that still had the choice names it generically
    // too. Core does not know which scopes exist, so the pair survives the read
    // like any key no scope declares - and stays inert, because a value only
    // reaches an engine when the descriptor names its key. What must NOT happen
    // is the retired value landing on the live setting.
    const TempFile file("generic-retired.txt");
    file.write("org.sidescopes.vectorscope.response=1\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, VectorscopeId, "response") == 1.0);
    CHECK(param(loaded, VectorscopeId, "gamma") == 0.65);
}

TEST_CASE("Preferences seed the parade from the waveform")
{
    // The parade persists nothing of its own; its gain and stride mirror the
    // waveform.
    const TempFile file("parade-seed.txt");
    file.write("org.sidescopes.waveform.gain=4.5\norg.sidescopes.waveform.stride=3\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, ParadeId, "gain") == 4.5);
    CHECK(param(loaded, ParadeId, "stride") == 3.0);
}

TEST_CASE("Preferences read current keys beside retired ones")
{
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

TEST_CASE("Preferences deduplicate the scopes a stack names")
{
    const TempFile file("dupe-stack.txt");
    file.write("scope_stack=" + testing::idTokens("VVH") + "\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.scopeStack == testing::idTokens("VH"));
}

TEST_CASE("A stack naming nothing readable loads empty")
{
    // Core never judges WHICH scopes an id names - that is the registry's - so
    // all it drops is what is not a token at all. A letter is not a token any
    // more, and the application opens on its own default when nothing is named.
    const TempFile file("junk-stack.txt");
    file.write("scope_stack=VWH\n");

    CHECK(loadPreferences(file.path()).scopeStack.empty());
    CHECK(Preferences{}.scopeStack.empty());
}

TEST_CASE("A preset slot round-trips the order its panes sit in")
{
    // The order the panes take belongs to the slot, not to the application:
    // restoring which scopes a slot shows without restoring how they are laid
    // out would be half a restore.
    Preferences saved;
    saved.layoutPresets[2].stack = "WV";
    saved.layoutPresets[2].order = "VWRHC";

    const TempFile file("preset-order.txt");
    REQUIRE(savePreferences(saved, file.path()));

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.layoutPresets[2].stack == "WV");
    CHECK(loaded.layoutPresets[2].order == "VWRHC");
    // A slot nothing was saved into carries no order either, and the
    // application seeds it from what the modules register.
    CHECK(loaded.layoutPresets[0].order.empty());
}

TEST_CASE("Preferences carry every scope as a bracketed id")
{
    // Core keeps an id it does not recognise: the registry is the only judge
    // of which scopes are real, and a build that has lost a module must not
    // rewrite the file that still names it.
    const TempFile file("id-stack.txt");
    file.write("scope_stack=[org.sidescopes.colorpicker][org.example.custom]\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.scopeStack == "[org.sidescopes.colorpicker][org.example.custom]");
}

TEST_CASE("Preferences tolerate unknown keys and malformed lines")
{
    const TempFile file("forward-compat.txt");
    file.write(
        "future_feature=42\n"
        "no separator here\n"
        "org.sidescopes.waveform.gain=0.2\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, WaveformId, "gain") == 0.2);
    CHECK(param(loaded, VectorscopeId, "gain") == 3.0);
}

// SYMPTOM IF BROKEN: the app stops sizing its interface from the display on a
// first run - silently, because 1.0 is also a perfectly good factor. The two
// readings have to stay distinguishable, so the zero is asserted rather than
// assumed.
TEST_CASE("Preferences tell an unset interface size from a chosen 100%")
{
    const TempFile named("ui-scale-named.txt");
    named.write("ui_scale_factor=1\n");
    CHECK(loadPreferences(named.path()).uiScaleFactor == 1.0f);

    const TempFile silent("ui-scale-silent.txt");
    silent.write("org.sidescopes.waveform.gain=0.2\n");
    CHECK(loadPreferences(silent.path()).uiScaleFactor == 0.0f);

    // A file that has never existed says nothing either.
    CHECK(Preferences{}.uiScaleFactor == 0.0f);

    // And a chosen step survives the round trip as itself, so the first save
    // turns a recommendation into a choice.
    Preferences saved;
    saved.uiScaleFactor = 1.25f;
    const TempFile written("ui-scale-written.txt");
    REQUIRE(savePreferences(saved, written.path()));
    CHECK(loadPreferences(written.path()).uiScaleFactor == 1.25f);
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

TEST_CASE("Preferences ignore retired scope shortcuts")
{
    const TempFile file("retired-shortcuts.txt");
    file.write(
        "shortcut_waveform=X\nshortcut_vectorscope=Y\nshortcut_parade=Z\n"
        "shortcut_histogram=J\nshortcut_color_picker=K\n");

    CHECK(loadPreferences(file.path()).scopeShortcuts.empty());
}

TEST_CASE("Preferences read scope ids beside retired shortcuts")
{
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

TEST_CASE("Preferences round-trip the colored-luma style")
{
    Preferences saved;
    saved.scopeParams[LumaWaveformId]["style"] = 1.0;

    const TempFile file("colored-luma.txt");
    REQUIRE(savePreferences(saved, file.path()));

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, LumaWaveformId, "style") == 1.0);
}

TEST_CASE("Preferences preserve fractional parameters and layout values exactly")
{
    Preferences saved;
    saved.scopeParams[VectorscopeId]["gain"] = 3.141592653589793;
    saved.graticuleStrength = 0.7324567f;
    saved.uiScaleFactor = 1.2345678f;
    saved.layoutWeights[VectorscopeId] = 1.2345678901234567;
    saved.layoutPresets[0].stack = testing::idTokens("V");
    saved.layoutPresets[0].weights[VectorscopeId] = static_cast<double>(0.8765432f);
    saved.layoutPresets[0].styles[VectorscopeId]["style"] = 0.12345678901234568;

    const TempFile file("odd-gain.txt");
    REQUIRE(savePreferences(saved, file.path()));

    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, VectorscopeId, "gain") == saved.scopeParams.at(VectorscopeId).at("gain"));
    CHECK(loaded.graticuleStrength == saved.graticuleStrength);
    CHECK(loaded.uiScaleFactor == saved.uiScaleFactor);
    CHECK(loaded.layoutWeights == saved.layoutWeights);
    CHECK(loaded.layoutPresets[0].weights == saved.layoutPresets[0].weights);
    CHECK(loaded.layoutPresets[0].styles == saved.layoutPresets[0].styles);
}

TEST_CASE("Preferences use a stable decimal separator regardless of the global locale")
{
    const TempFile file("comma-locale.conf");
    Preferences saved;
    saved.scopeParams[VectorscopeId]["gain"] = 2.75;
    saved.uiScaleFactor = 1.25f;
    saved.layoutWeights[VectorscopeId] = 1.5;
    saved.layoutPresets[0].stack = testing::idTokens("V");
    saved.layoutPresets[0].weights[VectorscopeId] = 2.5;
    saved.layoutPresets[0].styles[VectorscopeId]["style"] = 0.75;
    bool written = false;
    {
        const GlobalLocale locale(std::locale(std::locale::classic(), new CommaDecimal));
        written = savePreferences(saved, file.path());
    }
    REQUIRE(written);
    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, VectorscopeId, "gain") == 2.75);
    CHECK(loaded.uiScaleFactor == 1.25f);
    CHECK(loaded.layoutWeights == saved.layoutWeights);
    CHECK(loaded.layoutPresets[0].weights == saved.layoutPresets[0].weights);
    CHECK(loaded.layoutPresets[0].styles == saved.layoutPresets[0].styles);
}

TEST_CASE("Preferences ignore malformed numeric values")
{
    const TempFile file("invalid-numbers.txt");
    for (const std::string value : {"abc", "", "2junk", "nan", "inf", "-inf", "1e9999", "+2", " 2", "2 "}) {
        INFO(value);
        std::string contents;
        for (const char* key : {"org.sidescopes.waveform.gain", "ui_scale_factor", "graticule_strength", "window_width",
                                "layout_active_slot"}) {
            contents += key;
            contents += '=';
            contents += value;
            contents += '\n';
        }
        file.write(contents);
        const Preferences loaded = loadPreferences(file.path());
        CHECK(param(loaded, WaveformId, "gain") == 0.05);
        CHECK(loaded.uiScaleFactor == 0.0f);
        CHECK(loaded.graticuleStrength == 1.0f);
        CHECK(loaded.windowWidth == 340);
        CHECK(loaded.layoutActiveSlot == 1);
    }
}

TEST_CASE("Preferences ignore integers beyond the supported range")
{
    const TempFile file("integer-overflow.txt");
    file.write("window_width=4294967297\nwindow_height=-4294967297\nwindow_x=999999999999999999999\nwindow_y=1\n");
    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.windowWidth == 340);
    CHECK(loaded.windowHeight == 500);
    CHECK_FALSE(loaded.windowPosition);
}

TEST_CASE("Preferences keep the default size for non-positive dimensions")
{
    const TempFile file("invalid-window-size.txt");
    file.write("window_width=0\nwindow_height=-1\n");
    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.windowWidth == 340);
    CHECK(loaded.windowHeight == 500);
}

TEST_CASE("Preferences distinguish negative desktop coordinates from no position")
{
    const TempFile file("negative-window-position.txt");
    file.write("window_x=-1440\nwindow_y=-1\n");
    const Preferences loaded = loadPreferences(file.path());
    REQUIRE(loaded.windowPosition);
    CHECK(loaded.windowPosition->x == -1440);
    CHECK(loaded.windowPosition->y == -1);

    REQUIRE(savePreferences(loaded, file.path()));
    const Preferences reloaded = loadPreferences(file.path());
    REQUIRE(reloaded.windowPosition);
    CHECK(reloaded.windowPosition->x == -1440);
    CHECK(reloaded.windowPosition->y == -1);
}

TEST_CASE("Preferences require both saved window coordinates")
{
    const TempFile file("incomplete-window-position.txt");
    for (const std::string content : {"", "window_x=-1\n", "window_y=50\n", "window_x=bad\nwindow_y=50\n"}) {
        file.write(content);
        const Preferences loaded = loadPreferences(file.path());
        CHECK_FALSE(loaded.windowPosition);
        REQUIRE(savePreferences(loaded, file.path()));
        CHECK_FALSE(loadPreferences(file.path()).windowPosition);
    }
}

TEST_CASE("Preferences accept Windows line endings on every platform")
{
    const TempFile file("windows-line-endings.txt");
    file.write("org.sidescopes.waveform.gain=0.2\r\nshortcut_draw_region=X\r\nwindow_x=-10\r\nwindow_y=20\r\n");
    const Preferences loaded = loadPreferences(file.path());
    CHECK(param(loaded, WaveformId, "gain") == 0.2);
    CHECK(loaded.shortcuts.drawRegion == "X");
    REQUIRE(loaded.windowPosition);
    CHECK(loaded.windowPosition->x == -10);
    CHECK(loaded.windowPosition->y == 20);
}

TEST_CASE("Preferences reject invalid layout numbers without losing valid entries")
{
    const TempFile file("invalid-layout-numbers.txt");
    file.write(
        "layout_weights=org.example.valid:2,org.example.infinite:inf,org.example.overflow:1e300\n"
        "layout.preset1.styles=org.example.valid.style:1,org.example.invalid.style:nan,org.example.junk.style:1x\n"
        "layout.preset1.weights=org.example.valid:3,org.example.invalid:nan\n");
    const Preferences loaded = loadPreferences(file.path());
    REQUIRE(loaded.layoutWeights.size() == 1);
    CHECK(loaded.layoutWeights.at("org.example.valid") == 2.0);
    const LayoutPreset& preset = loaded.layoutPresets[0];
    REQUIRE(preset.styles.size() == 1);
    CHECK(preset.styles.at("org.example.valid").at("style") == 1.0);
    REQUIRE(preset.weights.size() == 1);
    CHECK(preset.weights.at("org.example.valid") == 3.0);
}

TEST_CASE("Preferences round-trip the live layout orientation and weights")
{
    Preferences saved;
    saved.layoutOrientation = 2;  // horizontal
    saved.layoutWeights[VectorscopeId] = 2.5;
    saved.layoutWeights[HistogramId] = 0.5;
    saved.layoutActiveSlot = 3;

    const TempFile file("layout-live.txt");
    REQUIRE(savePreferences(saved, file.path()));

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.layoutOrientation == 2);
    CHECK(loaded.layoutWeights.at(VectorscopeId) == 2.5);
    CHECK(loaded.layoutWeights.at(HistogramId) == 0.5);
    CHECK(loaded.layoutActiveSlot == 3);
}

TEST_CASE("Preferences clamp an out-of-range active preset slot")
{
    // The application is always on a preset, so a slot beyond 1-9 opens on the
    // first rather than on nothing - and never on an out-of-bounds badge.
    const TempFile high("layout-bad-slot.txt");
    high.write("layout_active_slot=12\n");
    CHECK(loadPreferences(high.path()).layoutActiveSlot == 1);

    const TempFile negative("layout-negative-slot.txt");
    negative.write("layout_active_slot=-1\n");
    CHECK(loadPreferences(negative.path()).layoutActiveSlot == 1);

    // Zero is what a file written before there was always an active preset
    // carries. It reads as the first slot, not as none.
    const TempFile none("layout-no-slot.txt");
    none.write("layout_active_slot=0\n");
    CHECK(loadPreferences(none.path()).layoutActiveSlot == 1);
}

TEST_CASE("A fresh install opens on the first preset slot")
{
    // The default a missing file yields, which is the one a first run gets.
    const TempFile file("layout-fresh.txt");
    CHECK(loadPreferences(file.path()).layoutActiveSlot == 1);
    CHECK(Preferences{}.layoutActiveSlot == 1);
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
    saved.layoutPresets[0].styles[LumaWaveformId]["style"] = 1.0;
    saved.layoutPresets[0].styles[VectorscopeId]["stride"] = 2.0;
    saved.layoutPresets[4].stack = "C";  // slot 5, another used slot

    const TempFile file("layout-presets.txt");
    REQUIRE(savePreferences(saved, file.path()));

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.layoutPresets[0].stack == "VWH");
    CHECK(loaded.layoutPresets[0].orientation == 1);
    CHECK(loaded.layoutPresets[0].weights.at(VectorscopeId) == 3.0);
    CHECK(loaded.layoutPresets[0].weights.at(HistogramId) == 0.75);
    CHECK(loaded.layoutPresets[0].styles.at(LumaWaveformId).at("style") == 1.0);
    CHECK(loaded.layoutPresets[0].styles.at(VectorscopeId).at("stride") == 2.0);
    CHECK(loaded.layoutPresets[4].stack == "C");
    // Unused slots write nothing and reload empty, and a preset without
    // styles keeps its empty map rather than inventing keys.
    CHECK(loaded.layoutPresets[1].stack.empty());
    CHECK(loaded.layoutPresets[4].styles.empty());
    CHECK(loaded.layoutPresets[8].stack.empty());
}

TEST_CASE("Preferences round-trip a preset's name")
{
    Preferences saved;
    saved.layoutPresets[0].stack = "VWH";
    saved.layoutPresets[0].name = "Portrait check";
    // Slot 3 is named but holds no layout: the name is the only thing worth
    // writing about it, and it must not be dropped with the empty slot.
    saved.layoutPresets[2].name = "Skin tones";

    const TempFile file("layout-preset-names.txt");
    REQUIRE(savePreferences(saved, file.path()));

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.layoutPresets[0].name == "Portrait check");
    CHECK(loaded.layoutPresets[2].name == "Skin tones");
    CHECK(loaded.layoutPresets[2].stack.empty());
    // A slot never named carries no name, which is what the default rests on.
    CHECK(loaded.layoutPresets[1].name.empty());
}

TEST_CASE("A preset file written before names still loads")
{
    // The key is added, never repurposed: an older file names no slot and every
    // slot falls back to its default name with the rest of it intact.
    const TempFile file("layout-preset-no-names.txt");
    file.write(
        "layout.preset1.stack=VH\n"
        "layout.preset1.orientation=2\n"
        "layout.preset1.weights=org.sidescopes.vectorscope:2\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.layoutPresets[0].stack == "VH");
    CHECK(loaded.layoutPresets[0].orientation == 2);
    CHECK(loaded.layoutPresets[0].weights.at(VectorscopeId) == 2.0);
    CHECK(loaded.layoutPresets[0].name.empty());
}

TEST_CASE("A preset name is cleaned to what one line can carry")
{
    // The file splits on the first '=' and ends at a newline, so a name is
    // bounded and stripped of anything that would end its line early.
    CHECK(sanitizedPresetName("Portrait check") == "Portrait check");
    CHECK(sanitizedPresetName("  Portrait check  ") == "Portrait check");
    CHECK(sanitizedPresetName("Portrait\ncheck") == "Portraitcheck");
    CHECK(sanitizedPresetName("Portrait\tcheck") == "Portraitcheck");
    // An '=' is safe: the loader splits at the FIRST one, which the key owns.
    CHECK(sanitizedPresetName("a=b") == "a=b");
    CHECK(sanitizedPresetName("   ").empty());
    CHECK(sanitizedPresetName("").empty());

    const std::string tooLong(MaximumPresetNameLength + 8, 'x');
    CHECK(sanitizedPresetName(tooLong).size() == MaximumPresetNameLength);

    // A cut landing inside a multi-byte character drops that character whole
    // rather than leaving a byte no font can draw.
    const std::string wide = std::string(MaximumPresetNameLength - 1, 'x') + "\xC3\xA9";
    const std::string cut = sanitizedPresetName(wide);
    CHECK(cut.size() == MaximumPresetNameLength - 1);
    CHECK(cut == std::string(MaximumPresetNameLength - 1, 'x'));
}

TEST_CASE("A hand-edited preset name is cleaned as it is read")
{
    // A name arrives from the file as well as from the user, and a file can be
    // edited by hand, so the reading applies the same rules.
    const TempFile file("layout-preset-long-name.txt");
    file.write("layout.preset1.name=   " + std::string(MaximumPresetNameLength + 5, 'y') + "\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.layoutPresets[0].name == std::string(MaximumPresetNameLength, 'y'));
}

TEST_CASE("Preferences drop malformed preset style pairs")
{
    // The styles list is scopeId.key:value pairs; a pair without a colon or
    // without a dot in its name is discarded while the valid pairs survive.
    const TempFile file("layout-bad-styles.txt");
    file.write(
        "layout.preset1.stack=VH\n"
        "layout.preset1.styles=org.sidescopes.waveform.luma.style:1,garbage,nodot:2,.style:1,"
        "org.sidescopes.nonesuch.style:1\n");

    // Core does not know which scopes exist, so it judges a pair's FORM and
    // nothing else: an id no build has ever registered survives the read.
    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.layoutPresets[0].styles.size() == 2);
    CHECK(loaded.layoutPresets[0].styles.at(LumaWaveformId).at("style") == 1.0);
    CHECK(loaded.layoutPresets[0].styles.at("org.sidescopes.nonesuch").at("style") == 1.0);
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

TEST_CASE("Preferences round-trip the pinned colors and their reference")
{
    Preferences saved;
    saved.pins = {FloatColor{125.0f, 19.0f, 17.0f}, FloatColor{33.0f, 221.0f, 101.0f}};
    saved.pinComparator = 1;

    const TempFile file("pins-roundtrip.txt");
    REQUIRE(savePreferences(saved, file.path()));

    // One line, oldest pin first, in the hex the picker itself shows, so a
    // hand edit reads exactly like the swatches on screen.
    std::ifstream text(file.path());
    std::string contents((std::istreambuf_iterator<char>(text)), std::istreambuf_iterator<char>());
    CHECK(contents.find("pins=7D1311,21DD65\n") != std::string::npos);
    CHECK(contents.find("pin_comparator=1\n") != std::string::npos);

    const Preferences loaded = loadPreferences(file.path());
    REQUIRE(loaded.pins.size() == 2);
    CHECK(loaded.pins[0].r == 125.0f);
    CHECK(loaded.pins[0].g == 19.0f);
    CHECK(loaded.pins[0].b == 17.0f);
    CHECK(loaded.pins[1].r == 33.0f);
    CHECK(loaded.pins[1].g == 221.0f);
    CHECK(loaded.pins[1].b == 101.0f);
    CHECK(loaded.pinComparator == 1);
}

TEST_CASE("Preferences write no pin keys for an empty board")
{
    Preferences saved;  // nothing pinned this session

    const TempFile file("pins-empty.txt");
    REQUIRE(savePreferences(saved, file.path()));

    std::ifstream text(file.path());
    std::string contents((std::istreambuf_iterator<char>(text)), std::istreambuf_iterator<char>());
    CHECK(contents.find("pins=") == std::string::npos);
    CHECK(contents.find("pin_comparator=") == std::string::npos);

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.pins.empty());
    CHECK(loaded.pinComparator == -1);
}

TEST_CASE("Preferences skip malformed pinned colors")
{
    // A pinned color is six hex digits, optionally behind the # the picker
    // copies; anything else is dropped while the valid colors keep their order.
    const TempFile file("pins-garbage.txt");
    file.write("pins=7D1311,zzzzzz,#21DD65,12345,,00FF00\n");

    const Preferences loaded = loadPreferences(file.path());
    REQUIRE(loaded.pins.size() == 3);
    CHECK(loaded.pins[0].r == 125.0f);
    CHECK(loaded.pins[1].g == 221.0f);
    CHECK(loaded.pins[2].g == 255.0f);
    CHECK(loaded.pinComparator == -1);  // no key at all means no reference
}

TEST_CASE("Preferences drop a comparison reference no pin answers")
{
    // Out of range, unparsable, or naming a pin in a file that lists none: each
    // means no reference rather than a pin the user never chose.
    const TempFile beyond("pins-comparator-beyond.txt");
    beyond.write("pins=7D1311,21DD65\npin_comparator=4\n");
    CHECK(loadPreferences(beyond.path()).pinComparator == -1);

    const TempFile text("pins-comparator-text.txt");
    text.write("pins=7D1311\npin_comparator=first\n");
    CHECK(loadPreferences(text.path()).pinComparator == -1);

    const TempFile orphan("pins-comparator-orphan.txt");
    orphan.write("pin_comparator=0\n");
    CHECK(loadPreferences(orphan.path()).pinComparator == -1);

    // Pins with nothing selected round-trip through the file's own -1.
    Preferences saved;
    saved.pins = {FloatColor{125.0f, 19.0f, 17.0f}};
    const TempFile none("pins-comparator-none.txt");
    REQUIRE(savePreferences(saved, none.path()));
    const Preferences loaded = loadPreferences(none.path());
    CHECK(loaded.pins.size() == 1);
    CHECK(loaded.pinComparator == -1);
}

TEST_CASE("Preferences cap a hand-edited pin list at the ring's capacity")
{
    // The board holds MaximumPins colors; a longer list fills it with the
    // leading ones, and a reference among the dropped ones selects nothing.
    const TempFile file("pins-overflow.txt");
    file.write(
        "pins=000000,010000,020000,030000,040000,050000,060000,070000,080000,090000,0A0000\n"
        "pin_comparator=9\n");

    const Preferences loaded = loadPreferences(file.path());
    REQUIRE(loaded.pins.size() == MaximumPins);
    CHECK(loaded.pins.front().r == 0.0f);
    CHECK(loaded.pins.back().r == static_cast<float>(MaximumPins) - 1.0f);
    CHECK(loaded.pinComparator == -1);
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
