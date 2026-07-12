#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "core/scopes/waveform.h"

namespace sidescopes {
namespace {

struct TestFrame {
    explicit TestFrame(int width, int height) : width(width), height(height) {
        pixels.resize(static_cast<std::size_t>(width) * height * 4, 255);
    }

    void Fill(Color color) { FillRows(0, height, color); }

    void FillRows(int y0, int y1, Color color) {
        for (int py = y0; py < y1; ++py) {
            for (int px = 0; px < width; ++px) {
                uint8_t* p = pixels.data() + (static_cast<std::size_t>(py) * width + px) * 4;
                p[0] = color.b;
                p[1] = color.g;
                p[2] = color.r;
            }
        }
    }

    [[nodiscard]] FrameView View() const {
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
std::vector<int> PeakRows(const ScopeImage& image, int channel) {
    std::vector<int> brightness(static_cast<std::size_t>(image.height), 0);
    for (int py = 0; py < image.height; ++py) {
        int row_max = 0;
        for (int px = 0; px < image.width; ++px) {
            const uint8_t* rgba =
                image.rgba.data() + (static_cast<std::size_t>(py) * image.width + px) * 4;
            row_max = std::max(row_max, static_cast<int>(rgba[channel]));
        }
        brightness[static_cast<std::size_t>(py)] = row_max;
    }
    std::vector<int> peaks;
    for (int py = 0; py < image.height; ++py) {
        const int value = brightness[static_cast<std::size_t>(py)];
        if (value == 0) continue;
        const int above = py > 0 ? brightness[static_cast<std::size_t>(py) - 1] : 0;
        const int below = py + 1 < image.height ? brightness[static_cast<std::size_t>(py) + 1] : 0;
        if ((value >= above && value > below) || (value > above && value >= below))
            peaks.push_back(py);
    }
    return peaks;
}

WaveformSettings SettingsFor(WaveformMode mode) {
    WaveformSettings settings;
    settings.mode = mode;
    return settings;
}

}  // namespace

TEST_CASE("Waveform in luma mode plots mid gray on one level") {
    // Rec.709 luma of (128, 128, 128) is 128, which is row 255 - 128 = 127.
    TestFrame frame(32, 16);
    frame.Fill(Color{128, 128, 128});

    Waveform scope;
    scope.Configure(SettingsFor(WaveformMode::Luma));
    scope.Accumulate(frame.View(), IntRect{0, 0, 32, 16});

    for (int channel = 0; channel < 3; ++channel) {
        CHECK(PeakRows(scope.Image(), channel) == std::vector<int>{127});
    }
}

TEST_CASE("Waveform in rgb mode plots each channel at its own level") {
    TestFrame frame(32, 16);
    frame.Fill(Color{10, 150, 240});

    Waveform scope;
    scope.Accumulate(frame.View(), IntRect{0, 0, 32, 16});  // RGB is the default

    CHECK(PeakRows(scope.Image(), 0) == std::vector<int>{255 - 10});
    CHECK(PeakRows(scope.Image(), 1) == std::vector<int>{255 - 150});
    CHECK(PeakRows(scope.Image(), 2) == std::vector<int>{255 - 240});
}

TEST_CASE("Waveform combined mode adds a white luma trace over rgb") {
    // Luma of (10, 150, 240) is (54*10 + 183*150 + 19*240) >> 8 = 127.
    TestFrame frame(32, 16);
    frame.Fill(Color{10, 150, 240});

    Waveform scope;
    scope.Configure(SettingsFor(WaveformMode::RgbAndLuma));
    scope.Accumulate(frame.View(), IntRect{0, 0, 32, 16});

    // Rows are reported top-down: the luma trace (row 128) precedes deeper
    // channel levels and follows shallower ones.
    CHECK(PeakRows(scope.Image(), 0) == std::vector<int>{255 - 127, 255 - 10});
    CHECK(PeakRows(scope.Image(), 1) == std::vector<int>{255 - 150, 255 - 127});
    CHECK(PeakRows(scope.Image(), 2) == std::vector<int>{255 - 240, 255 - 127});
}

TEST_CASE("Waveform parade shows each channel in its own third") {
    // Uniform (10, 150, 240): each third lights one row at its channel's
    // level, in that channel's color only.
    TestFrame frame(32, 16);
    frame.Fill(Color{10, 150, 240});

    Waveform scope;
    scope.Configure(SettingsFor(WaveformMode::RgbParade));
    scope.Accumulate(frame.View(), IntRect{0, 0, 32, 16});

    constexpr int kThird = Waveform::kColumns / 3;
    const auto value_at = [&](int column, int row, int channel) {
        return scope.Image()
            .rgba[(static_cast<std::size_t>(row) * Waveform::kColumns + column) * 4 + channel];
    };
    // Red third: lit at row 255-10 in red, dark in green and blue.
    CHECK(value_at(kThird / 2, 255 - 10, 0) > 0);
    CHECK(value_at(kThird / 2, 255 - 10, 1) == 0);
    CHECK(value_at(kThird / 2, 255 - 150, 0) == 0);
    // Green third: lit at row 255-150 in green only.
    CHECK(value_at(kThird + kThird / 2, 255 - 150, 1) > 0);
    CHECK(value_at(kThird + kThird / 2, 255 - 150, 0) == 0);
    // Blue third: lit at row 255-240 in blue only.
    CHECK(value_at(2 * kThird + kThird / 2, 255 - 240, 2) > 0);
    CHECK(value_at(2 * kThird + kThird / 2, 255 - 240, 1) == 0);
}

TEST_CASE("Waveform parade preserves horizontal position within each third") {
    // Left half bright red, right half dark red: within the red third the
    // left local half sits at the bright level and the right at the dark.
    TestFrame frame(64, 16);
    frame.Fill(Color{50, 0, 0});
    for (int py = 0; py < 16; ++py) {
        for (int px = 0; px < 32; ++px) {
            uint8_t* p = frame.pixels.data() + (static_cast<std::size_t>(py) * 64 + px) * 4;
            p[2] = 200;
        }
    }

    Waveform scope;
    scope.Configure(SettingsFor(WaveformMode::RgbParade));
    scope.Accumulate(frame.View(), IntRect{0, 0, 64, 16});

    constexpr int kThird = Waveform::kColumns / 3;
    const auto red_at = [&](int column, int row) {
        return scope.Image()
            .rgba[(static_cast<std::size_t>(row) * Waveform::kColumns + column) * 4];
    };
    CHECK(red_at(kThird / 4, 255 - 200) > 0);  // left local half: bright level
    CHECK(red_at(kThird / 4, 255 - 50) == 0);
    CHECK(red_at(3 * kThird / 4, 255 - 50) > 0);  // right local half: dark level
    CHECK(red_at(3 * kThird / 4, 255 - 200) == 0);
}

TEST_CASE("Waveform projection reports the luma level") {
    Waveform scope;
    const auto point = scope.Project(FloatColor{128.0f, 128.0f, 128.0f});
    REQUIRE(point.has_value());
    CHECK(point->x < 0.0f);  // horizontal position is the caller's problem
    CHECK(point->y > 0.49f);
    CHECK(point->y < 0.51f);
}

namespace {

uint8_t GreenValueAt(const ScopeImage& image, int px, int py) {
    return image.rgba[(static_cast<std::size_t>(py) * image.width + px) * 4 + 1];
}

}  // namespace

TEST_CASE("Waveform fills a missing level between populated neighbors") {
    // The capture pipeline's 8-bit color conversion leaves "missing
    // codes": level values that almost never occur. Each rendered as a
    // dark line across the trace once the pane grew large. Alternating
    // columns of 100 and 102 leave 101 empty; the wide vertical kernel
    // must light it comparably to its populated neighbors.
    TestFrame frame(64, 64);
    for (int py = 0; py < 64; ++py) {
        const uint8_t value = py % 2 == 0 ? 100 : 102;
        frame.FillRows(py, py + 1, Color{value, value, value});
    }

    Waveform scope;
    scope.Accumulate(frame.View(), IntRect{0, 0, 64, 64});

    const int populated = GreenValueAt(scope.Image(), 0, 255 - 100);
    const int gap = GreenValueAt(scope.Image(), 0, 255 - 101);
    REQUIRE(populated > 0);
    CHECK(gap >= populated * 3 / 4);
}

TEST_CASE("Waveform flattens the pipeline's code-density comb") {
    // Display pipelines populate 8-bit codes unevenly: here every column
    // holds value 100 twice as often as 102, frame-wide - the doubled
    // code used to render as a brighter line across the whole trace.
    // The flat-field correction must bring the two rows together.
    TestFrame frame(64, 66);
    for (int py = 0; py < 66; ++py) {
        const uint8_t value = py % 3 == 2 ? 102 : 100;
        frame.FillRows(py, py + 1, Color{value, value, value});
    }

    Waveform scope;
    scope.Accumulate(frame.View(), IntRect{0, 0, 64, 66});

    const int doubled = GreenValueAt(scope.Image(), 0, 255 - 100);
    const int single = GreenValueAt(scope.Image(), 0, 255 - 102);
    REQUIRE(single > 0);
    CHECK(doubled <= single * 5 / 4);
    CHECK(doubled >= single * 3 / 4);
}

TEST_CASE("Waveform attenuates a display-pipeline pileup") {
    // Display-profile conversions pile many source codes onto one output
    // code and starve its neighbors - here level 100 carries twenty rows
    // for every one row of its neighbors, next to a hole at 102. A real
    // photo feature cannot look like this; the pileup must come down to
    // its neighborhood instead of rendering as a bright line.
    TestFrame frame(64, 260);
    for (int py = 0; py < 260; ++py) {
        uint8_t value = static_cast<uint8_t>(90 + (py % 26));
        if (py % 26 == 10) value = 100;  // extra mass onto 100
        if (value == 102) value = 100;   // and 102 is never emitted
        frame.FillRows(py, py + 1, Color{value, value, value});
    }

    Waveform scope;
    scope.Accumulate(frame.View(), IntRect{0, 0, 64, 260});

    const int pileup = GreenValueAt(scope.Image(), 0, 255 - 100);
    const int neighbor = GreenValueAt(scope.Image(), 0, 255 - 97);
    REQUIRE(neighbor > 0);
    CHECK(pileup <= neighbor * 4 / 3);
}

TEST_CASE("Waveform keeps a real clipping line bright") {
    // Crushed blacks concentrate at the very bottom of the populated
    // range with nothing below - a real feature the photographer must
    // see. The pileup correction may not touch it.
    TestFrame frame(64, 64);
    frame.FillRows(0, 32, Color{16, 16, 16});  // crushed shadows
    for (int py = 32; py < 64; ++py) {
        const uint8_t value = static_cast<uint8_t>(80 + py);
        frame.FillRows(py, py + 1, Color{value, value, value});
    }

    Waveform scope;
    scope.Accumulate(frame.View(), IntRect{0, 0, 64, 64});

    const int clip = GreenValueAt(scope.Image(), 0, 255 - 16);
    const int body = GreenValueAt(scope.Image(), 0, 255 - 120);
    REQUIRE(body > 0);
    CHECK(clip > body);
}

TEST_CASE("Waveform renders taller images through the level spline") {
    // Level data always has 256 codes; a taller image samples them
    // through a spline, and a single-level trace must peak at the scaled
    // position.
    TestFrame frame(64, 64);
    frame.Fill(Color{127, 127, 127});

    Waveform scope;
    WaveformSettings settings;
    settings.image_height = 512;
    scope.Configure(settings);
    scope.Accumulate(frame.View(), IntRect{0, 0, 64, 64});

    const ScopeImage& image = scope.Image();
    REQUIRE(image.height == 512);
    int peak_row = -1;
    int peak_value = 0;
    for (int row = 0; row < image.height; ++row) {
        const int value = GreenValueAt(image, 0, row);
        if (value > peak_value) {
            peak_value = value;
            peak_row = row;
        }
    }
    REQUIRE(peak_value > 0);
    CHECK(peak_row >= (255 - 127) * 2 - 2);
    CHECK(peak_row <= (255 - 127) * 2 + 3);
}

TEST_CASE("Waveform respects a narrower column budget") {
    TestFrame frame(64, 64);
    frame.Fill(Color{127, 127, 127});

    Waveform scope;
    WaveformSettings settings;
    settings.columns = 512;
    scope.Configure(settings);
    scope.Accumulate(frame.View(), IntRect{0, 0, 64, 64});

    CHECK(scope.Image().width == 512);
    CHECK(PeakRows(scope.Image(), 1) == std::vector<int>{255 - 127});
}

TEST_CASE("Waveform column brightness is invariant to stride and region size") {
    // Two gray levels stacked 3:1 vertically, so every sampled column sees
    // the same 3:1 level mix at stride 1, stride 2, and in a half-size
    // region. Per-row density normalization must then light the shared
    // column zero identically in all three runs.
    TestFrame frame(64, 64);
    frame.FillRows(0, 48, Color{191, 191, 191});
    frame.FillRows(48, 64, Color{64, 64, 64});

    Waveform reference;
    reference.Accumulate(frame.View(), IntRect{0, 0, 64, 64});

    Waveform strided;
    WaveformSettings settings;
    settings.sampling_stride = 2;
    strided.Configure(settings);
    strided.Accumulate(frame.View(), IntRect{0, 0, 64, 64});

    Waveform smaller;
    // Half the width, full height: the level mix in each column stays 3:1.
    smaller.Accumulate(frame.View(), IntRect{0, 0, 32, 64});

    for (const int row : {255 - 191, 255 - 64}) {
        const uint8_t expected = GreenValueAt(reference.Image(), 0, row);
        CHECK(expected > 0);
        CHECK(GreenValueAt(strided.Image(), 0, row) == expected);
        CHECK(GreenValueAt(smaller.Image(), 0, row) == expected);
    }
}

}  // namespace sidescopes
