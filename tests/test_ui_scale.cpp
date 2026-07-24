// Unit tests for the interface-scale controller (ui_scale.cpp): the state that
// folds the user's size step onto the monitor's own scale and drives the style.
// The controller reads the OS scale and applies the result to ImGui, so both
// sides are injected here as seams - a probe returning a fixed monitor scale and
// a sink recording what lands - which lets the fold, the range guard, and the
// no-op-when-unchanged rule run with no window and no ImGui context.

#include <catch2/catch_test_macros.hpp>

#include "app/ui_scale.h"
#include "app/ui_scaling.h"

using namespace sidescopes;

namespace {

// A controller wired to a settable monitor scale and a sink that records the
// last folded scale it was handed. `applied`/`sinkCalls` are how a test observes
// that a change actually reached the style.
struct Harness
{
    float probed = 1.0f;
    float applied = -1.0f;
    int sinkCalls = 0;
    UiScaleController controller{[this](GLFWwindow*) { return probed; },
                                 [this](float scale) {
                                     applied = scale;
                                     ++sinkCalls;
                                 }};
};

}  // namespace

TEST_CASE("UiScaleController starts at the system scale and applies nothing")
{
    Harness h;
    CHECK(h.controller.scale() == 1.0f);
    CHECK(h.controller.userFactor() == 1.0f);
    CHECK(h.sinkCalls == 0);
}

TEST_CASE("UiScaleController folds the chosen step onto the OS scale")
{
    Harness h;
    h.probed = 1.25f;                     // the monitor's own recommendation
    h.controller.selectStep(4, nullptr);  // index 4 is the 1.5x step
    CHECK(h.controller.userFactor() == 1.5f);
    CHECK(h.controller.scale() == 1.25f * 1.5f);
    CHECK(h.applied == 1.25f * 1.5f);
}

TEST_CASE("UiScaleController ignores a step outside the offered range")
{
    Harness h;
    h.controller.selectStep(-1, nullptr);
    h.controller.selectStep(static_cast<int>(UiScaleSteps.size()), nullptr);
    CHECK(h.controller.userFactor() == 1.0f);
    CHECK(h.controller.scale() == 1.0f);
    CHECK(h.sinkCalls == 0);
}

TEST_CASE("UiScaleController applies a refresh once and then no-ops")
{
    Harness h;
    h.probed = 2.0f;
    CHECK(h.controller.refresh(nullptr));  // 1.0 -> 2.0, a real change
    CHECK(h.controller.scale() == 2.0f);
    CHECK(h.sinkCalls == 1);
    CHECK_FALSE(h.controller.refresh(nullptr));  // already there, nothing to apply
    CHECK(h.sinkCalls == 1);
}

TEST_CASE("UiScaleController restore adopts an offered saved factor")
{
    Harness h;
    h.controller.restore(0.5f, nullptr);  // 0.5 is itself a step, taken as-is
    CHECK(h.controller.userFactor() == 0.5f);
    CHECK(h.controller.scale() == 0.5f);
    CHECK(h.applied == 0.5f);
}

TEST_CASE("UiScaleController carries the factor through a monitor change")
{
    Harness h;
    h.controller.selectStep(3, nullptr);  // 1.25x, on a 1.0x monitor
    REQUIRE(h.controller.scale() == 1.25f);

    h.probed = 2.0f;  // the window moves to a 2x display
    CHECK(h.controller.refresh(nullptr));
    CHECK(h.controller.scale() == 2.0f * 1.25f);  // the size preference rides along
    CHECK(h.controller.userFactor() == 1.25f);
}
