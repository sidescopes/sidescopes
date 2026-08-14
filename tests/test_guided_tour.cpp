#include <catch2/catch_test_macros.hpp>

#include "app/guided_tour.h"

namespace sidescopes {
namespace {

std::vector<TourStep> threeStops()
{
    return {
        TourStep{"picture", "The picture", "This stands in for your screen."},
        TourStep{"region", "The region", "Drag the band to move it."},
        TourStep{"scopes", "The scopes", "They read what is inside the region."},
    };
}

}  // namespace

TEST_CASE("A tour walks its stops in order and finishes after the last")
{
    GuidedTour tour(threeStops());
    CHECK_FALSE(tour.running());
    CHECK(tour.count() == 3);

    tour.start();
    REQUIRE(tour.running());
    REQUIRE(tour.current() != nullptr);
    CHECK(tour.current()->anchor == "picture");
    CHECK(tour.position() == 1);
    CHECK_FALSE(tour.onLastStep());

    tour.advance();
    CHECK(tour.current()->anchor == "region");
    CHECK(tour.position() == 2);

    tour.advance();
    CHECK(tour.current()->anchor == "scopes");
    // The button says so rather than making the visitor discover it.
    CHECK(tour.onLastStep());

    tour.advance();
    CHECK_FALSE(tour.running());
    CHECK(tour.current() == nullptr);
    CHECK(tour.position() == 0);
}

TEST_CASE("Seeing a tour through settles it, exactly as skipping does")
{
    // Both mean the same thing to the visitor - do not open this by yourself
    // again - and a tour that only remembered the skip would greet everyone
    // who finished it all over again.
    GuidedTour finished(threeStops());
    finished.start();
    finished.advance();
    finished.advance();
    CHECK_FALSE(finished.settled());
    finished.advance();
    CHECK(finished.settled());

    GuidedTour waved(threeStops());
    waved.start();
    waved.skip();
    CHECK(waved.settled());
    CHECK_FALSE(waved.running());
}

TEST_CASE("A tour that was never settled opens itself")
{
    GuidedTour tour(threeStops());
    tour.restoreSettled(false);

    CHECK(tour.running());
    CHECK(tour.position() == 1);
}

TEST_CASE("A settled tour stays out of the way until it is asked for")
{
    GuidedTour tour(threeStops());
    tour.restoreSettled(true);
    CHECK_FALSE(tour.running());

    // The "take the tour" button, which has to work however settled it is.
    tour.start();
    CHECK(tour.running());
    CHECK(tour.position() == 1);
}

TEST_CASE("A tour with no stops never claims to be running")
{
    // The guard matters because a host assembles the steps: one that assembles
    // none would otherwise show an empty bubble pointing nowhere.
    GuidedTour empty{{}};
    empty.start();
    CHECK_FALSE(empty.running());
    CHECK(empty.current() == nullptr);
    CHECK_FALSE(empty.onLastStep());

    empty.restoreSettled(false);
    CHECK_FALSE(empty.running());
}

TEST_CASE("Advancing a tour that is not running does nothing")
{
    GuidedTour tour(threeStops());
    tour.advance();

    CHECK_FALSE(tour.running());
    CHECK_FALSE(tour.settled());
}

}  // namespace sidescopes
