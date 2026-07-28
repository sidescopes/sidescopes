#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/scopes/sampling.h"
#include "core/scopes/waveform.h"
#include "core/scopes/waveform_bins.h"
#include "scope_image.h"
#include "test_frame.h"

namespace sidescopes {

using namespace test;

namespace {

WaveformSettings settingsFor(WaveformMode mode)
{
    WaveformSettings settings;
    settings.mode = mode;
    return settings;
}

// A photograph-ish frame: the shared and unshared paths must agree over
// content with real structure, not only over a flat fill that every path
// draws identically.
TestFrame texturedFrame()
{
    TestFrame frame(200, 120, 0);
    for (int py = 0; py < 120; ++py) {
        for (int px = 0; px < 200; ++px) {
            const auto value = static_cast<uint8_t>((px * 7 + py * 13) % 256);
            frame.setColor(px, py,
                           Color{value, static_cast<uint8_t>(255 - value), static_cast<uint8_t>((value * 3) % 256)});
        }
    }

    return frame;
}

// The image an engine of its own draws for @p mode, which is what a shared one
// has to reproduce byte for byte.
std::vector<uint8_t> unsharedImage(const TestFrame& frame, WaveformMode mode)
{
    Waveform scope;
    scope.configure(settingsFor(mode));
    scope.accumulate(frame.view(), IntRect{0, 0, frame.width, frame.height});

    return scope.image().rgba;
}

}  // namespace

TEST_CASE("Two waveform scopes over one frame scatter it once")
{
    // The waveform and the parade bin a region identically - one engine, one
    // geometry, one sampling grid - so the first to run fills the shared bins
    // and the second must find its answer already there.
    const TestFrame frame = texturedFrame();
    WaveformBins shared;
    Waveform overlay;
    Waveform parade;
    overlay.lendBins(&shared);
    parade.lendBins(&shared);
    parade.configure(settingsFor(WaveformMode::RgbParade));

    overlay.accumulate(frame.view(), IntRect{0, 0, 200, 120});
    parade.accumulate(frame.view(), IntRect{0, 0, 200, 120});

    CHECK(shared.scatters() == 1);
}

TEST_CASE("A shared scatter draws exactly what an unshared one draws")
{
    // The condition on the whole optimization. Every scope image is compared
    // byte for byte against the same scope reading bins of its own, because a
    // trace that is merely close is a different measurement.
    const TestFrame frame = texturedFrame();
    const std::vector<uint8_t> overlayAlone = unsharedImage(frame, WaveformMode::Rgb);
    const std::vector<uint8_t> paradeAlone = unsharedImage(frame, WaveformMode::RgbParade);

    WaveformBins shared;
    Waveform overlay;
    Waveform parade;
    overlay.lendBins(&shared);
    parade.lendBins(&shared);
    parade.configure(settingsFor(WaveformMode::RgbParade));
    overlay.accumulate(frame.view(), IntRect{0, 0, 200, 120});
    parade.accumulate(frame.view(), IntRect{0, 0, 200, 120});

    CHECK(overlay.image().rgba == overlayAlone);
    CHECK(parade.image().rgba == paradeAlone);
}

TEST_CASE("A scope reading fewer planes does not answer for the rest")
{
    // Luma writes only its own plane, so the three channel planes still hold
    // whatever frame filled them last. A scope wanting those must scatter
    // again rather than read a pass that never touched them.
    const TestFrame frame = texturedFrame();
    const std::vector<uint8_t> paradeAlone = unsharedImage(frame, WaveformMode::RgbParade);

    WaveformBins shared;
    Waveform luma;
    Waveform parade;
    luma.lendBins(&shared);
    parade.lendBins(&shared);
    luma.configure(settingsFor(WaveformMode::Luma));
    parade.configure(settingsFor(WaveformMode::RgbParade));
    luma.accumulate(frame.view(), IntRect{0, 0, 200, 120});
    parade.accumulate(frame.view(), IntRect{0, 0, 200, 120});

    CHECK(shared.scatters() == 2);
    CHECK(parade.image().rgba == paradeAlone);
}

TEST_CASE("A scope binning the same planes differently does not share them")
{
    // Colored luma fills the three channel planes with value-weighted mass at
    // the luma level, where the combined mode fills them with counts at each
    // channel's own. The planes are the same three and the numbers in them are
    // not, so the two must never read each other's.
    const TestFrame frame = texturedFrame();
    const std::vector<uint8_t> coloredAlone = unsharedImage(frame, WaveformMode::ColoredLuma);

    WaveformBins shared;
    Waveform combined;
    Waveform colored;
    combined.lendBins(&shared);
    colored.lendBins(&shared);
    combined.configure(settingsFor(WaveformMode::RgbAndLuma));
    colored.configure(settingsFor(WaveformMode::ColoredLuma));
    combined.accumulate(frame.view(), IntRect{0, 0, 200, 120});
    colored.accumulate(frame.view(), IntRect{0, 0, 200, 120});

    CHECK(shared.scatters() == 2);
    CHECK(colored.image().rgba == coloredAlone);
}

TEST_CASE("A second frame is scattered again")
{
    // The bins answer for one frame. The next one must reach them however
    // little the pixels moved, so the key carries the frame's own identity and
    // not merely the geometry that was asked for.
    const TestFrame frame = texturedFrame();
    WaveformBins shared;
    Waveform overlay;
    overlay.lendBins(&shared);

    FrameView view = frame.view();
    overlay.accumulate(view, IntRect{0, 0, 200, 120});
    CHECK(shared.scatters() == 1);

    view.sequence = 2;
    overlay.accumulate(view, IntRect{0, 0, 200, 120});
    CHECK(shared.scatters() == 2);
}

TEST_CASE("Another buffer is scattered again whatever it is numbered")
{
    // The frame's number is the producer's promise that its pixels moved on,
    // and both capture backends keep it. The bins do not rest on that promise
    // alone: pixels somewhere else are a different frame however they are
    // numbered, which is the case a test or a second producer can reach.
    const TestFrame first = texturedFrame();
    TestFrame second(200, 120, 0);
    second.fill(Color{200, 30, 90});

    WaveformBins shared;
    Waveform overlay;
    overlay.lendBins(&shared);
    overlay.accumulate(first.view(), IntRect{0, 0, 200, 120});
    const std::vector<uint8_t> textured = overlay.image().rgba;
    overlay.accumulate(second.view(), IntRect{0, 0, 200, 120});

    CHECK(shared.scatters() == 2);
    CHECK(overlay.image().rgba != textured);
}

TEST_CASE("A region that moves without resizing is scattered again")
{
    // Scopes share only what they measure over the SAME region. The region is
    // CARRIED here rather than resized, which is what a user dragging one does
    // and the one case the sampling grid cannot stand in for: a grid is decided
    // by a region's size and says nothing about where it sits.
    const TestFrame frame = texturedFrame();
    WaveformBins shared;
    Waveform overlay;
    overlay.lendBins(&shared);

    overlay.accumulate(frame.view(), IntRect{0, 0, 100, 60});
    const std::vector<uint8_t> atOrigin = overlay.image().rgba;
    overlay.accumulate(frame.view(), IntRect{40, 20, 100, 60});

    CHECK(shared.scatters() == 2);
    CHECK(overlay.image().rgba != atOrigin);
}

TEST_CASE("Waveform in luma mode plots mid gray on one level")
{
    // Rec.709 luma of (128, 128, 128) is 128, which is row 255 - 128 = 127.
    TestFrame frame(32, 16, 255);
    frame.fill(Color{128, 128, 128});

    Waveform scope;
    scope.configure(settingsFor(WaveformMode::Luma));
    scope.accumulate(frame.view(), IntRect{0, 0, 32, 16});

    for (int channel = 0; channel < 3; ++channel) {
        CHECK(peakRows(scope.image(), channel) == std::vector<int>{127});
    }
}

TEST_CASE("Waveform in rgb mode plots each channel at its own level")
{
    TestFrame frame(32, 16, 255);
    frame.fill(Color{10, 150, 240});

    Waveform scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 32, 16});  // RGB is the default

    CHECK(peakRows(scope.image(), 0) == std::vector<int>{255 - 10});
    CHECK(peakRows(scope.image(), 1) == std::vector<int>{255 - 150});
    CHECK(peakRows(scope.image(), 2) == std::vector<int>{255 - 240});
}

TEST_CASE("Waveform plots the same levels when a tall region splits across threads")
{
    // A region tall enough to split across worker threads must plot each
    // channel on its own level exactly as the single-threaded path: the
    // privatized per-thread planes merge back by integer addition.
    TestFrame frame(32, 1024, 255);
    frame.fill(Color{10, 150, 240});

    Waveform scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 32, 1024});  // RGB is the default

    CHECK(peakRows(scope.image(), 0) == std::vector<int>{255 - 10});
    CHECK(peakRows(scope.image(), 1) == std::vector<int>{255 - 150});
    CHECK(peakRows(scope.image(), 2) == std::vector<int>{255 - 240});
}

TEST_CASE("Waveform combined mode adds a white luma trace over rgb")
{
    // Luma of (10, 150, 240) is (54*10 + 183*150 + 19*240) >> 8 = 127.
    TestFrame frame(32, 16, 255);
    frame.fill(Color{10, 150, 240});

    Waveform scope;
    scope.configure(settingsFor(WaveformMode::RgbAndLuma));
    scope.accumulate(frame.view(), IntRect{0, 0, 32, 16});

    // Rows are reported top-down: the luma trace (row 128) precedes deeper
    // channel levels and follows shallower ones.
    CHECK(peakRows(scope.image(), 0) == std::vector<int>{255 - 127, 255 - 10});
    CHECK(peakRows(scope.image(), 1) == std::vector<int>{255 - 150, 255 - 127});
    CHECK(peakRows(scope.image(), 2) == std::vector<int>{255 - 240, 255 - 127});
}

TEST_CASE("Waveform parade shows each channel in its own third")
{
    // Uniform (10, 150, 240): each third lights one row at its channel's
    // level, in that channel's color only.
    TestFrame frame(32, 16, 255);
    frame.fill(Color{10, 150, 240});

    Waveform scope;
    scope.configure(settingsFor(WaveformMode::RgbParade));
    scope.accumulate(frame.view(), IntRect{0, 0, 32, 16});

    constexpr int Third = Waveform::Columns / 3;
    // Sparse test frames splat isolated columns and the gutters shift
    // the pane mapping, so probe by scanning each pane's interior.
    const auto panePeak = [&](int pane, int row, int channel) {
        int peak = 0;
        for (int column = pane * Third; column < (pane + 1) * Third; ++column) {
            peak = std::max<int>(
                peak, scope.image().rgba[(static_cast<std::size_t>(row) * Waveform::Columns + column) * 4 + channel]);
        }
        return peak;
    };
    // Red third: lit at row 255-10 in red, dark in green and blue.
    CHECK(panePeak(0, 255 - 10, 0) > 0);
    CHECK(panePeak(0, 255 - 10, 1) == 0);
    CHECK(panePeak(0, 255 - 150, 0) == 0);
    // Green third: lit at row 255-150 in green only.
    CHECK(panePeak(1, 255 - 150, 1) > 0);
    CHECK(panePeak(1, 255 - 150, 0) == 0);
    // Blue third: lit at row 255-240 in blue only.
    CHECK(panePeak(2, 255 - 240, 2) > 0);
    CHECK(panePeak(2, 255 - 240, 1) == 0);
}

TEST_CASE("Waveform parade preserves horizontal position within each third")
{
    // Left half bright red, right half dark red: within the red third the
    // left local half sits at the bright level and the right at the dark.
    TestFrame frame(64, 16, 255);
    frame.fill(Color{50, 0, 0});
    for (int py = 0; py < 16; ++py) {
        for (int px = 0; px < 32; ++px) {
            uint8_t* p = frame.pixels.data() + (static_cast<std::size_t>(py) * 64 + px) * 4;
            p[2] = 200;
        }
    }

    Waveform scope;
    scope.configure(settingsFor(WaveformMode::RgbParade));
    scope.accumulate(frame.view(), IntRect{0, 0, 64, 16});

    constexpr int Third = Waveform::Columns / 3;
    // Scan each local half of the red pane: the gutters shift exact
    // column positions, the halves' contents do not move between them.
    const auto halfPeak = [&](int begin, int end, int row) {
        int peak = 0;
        for (int column = begin; column < end; ++column) {
            peak = std::max<int>(peak,
                                 scope.image().rgba[(static_cast<std::size_t>(row) * Waveform::Columns + column) * 4]);
        }
        return peak;
    };
    CHECK(halfPeak(0, Third / 2, 255 - 200) > 0);  // left local half: bright level
    CHECK(halfPeak(0, Third / 2 - 8, 255 - 50) == 0);
    CHECK(halfPeak(Third / 2 + 8, Third, 255 - 50) > 0);  // right local half: dark level
    CHECK(halfPeak(Third / 2 + 8, Third, 255 - 200) == 0);
}

TEST_CASE("Waveform projection reports the luma level")
{
    Waveform scope;
    const NormalizedPoint point = scope.project(FloatColor{128.0f, 128.0f, 128.0f});
    CHECK(point.x < 0.0f);  // horizontal position is the caller's problem
    CHECK(point.y > 0.49f);
    CHECK(point.y < 0.51f);
}

TEST_CASE("Waveform fills a missing level between populated neighbors")
{
    // The capture pipeline's 8-bit color conversion leaves "missing
    // codes": level values that almost never occur. Each rendered as a
    // dark line across the trace once the pane grew large. Alternating
    // columns of 100 and 102 leave 101 empty; the wide vertical kernel
    // must light it comparably to its populated neighbors.
    TestFrame frame(64, 64, 255);
    for (int py = 0; py < 64; ++py) {
        const uint8_t value = py % 2 == 0 ? 100 : 102;
        frame.fillRows(py, py + 1, Color{value, value, value});
    }

    Waveform scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    const int populated = channelAt(scope.image(), 0, 255 - 100, 1);
    const int gap = channelAt(scope.image(), 0, 255 - 101, 1);
    REQUIRE(populated > 0);
    CHECK(gap >= populated * 3 / 4);
}

TEST_CASE("Waveform flattens the pipeline's code-density comb")
{
    // Display pipelines populate 8-bit codes unevenly: here every column
    // holds value 100 twice as often as 102, frame-wide - the doubled
    // code used to render as a brighter line across the whole trace.
    // The flat-field correction must bring the two rows together.
    TestFrame frame(64, 66, 255);
    for (int py = 0; py < 66; ++py) {
        const uint8_t value = py % 3 == 2 ? 102 : 100;
        frame.fillRows(py, py + 1, Color{value, value, value});
    }

    Waveform scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 64, 66});

    const int doubled = channelAt(scope.image(), 0, 255 - 100, 1);
    const int single = channelAt(scope.image(), 0, 255 - 102, 1);
    REQUIRE(single > 0);
    CHECK(doubled <= single * 5 / 4);
    CHECK(doubled >= single * 3 / 4);
}

TEST_CASE("Waveform attenuates a display-pipeline pileup")
{
    // Display-profile conversions pile many source codes onto one output
    // code and starve its neighbors - here level 100 carries twenty rows
    // for every one row of its neighbors, next to a hole at 102. A real
    // photo feature cannot look like this; the pileup must come down to
    // its neighborhood instead of rendering as a bright line.
    TestFrame frame(64, 260, 255);
    for (int py = 0; py < 260; ++py) {
        uint8_t value = static_cast<uint8_t>(90 + (py % 26));
        if (py % 26 == 10) {
            value = 100;  // extra mass onto 100
        }
        if (value == 102) {
            value = 100;  // and 102 is never emitted
        }
        frame.fillRows(py, py + 1, Color{value, value, value});
    }

    Waveform scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 64, 260});

    const int pileup = channelAt(scope.image(), 0, 255 - 100, 1);
    const int neighbor = channelAt(scope.image(), 0, 255 - 97, 1);
    REQUIRE(neighbor > 0);
    CHECK(pileup <= neighbor * 4 / 3);
}

TEST_CASE("Waveform keeps a real clipping line bright")
{
    // Crushed blacks concentrate at the very bottom of the populated
    // range with nothing below - a real feature the photographer must
    // see. The pileup correction may not touch it.
    TestFrame frame(64, 64, 255);
    frame.fillRows(0, 32, Color{16, 16, 16});  // crushed shadows
    for (int py = 32; py < 64; ++py) {
        const uint8_t value = static_cast<uint8_t>(80 + py);
        frame.fillRows(py, py + 1, Color{value, value, value});
    }

    Waveform scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    const int clip = channelAt(scope.image(), 0, 255 - 16, 1);
    const int body = channelAt(scope.image(), 0, 255 - 120, 1);
    REQUIRE(body > 0);
    CHECK(clip > body);
}

TEST_CASE("Colored luma waveform tints the trace with the source color")
{
    // A solid 75% red frame: the trace must sit on red's luma level and
    // carry red's hue - density decides brightness, the value-weighted
    // planes decide only the color.
    TestFrame frame(64, 64, 255);
    frame.fill(Color{191, 0, 0});

    Waveform scope;
    scope.configure(settingsFor(WaveformMode::ColoredLuma));
    scope.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    // Rec.709 luma of (191, 0, 0): 54 * 191 / 256 = 40 -> row 255 - 40.
    const ScopeImage& image = scope.image();
    const int y = (255 - 40) * image.height / 256;
    const uint8_t* pixel = image.rgba.data() + (static_cast<std::size_t>(y) * image.width + image.width / 2) * 4;
    CHECK(static_cast<int>(pixel[0]) > 150);           // bright...
    CHECK(static_cast<int>(pixel[0]) > 4 * pixel[1]);  // ...and red
    CHECK(static_cast<int>(pixel[0]) > 4 * pixel[2]);
}

TEST_CASE("Waveform renders taller images through the level spline")
{
    // Level data always has 256 codes; a taller image samples them
    // through a spline, and a single-level trace must peak at the scaled
    // position.
    TestFrame frame(64, 64, 255);
    frame.fill(Color{127, 127, 127});

    Waveform scope;
    WaveformSettings settings;
    settings.imageHeight = 512;
    scope.configure(settings);
    scope.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    const ScopeImage& image = scope.image();
    REQUIRE(image.height == 512);
    int peakRow = -1;
    int peakValue = 0;
    for (int row = 0; row < image.height; ++row) {
        const int value = channelAt(image, 0, row, 1);
        if (value > peakValue) {
            peakValue = value;
            peakRow = row;
        }
    }
    REQUIRE(peakValue > 0);
    CHECK(peakRow >= (255 - 127) * 2 - 2);
    CHECK(peakRow <= (255 - 127) * 2 + 3);
}

TEST_CASE("Waveform respects a narrower column budget")
{
    TestFrame frame(64, 64, 255);
    frame.fill(Color{127, 127, 127});

    Waveform scope;
    WaveformSettings settings;
    settings.columns = 512;
    scope.configure(settings);
    scope.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    CHECK(scope.image().width == 512);
    CHECK(peakRows(scope.image(), 1) == std::vector<int>{255 - 127});
}

TEST_CASE("Waveform column brightness is invariant to stride and region size")
{
    // Two gray levels stacked 3:1 vertically, so every sampled column sees
    // the same 3:1 level mix at stride 1, stride 2, and in a half-size
    // region. Per-row density normalization must then light the shared
    // column zero identically in all three runs.
    TestFrame frame(64, 64, 255);
    frame.fillRows(0, 48, Color{191, 191, 191});
    frame.fillRows(48, 64, Color{64, 64, 64});

    Waveform reference;
    reference.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    Waveform strided;
    WaveformSettings settings;
    settings.samplingStride = 2;
    strided.configure(settings);
    strided.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    Waveform smaller;
    // Half the width, full height: the level mix in each column stays 3:1.
    smaller.accumulate(frame.view(), IntRect{0, 0, 32, 64});

    for (const int row : {255 - 191, 255 - 64}) {
        const uint8_t expected = channelAt(reference.image(), 0, row, 1);
        CHECK(expected > 0);
        CHECK(channelAt(strided.image(), 0, row, 1) == expected);
        CHECK(channelAt(smaller.image(), 0, row, 1) == expected);
    }
}

TEST_CASE("Waveform clamps the column and level budgets to their ranges")
{
    TestFrame frame(64, 64, 255);
    frame.fill(Color{127, 127, 127});

    Waveform tooLarge;
    WaveformSettings large;
    large.columns = 9999;
    large.imageHeight = 9999;
    tooLarge.configure(large);
    tooLarge.accumulate(frame.view(), IntRect{0, 0, 64, 64});
    // The ceilings moved up when the image was allowed to follow its pane: a
    // scope filling a second monitor was being magnified by the display, and
    // columns carry one sample per place in the region, so a wide pane deserves
    // them. Height only resolves the level spline, the levels themselves being
    // fixed at 256 by eight-bit input.
    CHECK(tooLarge.image().width == MaximumWaveformColumns);
    CHECK(tooLarge.image().height == MaximumWaveformHeight);

    Waveform tooSmall;
    WaveformSettings small;
    small.columns = 10;
    small.imageHeight = 10;
    tooSmall.configure(small);
    tooSmall.accumulate(frame.view(), IntRect{0, 0, 64, 64});
    CHECK(tooSmall.image().width == 256);
    CHECK(tooSmall.image().height == 256);
}

TEST_CASE("Waveform produces a black image for an empty region")
{
    TestFrame frame(64, 64, 255);
    frame.fill(Color{127, 127, 127});

    Waveform scope;
    scope.accumulate(frame.view(), IntRect{100, 100, 8, 8});  // off the frame

    const std::vector<uint8_t>& rgba = scope.image().rgba;
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        REQUIRE(rgba[i] + rgba[i + 1] + rgba[i + 2] == 0);
    }
}

TEST_CASE("Colored luma waveform draws a neutral trace for a pure black region")
{
    // Black carries no color mass, so the value-weighted planes cannot tint
    // the trace: the strongest-channel guard must fall back to a neutral gray
    // rather than dividing by zero.
    TestFrame frame(64, 64, 0);  // pure black, every channel zero

    Waveform scope;
    scope.configure(settingsFor(WaveformMode::ColoredLuma));
    scope.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    const ScopeImage& image = scope.image();
    const int row = brightestRow(image.rgba.data(), image.width, image.height, 0);
    REQUIRE(row >= 0);
    const int column = image.width / 2;
    const int r = channelAt(image, column, row, 0);
    const int g = channelAt(image, column, row, 1);
    const int b = channelAt(image, column, row, 2);
    REQUIRE(r > 0);
    CHECK(r == g);  // no color mass -> equal channels, a neutral trace
    CHECK(g == b);
}

TEST_CASE("A waveform wide enough to need every row gets every row")
{
    // The waveform's budget is its bin count times the samples each bin needs,
    // and its bins are an order of magnitude emptier than any other scope's, so
    // from its default width up that budget exceeds any region's pixel count and
    // no thinning happens. Nothing else guards that: the exact goldens run on
    // frames far too small to reach any budget, so a waveform that started
    // thinning would pass every one of them.
    //
    // So paint black exactly the rows a thinned pass would visit and white all
    // the rest, taking the pattern from the policy itself rather than hard
    // coding it. A waveform that samples every row sees white; one that thinned
    // would see nothing but black.
    constexpr int Width = 4201;
    constexpr int Height = 1000;
    const IntRect region{0, 0, Width, Height};
    // Below the waveform's own budget at its default width, and above the one a
    // scope with fixed bins would get - so this frame separates the two.
    REQUIRE(static_cast<long long>(Width) * Height >
            budgetForBins(static_cast<long long>(DefaultWaveformColumns) * WaveformLevels, 1));
    REQUIRE(static_cast<long long>(Width) * Height <
            budgetForBins(static_cast<long long>(DefaultWaveformColumns) * WaveformLevels, WaveformMinSamplesPerBin));

    const SampleGrid budgeted = sampleGridFor(1, region, SampleBudget);
    REQUIRE(budgeted.rowStride > 1);

    TestFrame frame(Width, Height, 0);
    frame.fill(Color{255, 255, 255});
    for (int index = 0; index < budgeted.rows; ++index) {
        const int row = sampleRowOf(budgeted, region, index);
        frame.fillRows(row, row + 1, Color{0, 0, 0});
    }

    Waveform waveform;
    waveform.configure(settingsFor(WaveformMode::Luma));
    waveform.accumulate(frame.view(), region);

    // White is luma 255, which is the image's top row; black is the bottom.
    // Both must be populated, because half the rows carry each.
    bool topLit = false;
    bool bottomLit = false;
    for (int column = 0; column < waveform.image().width; ++column) {
        topLit = topLit || pixelLit(waveform.image(), column, 0);
        bottomLit = bottomLit || pixelLit(waveform.image(), column, WaveformLevels - 1);
    }
    CHECK(topLit);
    CHECK(bottomLit);
}

TEST_CASE("A narrow waveform really does thin, through its own accumulate")
{
    // The sibling test above proves a wide waveform sees every row. This proves
    // the other half - that a narrow one thins - and it goes through the scope's
    // own accumulate rather than the policy function, because the policy holding
    // is no use if the waveform stops asking it. Reverting the waveform to opting
    // out of the budget passes every other test in the suite.
    constexpr int Columns = 512;
    constexpr int Width = 3000;
    constexpr int Height = 1500;
    const IntRect region{0, 0, Width, Height};
    const long long budget = budgetForBins(static_cast<long long>(Columns) * WaveformLevels, WaveformMinSamplesPerBin);
    REQUIRE(static_cast<long long>(Width) * Height > budget);

    const SampleGrid thinned = sampleGridFor(1, region, budget);
    REQUIRE(thinned.rowStride > 1);

    // Black on exactly the rows a thinned pass visits, white everywhere else. A
    // waveform that thins sees black alone; one that reads every row sees white.
    TestFrame frame(Width, Height, 0);
    frame.fill(Color{255, 255, 255});
    for (int index = 0; index < thinned.rows; ++index) {
        const int row = sampleRowOf(thinned, region, index);
        frame.fillRows(row, row + 1, Color{0, 0, 0});
    }

    Waveform waveform;
    WaveformSettings settings;
    settings.columns = Columns;
    settings.mode = WaveformMode::Luma;
    waveform.configure(settings);
    waveform.accumulate(frame.view(), region);

    const ScopeImage& image = waveform.image();
    // Level 255 sits in row zero and level 0 at the bottom, and the trace is in
    // the colour channels - the image is opaque throughout. A pass that saw only
    // black has its mass at the bottom and nothing at the top; one that read
    // every row would light the top too.
    const int column = image.width / 2;
    CHECK(channelAt(image, column, 0, 1) == 0);
    CHECK(channelAt(image, column, image.height - 1, 1) > 0);
}

TEST_CASE("A thinned waveform stops reading every row, through its accumulate")
{
    // The sibling tests above prove a wide waveform reads every row and a
    // narrow one thins. This proves the third case: a wide one told to thin.
    // Through the engine's own accumulate rather than the policy function,
    // because the policy holding is no use if the waveform stops asking it.
    constexpr int Columns = 1024;
    constexpr int Width = 3000;
    constexpr int Height = 1500;
    const IntRect region{0, 0, Width, Height};
    const long long full = budgetForBins(static_cast<long long>(Columns) * WaveformLevels, WaveformMinSamplesPerBin);
    REQUIRE(sampleGridFor(1, region, full).rowStride == 1);

    const long long halved =
        budgetForBins(static_cast<long long>(Columns) * WaveformLevels, WaveformMinSamplesPerBin / 2);
    const SampleGrid thinned = sampleGridFor(1, region, halved);
    REQUIRE(thinned.rowStride > 1);

    // Black on exactly the rows a thinned pass visits, white everywhere else,
    // so a pass that reads every row lights the top of the trace and one that
    // thins does not.
    TestFrame frame(Width, Height, 0);
    frame.fill(Color{255, 255, 255});
    for (int index = 0; index < thinned.rows; ++index) {
        const int row = sampleRowOf(thinned, region, index);
        frame.fillRows(row, row + 1, Color{0, 0, 0});
    }

    WaveformSettings settings;
    settings.columns = Columns;
    settings.mode = WaveformMode::Luma;

    Waveform every;
    every.configure(settings);
    every.accumulate(frame.view(), region);

    settings.sampleThinning = 2;
    Waveform sparse;
    sparse.configure(settings);
    sparse.accumulate(frame.view(), region);

    const int column = every.image().width / 2;
    CHECK(channelAt(every.image(), column, 0, 1) > 0);
    CHECK(channelAt(sparse.image(), column, 0, 1) == 0);
}

namespace {

// Textured content standing in for a photograph: every column spans many
// levels, which is what separates a picture from the editor's chrome.
void paintTexture(TestFrame& frame, IntRect area)
{
    uint32_t state = 0x9E3779B9u;
    const auto next = [&state] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;

        return state;
    };
    for (int py = area.y; py < area.y + area.height; ++py) {
        for (int px = area.x; px < area.x + area.width; ++px) {
            const int shade = 40 + static_cast<int>(next() % 170u);
            frame.setColor(px, py,
                           Color{static_cast<uint8_t>(shade), static_cast<uint8_t>(shade * 4 / 5),
                                 static_cast<uint8_t>(shade * 3 / 5)});
        }
    }
}

// The mean absolute difference over the columns both images built from the
// same pixels, skipping the flat strip and the columns its mass smears into.
double traceDifference(const ScopeImage& before, const ScopeImage& after, int firstColumn)
{
    double total = 0.0;
    long long counted = 0;
    for (int py = 0; py < before.height; ++py) {
        for (int px = firstColumn; px < before.width; ++px) {
            const auto base = (static_cast<std::size_t>(py) * before.width + px) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                total += std::abs(static_cast<double>(before.rgba[base + channel]) -
                                  static_cast<double>(after.rgba[base + channel]));
                ++counted;
            }
        }
    }

    return counted > 0 ? total / static_cast<double>(counted) : 0.0;
}

}  // namespace

TEST_CASE("A flat tone beside the picture leaves the rest of the trace alone")
{
    // The reported bug: sliding a region a few pixels off the photograph, onto
    // the editor's flat chrome, dimmed the whole waveform. Those columns put
    // every sample they have into one bin, and the ceiling the log
    // normalization divides by was simply the densest bin, so the picture's own
    // trace lost a quarter of its brightness for content beside it.
    //
    // Same region, same pixels, with a strip of one flat tone painted over the
    // leading columns: the part of the trace built from the shared pixels must
    // not move.
    const IntRect region{0, 0, 480, 320};
    const int strip = 12;  // Two and a half percent of the region's width.

    TestFrame picture(480, 320, 0);
    paintTexture(picture, region);

    // One flat tone, and the same tone dithered over two levels: a window
    // background is not always a single code, and the share that decides what
    // counts as flat has to hold for both. Two levels put 5/12 of a column's
    // mass on one bin against 4/6 for one level, and at most a quarter for a
    // photographic column.
    for (const int spread : {1, 2}) {
        TestFrame withChrome(480, 320, 0);
        withChrome.pixels = picture.pixels;
        for (int py = region.y; py < region.y + region.height; ++py) {
            for (int px = 0; px < strip; ++px) {
                const auto shade = static_cast<uint8_t>(45 + (px + py) % spread);
                withChrome.setColor(px, py, Color{shade, shade, static_cast<uint8_t>(shade + 2)});
            }
        }

        for (const WaveformMode mode :
             {WaveformMode::Luma, WaveformMode::Rgb, WaveformMode::RgbParade, WaveformMode::ColoredLuma}) {
            WaveformSettings settings = settingsFor(mode);
            settings.columns = 256;

            Waveform alone;
            alone.configure(settings);
            alone.accumulate(picture.view(), region);
            Waveform beside;
            beside.configure(settings);
            beside.accumulate(withChrome.view(), region);

            // The image columns the strip feeds, plus a guard for the
            // horizontal kernel; everything past them comes from identical
            // pixels.
            const int shared = strip * settings.columns / region.width + 4;
            INFO("waveform mode " << static_cast<int>(mode) << " over " << spread << " level(s)");
            CHECK(traceDifference(alone.image(), beside.image(), shared) < 2.0);
        }
    }
}

TEST_CASE("A frame of one flat tone still normalizes to its own peak")
{
    // The ceiling passes over columns that are a single flat tone, so a frame
    // with nothing else in it has no column left to measure. It falls back to
    // the plain maximum rather than dividing by nothing and rendering black -
    // and that fallback is what keeps the colour-bar and ramp goldens exact.
    TestFrame frame(320, 240, 0);
    frame.fill(Color{90, 140, 200});

    Waveform waveform;
    waveform.configure(settingsFor(WaveformMode::Rgb));
    waveform.accumulate(frame.view(), IntRect{0, 0, 320, 240});

    const auto [litColumn, litRow] = brightestPixel(waveform.image());
    CHECK(channelAt(waveform.image(), litColumn, litRow, 2) == 255);
}

TEST_CASE("Thinning cannot take a waveform below one sample a bin")
{
    // The divisor is clamped and the samples per bin it produces has a floor,
    // so an extreme value asks for a coarse pass rather than an empty one.
    WaveformSettings settings;
    settings.sampleThinning = 64;
    Waveform waveform;
    waveform.configure(settings);

    TestFrame frame(400, 300, 0);
    frame.fill(Color{200, 200, 200});
    waveform.accumulate(frame.view(), IntRect{0, 0, 400, 300});

    const auto [litColumn, litRow] = brightestPixel(waveform.image());
    CHECK(pixelLit(waveform.image(), litColumn, litRow));
}

}  // namespace sidescopes
