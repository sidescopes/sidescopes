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

}  // namespace
}  // namespace sidescopes
