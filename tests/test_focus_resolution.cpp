#include <catch2/catch_test_macros.hpp>

#include "platform/focus_resolution.h"

namespace sidescopes {

namespace {

// The desk this suite works at: an editor application (pid 100) with its
// main window, plus assorted neighbours. Every scenario is a real one
// the Finder Quick Look preview taught us the hard way.
OrderedWindow window(uint64_t identity, int64_t pid, double x, double y, double width, double height)
{
    return OrderedWindow{identity, pid, x, y, width, height};
}

constexpr int64_t Editor = 100;
constexpr int64_t Helper = 200;
constexpr int64_t Other = 300;

}  // namespace

TEST_CASE("The foreground application's first window is the baseline")
{
    const std::vector<OrderedWindow> desk = {
        window(9, Other, 0, 0, 800, 600),
        window(1, Editor, 100, 100, 1200, 800),
        window(2, Editor, 150, 150, 1200, 800),
    };
    CHECK(resolveTrackedFocus(desk, Editor, {}) == 1);
}

TEST_CASE("An application with nothing on screen resolves to nothing")
{
    const std::vector<OrderedWindow> desk = {window(9, Other, 0, 0, 800, 600)};
    CHECK_FALSE(resolveTrackedFocus(desk, Editor, {9}).has_value());
}

TEST_CASE("A tracked helper-process preview above the application wins")
{
    // The Quick Look panel as first met: rendered by another process,
    // floating above the file list it belongs to.
    const std::vector<OrderedWindow> desk = {
        window(7, Helper, 300, 200, 900, 700),
        window(1, Editor, 100, 100, 1200, 800),
    };
    CHECK(resolveTrackedFocus(desk, Editor, {7}) == 7);
}

TEST_CASE("A tracked overlay that does not overlap stays out of it")
{
    const std::vector<OrderedWindow> desk = {
        window(7, Helper, 2000, 2000, 400, 300),
        window(1, Editor, 100, 100, 1200, 800),
    };
    CHECK(resolveTrackedFocus(desk, Editor, {7}) == 1);
}

TEST_CASE("A tracked same-application panel listed deeper wins")
{
    // The list reorders the sibling above the panel while the visual
    // stacking keeps the panel on top; the sibling only clips a corner.
    const std::vector<OrderedWindow> desk = {
        window(2, Editor, 900, 700, 900, 400),
        window(1, Editor, 100, 100, 1300, 900),
    };
    CHECK(resolveTrackedFocus(desk, Editor, {1}) == 1);
}

TEST_CASE("A tracked window genuinely buried by a sibling stays hidden")
{
    const std::vector<OrderedWindow> desk = {
        window(2, Editor, 120, 120, 1300, 900),
        window(1, Editor, 100, 100, 1300, 900),
    };
    CHECK(resolveTrackedFocus(desk, Editor, {1}) == 2);
}

TEST_CASE("A tracked window buried by a foreign window stays hidden")
{
    const std::vector<OrderedWindow> desk = {
        window(9, Other, 90, 90, 1400, 1000),
        window(2, Editor, 0, 0, 700, 500),
        window(1, Editor, 100, 100, 1300, 900),
    };
    CHECK(resolveTrackedFocus(desk, Editor, {1}) == 2);
}

TEST_CASE("The first window being tracked needs no help")
{
    const std::vector<OrderedWindow> desk = {
        window(1, Editor, 100, 100, 1200, 800),
        window(2, Editor, 150, 150, 1200, 800),
    };
    CHECK(resolveTrackedFocus(desk, Editor, {1}) == 1);
}

TEST_CASE("A level-shifted tracked panel present in the list wins")
{
    // macOS lifts a key panel above the ordinary window layer; the
    // harvest admits tracked windows at any layer, so the panel appears
    // here first even while the layer-zero list would have lost it.
    const std::vector<OrderedWindow> desk = {
        window(7, Editor, 300, 200, 900, 700),
        window(1, Editor, 100, 100, 1300, 900),
    };
    CHECK(resolveTrackedFocus(desk, Editor, {7}) == 7);
}

}  // namespace sidescopes
