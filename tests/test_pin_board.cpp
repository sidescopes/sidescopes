#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <vector>

#include "app/pin_board.h"

namespace sidescopes {
namespace {

FloatColor gray(float value)
{
    return FloatColor{value, value, value};
}

}  // namespace

TEST_CASE("PinBoard starts empty with no comparator")
{
    PinBoard board;
    CHECK(board.empty());
    CHECK(board.size() == 0);
    CHECK_FALSE(board.hasComparator());
    CHECK(board.comparator() == -1);
    CHECK(board.managed() == -1);
}

TEST_CASE("Pinning a color makes it the comparison reference")
{
    PinBoard board;
    board.pin(gray(10.0f));
    board.pin(gray(20.0f));
    REQUIRE(board.size() == 2);
    CHECK(board.color(0).r == 10.0f);
    CHECK(board.color(1).r == 20.0f);
    CHECK(board.comparator() == 1);
    REQUIRE(board.hasComparator());
    CHECK(board.comparatorColor().r == 20.0f);
}

TEST_CASE("Pinning past capacity drops the oldest pin")
{
    PinBoard board;
    for (std::size_t index = 0; index < PinBoard::Maximum + 2; ++index) {
        board.pin(gray(static_cast<float>(index)));
    }
    CHECK(board.size() == PinBoard::Maximum);
    // The two oldest fell out of the ring; the front is now the third color.
    CHECK(board.color(0).r == 2.0f);
    CHECK(board.color(PinBoard::Maximum - 1).r == static_cast<float>(PinBoard::Maximum + 1));
    CHECK(board.comparator() == static_cast<int>(PinBoard::Maximum) - 1);
}

TEST_CASE("Removing a pin keeps the comparator index valid")
{
    PinBoard board;
    board.pin(gray(10.0f));  // index 0
    board.pin(gray(20.0f));  // index 1
    board.pin(gray(30.0f));  // index 2, the comparator

    SECTION("removing the comparator clears it")
    {
        board.removeAt(2);
        CHECK(board.size() == 2);
        CHECK_FALSE(board.hasComparator());
    }

    SECTION("removing before the comparator shifts it down")
    {
        board.selectComparator(2);
        board.removeAt(0);
        REQUIRE(board.size() == 2);
        CHECK(board.comparator() == 1);
        CHECK(board.comparatorColor().r == 30.0f);
    }

    SECTION("removing after the comparator leaves it in place")
    {
        board.selectComparator(0);
        board.removeAt(2);
        CHECK(board.comparator() == 0);
        CHECK(board.comparatorColor().r == 10.0f);
    }

    SECTION("an out-of-range index is a no-op")
    {
        board.removeAt(99);
        CHECK(board.size() == 3);
        CHECK(board.comparator() == 2);
    }
}

TEST_CASE("Restoring a saved board brings back its colors and comparator")
{
    PinBoard board;
    board.restore({gray(10.0f), gray(20.0f), gray(30.0f)}, 1);
    REQUIRE(board.size() == 3);
    CHECK(board.color(0).r == 10.0f);
    CHECK(board.color(2).r == 30.0f);
    REQUIRE(board.hasComparator());
    CHECK(board.comparator() == 1);
    CHECK(board.comparatorColor().r == 20.0f);
}

TEST_CASE("Restoring caps the board at its capacity")
{
    // A hand-edited preferences file can name more colors than the ring holds;
    // the leading ones fill it and the rest are dropped.
    std::vector<FloatColor> saved;
    for (std::size_t index = 0; index < PinBoard::Maximum + 3; ++index) {
        saved.push_back(gray(static_cast<float>(index)));
    }
    PinBoard board;
    board.restore(saved, 0);
    CHECK(board.size() == PinBoard::Maximum);
    CHECK(board.color(0).r == 0.0f);
    CHECK(board.color(PinBoard::Maximum - 1).r == static_cast<float>(PinBoard::Maximum) - 1.0f);
}

TEST_CASE("Restoring rejects a comparator no restored color answers")
{
    PinBoard board;

    SECTION("an index past the colors selects nothing")
    {
        board.restore({gray(10.0f), gray(20.0f)}, 5);
        CHECK(board.size() == 2);
        CHECK_FALSE(board.hasComparator());
        CHECK(board.comparator() == -1);
    }

    SECTION("a negative index other than none selects nothing")
    {
        board.restore({gray(10.0f)}, -7);
        CHECK(board.comparator() == -1);
    }

    SECTION("a comparator on an empty board selects nothing")
    {
        board.restore({}, 0);
        CHECK(board.empty());
        CHECK(board.comparator() == -1);
    }

    SECTION("a comparator dropped by the capacity cap selects nothing")
    {
        std::vector<FloatColor> saved;
        for (std::size_t index = 0; index < PinBoard::Maximum + 1; ++index) {
            saved.push_back(gray(static_cast<float>(index)));
        }
        board.restore(saved, static_cast<int>(PinBoard::Maximum));
        CHECK(board.size() == PinBoard::Maximum);
        CHECK(board.comparator() == -1);
    }
}

TEST_CASE("Restoring replaces whatever the board already held")
{
    PinBoard board;
    board.pin(gray(10.0f));
    board.pin(gray(20.0f));
    board.restore({gray(30.0f)}, 0);
    REQUIRE(board.size() == 1);
    CHECK(board.color(0).r == 30.0f);
    CHECK(board.comparator() == 0);
}

TEST_CASE("Clearing empties the board and drops the comparator")
{
    PinBoard board;
    board.pin(gray(10.0f));
    board.pin(gray(20.0f));
    board.clear();
    CHECK(board.empty());
    CHECK_FALSE(board.hasComparator());
}

TEST_CASE("The managed pin index is tracked independently")
{
    PinBoard board;
    board.pin(gray(10.0f));
    board.manage(0);
    CHECK(board.managed() == 0);
    board.manage(-1);
    CHECK(board.managed() == -1);
}

}  // namespace sidescopes
