#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <new>
#include <thread>
#include <vector>

#include "core/scopes/sampling.h"
#include "core/scopes/waveform.h"
#include "core/scopes/waveform_bins.h"
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

TEST_CASE("An RGB and a luma waveform settle into one scatter a frame")
{
    // The two ask for different planes, so the first frame costs two passes -
    // neither can answer the other, and the bins cannot see a stack. From the
    // second on, ONE pass writes all four planes because that is what the
    // family asked for over the frame before, and the second scope finds its
    // answer already there.
    const TestFrame frame = texturedFrame();
    WaveformBins shared;
    Waveform overlay;
    Waveform luma;
    overlay.lendBins(&shared);
    luma.lendBins(&shared);
    luma.configure(settingsFor(WaveformMode::Luma));

    FrameView view = frame.view();
    overlay.accumulate(view, IntRect{0, 0, 200, 120});
    luma.accumulate(view, IntRect{0, 0, 200, 120});
    CHECK(shared.scatters() == 2);

    // Every frame after it costs ONE, and goes on costing one: a scope whose
    // request the leading pass already met still wants those planes, so what
    // the frame asked for has to be recorded whether or not it did any work.
    // Four frames rather than two, because a stack that forgets an answered
    // request oscillates between one pass and two rather than settling.
    for (uint64_t sequence = 2; sequence <= 4; ++sequence) {
        view.sequence = sequence;
        overlay.accumulate(view, IntRect{0, 0, 200, 120});
        luma.accumulate(view, IntRect{0, 0, 200, 120});
    }
    CHECK(shared.scatters() == 5);
    CHECK(shared.writtenSpan() == WaveformPlaneSpan{0, 4});

    // And either scope may lead it: the pass is decided by what the family
    // asked for, not by which of them runs first.
    view.sequence = 5;
    luma.accumulate(view, IntRect{0, 0, 200, 120});
    overlay.accumulate(view, IntRect{0, 0, 200, 120});
    CHECK(shared.scatters() == 6);
}

TEST_CASE("A widened scatter draws exactly what an unwidened one draws")
{
    // The condition on the widening: writing more planes than a scope reads
    // must leave the ones it does read bit-identical. The RGB splat and the
    // luma splat write different planes and neither sees the other, which is
    // what makes that true - and what this pins.
    const TestFrame frame = texturedFrame();
    const std::vector<uint8_t> overlayAlone = unsharedImage(frame, WaveformMode::Rgb);
    const std::vector<uint8_t> lumaAlone = unsharedImage(frame, WaveformMode::Luma);

    WaveformBins shared;
    Waveform overlay;
    Waveform luma;
    overlay.lendBins(&shared);
    luma.lendBins(&shared);
    luma.configure(settingsFor(WaveformMode::Luma));

    FrameView view = frame.view();
    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
        view.sequence = sequence;
        overlay.accumulate(view, IntRect{0, 0, 200, 120});
        luma.accumulate(view, IntRect{0, 0, 200, 120});
    }

    // The last frame ran as one widened pass, and both images are what each
    // scope draws reading bins of its own.
    REQUIRE(shared.writtenSpan() == WaveformPlaneSpan{0, 4});
    CHECK(overlay.image().rgba == overlayAlone);
    CHECK(luma.image().rgba == lumaAlone);
}

TEST_CASE("A colored luma pass is never widened into")
{
    // Colored luma fills the three channel planes with value-weighted mass
    // where every other mode puts counts at each channel's own. No pass may be
    // widened to cover both: the numbers are different measurements, so one
    // scatter can never answer for the two of them however many planes it
    // writes.
    const TestFrame frame = texturedFrame();
    const std::vector<uint8_t> overlayAlone = unsharedImage(frame, WaveformMode::Rgb);
    const std::vector<uint8_t> coloredAlone = unsharedImage(frame, WaveformMode::ColoredLuma);

    WaveformBins shared;
    Waveform overlay;
    Waveform colored;
    overlay.lendBins(&shared);
    colored.lendBins(&shared);
    colored.configure(settingsFor(WaveformMode::ColoredLuma));

    FrameView view = frame.view();
    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
        view.sequence = sequence;
        overlay.accumulate(view, IntRect{0, 0, 200, 120});
        colored.accumulate(view, IntRect{0, 0, 200, 120});
    }

    CHECK(shared.scatters() == 6);
    CHECK(overlay.image().rgba == overlayAlone);
    CHECK(colored.image().rgba == coloredAlone);
}

TEST_CASE("A colored luma scope does not widen the plain passes")
{
    // Nothing a plain pass writes can answer a colored-luma one, so its
    // request must not enter what the plain passes widen to: doing so would
    // buy a plane of bin traffic every frame for a scope that cannot read it.
    const TestFrame frame = texturedFrame();
    WaveformBins shared;
    Waveform overlay;
    Waveform colored;
    overlay.lendBins(&shared);
    colored.lendBins(&shared);
    colored.configure(settingsFor(WaveformMode::ColoredLuma));

    FrameView view = frame.view();
    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
        view.sequence = sequence;
        colored.accumulate(view, IntRect{0, 0, 200, 120});
        overlay.accumulate(view, IntRect{0, 0, 200, 120});
    }

    // The overlay ran last, so what stands in the bins is its own pass.
    CHECK(shared.writtenSpan() == WaveformPlaneSpan{0, 3});
}

TEST_CASE("A pass narrows again once a scope stops asking")
{
    // The widening is a prediction from the frame before, so it has to decay:
    // a stack that loses its luma waveform must stop paying for the luma plane
    // rather than writing it forever.
    const TestFrame frame = texturedFrame();
    WaveformBins shared;
    Waveform overlay;
    Waveform luma;
    overlay.lendBins(&shared);
    luma.lendBins(&shared);
    luma.configure(settingsFor(WaveformMode::Luma));

    FrameView view = frame.view();
    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
        view.sequence = sequence;
        overlay.accumulate(view, IntRect{0, 0, 200, 120});
        luma.accumulate(view, IntRect{0, 0, 200, 120});
    }
    REQUIRE(shared.writtenSpan() == WaveformPlaneSpan{0, 4});

    // The luma waveform is taken off screen; the passes after it narrow.
    for (uint64_t sequence = 4; sequence <= 6; ++sequence) {
        view.sequence = sequence;
        overlay.accumulate(view, IntRect{0, 0, 200, 120});
    }

    CHECK(shared.writtenSpan() == WaveformPlaneSpan{0, 3});
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

TEST_CASE("A waveform scatter retries after its scratch allocation fails")
{
    if (std::thread::hardware_concurrency() < 2) {
        SKIP("A single-threaded scatter does not borrow chunk scratch");
    }

    TestFrame frame(64, 256, 0);
    frame.fill(Color{30, 80, 160});
    FrameView view = frame.view();
    const IntRect region{0, 0, frame.width, frame.height};
    const SampleGrid grid = sampleGridFor(1, region, SampleBudget);
    constexpr int Columns = 256;
    constexpr WaveformMode Mode = WaveformMode::RgbAndLuma;

    bool allocationFails = false;
    WaveformBins bins;
    bins.lendScratch(
        [](const void* context, std::size_t) -> uint32_t* {
            if (*static_cast<const bool*>(context)) {
                throw std::bad_alloc{};
            }
            return nullptr;  // The ordinary owned scratch supplies successful passes.
        },
        &allocationFails);
    bins.scatter(view, region, grid, Mode, Columns);
    REQUIRE(bins.scatters() == 1);

    // A new frame must invalidate the old answer even when acquiring its
    // scratch fails. Retrying the same request must perform a complete pass.
    frame.fill(Color{100, 150, 200});
    ++view.sequence;
    allocationFails = true;
    REQUIRE_THROWS_AS(bins.scatter(view, region, grid, Mode, Columns), std::bad_alloc);
    CHECK(bins.scatters() == 1);
    CHECK(bins.writtenSpan() == WaveformPlaneSpan{0, 0});

    allocationFails = false;
    bins.scatter(view, region, grid, Mode, Columns);
    REQUIRE(bins.scatters() == 2);
    CHECK(bins.writtenSpan() == WaveformPlaneSpan{0, 4});

    WaveformBins fresh;
    fresh.scatter(view, region, grid, Mode, Columns);
    CHECK(std::vector<uint32_t>(bins.data(), bins.data() + bins.size()) ==
          std::vector<uint32_t>(fresh.data(), fresh.data() + fresh.size()));
    bins.scatter(view, region, grid, Mode, Columns);
    CHECK(bins.scatters() == 2);
}

}  // namespace sidescopes
