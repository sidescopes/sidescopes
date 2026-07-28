// Unit tests for the platform scaling policy (ui_scaling.cpp): the interface
// font's rasterizer density and the whole-UI scale factor. Both differ between
// macOS and Windows for a concrete reason, and both are pure math over window
// and framebuffer sizes, so these run on every platform and name each OS's real
// numbers.
//
// The interfaceFontDensity cases are the regression guard for b824e9d, which
// dropped the Retina supersample: the "Retina supersamples" case below fails
// against that commit and passes once the density is gated back in.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "app/ui_scaling.h"

namespace sidescopes {
namespace {

using Catch::Approx;

TEST_CASE("Interface font supersamples only where the framebuffer exceeds the window")
{
    // Retina: the 463 pt window backs onto a 926 px framebuffer, so the font
    // bakes at 2x and downsamples crisp. This is the case b824e9d regressed.
    CHECK(interfaceFontDensity(463, 926) == Approx(2.0f));

    // A 1.5x HiDPI panel supersamples by exactly its ratio, whatever it is.
    CHECK(interfaceFontDensity(800, 1200) == Approx(1.5f));

    // Windows sizes its window in physical pixels at any DPI, so framebuffer
    // and window match and there is nothing to supersample - baking larger than
    // the drawn box would only cost atlas memory. 125% and 150% both land here.
    CHECK(interfaceFontDensity(579, 579) == Approx(1.0f));
    CHECK(interfaceFontDensity(1000, 1000) == Approx(1.0f));

    // A standard-DPI macOS display (no Retina backing) is 1:1 as well.
    CHECK(interfaceFontDensity(800, 800) == Approx(1.0f));
}

TEST_CASE("Interface font density never drops below native")
{
    // A framebuffer smaller than the window would minify, not supersample; the
    // font must never bake below the box, so the policy floors at 1.0.
    CHECK(interfaceFontDensity(1000, 500) == Approx(1.0f));

    // Degenerate sizes fall back to the native default rather than dividing by
    // zero or returning nonsense.
    CHECK(interfaceFontDensity(0, 926) == Approx(1.0f));
    CHECK(interfaceFontDensity(-4, 100) == Approx(1.0f));
}

TEST_CASE("UI scale keeps the interface one physical size across platforms")
{
    // macOS: window coordinates are logical points and the framebuffer carries
    // the Retina factor, so the interface needs no scaling of its own.
    CHECK(uiScaleForWindow(2.0f, 463, 926) == Approx(1.0f));
    CHECK(uiScaleForWindow(1.0f, 800, 800) == Approx(1.0f));

    // Windows: the window is physical pixels (framebuffer ratio 1), so the
    // monitor's content scale is exactly what the interface must scale by.
    CHECK(uiScaleForWindow(1.25f, 579, 579) == Approx(1.25f));
    CHECK(uiScaleForWindow(1.5f, 1000, 1000) == Approx(1.5f));
}

TEST_CASE("UI scale degrades gracefully on empty windows")
{
    // Before the first real layout the window can report zero; returning the
    // content scale keeps the factor sane until the window is sized.
    CHECK(uiScaleForWindow(1.5f, 0, 0) == Approx(1.5f));
}

TEST_CASE("A stored interface-size factor snaps to an offered step")
{
    // The exact steps pass through untouched.
    for (const float step : UiScaleSteps) {
        CHECK(cleanedUiScaleFactor(step) == Approx(step));
    }
    // A value between steps snaps to the nearest.
    CHECK(cleanedUiScaleFactor(1.30f) == Approx(1.25f));
    CHECK(cleanedUiScaleFactor(1.40f) == Approx(1.50f));
    CHECK(cleanedUiScaleFactor(0.6f) == Approx(0.5f));
    // Above the range clamps to the largest step.
    CHECK(cleanedUiScaleFactor(4.0f) == Approx(2.0f));
    // Zero, negative, and NaN are not scales - they fall back to Default (1.0),
    // not to the smallest step.
    CHECK(cleanedUiScaleFactor(0.0f) == Approx(1.0f));
    CHECK(cleanedUiScaleFactor(-1.0f) == Approx(1.0f));
    CHECK(cleanedUiScaleFactor(std::nanf("")) == Approx(1.0f));
}

// The two panels below are measured, not imagined, and both are pinned with the
// numbers that were read off them:
//
//   a 14-inch MacBook Pro states a 1512 x 982 mode across a 301 x 195 mm panel
//   at content scale 2.0, which is 127.6 interface units an inch;
//   a 14-inch 1920x1080 ThinkPad states 1920 pixels across 310 mm, which is
//   157.3.
//
// The macOS mode width is in POINTS and its uiScaleForWindow is 1.0 - the
// framebuffer carries the Retina factor, not the interface - so 1512 and 1.0 go
// in together. Passing 3024, or 2.0, would be reading one platform's numbers in
// the other's units.
TEST_CASE("A display's density recommends the size a first run opens at")
{
    // The Retina panel at its 2x default is where the shipped size reads
    // correctly, so the app asks for nothing on top of it.
    CHECK(recommendedUiScaleFactor(1512, 301, 1.0f) == Approx(1.0f));

    // The same interface on the denser Windows panel, left at 100% scaling, is
    // a quarter smaller - which is exactly the step that answers it.
    CHECK(recommendedUiScaleFactor(1920, 310, 1.0f) == Approx(1.25f));

    // With Windows itself at 125% that panel is already at the reference, so
    // the recommendation divides the OS scale back out and asks for nothing.
    // Without that division the two would compound to 156%.
    CHECK(recommendedUiScaleFactor(1920, 310, 1.25f) == Approx(1.0f));

    // A standard-density desktop monitor - 1920 across 510 mm, 96 an inch - is
    // the case the interface is authored for.
    CHECK(recommendedUiScaleFactor(1920, 510, 1.0f) == Approx(1.0f));
}

// SYMPTOM IF BROKEN: two people on identical laptops get differently sized
// interfaces because one of them moved a Windows display setting - or the same
// person does, by changing it.
//
// This is the property that makes the derivation a panel measurement rather
// than a tuned constant, so it is asserted as a property and not as three
// values. The factor is a MULTIPLIER on the OS scale, so what the interface
// finally occupies is the product; deriving from physical density and dividing
// the OS scale back out makes that product depend on the panel alone, and the
// display setting cancels exactly.
TEST_CASE("The system's own scaling cancels out of the size that results")
{
    // The ThinkPad panel, at the setting Windows recommends for it and at the
    // one step below that its owner actually runs. Same panel, same product.
    const float atRecommended = 1.25f * recommendedUiScaleFactor(1920, 310, 1.25f);
    const float atOneStepDown = 1.0f * recommendedUiScaleFactor(1920, 310, 1.0f);
    CHECK(atRecommended == Approx(atOneStepDown));
    CHECK(atRecommended == Approx(1.25f));

    // Above the reference the app defers to the system instead of shrinking, so
    // the cancellation stops there by design: a panel the system already scales
    // past the reference keeps the larger interface its owner asked for.
    CHECK(1.5f * recommendedUiScaleFactor(1920, 310, 1.5f) == Approx(1.5f));
}

TEST_CASE("The recommended size never shrinks the interface")
{
    // Below the reference the interface is already as large as it was authored
    // to be; making it smaller is the size menu's business, never the display's.
    CHECK(recommendedUiScaleFactor(1366, 340, 1.0f) == Approx(1.0f));
    CHECK(recommendedUiScaleFactor(1920, 700, 1.0f) == Approx(1.0f));
}

TEST_CASE("The recommended size stops at one and a half")
{
    // A 13-inch 4K panel left unscaled reads 333 units an inch, which would ask
    // for 2.6. A panel that misstates its size can cost a step and a half of
    // chrome and no more - and that is recoverable from a menu the factor never
    // touches.
    CHECK(recommendedUiScaleFactor(3840, 293, 1.0f) == Approx(1.5f));
}

TEST_CASE("A display that cannot state its size gets no recommendation")
{
    // GLFW reports a zero physical size for a display whose EDID it could not
    // read, and a headless or virtual display can report a zero mode.
    CHECK(recommendedUiScaleFactor(1920, 0, 1.0f) == Approx(1.0f));
    CHECK(recommendedUiScaleFactor(0, 310, 1.0f) == Approx(1.0f));
    CHECK(recommendedUiScaleFactor(-1920, 310, 1.0f) == Approx(1.0f));
    CHECK(recommendedUiScaleFactor(1920, 310, 0.0f) == Approx(1.0f));
    CHECK(recommendedUiScaleFactor(1920, 310, std::nanf("")) == Approx(1.0f));

    // A stated width of two millimetres reads as 24000 units an inch. Without
    // the plausibility bound that is not rejected but CAPPED, so a panel with a
    // broken EDID would open the interface at 150% and look deliberate.
    CHECK(recommendedUiScaleFactor(1920, 2, 1.0f) == Approx(1.0f));
}

TEST_CASE("The reference interface density holds both measured panels")
{
    // Pinned because it came from a measurement rather than from arithmetic:
    // the Retina panel's own 127.6 units an inch, at the size its user reads
    // comfortably. The band that keeps both panels' answers is 114 to 139 - one
    // step boundary each side - so 128 has room, and moving it is deliberate.
    CHECK(ReferenceUiDensity == Approx(128.0f));
    CHECK(MaximumAutomaticUiScaleFactor == Approx(1.5f));

    // The band itself, stated as the answers it protects rather than as its
    // ends: a reference far below turns the Retina panel's 100% into 125%, and
    // one far above drops the Windows panel back to 100%.
    CHECK(127.6f / ReferenceUiDensity < 1.125f);
    CHECK(157.3f / ReferenceUiDensity >= 1.125f);
}

TEST_CASE("Every interface-size step is a visibly distinct font size")
{
    // ImGui rounds the baked font size, so the useful test of the step set is
    // that each one lands on a DIFFERENT integer size, strictly increasing -
    // the property a free slider or too-fine steps would break. This is why the
    // preference is discrete, and it is scale-independent of the row-seating
    // invariants, which round from the line height and hold at any factor.
    int previous = 0;
    for (const float step : UiScaleSteps) {
        const int baked = static_cast<int>(std::lround(InterfaceFontSize * step));
        CHECK(baked > previous);
        previous = baked;
    }
    // The sizes the current set produces, pinned so a step change is deliberate.
    CHECK(static_cast<int>(std::lround(InterfaceFontSize * 1.0f)) == 13);
    CHECK(static_cast<int>(std::lround(InterfaceFontSize * 2.0f)) == 26);
}

}  // namespace
}  // namespace sidescopes
