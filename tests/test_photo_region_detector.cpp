#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "core/photo_region_detector.h"

namespace sidescopes {
namespace {

// A synthetic screen: chrome-colored backdrop with rectangles of content.
struct TestScreen {
    explicit TestScreen(int width, int height, Color backdrop = Color{40, 40, 42})
        : width(width), height(height) {
        pixels.resize(static_cast<std::size_t>(width) * height * 4);
        FillRect(IntRect{0, 0, width, height}, backdrop);
    }

    void Set(int px, int py, Color color) {
        uint8_t* p = pixels.data() + (static_cast<std::size_t>(py) * width + px) * 4;
        p[0] = color.b;
        p[1] = color.g;
        p[2] = color.r;
        p[3] = 255;
    }

    void FillRect(IntRect rect, Color color) {
        for (int py = rect.y; py < rect.y + rect.height; ++py)
            for (int px = rect.x; px < rect.x + rect.width; ++px) Set(px, py, color);
    }

    void AddPhoto(IntRect rect, uint32_t seed = 0x12345678u) {
        uint32_t state = seed;
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

bool AnyCandidateNear(const std::vector<RegionCandidate>& candidates, const IntRect& rect,
                      int tolerance = 3) {
    for (const RegionCandidate& candidate : candidates) {
        if (NearlyEqual(candidate.rect, rect, tolerance)) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("Detector finds a photo rectangle inside chrome") {
    TestScreen screen(640, 480);
    const IntRect photo{96, 64, 400, 320};
    screen.AddPhoto(photo);

    const auto candidates = DetectPhotoRegions(screen.View());
    CHECK(AnyCandidateNear(candidates, photo));
}

TEST_CASE("Detector finds a smooth gradient rectangle") {
    // Content statistics cannot tell a rendered-smooth region from chrome,
    // but its BOUNDARY can: whatever the interior, a displayed photo is a
    // rectangle of sharp contrast edges.
    TestScreen screen(640, 480);
    const IntRect photo{96, 64, 400, 320};
    for (int py = photo.y; py < photo.y + photo.height; ++py)
        for (int px = photo.x; px < photo.x + photo.width; ++px) {
            const auto level = static_cast<uint8_t>(120 + (px - photo.x) * 30 / photo.width);
            screen.Set(px, py, Color{level, level, static_cast<uint8_t>(level + 8)});
        }

    CHECK(AnyCandidateNear(DetectPhotoRegions(screen.View()), photo));
}

TEST_CASE("Detector finds black-and-white photographs") {
    TestScreen screen(640, 480);
    const IntRect photo{96, 64, 400, 320};
    uint32_t state = 0x0badf00du;
    for (int py = photo.y; py < photo.y + photo.height; ++py)
        for (int px = photo.x; px < photo.x + photo.width; ++px) {
            state = state * 1664525u + 1013904223u;
            const auto level = static_cast<uint8_t>(state >> 24);
            screen.Set(px, py, Color{level, level, level});
        }

    CHECK(AnyCandidateNear(DetectPhotoRegions(screen.View()), photo));
}

TEST_CASE("Detector reports both a window and the photo inside it") {
    // The recall-over-precision contract: nested rectangles are both
    // offered; the picker highlights the smallest under the cursor.
    TestScreen screen(800, 600);
    const IntRect window{60, 40, 640, 500};
    const IntRect photo{140, 120, 480, 340};
    screen.FillRect(window, Color{92, 92, 94});  // a lighter window on dark chrome
    screen.AddPhoto(photo);

    const auto candidates = DetectPhotoRegions(screen.View());
    CHECK(AnyCandidateNear(candidates, window));
    CHECK(AnyCandidateNear(candidates, photo));
}

TEST_CASE("Detector covers flat patches inside a photo") {
    // A rendered-smooth band does not break the photo's boundary rectangle.
    TestScreen screen(640, 480);
    const IntRect photo{96, 64, 400, 320};
    screen.AddPhoto(photo);
    screen.FillRect(IntRect{photo.x, photo.y + 100, photo.width, 110}, Color{150, 170, 200});

    CHECK(AnyCandidateNear(DetectPhotoRegions(screen.View()), photo));
}

TEST_CASE("Detector finds side-by-side photos separately") {
    TestScreen screen(800, 480);
    const IntRect left{48, 96, 320, 300};
    const IntRect right{432, 96, 320, 300};
    screen.AddPhoto(left, 0x11111111u);
    screen.AddPhoto(right, 0x22222222u);

    const auto candidates = DetectPhotoRegions(screen.View());
    CHECK(AnyCandidateNear(candidates, left));
    CHECK(AnyCandidateNear(candidates, right));
}

TEST_CASE("Detector tolerates a masked window occluding a photo edge") {
    // The application's own always-on-top scope window habitually floats
    // over the photo being scoped. Its pixels prove nothing about the
    // desktop underneath: the photo's border must survive beneath it, and
    // content drawn inside it (graticule lines) must yield no candidates.
    TestScreen screen(800, 600);
    const IntRect photo{100, 80, 500, 400};
    screen.AddPhoto(photo);
    const IntRect scope_window{430, 140, 350, 280};
    screen.FillRect(scope_window, Color{20, 20, 20});
    for (int line = 0; line < 5; ++line)
        screen.FillRect(IntRect{450, 170 + line * 50, 300, 2}, Color{200, 200, 200});

    const auto candidates = DetectPhotoRegions(screen.View(), {scope_window});
    CHECK(AnyCandidateNear(candidates, photo));
    for (const RegionCandidate& candidate : candidates) {
        CHECK(!(candidate.rect.x >= scope_window.x && candidate.rect.y >= scope_window.y &&
                candidate.rect.x + candidate.rect.width <= scope_window.x + scope_window.width &&
                candidate.rect.y + candidate.rect.height <= scope_window.y + scope_window.height));
    }
}

TEST_CASE("Detector reports nothing on a flat frame") {
    TestScreen screen(640, 480);
    CHECK(DetectPhotoRegions(screen.View()).empty());
}

TEST_CASE("Detector ignores rectangles below photo size") {
    TestScreen screen(640, 480);
    screen.AddPhoto(IntRect{100, 100, 60, 40});  // an icon, not a photo

    CHECK(DetectPhotoRegions(screen.View()).empty());
}

TEST_CASE("Detector orders candidates largest first and respects the cap") {
    TestScreen screen(800, 600);
    screen.AddPhoto(IntRect{40, 40, 500, 400}, 0x11111111u);
    screen.AddPhoto(IntRect{600, 60, 160, 120}, 0x22222222u);

    const auto candidates = DetectPhotoRegions(screen.View(), {}, 1);
    REQUIRE(candidates.size() == 1);
    CHECK(NearlyEqual(candidates.front().rect, IntRect{40, 40, 500, 400}, 3));
}

}  // namespace sidescopes
