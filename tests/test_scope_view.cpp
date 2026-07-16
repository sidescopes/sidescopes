#include <catch2/catch_test_macros.hpp>

#include "app/scope_view.h"

namespace sidescopes {

TEST_CASE("ScopeView starts on the vectorscope")
{
    ScopeView view;
    CHECK(view.shows(ScopeGlyph::Vectorscope));
    CHECK_FALSE(view.shows(ScopeGlyph::Histogram));
    CHECK(view.stack().size() == 1);
}

TEST_CASE("Toggling adds a scope and reports it newly visible")
{
    ScopeView view;
    CHECK(view.toggle(ScopeGlyph::Histogram));
    CHECK(view.shows(ScopeGlyph::Histogram));
    CHECK(view.stack().size() == 2);
    // Toggling it back off is not an activation.
    CHECK_FALSE(view.toggle(ScopeGlyph::Histogram));
    CHECK_FALSE(view.shows(ScopeGlyph::Histogram));
}

TEST_CASE("The last scope cannot be toggled away")
{
    ScopeView view;
    REQUIRE(view.stack().size() == 1);
    CHECK_FALSE(view.toggle(ScopeGlyph::Vectorscope));
    CHECK(view.stack().size() == 1);
    CHECK(view.shows(ScopeGlyph::Vectorscope));
}

TEST_CASE("Choosing solos a scope unless stacking")
{
    ScopeView view;
    view.toggle(ScopeGlyph::Waveform);
    REQUIRE(view.stack().size() == 2);

    SECTION("solo replaces the stack")
    {
        CHECK(view.choose(ScopeGlyph::Histogram, false));
        CHECK(view.stack().size() == 1);
        CHECK(view.shows(ScopeGlyph::Histogram));
        CHECK_FALSE(view.shows(ScopeGlyph::Vectorscope));
    }

    SECTION("soloing an already-shown scope is not an activation")
    {
        CHECK_FALSE(view.choose(ScopeGlyph::Waveform, false));
        CHECK(view.stack().size() == 1);
        CHECK(view.shows(ScopeGlyph::Waveform));
    }

    SECTION("stacking keeps the others")
    {
        CHECK(view.choose(ScopeGlyph::Histogram, true));
        CHECK(view.stack().size() == 3);
    }
}

TEST_CASE("The enabled mask covers the stack; the color picker asks nothing")
{
    ScopeView view;
    view.restoreStack("V");
    CHECK(view.enabledMask() == ScopeVectorscope);

    view.restoreStack("VH");
    CHECK(view.enabledMask() == (ScopeVectorscope | ScopeHistogram));

    // The color picker reads the sampled cursor color, not worker output.
    view.restoreStack("C");
    CHECK(view.enabledMask() == 0u);
}

TEST_CASE("The stack round-trips through preference letters")
{
    ScopeView view;
    view.restoreStack("VWRHC");
    CHECK(view.stackLetters() == "VWRHC");
    CHECK(view.stack().size() == 5);

    SECTION("unknown letters are ignored")
    {
        view.restoreStack("VxH");
        CHECK(view.stackLetters() == "VH");
    }

    SECTION("naming nothing valid falls back to the vectorscope")
    {
        view.restoreStack("zzz");
        CHECK(view.stackLetters() == "V");
        view.restoreStack("");
        CHECK(view.stackLetters() == "V");
    }
}

TEST_CASE("The trace flash remembers which trace was adjusted")
{
    TraceFlash flash;
    CHECK_FALSE(flash.showing(TraceControl::Vectorscope, 0.0));

    flash.show(TraceControl::Vectorscope, 10.0);
    CHECK(flash.showing(TraceControl::Vectorscope, 9.0));
    CHECK_FALSE(flash.showing(TraceControl::Waveform, 9.0));
    CHECK_FALSE(flash.showing(TraceControl::Vectorscope, 10.0));
    CHECK_FALSE(flash.showing(TraceControl::Vectorscope, 11.0));

    // The newest gesture wins.
    flash.show(TraceControl::Waveform, 20.0);
    CHECK(flash.showing(TraceControl::Waveform, 15.0));
    CHECK_FALSE(flash.showing(TraceControl::Vectorscope, 15.0));
}

TEST_CASE("Intensity and smoothing are tracked per trace")
{
    ScopeView view;
    view.setIntensity(TraceControl::Vectorscope, 40.0f);
    view.setIntensity(TraceControl::Waveform, 60.0f);
    CHECK(view.intensity(TraceControl::Vectorscope) == 40.0f);
    CHECK(view.intensity(TraceControl::Waveform) == 60.0f);

    view.setSmoothing(TraceControl::Vectorscope, 120.0f);
    view.setSmoothing(TraceControl::Waveform, 250.0f);
    CHECK(view.smoothing(TraceControl::Vectorscope) == 120.0f);
    CHECK(view.smoothing(TraceControl::Waveform) == 250.0f);
}

}  // namespace sidescopes
