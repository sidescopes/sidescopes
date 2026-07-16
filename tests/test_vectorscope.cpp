#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdlib>
#include <utility>
#include <vector>

#include "core/scopes/vectorscope.h"
#include "scope_image.h"
#include "test_frame.h"

namespace sidescopes {

using namespace test;

TEST_CASE("Vectorscope places 75% red on the classic BT.601 target")
{
    // 75% red (191, 0, 0) sits at Cb = 99.65, Cr = 211.56 under BT.601.
    // The bilinear splat peaks on the ROUNDED position - bin
    // (100, 255 - 212 = 43) - where integer truncation used to floor it
    // to (99, 44), half a bin away from where the projection puts the
    // markers.
    TestFrame frame(8, 8, 255);
    frame.fill(0, 8, Color{191, 0, 0});

    Vectorscope scope;
    VectorscopeSettings settings;
    settings.matrix = ChromaMatrix::Bt601;
    scope.configure(settings);
    scope.accumulate(frame.view(), IntRect{0, 0, 8, 8});

    CHECK(brightestPixel(scope.image()) == std::pair<int, int>{100, 43});
}

TEST_CASE("Vectorscope defaults to the BT.709 matrix")
{
    // Every HD-era scope measures with 709; with no configuration at all
    // 75% red must land on the 709 position (Cb 108.6 -> bin 109), not
    // the 601 one.
    TestFrame frame(8, 8, 255);
    frame.fill(0, 8, Color{191, 0, 0});

    Vectorscope scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 8, 8});

    CHECK(brightestPixel(scope.image()) == std::pair<int, int>{109, 43});
}

TEST_CASE("Vectorscope maps neutral gray to the center")
{
    TestFrame frame(8, 8, 255);
    frame.fill(0, 8, Color{128, 128, 128});

    Vectorscope scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 8, 8});

    CHECK(brightestPixel(scope.image()) == std::pair<int, int>{128, 127});
}

TEST_CASE("Vectorscope projection agrees with accumulation")
{
    Vectorscope scope;
    VectorscopeSettings settings;
    settings.matrix = ChromaMatrix::Bt601;
    scope.configure(settings);
    const auto point = scope.project(FloatColor{191.0f, 0.0f, 0.0f});
    REQUIRE(point.has_value());
    // Floating-point chroma for 75% red: Cb = 99.65, Cr = 211.56.
    CHECK(point->x == Catch::Approx(99.65 / 255.0).margin(0.005));
    CHECK(point->y == Catch::Approx((255.0 - 211.56) / 255.0).margin(0.005));
}

TEST_CASE("Vectorscope matrix selection moves chroma targets")
{
    TestFrame frame(8, 8, 255);
    frame.fill(0, 8, Color{191, 0, 0});

    Vectorscope scope;
    VectorscopeSettings settings;
    settings.matrix = ChromaMatrix::Bt709;
    scope.configure(settings);
    scope.accumulate(frame.view(), IntRect{0, 0, 8, 8});

    // BT.709: Cb = -26 * 191 / 256 + 128 = 108.6, rounding to bin 109;
    // Cr unchanged from BT.601 (both use 112), peaking at row 43.
    CHECK(brightestPixel(scope.image()) == std::pair<int, int>{109, 43});
}

TEST_CASE("Vectorscope carries real detail on a finer grid")
{
    // The fixed-point chroma transform holds more precision than the
    // classic 256 grid uses; the finer grid must place the same color on
    // the scaled coordinate rather than upscale the coarse one.
    TestFrame frame(32, 32, 255);
    frame.fill(0, 32, Color{191, 0, 0});

    Vectorscope scope;
    VectorscopeSettings settings;
    settings.matrix = ChromaMatrix::Bt601;
    settings.size = 512;
    scope.configure(settings);
    scope.accumulate(frame.view(), IntRect{0, 0, 32, 32});

    CHECK(scope.image().width == 512);
    const auto [px, py] = brightestPixel(scope.image());
    CHECK(px >= 197);
    CHECK(px <= 201);
    CHECK(py >= 86);
    CHECK(py <= 90);
}

TEST_CASE("Vectorscope leaves no gap between adjacent chroma codes on the fine grid")
{
    // 8-bit content quantizes chroma to whole codes, so neighboring
    // colors in a photograph sit one code apart - two pixels on the 512
    // image. Accumulating on a grid that fine renders the quantization
    // as gridded texture; the fine image must instead interpolate the
    // code grid, keeping the space between two equally-strong adjacent
    // codes as bright as the codes themselves.
    TestFrame frame(32, 32, 255);
    frame.fill(0, 16, Color{191, 0, 0});   // Cb 99.65 -> code 100
    frame.fill(16, 32, Color{191, 0, 2});  // Cb 100.52 -> code 101

    Vectorscope scope;
    VectorscopeSettings settings;
    settings.size = 512;
    // Linear response at unit gain keeps the peak below the bloom knee,
    // so the ratio below measures the interpolation alone.
    settings.response = TraceResponse::Linear;
    settings.gain = 1.0f;
    scope.configure(settings);
    scope.accumulate(frame.view(), IntRect{0, 0, 32, 32});

    const auto [px, py] = brightestPixel(scope.image());
    const auto brightness = [&](int x, int y) {
        const uint8_t* pixel = scope.image().rgba.data() + (static_cast<std::size_t>(y) * 512 + x) * 4;
        return static_cast<int>(pixel[0]) + pixel[1] + pixel[2];
    };
    // The two codes render as two nearby peaks; the space between them
    // must hold, not fall dark.
    const int peak = brightness(px, py);
    int secondX = px - 2;
    for (int x = px - 6; x <= px + 6; ++x) {
        if (std::abs(x - px) >= 2 && brightness(x, py) > brightness(secondX, py)) {
            secondX = x;
        }
    }
    const int second = brightness(secondX, py);
    int valley = peak;
    for (int x = std::min(px, secondX); x <= std::max(px, secondX); ++x) {
        valley = std::min(valley, brightness(x, py));
    }
    CHECK(second * 2 >= peak);
    CHECK(valley * 4 >= second * 3);
}

TEST_CASE("Vectorscope linear response keeps sparse mass faint")
{
    // 63 parts red to 1 part blue. The boosted log curve lifts the blue
    // speck into clear visibility; the phosphor-linear response must
    // leave it far dimmer than the dominant mass, the way a hardware
    // scope would.
    TestFrame frame(64, 64, 255);
    frame.fill(0, 63, Color{191, 0, 0});
    frame.fill(63, 64, Color{0, 0, 191});

    const auto brightnessAtBlue = [](TraceResponse response) {
        Vectorscope scope;
        VectorscopeSettings settings;
        settings.response = response;
        scope.configure(settings);
        TestFrame frame(64, 64, 255);
        frame.fill(0, 63, Color{191, 0, 0});
        frame.fill(63, 64, Color{0, 0, 191});
        scope.accumulate(frame.view(), IntRect{0, 0, 64, 64});
        // 75% blue under BT.709: Cb = 112 * 191 / 256 = 83.6 -> bin 212,
        // Cr = -10 * 191 / 256 = -7.5 -> row 255 - 121 = 134.
        const uint8_t* pixel = scope.image().rgba.data() + (static_cast<std::size_t>(134) * 256 + 212) * 4;
        return static_cast<int>(pixel[0]) + pixel[1] + pixel[2];
    };

    const int boosted = brightnessAtBlue(TraceResponse::Boosted);
    const int linear = brightnessAtBlue(TraceResponse::Linear);
    CHECK(boosted > 150);
    CHECK(linear * 3 < boosted);
}

TEST_CASE("Vectorscope blooms the densest mass toward white")
{
    // A solid color parks all mass on one spot; the phosphor bloom must
    // desaturate that core toward white while the same tint one code
    // away from saturation stays clearly colored.
    TestFrame frame(16, 16, 255);
    frame.fill(0, 16, Color{191, 0, 0});

    Vectorscope scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 16, 16});

    const auto [px, py] = brightestPixel(scope.image());
    const uint8_t* peak = scope.image().rgba.data() + (static_cast<std::size_t>(py) * 256 + px) * 4;
    const int strongest = std::max({peak[0], peak[1], peak[2]});
    const int weakest = std::min({peak[0], peak[1], peak[2]});
    CHECK(strongest >= 200);
    CHECK(weakest * 10 >= strongest * 7);  // near-white core
}

TEST_CASE("Vectorscope trace is invariant to the sampling stride")
{
    // Two colors in a 3:1 area ratio, chosen so the ratio is preserved under
    // stride-2 sampling. Per-sample density normalization must then produce
    // identical images at both strides.
    TestFrame frame(64, 64, 255);
    frame.fill(0, 48, Color{191, 0, 0});
    frame.fill(48, 64, Color{0, 0, 191});

    Vectorscope full;
    Vectorscope strided;
    VectorscopeSettings settings;
    settings.samplingStride = 2;
    strided.configure(settings);

    full.accumulate(frame.view(), IntRect{0, 0, 64, 64});
    strided.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    CHECK(full.image().rgba == strided.image().rgba);
}

TEST_CASE("Vectorscope produces a black image for an empty region")
{
    TestFrame frame(8, 8, 255);
    frame.fill(0, 8, Color{191, 0, 0});

    Vectorscope scope;
    scope.accumulate(frame.view(), IntRect{20, 20, 4, 4});  // outside the frame

    // Every color channel of every pixel must be dark, not just the red
    // byte the check used to read - a stray green or blue trace would slip
    // straight past a red-only scan.
    const std::vector<uint8_t>& rgba = scope.image().rgba;
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        REQUIRE(rgba[i] + rgba[i + 1] + rgba[i + 2] == 0);
    }
}

TEST_CASE("Vectorscope clamps the display size to the supported range")
{
    TestFrame frame(16, 16, 255);
    frame.fill(0, 16, Color{191, 0, 0});

    Vectorscope tooLarge;
    VectorscopeSettings large;
    large.size = 9999;
    tooLarge.configure(large);
    tooLarge.accumulate(frame.view(), IntRect{0, 0, 16, 16});
    CHECK(tooLarge.image().width == 512);
    CHECK(tooLarge.image().height == 512);

    Vectorscope tooSmall;
    VectorscopeSettings small;
    small.size = 10;
    tooSmall.configure(small);
    tooSmall.accumulate(frame.view(), IntRect{0, 0, 16, 16});
    CHECK(tooSmall.image().width == 256);
    CHECK(tooSmall.image().height == 256);
}

TEST_CASE("Vectorscope clamps an out-of-range sampling stride to eight")
{
    // Stride 99 is meaningless; it must behave exactly as the maximum stride
    // of 8. Two colors placed so stride-8 sampling still sees both give the
    // comparison something to bite on.
    TestFrame frame(64, 64, 255);
    frame.fill(0, 32, Color{191, 0, 0});
    frame.fill(32, 64, Color{0, 0, 191});

    Vectorscope clamped;
    VectorscopeSettings tooWide;
    tooWide.samplingStride = 99;
    clamped.configure(tooWide);
    clamped.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    Vectorscope maxStride;
    VectorscopeSettings eight;
    eight.samplingStride = 8;
    maxStride.configure(eight);
    maxStride.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    CHECK(clamped.image().rgba == maxStride.image().rgba);
}

}  // namespace sidescopes
