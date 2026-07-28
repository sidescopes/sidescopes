#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdlib>
#include <utility>
#include <vector>

#include "core/scopes/sampling.h"
#include "core/scopes/vectorscope.h"
#include "scope_image.h"
#include "test_frame.h"

namespace sidescopes {

using namespace test;

TEST_CASE("Vectorscope places 75% red on its BT.709 target")
{
    // BT.709: Cb = -26 * 191 / 256 + 128 = 108.6, Cr = 211.56. The bilinear
    // splat peaks on the ROUNDED position - bin (109, 255 - 212 = 43) - where
    // integer truncation used to floor it half a bin away from where the
    // projection puts the markers.
    TestFrame frame(8, 8, 255);
    frame.fill(0, 8, Color{191, 0, 0});

    Vectorscope scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 8, 8});

    CHECK(brightestPixel(scope.image()) == std::pair<int, int>{109, 43});
}

TEST_CASE("Vectorscope folds a tall region across threads onto the same bin")
{
    // A region tall enough to split across worker threads must land 75% red on
    // the same bin the single-threaded 8x8 case does: each thread scatters into
    // its own code grid, and integer addition merges them order-independently.
    TestFrame frame(8, 1024, 255);
    frame.fill(0, 8, Color{191, 0, 0});

    Vectorscope scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 8, 1024});

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
    const NormalizedPoint point = scope.project(FloatColor{191.0f, 0.0f, 0.0f});
    // Floating-point BT.709 chroma for 75% red: Cb = 108.60, Cr = 211.56.
    CHECK(point.x == Catch::Approx(108.60 / 255.0).margin(0.005));
    CHECK(point.y == Catch::Approx((255.0 - 211.56) / 255.0).margin(0.005));
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
    settings.size = 512;
    scope.configure(settings);
    scope.accumulate(frame.view(), IntRect{0, 0, 32, 32});

    // BT.709 puts 75% red at bin (109, 43) on the 256 grid, so the 512 grid
    // must place it near twice that rather than at a magnified 256 bin.
    CHECK(scope.image().width == 512);
    const auto [px, py] = brightestPixel(scope.image());
    CHECK(px >= 216);
    CHECK(px <= 220);
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
    // The two codes carry equal mass, so both reach the ceiling and bloom
    // alike; what the ratios below measure is the interpolation between them.
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

namespace {

/// 75% blue under BT.709: Cb = 112 * 191 / 256 = 83.6 -> bin 212, Cr =
/// -10 * 191 / 256 = -7.5 -> row 255 - 121 = 134. Read as the sum of the
/// three channels, the way every brightness comparison here does.
int brightnessAtBlue(const ScopeImage& image)
{
    const uint8_t* pixel = image.rgba.data() + (static_cast<std::size_t>(134) * 256 + 212) * 4;

    return static_cast<int>(pixel[0]) + pixel[1] + pixel[2];
}

/// A photograph's chroma distribution in SHAPE rather than in content: a dense
/// near-neutral body, a moderate mid-saturation ring, and a thin saturated
/// tail, which is what a picture looks like on this scope. Deterministic - the
/// counter below is a fixed sequence, not a random one - so the percentiles
/// the range test reads mean the same thing on every runner.
TestFrame photographicChroma()
{
    constexpr int Side = 512;
    TestFrame frame(Side, Side, 0);
    uint32_t state = 12345u;
    const auto next = [&state] {
        state = state * 1664525u + 1013904223u;

        return state;
    };
    const auto clamp8 = [](int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); };
    for (int y = 0; y < Side; ++y) {
        for (int x = 0; x < Side; ++x) {
            const uint32_t roll = next() % 1000u;
            const int luma = 40 + static_cast<int>(next() % 180u);
            const int spread = roll < 700u ? 13 : (roll < 950u ? 61 : 161);
            const int red = static_cast<int>(next() % static_cast<uint32_t>(spread)) - spread / 2;
            const int blue = static_cast<int>(next() % static_cast<uint32_t>(spread)) - spread / 2;
            frame.setColor(x, y, Color{clamp8(luma + red), clamp8(luma), clamp8(luma + blue)});
        }
    }

    return frame;
}

/// The lit pixels' peak channels, sorted, so a percentile of the trace's
/// brightness can be read off it.
std::vector<int> litBrightnesses(const ScopeImage& image)
{
    std::vector<int> lit;
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        const int peak = std::max({image.rgba[i], image.rgba[i + 1], image.rgba[i + 2]});
        if (peak > 0) {
            lit.push_back(peak);
        }
    }
    std::sort(lit.begin(), lit.end());

    return lit;
}

}  // namespace

TEST_CASE("Vectorscope trace gamma is the one thing that lifts sparse mass")
{
    // 63 parts red to 1 part blue: the blue speck sits far below the peak the
    // trace normalizes to, which is exactly where the gamma acts. Gamma is the
    // ONLY setting that moves between these three passes - same frame, same
    // gain, same stride, same size - so the brightness it reads is the gamma's
    // and nothing else's.
    const auto blueAtGamma = [](float gamma) {
        TestFrame frame(64, 64, 255);
        frame.fill(0, 63, Color{191, 0, 0});
        frame.fill(63, 64, Color{0, 0, 191});

        Vectorscope scope;
        VectorscopeSettings settings;
        settings.traceGamma = gamma;
        scope.configure(settings);
        scope.accumulate(frame.view(), IntRect{0, 0, 64, 64});

        return brightnessAtBlue(scope.image());
    };

    const int lifted = blueAtGamma(MinTraceGamma);
    const int shipped = blueAtGamma(MidDensityGamma);
    const int flat = blueAtGamma(MaxTraceGamma);
    // Lower gamma lifts the sparse trace towards the peak, higher leaves it
    // nearer its own evidence, and the shipped default sits between them.
    CHECK(lifted > shipped);
    CHECK(shipped > flat);
    // The default keeps a speck this sparse plainly visible; that is what the
    // log curve and this lift exist for.
    CHECK(shipped > 150);
}

TEST_CASE("Vectorscope defaults to the lift the waveform makes")
{
    // The default is not a number of its own: it is the fixed mid-density
    // gamma the waveform applies, so an untouched vectorscope and the waveform
    // beside it read the same density the same way. Moving it here would move
    // the scope the owner has used every day, which is the one thing this
    // control must not do on its own.
    CHECK(VectorscopeSettings{}.traceGamma == MidDensityGamma);

    // ...and configure leaves it alone, rather than clamping it to an end.
    Vectorscope scope;
    VectorscopeSettings settings;
    scope.configure(settings);
    TestFrame frame(64, 64, 255);
    frame.fill(0, 63, Color{191, 0, 0});
    frame.fill(63, 64, Color{0, 0, 191});
    scope.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    Vectorscope explicitly;
    VectorscopeSettings pinned;
    pinned.traceGamma = MidDensityGamma;
    explicitly.configure(pinned);
    explicitly.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    CHECK(scope.image().rgba == explicitly.image().rgba);
}

TEST_CASE("Vectorscope clamps the trace gamma to its measured range")
{
    // The range ends are a display decision, so a value past either end must
    // behave exactly as that end rather than render a curve nobody has looked
    // at.
    TestFrame frame(64, 64, 255);
    frame.fill(0, 63, Color{191, 0, 0});
    frame.fill(63, 64, Color{0, 0, 191});

    const auto imageAtGamma = [&frame](float gamma) {
        Vectorscope scope;
        VectorscopeSettings settings;
        settings.traceGamma = gamma;
        scope.configure(settings);
        scope.accumulate(frame.view(), IntRect{0, 0, 64, 64});

        return scope.image().rgba;
    };

    CHECK(imageAtGamma(0.01f) == imageAtGamma(MinTraceGamma));
    CHECK(imageAtGamma(9.0f) == imageAtGamma(MaxTraceGamma));
    // Not a clamp to one constant: the two ends are different images.
    CHECK(imageAtGamma(MinTraceGamma) != imageAtGamma(MaxTraceGamma));
}

TEST_CASE("Both ends of the trace gamma still draw a usable trace")
{
    // What makes an end an end, measured on a photograph-shaped chroma
    // distribution. Each end fails in its own way, so each is pinned by the
    // measure it fails: the lifted end flattens the cloud towards one
    // brightness, the flat end drops its body into the dark.
    TestFrame frame = photographicChroma();
    const auto litAtGamma = [&frame](float gamma) {
        Vectorscope scope;
        VectorscopeSettings settings;
        settings.traceGamma = gamma;
        scope.configure(settings);
        scope.accumulate(frame.view(), IntRect{0, 0, 512, 512});

        return litBrightnesses(scope.image());
    };
    const auto percentile = [](const std::vector<int>& sorted, std::size_t tenths) {
        REQUIRE_FALSE(sorted.empty());

        return sorted[sorted.size() * tenths / 10];
    };

    // The lifted end still separates dense trace from sparse: measured, the
    // ninetieth percentile is 2.2x the tenth (152 against 68), where the
    // shipped default is 3.1x. Lowering the end to 0.2 collapses it to 1.6x -
    // a cloud that says nothing about where the mass is.
    const std::vector<int> lifted = litAtGamma(MinTraceGamma);
    CHECK(percentile(lifted, 9) >= 2 * percentile(lifted, 1));

    // The flat end still draws a trace rather than a few bright specks:
    // measured, the median lit pixel is 40 of 255 and 5% of the trace falls
    // under 8. At 2.0 the median is 19, which on screen is nearly nothing.
    const std::vector<int> flat = litAtGamma(MaxTraceGamma);
    CHECK(percentile(flat, 5) >= 32);
    const auto barelyLit = [](int brightness) { return brightness < 8; };
    const std::size_t faint = static_cast<std::size_t>(std::count_if(flat.begin(), flat.end(), barelyLit));
    CHECK(faint * 100 <= flat.size() * 6);

    // And the default sits inside both, which is the point of the range.
    const std::vector<int> shipped = litAtGamma(MidDensityGamma);
    CHECK(percentile(shipped, 9) >= 2 * percentile(shipped, 1));
    CHECK(percentile(shipped, 5) >= 32);
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

TEST_CASE("The vectorscope thins to its own budget, not the global ceiling")
{
    // Its bins are the fixed 256x256 code grid, so like the histogram its budget
    // does not grow with the region and sits below the global ceiling. Reverting
    // that wiring changes nothing observable either - only the cost - so this is
    // what stands between the saving and a silent regression.
    constexpr int Width = 3000;
    constexpr int Height = 1500;
    const IntRect region{0, 0, Width, Height};
    const long long own = budgetForBins(256LL * 256, VectorscopeMinSamplesPerBin);
    const SampleGrid mine = sampleGridFor(1, region, own);
    const SampleGrid global = sampleGridFor(1, region, SampleBudget);
    REQUIRE(mine.rowStride > global.rowStride);

    // Neutral grey on exactly the rows its own budget visits, saturated red on
    // the rest. Grey carries no chroma and lands at the centre; any red at all
    // lands far out towards the red target.
    TestFrame frame(Width, Height, 0);
    frame.fill(Color{220, 20, 20});
    for (int index = 0; index < mine.rows; ++index) {
        const int row = sampleRowOf(mine, region, index);
        frame.fillRows(row, row + 1, Color{128, 128, 128});
    }

    Vectorscope vectorscope;
    vectorscope.configure(VectorscopeSettings{});
    vectorscope.accumulate(frame.view(), region);

    const ScopeImage& image = vectorscope.image();
    const int centre = image.width / 2;
    CHECK(pixelLit(image, centre, centre));

    // Every lit pixel anywhere in the image, not four sampled directions: red
    // lands at its own angle, and probing the cardinals alone misses it.
    int furthest = 0;
    for (int py = 0; py < image.height; ++py) {
        for (int px = 0; px < image.width; ++px) {
            if (pixelLit(image, px, py)) {
                const int dx = px - centre;
                const int dy = py - centre;
                furthest = std::max(furthest, dx * dx + dy * dy);
            }
        }
    }
    // Grey alone stays at the centre, spread only by the splat and the
    // reconstruction. Reading the red rows too would light a bin a third of the
    // way out towards the red target.
    const int allowed = image.width / 16;
    CHECK(furthest < allowed * allowed);
}

}  // namespace sidescopes
