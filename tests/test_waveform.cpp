#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "core/scopes/waveform.h"

namespace sidescopes {
namespace {

struct TestFrame
{
    explicit TestFrame(int width, int height)
        : width(width),
          height(height)
    {
        pixels.resize(static_cast<std::size_t>(width) * height * 4, 255);
    }

    void fill(Color color)
    {
        fillRows(0, height, color);
    }

    void fillRows(int y0, int y1, Color color)
    {
        for (int py = y0; py < y1; ++py) {
            for (int px = 0; px < width; ++px) {
                uint8_t* p = pixels.data() + (static_cast<std::size_t>(py) * width + px) * 4;
                p[0] = color.b;
                p[1] = color.g;
                p[2] = color.r;
            }
        }
    }

    [[nodiscard]] FrameView view() const
    {
        return FrameView{pixels.data(), width * 4, width, height, ColorSpaceHint::Srgb, 1};
    }

    std::vector<uint8_t> pixels;
    int width;
    int height;
};

// Collects the set of lit rows for one output channel (0=r, 1=g, 2=b).
// Rows that are local brightness peaks in one channel: smoothing spreads
// a flat color's trace one row up and down, and what must stay exact is
// where the peaks sit.
std::vector<int> peakRows(const ScopeImage& image, int channel)
{
    std::vector<int> brightness(static_cast<std::size_t>(image.height), 0);
    for (int py = 0; py < image.height; ++py) {
        int rowMax = 0;
        for (int px = 0; px < image.width; ++px) {
            const uint8_t* rgba = image.rgba.data() + (static_cast<std::size_t>(py) * image.width + px) * 4;
            rowMax = std::max(rowMax, static_cast<int>(rgba[channel]));
        }
        brightness[static_cast<std::size_t>(py)] = rowMax;
    }
    std::vector<int> peaks;
    for (int py = 0; py < image.height; ++py) {
        const int value = brightness[static_cast<std::size_t>(py)];
        if (value == 0) {
            continue;
        }
        const int above = py > 0 ? brightness[static_cast<std::size_t>(py) - 1] : 0;
        const int below = py + 1 < image.height ? brightness[static_cast<std::size_t>(py) + 1] : 0;
        if ((value >= above && value > below) || (value > above && value >= below)) {
            peaks.push_back(py);
        }
    }
    return peaks;
}

WaveformSettings settingsFor(WaveformMode mode)
{
    WaveformSettings settings;
    settings.mode = mode;
    return settings;
}

}  // namespace

TEST_CASE("Waveform in luma mode plots mid gray on one level")
{
    // Rec.709 luma of (128, 128, 128) is 128, which is row 255 - 128 = 127.
    TestFrame frame(32, 16);
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
    TestFrame frame(32, 16);
    frame.fill(Color{10, 150, 240});

    Waveform scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 32, 16});  // RGB is the default

    CHECK(peakRows(scope.image(), 0) == std::vector<int>{255 - 10});
    CHECK(peakRows(scope.image(), 1) == std::vector<int>{255 - 150});
    CHECK(peakRows(scope.image(), 2) == std::vector<int>{255 - 240});
}

TEST_CASE("Waveform combined mode adds a white luma trace over rgb")
{
    // Luma of (10, 150, 240) is (54*10 + 183*150 + 19*240) >> 8 = 127.
    TestFrame frame(32, 16);
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
    TestFrame frame(32, 16);
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
    TestFrame frame(64, 16);
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
    const auto point = scope.project(FloatColor{128.0f, 128.0f, 128.0f});
    REQUIRE(point.has_value());
    CHECK(point->x < 0.0f);  // horizontal position is the caller's problem
    CHECK(point->y > 0.49f);
    CHECK(point->y < 0.51f);
}

namespace {

uint8_t greenValueAt(const ScopeImage& image, int px, int py)
{
    return image.rgba[(static_cast<std::size_t>(py) * image.width + px) * 4 + 1];
}

}  // namespace

TEST_CASE("Waveform fills a missing level between populated neighbors")
{
    // The capture pipeline's 8-bit color conversion leaves "missing
    // codes": level values that almost never occur. Each rendered as a
    // dark line across the trace once the pane grew large. Alternating
    // columns of 100 and 102 leave 101 empty; the wide vertical kernel
    // must light it comparably to its populated neighbors.
    TestFrame frame(64, 64);
    for (int py = 0; py < 64; ++py) {
        const uint8_t value = py % 2 == 0 ? 100 : 102;
        frame.fillRows(py, py + 1, Color{value, value, value});
    }

    Waveform scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    const int populated = greenValueAt(scope.image(), 0, 255 - 100);
    const int gap = greenValueAt(scope.image(), 0, 255 - 101);
    REQUIRE(populated > 0);
    CHECK(gap >= populated * 3 / 4);
}

TEST_CASE("Waveform flattens the pipeline's code-density comb")
{
    // Display pipelines populate 8-bit codes unevenly: here every column
    // holds value 100 twice as often as 102, frame-wide - the doubled
    // code used to render as a brighter line across the whole trace.
    // The flat-field correction must bring the two rows together.
    TestFrame frame(64, 66);
    for (int py = 0; py < 66; ++py) {
        const uint8_t value = py % 3 == 2 ? 102 : 100;
        frame.fillRows(py, py + 1, Color{value, value, value});
    }

    Waveform scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 64, 66});

    const int doubled = greenValueAt(scope.image(), 0, 255 - 100);
    const int single = greenValueAt(scope.image(), 0, 255 - 102);
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
    TestFrame frame(64, 260);
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

    const int pileup = greenValueAt(scope.image(), 0, 255 - 100);
    const int neighbor = greenValueAt(scope.image(), 0, 255 - 97);
    REQUIRE(neighbor > 0);
    CHECK(pileup <= neighbor * 4 / 3);
}

TEST_CASE("Waveform keeps a real clipping line bright")
{
    // Crushed blacks concentrate at the very bottom of the populated
    // range with nothing below - a real feature the photographer must
    // see. The pileup correction may not touch it.
    TestFrame frame(64, 64);
    frame.fillRows(0, 32, Color{16, 16, 16});  // crushed shadows
    for (int py = 32; py < 64; ++py) {
        const uint8_t value = static_cast<uint8_t>(80 + py);
        frame.fillRows(py, py + 1, Color{value, value, value});
    }

    Waveform scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    const int clip = greenValueAt(scope.image(), 0, 255 - 16);
    const int body = greenValueAt(scope.image(), 0, 255 - 120);
    REQUIRE(body > 0);
    CHECK(clip > body);
}

TEST_CASE("Colored luma waveform tints the trace with the source color")
{
    // A solid 75% red frame: the trace must sit on red's luma level and
    // carry red's hue - density decides brightness, the value-weighted
    // planes decide only the color.
    TestFrame frame(64, 64);
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
    TestFrame frame(64, 64);
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
        const int value = greenValueAt(image, 0, row);
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
    TestFrame frame(64, 64);
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
    TestFrame frame(64, 64);
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
        const uint8_t expected = greenValueAt(reference.image(), 0, row);
        CHECK(expected > 0);
        CHECK(greenValueAt(strided.image(), 0, row) == expected);
        CHECK(greenValueAt(smaller.image(), 0, row) == expected);
    }
}

}  // namespace sidescopes
