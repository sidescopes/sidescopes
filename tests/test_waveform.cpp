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
std::vector<int> LitRows(const ScopeImage& image, int channel) {
    std::vector<int> rows;
    for (int py = 0; py < image.height; ++py) {
        for (int px = 0; px < image.width; ++px) {
            const uint8_t* rgba =
                image.rgba.data() + (static_cast<std::size_t>(py) * image.width + px) * 4;
            if (rgba[channel] > 0) {
                rows.push_back(py);
                break;
            }
        }
    }
    return rows;
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
        CHECK(LitRows(scope.Image(), channel) == std::vector<int>{127});
    }
}

TEST_CASE("Waveform in rgb mode plots each channel at its own level") {
    TestFrame frame(32, 16);
    frame.Fill(Color{10, 150, 240});

    Waveform scope;
    scope.Accumulate(frame.View(), IntRect{0, 0, 32, 16});  // RGB is the default

    CHECK(LitRows(scope.Image(), 0) == std::vector<int>{255 - 10});
    CHECK(LitRows(scope.Image(), 1) == std::vector<int>{255 - 150});
    CHECK(LitRows(scope.Image(), 2) == std::vector<int>{255 - 240});
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
    CHECK(LitRows(scope.Image(), 0) == std::vector<int>{255 - 127, 255 - 10});
    CHECK(LitRows(scope.Image(), 1) == std::vector<int>{255 - 150, 255 - 127});
    CHECK(LitRows(scope.Image(), 2) == std::vector<int>{255 - 240, 255 - 127});
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
