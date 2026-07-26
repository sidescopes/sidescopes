#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string_view>
#include <utility>

#include "app/adaptive_detail.h"
#include "app/quality.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "core/analysis_worker.h"
#include "core/scopes/neutral.h"
#include "core/scopes/waveform.h"
#include "modules/module_registry.h"

namespace sidescopes {
namespace {

// The built-in scope registry, shared across the cases: it is immutable, so one
// instance serves every ScopeView under test.
const ScopeRegistry& registry()
{
    static const ScopeRegistry instance{builtinModules()};

    return instance;
}

// Panes clearing every threshold, and panes clearing none of them.
constexpr ScopePaneSizes LargePanes{
    {1600.0f, 700.0f}, {1600.0f, 700.0f}, {1600.0f, 700.0f}, {700.0f, 700.0f}, {700.0f, 700.0f}};
constexpr ScopePaneSizes SmallPanes{
    {400.0f, 300.0f}, {400.0f, 300.0f}, {400.0f, 300.0f}, {300.0f, 300.0f}, {300.0f, 300.0f}};

// The controller plus the state it reads, in declaration order so the refs it
// stores outlive nothing.
struct DetailFixture
{
    ScopeView view{registry()};
    AnalysisSettings analysis;
    AdaptiveDetail detail{view, analysis};
};

}  // namespace

TEST_CASE("A resolution change waits out the settle time")
{
    DetailFixture fixture;
    fixture.view.stack().choose(WaveformScopeId, false);
    fixture.analysis.imageSizes[WaveformScopeId] = {512, WaveformLevels};

    // The first sight of the larger pane only starts the clock, and a step
    // inside the settle time changes nothing either.
    CHECK_FALSE(fixture.detail.update(LargePanes, 1.0f, std::nullopt, 10.0).has_value());
    CHECK_FALSE(fixture.detail.update(LargePanes, 1.0f, std::nullopt, 10.0 + DetailSettleSeconds / 2.0).has_value());

    const std::optional<DetailSizes> settled =
        fixture.detail.update(LargePanes, 1.0f, std::nullopt, 10.5 + DetailSettleSeconds);
    REQUIRE(settled.has_value());
    CHECK(settled->waveform == std::pair<int, int>{2048, 512});
}

TEST_CASE("A resolution that reverts during the wait is dropped")
{
    DetailFixture fixture;
    fixture.view.stack().choose(WaveformScopeId, false);
    fixture.analysis.imageSizes[WaveformScopeId] = {512, WaveformLevels};

    CHECK_FALSE(fixture.detail.update(LargePanes, 1.0f, std::nullopt, 10.0).has_value());
    // Back to the size already in force before the wait is up: nothing is
    // pending any more, so the time that has passed buys the change nothing.
    CHECK_FALSE(fixture.detail.update(SmallPanes, 1.0f, std::nullopt, 10.2).has_value());
    CHECK_FALSE(fixture.detail.update(SmallPanes, 1.0f, std::nullopt, 11.0).has_value());

    // The next change waits out its own settle time rather than inheriting the
    // one that already elapsed.
    CHECK_FALSE(fixture.detail.update(LargePanes, 1.0f, std::nullopt, 11.0).has_value());
    CHECK_FALSE(fixture.detail.update(LargePanes, 1.0f, std::nullopt, 11.0 + DetailSettleSeconds / 2.0).has_value());
    CHECK(fixture.detail.update(LargePanes, 1.0f, std::nullopt, 11.5 + DetailSettleSeconds).has_value());
}

TEST_CASE("The display density decides which threshold a pane clears")
{
    DetailFixture fixture;
    fixture.view.stack().choose(WaveformScopeId, false);
    fixture.analysis.imageSizes[WaveformScopeId] = {512, WaveformLevels};

    // At one framebuffer pixel per point the small pane asks for the smallest
    // image, so the resolution in force stands and nothing is ever pending.
    CHECK_FALSE(fixture.detail.update(SmallPanes, 1.0f, std::nullopt, 1.0).has_value());
    CHECK_FALSE(fixture.detail.update(SmallPanes, 1.0f, std::nullopt, 2.0).has_value());

    // The same pane on a Retina panel covers twice the pixels, clearing the
    // next thresholds up.
    CHECK_FALSE(fixture.detail.update(SmallPanes, 2.0f, std::nullopt, 3.0).has_value());
    const std::optional<DetailSizes> settled = fixture.detail.update(SmallPanes, 2.0f, std::nullopt, 3.5);
    REQUIRE(settled.has_value());
    CHECK(settled->waveform == std::pair<int, int>{1024, 512});
}

TEST_CASE("Each scope's resolution follows its own pane")
{
    DetailFixture fixture;
    fixture.view.stack().choose(WaveformScopeId, true);
    fixture.view.stack().choose(HistogramScopeId, true);

    // The wider of the waveform and its parade decides the columns, the taller
    // the levels; the panes here are in framebuffer pixels already.
    CHECK(fixture.detail.desiredWaveformSize(LargePanes, 0) == std::pair<int, int>{2048, 512});
    CHECK(fixture.detail.desiredWaveformSize(SmallPanes, 0) == std::pair<int, int>{512, WaveformLevels});

    constexpr ScopePaneSizes MidPanes{{800.0f, 400.0f}, {}, {800.0f, 400.0f}, {400.0f, 400.0f}, {400.0f, 400.0f}};
    CHECK(fixture.detail.desiredWaveformSize(MidPanes, 0) == std::pair<int, int>{1024, WaveformLevels});
    // A narrow region cannot populate more columns than it has pixels.
    CHECK(fixture.detail.desiredWaveformSize(LargePanes, 1500).first == 1024);

    CHECK(fixture.detail.desiredHistogramSize(LargePanes) == std::pair<int, int>{2048, 768});
    CHECK(fixture.detail.desiredHistogramSize(SmallPanes) == std::pair<int, int>{512, 384});

    // The vectorscope is square, so its shorter side decides.
    CHECK(fixture.detail.desiredVectorscopeSize(LargePanes) == 512);
    CHECK(fixture.detail.desiredVectorscopeSize(MidPanes) == 256);
}

TEST_CASE("The neutral plane follows its pane, which nothing used to")
{
    // The plane was fixed at its module default however large a pane it got, so
    // a neutral scope filling a second monitor was a 256-pixel image stretched
    // eightfold. Its cloud is accumulated at this resolution, so the ladder buys
    // real detail rather than interpolation.
    DetailFixture fixture;
    fixture.view.stack().choose(NeutralScopeId, true);

    constexpr ScopePaneSizes Huge{{}, {}, {}, {}, {2400.0f, 2100.0f}};
    constexpr ScopePaneSizes Mid{{}, {}, {}, {}, {700.0f, 700.0f}};
    constexpr ScopePaneSizes Tiny{{}, {}, {}, {}, {300.0f, 300.0f}};

    CHECK(fixture.detail.desiredNeutralSize(Huge) == MaximumNeutralSize);
    CHECK(fixture.detail.desiredNeutralSize(Mid) == 512);
    CHECK(fixture.detail.desiredNeutralSize(Tiny) == 256);

    // Square, so the shorter side decides: a wide, shallow pane gets the step
    // its height can show, not its width.
    constexpr ScopePaneSizes Wide{{}, {}, {}, {}, {2400.0f, 300.0f}};
    CHECK(fixture.detail.desiredNeutralSize(Wide) == 256);
}

TEST_CASE("A scope filling a second monitor is resolved, not magnified")
{
    // The reported symptom: scopes read sharp at small and medium sizes and
    // visibly soft once enlarged, because every ladder here stopped well below
    // what a full-screen pane covers. Each of these panes is what a scope gets
    // filling a 4K display, and each has to reach the top of its ladder.
    DetailFixture fixture;
    // Replace the stack, then add: stacking a scope already shown toggles it off.
    fixture.view.stack().choose(WaveformScopeId, false);
    for (const std::string_view id : {HistogramScopeId, VectorscopeScopeId, NeutralScopeId}) {
        fixture.view.stack().choose(id, true);
    }

    constexpr ScopePaneSizes FullScreen{
        {3840.0f, 2160.0f}, {3840.0f, 2160.0f}, {3840.0f, 2160.0f}, {2160.0f, 2160.0f}, {2160.0f, 2160.0f}};

    // Given a region wide enough to populate them, the waveform takes every
    // column the pane can show, and the neutral plane every pixel: both carry
    // real data. Height and the vectorscope image do not follow the pane -
    // measurement says neither resolves anything a large pane can show - so
    // they stay at the steps their thresholds pick.
    CHECK(fixture.detail.desiredWaveformSize(FullScreen, 3840).first == MaximumWaveformColumns);
    CHECK(fixture.detail.desiredWaveformSize(FullScreen, 3840).second == 512);
    CHECK(fixture.detail.desiredHistogramSize(FullScreen).first == 3072);
    CHECK(fixture.detail.desiredNeutralSize(FullScreen) == MaximumNeutralSize);

    // And the cost stays proportional to the region, not just the pane: the
    // same pane over a small region resolves only what that region can fill.
    CHECK(fixture.detail.desiredWaveformSize(FullScreen, 800).first == 512);
}

TEST_CASE("The neutral plane keeps its resolution while it is off screen")
{
    DetailFixture fixture;
    fixture.analysis.imageSizes[NeutralScopeId] = {512, 512};

    // Nothing is drawing it, so however large a pane it is measured at, the
    // resolution in force stands.
    constexpr ScopePaneSizes Huge{{}, {}, {}, {}, {2400.0f, 2100.0f}};
    CHECK(fixture.detail.desiredNeutralSize(Huge) == 512);
}

TEST_CASE("A dragged region is analysed at a fraction of the detail")
{
    // A pass costs roughly what its image covers, so a region being dragged
    // across a picture - where the reading wanted is a blown highlight or a
    // colour cast, not the finest detail of a trace - is computed at half of
    // each side.
    AnalysisSettings settings;
    settings.imageSizes[VectorscopeScopeId] = {512, 512};
    settings.imageSizes[HistogramScopeId] = {2048, 768};
    settings.imageSizes[NeutralScopeId] = {512, 512};
    settings.region = RegionOfInterest{10.0, 20.0, 60.0, 70.0};
    settings.enabledScopes = {std::string(VectorscopeScopeId)};
    settings.scopeParams[VectorscopeScopeId]["gain"] = 3.0;

    const AnalysisSettings dragged = coarsenedForDrag(settings);
    CHECK(dragged.imageSizes.at(VectorscopeScopeId) == std::pair<int, int>{256, 256});
    CHECK(dragged.imageSizes.at(HistogramScopeId) == std::pair<int, int>{1024, 384});
    CHECK(dragged.imageSizes.at(NeutralScopeId) == std::pair<int, int>{256, 256});

    // Only the resolutions: the region, the parameters and the scopes computed
    // are what the user asked for, dragged or still.
    CHECK(dragged.region == settings.region);
    CHECK(dragged.enabledScopes == settings.enabledScopes);
    CHECK(dragged.scopeParams == settings.scopeParams);
}

TEST_CASE("The waveform keeps its columns while the region is dragged")
{
    // A column is a place in the region, and scanning for a blown highlight or
    // a skin tone is exactly when that scope matters most: measured on a
    // photograph, halving the columns moves the trace by twenty times what
    // halving the height does. So the height is what a drag gives up, and the
    // parade - which is the same engine - gives up the same.
    AnalysisSettings settings;
    settings.imageSizes[WaveformScopeId] = {2048, 512};
    settings.imageSizes[ParadeScopeId] = {1024, 512};

    const AnalysisSettings dragged = coarsenedForDrag(settings);
    CHECK(dragged.imageSizes.at(WaveformScopeId) == std::pair<int, int>{2048, 256});
    CHECK(dragged.imageSizes.at(ParadeScopeId) == std::pair<int, int>{1024, 256});
}

TEST_CASE("A dragged pass thins its samples where its columns are places")
{
    // The waveform keeps every column, so what it gives up while the region
    // moves is how densely each one is filled. Measured at 1024 columns over a
    // whole display, that is 64% of the pass for a mean of 1.3 of 255 - the
    // best trade per millisecond of any knob on any scope.
    AnalysisSettings settings;
    settings.imageSizes[WaveformScopeId] = {2048, 512};
    CHECK(settings.sampleThinning == 1);

    const AnalysisSettings dragged = coarsenedForDrag(settings);
    CHECK(dragged.sampleThinning == DraggedSampleDivisor);
    CHECK(DraggedSampleDivisor == 2);
}

TEST_CASE("A small scope image is left alone while the region is dragged")
{
    // Halving without a floor turns a small pane's image into a handful of
    // cells, which is a different picture rather than a coarser one.
    AnalysisSettings settings;
    settings.imageSizes[VectorscopeScopeId] = {DraggedDetailFloor + 40, DraggedDetailFloor + 40};
    settings.imageSizes[NeutralScopeId] = {DraggedDetailFloor / 2, DraggedDetailFloor / 2};

    const AnalysisSettings dragged = coarsenedForDrag(settings);
    CHECK(dragged.imageSizes.at(VectorscopeScopeId) == std::pair<int, int>{DraggedDetailFloor, DraggedDetailFloor});
    // Already below the floor: coarsening it further would be all that is left
    // of it, so it stands as it is.
    CHECK(dragged.imageSizes.at(NeutralScopeId) == std::pair<int, int>{DraggedDetailFloor / 2, DraggedDetailFloor / 2});
}

TEST_CASE("A scope off screen keeps the resolution in force")
{
    DetailFixture fixture;
    fixture.analysis.imageSizes[VectorscopeScopeId] = {256, 256};
    fixture.analysis.imageSizes[WaveformScopeId] = {1024, 512};

    // Only the vectorscope is on screen: the waveform's pane says nothing about
    // an image nobody is drawing, so a pane that would grow it moves nothing.
    CHECK(fixture.detail.desiredWaveformSize(LargePanes, 0) == std::pair<int, int>{1024, 512});
    CHECK_FALSE(fixture.detail.update(SmallPanes, 1.0f, std::nullopt, 1.0).has_value());
    CHECK_FALSE(fixture.detail.update(SmallPanes, 1.0f, std::nullopt, 2.0).has_value());
}

TEST_CASE("A low level asks for less of everything but the columns")
{
    // The order measurement ranked them in: the vectorscope's image and the
    // histogram's plot are 45-49% of their passes for a tenth and a hundredth
    // of a code, the neutral plane is the largest memory item on the list with
    // its cast reading unmoved, and the waveform gives up its height. Its
    // COLUMNS do not move, because a column is a place in the region.
    DetailFixture fixture;
    // The vectorscope is the stack a fresh view starts on; the rest stack onto
    // it.
    fixture.view.stack().choose(WaveformScopeId, true);
    fixture.view.stack().choose(HistogramScopeId, true);
    fixture.view.stack().choose(NeutralScopeId, true);

    const std::pair<int, int> standardWaveform = fixture.detail.desiredWaveformSize(LargePanes, 0);
    const std::pair<int, int> standardHistogram = fixture.detail.desiredHistogramSize(LargePanes);
    const int standardVectorscope = fixture.detail.desiredVectorscopeSize(LargePanes);
    const int standardNeutral = fixture.detail.desiredNeutralSize(LargePanes);

    fixture.detail.setQuality(QualityLevel::Low);
    const std::pair<int, int> lowWaveform = fixture.detail.desiredWaveformSize(LargePanes, 0);
    CHECK(lowWaveform.first == standardWaveform.first);
    CHECK(lowWaveform.second < standardWaveform.second);
    CHECK(lowWaveform.second == WaveformLevels);

    const std::pair<int, int> lowHistogram = fixture.detail.desiredHistogramSize(LargePanes);
    CHECK(lowHistogram.first < standardHistogram.first);
    CHECK(lowHistogram.second < standardHistogram.second);
    CHECK(fixture.detail.desiredVectorscopeSize(LargePanes) < standardVectorscope);
    CHECK(fixture.detail.desiredNeutralSize(LargePanes) < standardNeutral);
}

TEST_CASE("A high level asks for more where the pane can use it")
{
    // A pane between two steps: Standard tolerates the magnification and stops
    // where it is, High does not and climbs. Every axis here is either a
    // display resolution over a fixed grid or - for the neutral plane and the
    // waveform's columns - real data the region can still populate.
    constexpr ScopePaneSizes ClimbablePanes{
        {1100.0f, 400.0f}, {1100.0f, 400.0f}, {1100.0f, 400.0f}, {400.0f, 400.0f}, {600.0f, 600.0f}};
    DetailFixture fixture;
    // The vectorscope is the stack a fresh view starts on; the rest stack onto
    // it.
    fixture.view.stack().choose(WaveformScopeId, true);
    fixture.view.stack().choose(HistogramScopeId, true);
    fixture.view.stack().choose(NeutralScopeId, true);

    const std::pair<int, int> standardWaveform = fixture.detail.desiredWaveformSize(ClimbablePanes, 0);
    const std::pair<int, int> standardHistogram = fixture.detail.desiredHistogramSize(ClimbablePanes);
    const int standardVectorscope = fixture.detail.desiredVectorscopeSize(ClimbablePanes);
    const int standardNeutral = fixture.detail.desiredNeutralSize(ClimbablePanes);

    fixture.detail.setQuality(QualityLevel::High);
    const std::pair<int, int> highWaveform = fixture.detail.desiredWaveformSize(ClimbablePanes, 0);
    CHECK(highWaveform.first > standardWaveform.first);
    CHECK(fixture.detail.desiredHistogramSize(ClimbablePanes).first > standardHistogram.first);
    CHECK(fixture.detail.desiredVectorscopeSize(ClimbablePanes) > standardVectorscope);
    CHECK(fixture.detail.desiredNeutralSize(ClimbablePanes) > standardNeutral);

    // And buys nothing on the two axes measurement closed: the waveform's
    // height only draws a finer spline through levels eight-bit input fixes at
    // 256, and the histogram's height is the least visible of any of these.
    CHECK(highWaveform.second == standardWaveform.second);
    CHECK(fixture.detail.desiredHistogramSize(ClimbablePanes).second == standardHistogram.second);
}

TEST_CASE("A high level never resolves more than the region can fill")
{
    // Tolerating no magnification is still bounded by what is there to see: a
    // column with no pixels behind it is an empty column, whatever the pane.
    DetailFixture fixture;
    fixture.view.stack().choose(WaveformScopeId, false);
    fixture.detail.setQuality(QualityLevel::High);

    CHECK(fixture.detail.desiredWaveformSize(LargePanes, 1500).first == 1024);
    CHECK(fixture.detail.desiredWaveformSize(LargePanes, 600).first == 512);
}

TEST_CASE("Changing the level moves the resolutions in force")
{
    // The level reaches the worker by the same debounced route a resized pane
    // does, so a change made from the menu is applied once rather than per
    // frame.
    DetailFixture fixture;
    fixture.view.stack().choose(VectorscopeScopeId, false);
    fixture.analysis.imageSizes[VectorscopeScopeId] = {512, 512};

    CHECK_FALSE(fixture.detail.update(LargePanes, 1.0f, std::nullopt, 1.0).has_value());
    fixture.detail.setQuality(QualityLevel::Low);
    CHECK_FALSE(fixture.detail.update(LargePanes, 1.0f, std::nullopt, 2.0).has_value());

    const std::optional<DetailSizes> settled =
        fixture.detail.update(LargePanes, 1.0f, std::nullopt, 2.5 + DetailSettleSeconds);
    REQUIRE(settled.has_value());
    CHECK(settled->vectorscope == 256);
}

TEST_CASE("A dragged pass thins on top of the level's own thinning")
{
    // The drag divisor multiplies rather than replaces, so a level that
    // already samples thinly still gives something up under the hand. Without
    // that, choosing Low would silently cost a drag the coarsening a Standard
    // drag gets.
    AnalysisSettings settings;
    settings.imageSizes[WaveformScopeId] = {2048, 512};
    settings.sampleThinning = profileFor(QualityLevel::Low).sampleThinning;

    CHECK(coarsenedForDrag(settings).sampleThinning ==
          profileFor(QualityLevel::Low).sampleThinning * DraggedSampleDivisor);
}

}  // namespace sidescopes
