#include <catch2/catch_test_macros.hpp>

#include "core/frame.h"
#include "core/scopes/sampling.h"

namespace sidescopes {
namespace {

// The regions the policy has to get right: a swatch, a photo-editor canvas,
// a whole 1080p screen, a whole Retina laptop display, and a 5K panel.
constexpr IntRect Swatch{0, 0, 200, 200};
constexpr IntRect Canvas{0, 0, 1000, 700};
constexpr IntRect Screen1080{0, 0, 1920, 1080};
constexpr IntRect Retina{0, 0, 3456, 2234};
constexpr IntRect Panel5K{0, 0, 5120, 2880};

long long samplesIn(const SampleGrid& grid)
{
    return static_cast<long long>(grid.rows) * grid.columnsPerRow;
}

}  // namespace

TEST_CASE("Everyday regions are sampled whole")
{
    // Nothing a photographer scopes day to day reaches the budget, so the
    // thinning must be invisible there - not merely small.
    for (const IntRect& region : {Swatch, Canvas, Screen1080}) {
        CAPTURE(region.width, region.height);
        const SampleGrid grid = sampleGridFor(1, region, SampleBudget);
        CHECK(grid.rowStride == 1);
        CHECK(grid.columnStride == 1);
        CHECK(grid.rows == region.height);
        CHECK(grid.columnsPerRow == region.width);
    }
}

TEST_CASE("A region past the budget is thinned by rows until it fits")
{
    const SampleGrid retina = sampleGridFor(1, Retina, SampleBudget);
    CHECK(retina.rowStride == 2);
    CHECK(retina.columnStride == 1);
    CHECK(samplesIn(retina) <= SampleBudget);

    // A bigger panel thins further rather than giving up at two.
    const SampleGrid panel = sampleGridFor(1, Panel5K, SampleBudget);
    CHECK(panel.rowStride == 4);
    CHECK(samplesIn(panel) <= SampleBudget);

    // Columns are never dropped: an image column that some source columns
    // never reach is the density comb the fractional splat exists to prevent.
    CHECK(panel.columnStride == 1);
    CHECK(panel.columnsPerRow == Panel5K.width);
}

TEST_CASE("The user's stride is a floor the policy never undercuts")
{
    // Asking for coarser sampling must still give coarser sampling, on both
    // axes, whatever the budget would have chosen.
    const SampleGrid coarse = sampleGridFor(4, Canvas, SampleBudget);
    CHECK(coarse.rowStride == 4);
    CHECK(coarse.columnStride == 4);
    CHECK(coarse.rows == 175);
    CHECK(coarse.columnsPerRow == 250);

    // Out-of-range values clamp into the parameter's own 1..8 range.
    CHECK(sampleGridFor(0, Canvas, SampleBudget).columnStride == 1);
    CHECK(sampleGridFor(99, Canvas, SampleBudget).columnStride == 8);

    // A user stride that already fits is not raised further.
    const SampleGrid strided = sampleGridFor(2, Retina, SampleBudget);
    CHECK(strided.rowStride == 2);
    CHECK(strided.columnStride == 2);
}

TEST_CASE("An unlimited budget samples every row however large the region")
{
    // What the waveform asks for: its bins are too sparsely populated to
    // afford thinning, so the policy must leave it alone at any size.
    for (const IntRect& region : {Retina, Panel5K}) {
        CAPTURE(region.width, region.height);
        const SampleGrid grid = sampleGridFor(1, region, UnlimitedSamples);
        CHECK(grid.rowStride == 1);
        CHECK(grid.rows == region.height);
    }

    // The user's own stride still applies.
    CHECK(sampleGridFor(3, Retina, UnlimitedSamples).rowStride == 3);
}

TEST_CASE("An empty region asks for no samples")
{
    const SampleGrid grid = sampleGridFor(1, IntRect{10, 10, 0, 0}, SampleBudget);
    CHECK(grid.rows == 0);
    CHECK(grid.columnsPerRow == 0);
}

}  // namespace sidescopes
