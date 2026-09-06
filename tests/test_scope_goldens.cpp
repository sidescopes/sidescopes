// Golden hashes over each scope's rendered image, so the pixel decisions
// cannot drift unnoticed.
//
// The engines are pure CPU and bit-exact across platforms - the parallel
// rewrite is bit-identical by test - so these goldens are EXACT and shared by
// every OS. No threshold, no tolerance: a single changed byte fails.
//
// WHAT A FAILURE MEANS: the rendering changed. That is not automatically a
// bug - the goldens freeze current behaviour, they do not certify it - but the
// change must be deliberate and reviewed, not a side effect. To inspect it,
// build with the dump enabled and open the images:
//
//     SIDESCOPES_GOLDEN_DUMP=<dir> ctest -R "renderings match their goldens"
//
// which writes one binary PPM per case, then update the table below in the
// same commit as the change that moved them.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <share.h>
#endif

#include "core/scopes/histogram.h"
#include "core/scopes/vectorscope.h"
#include "core/scopes/waveform.h"
#include "scope_image.h"
#include "test_frame.h"

namespace sidescopes {

using namespace test;

namespace {

/// 75% color bars, the classic eight: white, yellow, cyan, green, magenta,
/// red, blue, black. Every vectorscope target and every waveform separation
/// shows up in one frame.
TestFrame colorBars()
{
    const Color bars[8] = {Color{191, 191, 191}, Color{191, 191, 0}, Color{0, 191, 191}, Color{0, 191, 0},
                           Color{191, 0, 191},   Color{191, 0, 0},   Color{0, 0, 191},   Color{0, 0, 0}};
    TestFrame frame(64, 16, 0);
    for (int bar = 0; bar < 8; ++bar) {
        frame.fill(bar * 8, (bar + 1) * 8, bars[bar]);
    }

    return frame;
}

/// A left-to-right ramp carrying a warm cast: every code from black to white,
/// with the channels separated.
///
/// The cast is load-bearing. On a neutral ramp the luma, RGB and colored-luma
/// waveforms render the SAME image - measured, all three hashed alike - so
/// three of the goldens could never tell those modes apart. Separating the
/// channels makes every case discriminating.
TestFrame tintedRamp()
{
    TestFrame frame(64, 16, 0);
    for (int column = 0; column < 64; ++column) {
        const int level = column * 4;
        frame.fill(column, column + 1,
                   Color{static_cast<uint8_t>(level), static_cast<uint8_t>(level * 4 / 5),
                         static_cast<uint8_t>(level * 3 / 5)});
    }

    return frame;
}

/// FNV-1a over the image's bytes and its dimensions, so a resize fails even if
/// the pixels happen to collide.
uint64_t hashImage(const ScopeImage& image)
{
    uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&hash](uint8_t byte) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    };
    for (const int dimension : {image.width, image.height}) {
        for (int shift = 0; shift < 32; shift += 8) {
            mix(static_cast<uint8_t>((dimension >> shift) & 0xFF));
        }
    }
    for (const uint8_t byte : image.rgba) {
        mix(byte);
    }

    return hash;
}

/// Writes the case as a binary PPM when SIDESCOPES_GOLDEN_DUMP names a
/// directory, so a moved golden can be looked at rather than guessed about.
void dumpIfRequested(const std::string& caseName, const ScopeImage& image)
{
#if defined(_MSC_VER)
    char* directory = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&directory, &size, "SIDESCOPES_GOLDEN_DUMP") != 0 || directory == nullptr) {
        return;
    }
#else
    const char* directory = std::getenv("SIDESCOPES_GOLDEN_DUMP");
    if (directory == nullptr) {
        return;
    }
#endif
    const std::string path = std::string(directory) + "/" + caseName + ".ppm";
    // The secure-CRT deprecations make std::fopen a hard error under /WX, the
    // same reason src/core/diagnostics.cpp opens its sink this way.
#ifdef _MSC_VER
    std::FILE* file = _fsopen(path.c_str(), "wb", _SH_DENYWR);
#else
    std::FILE* file = std::fopen(path.c_str(), "wb");
#endif
    if (file != nullptr) {
        std::fprintf(file, "P6\n%d %d\n255\n", image.width, image.height);
        for (std::size_t pixel = 0; pixel + 3 < image.rgba.size(); pixel += 4) {
            std::fwrite(image.rgba.data() + pixel, 1, 3, file);
        }
        std::fclose(file);
    }
#if defined(_MSC_VER)
    free(directory);
#endif
}

void checkGolden(const std::string& caseName, const ScopeImage& image, uint64_t expected)
{
    dumpIfRequested(caseName, image);
    INFO("golden case " << caseName << " hashed 0x" << std::hex << hashImage(image) << ", expected 0x" << expected);
    CHECK(hashImage(image) == expected);
}

}  // namespace

TEST_CASE("Vectorscope renderings match their goldens")
{
    TestFrame bars = colorBars();

    // The default case keeps the hash it was recorded with under the retired
    // Boosted response: making the gamma a setting left the shipped rendering
    // bit-for-bit as it was, and this is what says so.
    Vectorscope shipped;
    shipped.accumulate(bars.view(), IntRect{0, 0, 64, 16});
    checkGolden("vectorscope-bars-gamma-default", shipped.image(), 0xacede1806935413eULL);

    // Both ends of the gamma's range, recorded fresh. The case they replace
    // was the linear response, whose input this change deleted.
    Vectorscope lifted;
    VectorscopeSettings liftedSettings;
    liftedSettings.traceGamma = MinTraceGamma;
    lifted.configure(liftedSettings);
    lifted.accumulate(bars.view(), IntRect{0, 0, 64, 16});
    checkGolden("vectorscope-bars-gamma-min", lifted.image(), 0x628982161fd5228dULL);

    Vectorscope flat;
    VectorscopeSettings flatSettings;
    flatSettings.traceGamma = MaxTraceGamma;
    flat.configure(flatSettings);
    flat.accumulate(bars.view(), IntRect{0, 0, 64, 16});
    checkGolden("vectorscope-bars-gamma-max", flat.image(), 0x0e2773c5aa94569cULL);
}

TEST_CASE("Waveform renderings match their goldens")
{
    TestFrame bars = colorBars();
    TestFrame ramp = tintedRamp();
    const WaveformMode modes[4] = {WaveformMode::Luma, WaveformMode::Rgb, WaveformMode::RgbParade,
                                   WaveformMode::ColoredLuma};
    const char* names[4] = {"luma", "rgb", "parade", "colored-luma"};
    const uint64_t barsGoldens[4] = {0x1a05fc3e383ca8fbULL, 0xb1f61d61017a717bULL, 0x33b82dbeb4575136ULL,
                                     0x29fedbc4579c397bULL};
    const uint64_t rampGoldens[4] = {0xed9a923f6a4a2153ULL, 0x6fee693f0b671ac7ULL, 0xc497b08bb92809a2ULL,
                                     0x3a1cea61ee4f9158ULL};

    for (int mode = 0; mode < 4; ++mode) {
        WaveformSettings settings;
        settings.mode = modes[mode];

        Waveform onBars;
        onBars.configure(settings);
        onBars.accumulate(bars.view(), IntRect{0, 0, 64, 16});
        checkGolden(std::string("waveform-bars-") + names[mode], onBars.image(), barsGoldens[mode]);

        Waveform onRamp;
        onRamp.configure(settings);
        onRamp.accumulate(ramp.view(), IntRect{0, 0, 64, 16});
        checkGolden(std::string("waveform-ramp-") + names[mode], onRamp.image(), rampGoldens[mode]);
    }
}

TEST_CASE("Histogram renderings match their goldens")
{
    TestFrame ramp = tintedRamp();
    TestFrame bars = colorBars();

    Histogram onRamp;
    onRamp.accumulate(ramp.view(), IntRect{0, 0, 64, 16});
    checkGolden("histogram-ramp-per-channel", onRamp.image(), 0x120d54c45a86bd81ULL);

    Histogram onBars;
    onBars.accumulate(bars.view(), IntRect{0, 0, 64, 16});
    checkGolden("histogram-bars-per-channel", onBars.image(), 0xdbb1d3981074b0e2ULL);
}

}  // namespace sidescopes
