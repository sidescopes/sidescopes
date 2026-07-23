#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <utility>

#include "app/adaptive_detail.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "core/analysis_worker.h"
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
constexpr ScopePaneSizes LargePanes{{1600.0f, 700.0f}, {1600.0f, 700.0f}, {1600.0f, 700.0f}, {700.0f, 700.0f}};
constexpr ScopePaneSizes SmallPanes{{400.0f, 300.0f}, {400.0f, 300.0f}, {400.0f, 300.0f}, {300.0f, 300.0f}};

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

    constexpr ScopePaneSizes MidPanes{{800.0f, 400.0f}, {}, {800.0f, 400.0f}, {400.0f, 400.0f}};
    CHECK(fixture.detail.desiredWaveformSize(MidPanes, 0) == std::pair<int, int>{1024, WaveformLevels});
    // A narrow region cannot populate more columns than it has pixels.
    CHECK(fixture.detail.desiredWaveformSize(LargePanes, 1500).first == 1024);

    CHECK(fixture.detail.desiredHistogramSize(LargePanes) == std::pair<int, int>{2048, 768});
    CHECK(fixture.detail.desiredHistogramSize(SmallPanes) == std::pair<int, int>{512, 384});

    // The vectorscope is square, so its shorter side decides.
    CHECK(fixture.detail.desiredVectorscopeSize(LargePanes) == 512);
    CHECK(fixture.detail.desiredVectorscopeSize(MidPanes) == 256);
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

}  // namespace sidescopes
