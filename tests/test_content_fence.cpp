#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "core/content_fence.h"

// The window and chrome rectangles below are Lightroom Classic's real
// geometry as measured live over the accessibility API (points doubled to
// Retina pixels). The fence must converge to the canvas area between the
// panels, the top bar, and the filmstrip - the exact regression that
// shipped twice with untested glue.

namespace sidescopes {

TEST_CASE("Fence converges to Lightroom's canvas area") {
    const IntRect frame{0, 0, 3024, 1964};
    const IntRect window{0, 66, 3024, 1778};
    const std::vector<IntRect> chrome{
        {0, 1544, 3024, 300},    // filmstrip
        {0, 122, 3024, 140},     // top bar
        {2354, 242, 670, 1566},  // right panel, outer variant
        {0, 242, 648, 1566},     // left panel
        {2376, 242, 648, 1566},  // right panel
        {0, 242, 670, 1566},     // left panel, outer variant
    };

    const ContentFence fence = FenceContent(frame, window, chrome, /*slack=*/64);
    CHECK(fence.content.x == 670);
    CHECK(fence.content.y == 262);
    CHECK(fence.content.x + fence.content.width == 2354);
    CHECK(fence.content.y + fence.content.height == 1544);
    CHECK(fence.occluders.empty());
}

TEST_CASE("Fence keeps a floating overlay as an occluder") {
    const IntRect frame{0, 0, 2000, 1200};
    const IntRect window{0, 40, 2000, 1100};
    const std::vector<IntRect> chrome{
        {0, 40, 300, 1100},    // left panel: trims
        {500, 200, 900, 250},  // loupe info overlay: floats over content
    };

    const ContentFence fence = FenceContent(frame, window, chrome, /*slack=*/40);
    CHECK(fence.content.x == 300);
    REQUIRE(fence.occluders.size() == 1);
    // Fence-relative coordinates.
    CHECK(fence.occluders[0].x == 200);
    CHECK(fence.occluders[0].y == 160);
    CHECK(fence.occluders[0].width == 900);
}

TEST_CASE("Fence without windows or chrome is the whole frame") {
    const IntRect frame{0, 0, 1512, 982};
    const ContentFence fence = FenceContent(frame, IntRect{}, {}, 32);
    CHECK(fence.content.x == 0);
    CHECK(fence.content.width == 1512);
    CHECK(fence.content.height == 982);
}

TEST_CASE("Fence never trims itself away") {
    // Chrome covering nearly the whole window must not leave a degenerate
    // fence: a trim that would empty the content is refused.
    const IntRect frame{0, 0, 1000, 800};
    const IntRect window{0, 0, 1000, 800};
    const std::vector<IntRect> chrome{{0, 0, 1000, 790}};

    const ContentFence fence = FenceContent(frame, window, chrome, 16);
    CHECK(fence.content.width > 0);
    CHECK(fence.content.height > 0);
}

}  // namespace sidescopes
