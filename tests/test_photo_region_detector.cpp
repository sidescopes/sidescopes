#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "core/photo_region_detector.h"

namespace sidescopes {
namespace {

// A synthetic "editor screenshot": uniform chrome with photo-like regions of
// deterministic pseudo-noise (linear congruential generator, fixed seed).
struct EditorFrame {
    explicit EditorFrame(int width, int height, Color chrome = Color{40, 40, 42})
        : width(width), height(height) {
        pixels.resize(static_cast<std::size_t>(width) * height * 4);
        for (int py = 0; py < height; ++py)
            for (int px = 0; px < width; ++px) Set(px, py, chrome);
    }

    void Set(int px, int py, Color color) {
        uint8_t* p = pixels.data() + (static_cast<std::size_t>(py) * width + px) * 4;
        p[0] = color.b;
        p[1] = color.g;
        p[2] = color.r;
        p[3] = 255;
    }

    void AddPhoto(IntRect rect) {
        uint32_t state = 0x12345678u;
        const auto next = [&state] {
            state = state * 1664525u + 1013904223u;
            return static_cast<uint8_t>(state >> 24);
        };
        for (int py = rect.y; py < rect.y + rect.height; ++py)
            for (int px = rect.x; px < rect.x + rect.width; ++px)
                Set(px, py, Color{next(), next(), next()});
    }

    [[nodiscard]] FrameView View() const {
        return FrameView{pixels.data(), width * 4, width, height, ColorSpaceHint::Srgb, 1};
    }

    std::vector<uint8_t> pixels;
    int width;
    int height;
};

bool NearlyEqual(const IntRect& a, const IntRect& b, int tolerance) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance &&
           std::abs(a.x + a.width - (b.x + b.width)) <= tolerance &&
           std::abs(a.y + a.height - (b.y + b.height)) <= tolerance;
}

}  // namespace

TEST_CASE("Detector finds a photo canvas inside editor chrome") {
    EditorFrame frame(640, 480);
    const IntRect photo{96, 64, 400, 320};
    frame.AddPhoto(photo);

    const auto candidates = DetectPhotoRegions(frame.View());
    REQUIRE_FALSE(candidates.empty());
    // Edge refinement recovers the boundary to within a couple of pixels.
    CHECK(NearlyEqual(candidates.front().rect, photo, 2));
    CHECK(candidates.front().confidence > 0.8f);
}

TEST_CASE("Detector reports nothing on a flat frame") {
    EditorFrame frame(640, 480);
    CHECK(DetectPhotoRegions(frame.View()).empty());
}

TEST_CASE("Detector finds side-by-side photos as separate candidates") {
    // A compare view: two photos with a chrome gutter between them.
    EditorFrame frame(800, 480);
    const IntRect left{48, 96, 320, 300};
    const IntRect right{432, 96, 320, 300};
    frame.AddPhoto(left);
    frame.AddPhoto(right);

    const auto candidates = DetectPhotoRegions(frame.View());
    REQUIRE(candidates.size() >= 2);
    const bool found_left =
        NearlyEqual(candidates[0].rect, left, 2) || NearlyEqual(candidates[1].rect, left, 2);
    const bool found_right =
        NearlyEqual(candidates[0].rect, right, 2) || NearlyEqual(candidates[1].rect, right, 2);
    CHECK(found_left);
    CHECK(found_right);
}

TEST_CASE("Detector handles a photo flush with the frame edge") {
    EditorFrame frame(640, 480);
    const IntRect photo{0, 0, 320, 480};  // fills the whole left edge
    frame.AddPhoto(photo);

    const auto candidates = DetectPhotoRegions(frame.View());
    REQUIRE_FALSE(candidates.empty());
    CHECK(NearlyEqual(candidates.front().rect, photo, 2));
}

TEST_CASE("Detector ranks the dominant photo first") {
    EditorFrame frame(800, 600);
    const IntRect main_photo{64, 64, 480, 400};
    const IntRect thumbnail{640, 480, 96, 80};
    frame.AddPhoto(main_photo);
    frame.AddPhoto(thumbnail);

    const auto candidates = DetectPhotoRegions(frame.View());
    REQUIRE_FALSE(candidates.empty());
    CHECK(NearlyEqual(candidates.front().rect, main_photo, 2));
}

TEST_CASE("Detector ignores tiny specks") {
    EditorFrame frame(640, 480);
    frame.AddPhoto(IntRect{100, 100, 20, 20});  // a busy icon, not a photo

    const auto candidates = DetectPhotoRegions(frame.View());
    CHECK(candidates.empty());
}

}  // namespace sidescopes
