// The ten-bit capture path: the unpack, the level scale every scope shares,
// and the two guarantees that matter - an eight-bit frame is untouched, and a
// ten-bit frame really does reach positions an eight-bit one cannot.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/analysis_worker.h"
#include "core/frame.h"
#include "core/scopes/histogram.h"
#include "core/scopes/neutral.h"
#include "core/scopes/vectorscope.h"
#include "core/scopes/waveform.h"
#include "modules/module_frame.h"
#include "sidescopes/module.h"
#include "support/scope_image.h"
#include "support/test_frame.h"

using namespace sidescopes;
using namespace sidescopes::test;

namespace {

// The scope the crossing test reads, keyed the way the worker keys its
// settings and its output.
const std::string WaveformId = "org.sidescopes.waveform";

// The worker computes on a thread of its own, so its first output has to be
// waited for rather than assumed.
bool waitForOutput(const std::function<bool()>& condition)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return condition();
}

// The eight-bit frame carrying the same colour a ten-bit code rounds to, so
// the two can be compared where the content really is eight-bit.
Color roundedToEight(uint16_t r, uint16_t g, uint16_t b)
{
    const auto round = [](uint16_t code) { return static_cast<uint8_t>((code * 255 + 511) / 1023); };

    return Color{round(r), round(g), round(b)};
}

// How many bytes two scope images differ in, and -1 if they are not even the
// same shape. Compared this way rather than as whole vectors so that a failure
// prints a number: Catch2 stringifies a mismatched container, and a quarter of
// a megabyte of that is not a diagnosis.
int differingBytes(const std::vector<uint8_t>& left, const std::vector<uint8_t>& right)
{
    if (left.size() != right.size() || left.empty()) {
        return -1;
    }
    int differing = 0;
    for (std::size_t byte = 0; byte < left.size(); ++byte) {
        differing += left[byte] != right[byte] ? 1 : 0;
    }

    return differing;
}

std::vector<uint8_t> vectorscopeImageOf(const FrameView& frame)
{
    Vectorscope scope;
    scope.accumulate(frame, IntRect{0, 0, frame.width, frame.height});

    return scope.image().rgba;
}

}  // namespace

TEST_CASE("A ten-bit pixel unpacks from the layout the capture delivers")
{
    // Alpha 3, red 1023, green 512, blue 1 - each channel a different value so
    // a swapped pair cannot pass.
    TenBitTestFrame frame(1, 1);
    frame.setCodes(0, 0, 1023, 512, 1);

    const Sample sample = frame.view().sampleAt(0, 0);
    CHECK(sample.r == 1023);
    CHECK(sample.g == 512);
    CHECK(sample.b == 1);
    CHECK(frame.view().maxCode() == 1023);
}

TEST_CASE("An eight-bit frame reports the scale it always had")
{
    TestFrame frame(1, 1);
    frame.setColor(0, 0, Color{255, 128, 1});

    const Sample sample = frame.view().sampleAt(0, 0);
    CHECK(sample.r == 255);
    CHECK(sample.g == 128);
    CHECK(sample.b == 1);
    CHECK(frame.view().maxCode() == 255);
    CHECK(frame.view().format == PixelFormat::Bgra8);
}

TEST_CASE("The level scale leaves every eight-bit code exactly where it was")
{
    // The guarantee the whole change rests on: an eight-bit frame's arithmetic
    // is untouched, at every fraction any scope asks for. Anything else would
    // move the exact scope goldens.
    for (int code = 0; code <= 255; ++code) {
        CHECK(levelIn<Bgra8Pixels, 0>(code) == code);
        CHECK(levelIn<Bgra8Pixels, 2>(code) == code * 4);
        CHECK(levelIn<Bgra8Pixels, 4>(code) == code * 16);
    }
}

TEST_CASE("The level scale puts ten-bit black and white exactly on the ends")
{
    CHECK(levelIn<Argb2101010Pixels, 0>(0) == 0);
    CHECK(levelIn<Argb2101010Pixels, 0>(1023) == 255);
    CHECK(levelIn<Argb2101010Pixels, 4>(0) == 0);
    CHECK(levelIn<Argb2101010Pixels, 4>(1023) == 255 * 16);
}

TEST_CASE("The level scale rounds a ten-bit code rather than truncating it")
{
    // Truncation would bias every sample half a step toward black. Code 3 sits
    // at 0.7479 of a level, which rounds to 1 and truncates to 0.
    CHECK(levelIn<Argb2101010Pixels, 0>(3) == 1);
    // In sixteenths the same code is 11.97, which rounds to 12.
    CHECK(levelIn<Argb2101010Pixels, 4>(3) == 12);

    // Across the range, rounding is what keeps the mean error centred: the
    // eight-bit level of a ten-bit code is never more than half a level out.
    for (int code = 0; code <= 1023; ++code) {
        const double exact = code * 255.0 / 1023.0;
        CHECK(std::abs(levelIn<Argb2101010Pixels, 0>(code) - exact) <= 0.5);
    }
}

TEST_CASE("The two depths agree exactly on the codes they share")
{
    // The scales meet only where 1023 and 255 do: every 85th eight-bit code
    // against every 341st ten-bit one. There is no exact promotion in between -
    // 1023/255 is not a whole number - so those four values are the whole of
    // what "the same colour at both depths" can mean, and on them the two
    // paths must produce the identical image.
    //
    // That eight-bit frames are untouched generally is proven by the exact
    // scope goldens, which did not move.
    const uint8_t eightBit[] = {0, 85, 170, 255};
    const uint16_t tenBit[] = {0, 341, 682, 1023};
    for (int index = 0; index < 4; ++index) {
        TenBitTestFrame deep(8, 8);
        TestFrame shallow(8, 8, 255);
        deep.fill(tenBit[index], tenBit[3 - index], tenBit[1]);
        shallow.fill(Color{eightBit[index], eightBit[3 - index], eightBit[1]});

        CHECK(differingBytes(vectorscopeImageOf(deep.view()), vectorscopeImageOf(shallow.view())) == 0);
    }
}

TEST_CASE("A ten-bit frame reaches chroma positions an eight-bit frame cannot")
{
    // The feature itself. A gradient stepping ONE ten-bit code at a time is
    // finer than the eight-bit lattice, so the eight-bit rendering collapses
    // groups of columns onto one chroma position and the ten-bit one does not.
    //
    // Compared as images rather than as internals: if this passes while the
    // scopes still read bytes, the two would be identical.
    constexpr int Width = 64;
    TenBitTestFrame deep(Width, 8);
    TestFrame shallow(Width, 8, 255);
    for (int px = 0; px < Width; ++px) {
        const auto blue = static_cast<uint16_t>(400 + px);
        for (int py = 0; py < 8; ++py) {
            deep.setCodes(px, py, 500, 500, blue);
            const Color rounded = roundedToEight(500, 500, blue);
            shallow.setColor(px, py, rounded);
        }
    }

    CHECK(differingBytes(vectorscopeImageOf(deep.view()), vectorscopeImageOf(shallow.view())) > 0);
}

TEST_CASE("The neutral plane reads a ten-bit frame as the colour it is")
{
    // A correctness guard on the one path this change added to that scope, and
    // no more than that. Its accumulate now converts through the ten-bit
    // transfer table, so the cast it reports must agree with projecting the
    // same colour directly - two independent routes to one number, which a
    // misread pixel or the wrong table would separate.
    TenBitTestFrame frame(32, 32);
    frame.fill(600, 603, 610);
    Neutral scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 32, 32});

    const NormalizedPoint projected = scope.project(frame.view().srgbAt(0, 0));
    CHECK(scope.averagePoint().x == Catch::Approx(projected.x).margin(0.002));
    CHECK(scope.averagePoint().y == Catch::Approx(projected.y).margin(0.002));
}

TEST_CASE("A bin-bound scope reads a ten-bit frame as the level it rounds to")
{
    // The waveform and the histogram bin BY code, so they are bounded by their
    // bin count and not by the capture's depth. What must hold is that a
    // ten-bit frame lands on the same level its colour rounds to - no bias, no
    // drift - not that it resolves anything finer.
    TenBitTestFrame deep(32, 16);
    deep.fill(514, 514, 514);  // 128.1 of 255, so level 128
    Waveform waveform;
    waveform.accumulate(deep.view(), IntRect{0, 0, 32, 16});
    const ScopeImage& trace = waveform.image();
    CHECK(brightestRow(trace.rgba.data(), trace.width, trace.height, 0) == WaveformLevels - 1 - 128);

    Histogram histogram;
    histogram.accumulate(deep.view(), IntRect{0, 0, 32, 16});
    const std::vector<float>& outline = histogram.outlineHeights();
    const auto peak = static_cast<int>(
        std::distance(outline.begin(), std::max_element(outline.begin(), outline.begin() + Histogram::Bins)));
    CHECK(peak == 128);
}

TEST_CASE("A boundary frame's format picks the reader the host asked for")
{
    // The engine half of the crossing, in isolation. Every other test in this
    // file hands an engine a FrameView directly; in the running application a
    // scope only ever sees one built from an SsFrameView.
    SsFrameView boundary{};
    boundary.pixel_format = SS_PIXEL_FORMAT_ARGB2101010;
    CHECK(frameFromBoundary(boundary).format == PixelFormat::Argb2101010);

    boundary.pixel_format = SS_PIXEL_FORMAT_BGRA8;
    CHECK(frameFromBoundary(boundary).format == PixelFormat::Bgra8);

    // A host older than ABI minor 4 sends the field zeroed, and a host newer
    // than this engine may send a format it has never heard of. Both read as
    // the eight-bit layout every host sent before the field existed.
    boundary.pixel_format = 4242u;
    CHECK(frameFromBoundary(boundary).format == PixelFormat::Bgra8);
}

TEST_CASE("The pixel format survives the crossing into a scope")
{
    // THE GAP THIS CLOSES. A scope never reads a FrameView the host built; it
    // reads one rebuilt from an SsFrameView, across toBoundaryFrame ->
    // frameFromBoundary. Every SsFrameView in this suite used to carry
    // SS_PIXEL_FORMAT_BGRA8, so the engines were covered and the CROSSING was
    // not: inverting either ternary left the whole suite green while every
    // scope in the running application read ten-bit words as bytes.
    //
    // One buffer of bytes, published twice, differing only in its label -
    // which is what makes this a test of the label rather than of the pixels.
    // Ten-bit (616, 0, 0) packs to 0xE6800000, whose third byte is 0x80. So
    // the same word is red 616 of 1023 read packed, and red 128 of 255 read
    // flat: level 154 against level 128.
    //
    // It pins the two LEVELS rather than merely asserting the two labels
    // differ, and that distinction is the whole guard. Inverting a ternary
    // EXCHANGES the two readings, so a test that only demanded they differ
    // would pass against the very defect it exists to catch.
    constexpr uint16_t DeepRed = 616;
    constexpr int PackedLevel = 154;  // (616 * 255 + 511) / 1023
    constexpr int FlatLevel = 128;    // the third byte of the packed word
    constexpr int Columns = 64;

    const auto redTraceRow = [&](PixelFormat label) {
        FrameMailbox mailbox;
        AnalysisWorker worker(mailbox);
        AnalysisSettings settings;
        settings.region = RegionOfInterest{0.0, 0.0, 100.0, 100.0};
        settings.enabledScopes = {WaveformId};
        settings.imageSizes[WaveformId] = {Columns, WaveformLevels};
        worker.updateSettings(settings);
        worker.start();

        FrameBuffer frame = makeTenBitFrameBuffer(32, 16, DeepRed, 0, 0, 1);
        frame.format = label;
        mailbox.publish(std::move(frame));

        uint64_t seen = 0;
        AnalysisWorker::Output output;
        REQUIRE(waitForOutput([&] { return worker.fetchOutput(seen, output); }));
        const ScopeImage& trace = output.images.at(WaveformId);

        return brightestRow(trace.rgba.data(), trace.width, trace.height, 0);
    };

    // The waveform draws level 0 at the bottom row.
    CHECK(redTraceRow(PixelFormat::Argb2101010) == WaveformLevels - 1 - PackedLevel);
    CHECK(redTraceRow(PixelFormat::Bgra8) == WaveformLevels - 1 - FlatLevel);
}

TEST_CASE("The colour under the cursor keeps a ten-bit frame's precision")
{
    // The readout, the markers and a pin all take their colour here, on the
    // 0..255 display scale. A ten-bit frame must land BETWEEN levels rather
    // than snapping to one.
    TenBitTestFrame frame(1, 1);
    frame.setCodes(0, 0, 514, 512, 511);

    const FloatColor colour = frame.view().srgbAt(0, 0);
    CHECK(colour.r > 128.0f);
    CHECK(colour.r < 128.5f);
    CHECK(colour.g < 128.0f);
    CHECK(colour.g > 127.5f);
    // Two codes apart at ten bits is half a level, which a byte cannot hold.
    CHECK(colour.r - colour.b > 0.4f);
    CHECK(colour.r - colour.b < 0.9f);
}
