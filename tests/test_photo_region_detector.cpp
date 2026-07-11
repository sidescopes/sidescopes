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

TEST_CASE("Detector absorbs rounded corners at Retina density") {
    // macOS clips window content by a rounded rectangle (about a 12-point
    // corner radius), so a photo's border runs start well after its true
    // corner - twice as far at 2x pixel density. The side search must scale
    // with density or the border falls out of reach.
    constexpr float kPixelsPerPoint = 2.0f;
    constexpr int kCornerRadius = 24;  // 12 points at 2x
    TestScreen screen(1280, 960);
    const IntRect photo{192, 128, 800, 640};
    screen.AddPhoto(photo);
    const int corner_centers_x[] = {photo.x + kCornerRadius,
                                    photo.x + photo.width - 1 - kCornerRadius};
    const int corner_centers_y[] = {photo.y + kCornerRadius,
                                    photo.y + photo.height - 1 - kCornerRadius};
    for (int py = photo.y; py < photo.y + photo.height; ++py)
        for (int px = photo.x; px < photo.x + photo.width; ++px) {
            const int cx = px < corner_centers_x[0]   ? corner_centers_x[0]
                           : px > corner_centers_x[1] ? corner_centers_x[1]
                                                      : px;
            const int cy = py < corner_centers_y[0]   ? corner_centers_y[0]
                           : py > corner_centers_y[1] ? corner_centers_y[1]
                                                      : py;
            const int dx = px - cx;
            const int dy = py - cy;
            if (dx * dx + dy * dy > kCornerRadius * kCornerRadius)
                screen.Set(px, py, Color{40, 40, 42});
        }

    const auto candidates = DetectPhotoRegions(screen.View(), {}, kPixelsPerPoint);
    CHECK(AnyCandidateNear(candidates, photo));
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

TEST_CASE("Detector sees antialiased low-contrast borders") {
    // A photograph on a barely different gray canvas: the border step is
    // small, and antialiasing splits it across two pixel pairs, leaving
    // every adjacent difference under the edge threshold. The wide baseline
    // must recover it. Modeled on Lightroom's gray background, where photos
    // vanished while white and black backgrounds worked.
    TestScreen screen(640, 480, Color{146, 146, 146});
    const IntRect photo{96, 64, 400, 320};
    screen.FillRect(photo, Color{122, 122, 122});
    const auto halfway = Color{134, 134, 134};
    screen.FillRect(IntRect{photo.x, photo.y, photo.width, 1}, halfway);
    screen.FillRect(IntRect{photo.x, photo.y + photo.height - 1, photo.width, 1}, halfway);
    screen.FillRect(IntRect{photo.x, photo.y, 1, photo.height}, halfway);
    screen.FillRect(IntRect{photo.x + photo.width - 1, photo.y, 1, photo.height}, halfway);

    CHECK(AnyCandidateNear(DetectPhotoRegions(screen.View()), photo, 4));
}

TEST_CASE("Detector bridges tone-matching dropouts along a border") {
    // Where the photo's own content happens to match the canvas tone, the
    // border is locally invisible - a gray pavement meeting a gray canvas
    // drops out for a couple dozen points at a time. Such gaps must be
    // bridged, not treated as the end of the border.
    TestScreen screen(640, 480, Color{146, 146, 146});
    const IntRect photo{96, 64, 400, 320};
    screen.AddPhoto(photo);
    for (const int gap_x : {150, 250, 330})
        screen.FillRect(IntRect{gap_x, photo.y + photo.height - 4, 25, 4}, Color{146, 146, 146});

    CHECK(AnyCandidateNear(DetectPhotoRegions(screen.View()), photo, 4));
}

TEST_CASE("Detector keeps a small photo despite stacked window variants") {
    // Application chrome (menu line, toolbar line, content top) stacks
    // several horizontal lines above one content area. Every line pairs
    // with the window bottom into a near-copy of the whole window; the
    // candidate cap must not let those copies crowd out the photograph.
    // Modeled on Lightroom's library view, where exactly this happened.
    TestScreen screen(1000, 800);
    const IntRect window{10, 10, 980, 780};
    screen.FillRect(window, Color{240, 240, 240});  // a light app on dark chrome
    for (int line_y : {40, 80, 120, 160, 200})
        screen.FillRect(IntRect{window.x, line_y, window.width, 2}, Color{120, 120, 120});
    const IntRect photo{600, 300, 300, 220};
    screen.AddPhoto(photo);

    CHECK(AnyCandidateNear(DetectPhotoRegions(screen.View()), photo));
}

TEST_CASE("Detector separates photos across a narrow gutter") {
    // Compare views put two photos a dozen points apart. Gap bridging
    // (needed for tone-matching dropouts) would merge their border runs
    // into one; gutter-sized gaps therefore emit the segments as well,
    // so the photos are found individually - the union may also appear,
    // and hover resolves it.
    TestScreen screen(800, 480);
    const IntRect left{48, 96, 320, 300};
    const IntRect right{384, 96, 320, 300};  // 16-point gutter
    screen.AddPhoto(left, 0x11111111u);
    screen.AddPhoto(right, 0x22222222u);

    const auto candidates = DetectPhotoRegions(screen.View());
    CHECK(AnyCandidateNear(candidates, left));
    CHECK(AnyCandidateNear(candidates, right));
}

TEST_CASE("Detector handles fractional pixel density") {
    // Windows scales at 125 and 150 percent; nothing may assume integer
    // density.
    TestScreen screen(960, 720);
    const IntRect photo{144, 96, 600, 480};
    screen.AddPhoto(photo);

    CHECK(AnyCandidateNear(DetectPhotoRegions(screen.View(), {}, 1.5f), photo));
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

TEST_CASE("Detector finds a noisy canvas sharing a color bucket with a panel") {
    // A 48-gray panel and a noisy 50..52-gray canvas meet in one
    // quantized color bucket. A single stored sample forms the matching
    // window around the panel tone, the canvas's own noise falls outside
    // it, and the shredded canvas loses to the panel - so the photograph,
    // whose borders are too soft for the edge passes, is never found.
    // The averaged bucket color covers both tones. Seen live on
    // Lightroom's develop module at 2x. The unrelated flat blocks crowd
    // the neighboring bucket (the pure-52 samples) out of the attempts,
    // as a real screen full of panels does.
    TestScreen screen(800, 720, Color{48, 48, 48});
    const IntRect canvas{120, 20, 640, 520};
    uint32_t state = 0x51515151u;
    for (int py = canvas.y; py < canvas.y + canvas.height; ++py)
        for (int px = canvas.x; px < canvas.x + canvas.width; ++px) {
            state = state * 1664525u + 1013904223u;
            const auto tone = static_cast<uint8_t>(50 + (state >> 24) % 3);  // 50..52
            screen.Set(px, py, Color{tone, tone, tone});
        }
    screen.FillRect(IntRect{0, 560, 200, 160}, Color{0, 0, 0});
    screen.FillRect(IntRect{200, 560, 200, 160}, Color{80, 80, 80});
    screen.FillRect(IntRect{400, 560, 200, 160}, Color{233, 233, 233});
    screen.FillRect(IntRect{600, 560, 200, 160}, Color{120, 120, 120});
    // The photo: noise straddling the canvas tone, every adjacent
    // difference below the edge thresholds - only the canvas hole can
    // find it.
    const IntRect photo{200, 100, 480, 360};
    state = 0x87654321u;
    for (int py = photo.y; py < photo.y + photo.height; ++py)
        for (int px = photo.x; px < photo.x + photo.width; ++px) {
            state = state * 1664525u + 1013904223u;
            const auto tone = static_cast<uint8_t>(45 + (state >> 24) % 12);  // 45..56
            screen.Set(px, py, Color{tone, tone, tone});
        }

    CHECK(AnyCandidateNear(DetectPhotoRegions(screen.View()), photo, 6));
}

TEST_CASE("Detector does not bridge rows of list text into borders") {
    // Rows of text sit closer together than the run gap tolerance, so
    // their glyph fragments would bridge into arbitrarily long phantom
    // runs; the density requirement rejects them. Seen live as boxes
    // slicing through Lightroom's preset list.
    TestScreen screen(640, 480);
    // A list: full-width row bars every 24 points, 10 points tall - their
    // shared left and right ends line up into perfect phantom sides, and
    // the 14-point gaps between rows sit inside the bridging tolerance.
    for (int row = 0; row < 14; ++row)
        screen.FillRect(IntRect{40, 60 + row * 24, 400, 10}, Color{200, 200, 200});

    for (const RegionCandidate& candidate : DetectPhotoRegions(screen.View())) {
        INFO("candidate " << candidate.rect.x << "," << candidate.rect.y << " "
                          << candidate.rect.width << "x" << candidate.rect.height);
        // Nothing photo-sized may assemble out of the text block.
        CHECK(candidate.rect.height < 72);
    }
}

TEST_CASE("Detector offers the visible part of a photo fully occluded on one side") {
    // The application's own window covers the photo's right edge for its
    // whole height, shadow halo included, so no hole row ends against
    // clean canvas on that side. The truncated rows still prove content
    // up to the occluder, and a text overlay near the top - whose rows DO
    // end against clean canvas - must not pull the side in to the text's
    // edge. Seen live with a large scope window over Lightroom.
    TestScreen screen(800, 600, Color{51, 51, 51});
    // The photo: noise straddling the canvas tone, every adjacent
    // difference below the edge thresholds, so only the canvas hole can
    // see it - as on Lightroom's low-contrast canvas.
    // Noise wholly outside the canvas matching window but under the edge
    // thresholds: a solid hole with no borders, running flush into the
    // occluder's halo.
    const IntRect photo{96, 96, 480, 400};
    uint32_t state = 0x87654321u;
    for (int py = photo.y; py < photo.y + photo.height; ++py)
        for (int px = photo.x; px < photo.x + photo.width; ++px) {
            state = state * 1664525u + 1013904223u;
            const auto tone = static_cast<uint8_t>(55 + (state >> 24) % 8);  // 55..62
            screen.Set(px, py, Color{tone, tone, tone});
        }
    // Overlay text bar on the canvas just above the photo, ending mid-way;
    // soft-toned so it forms no border of its own, only hole rows.
    screen.FillRect(IntRect{96, 76, 200, 20}, Color{57, 58, 57});
    // The occluder: flush over the photo's right side, taller than the
    // photo so no visible strip escapes its halo.
    const IntRect occluder{400, 0, 240, 600};
    screen.FillRect(occluder, Color{12, 12, 12});

    const auto candidates = DetectPhotoRegions(screen.View(), {occluder});
    // The honest offering ends at the occluder's shadow halo, not at the
    // overlay text's right edge and not beyond the occluder.
    bool found = false;
    for (const RegionCandidate& candidate : candidates) {
        const IntRect& r = candidate.rect;
        // The touching overlay bar may merge into the hole and lift its
        // top a little; the sides and bottom are what this test pins.
        if (std::abs(r.x - photo.x) <= 4 && r.y >= photo.y - 24 && r.y <= photo.y + 4 &&
            std::abs(r.y + r.height - (photo.y + photo.height)) <= 4 && r.x + r.width > 340 &&
            r.x + r.width <= occluder.x)
            found = true;
    }
    CHECK(found);
}

TEST_CASE("Detector recovers a photo whose sky melts into the canvas") {
    // A hazy sky over a matching canvas dissolves into the canvas: only
    // the photo's solid lower half is unambiguously not-canvas. The full
    // rectangle must still come out - through the hole around the frame
    // outline, or through an edge rectangle confirmed by the truncated
    // hole - and the lower half alone must not be offered beside it.
    // Seen live on Lightroom's library loupe with a fog photograph.
    TestScreen screen(760, 640, Color{51, 51, 51});
    const IntRect photo{120, 80, 480, 440};
    // A matted frame: the outline sits a wide canvas-toned gap away from
    // the photo, so it stays a thin component of its own and the gap
    // keeps the canvas flood flowing in to claim the sky.
    for (int x = photo.x - 10; x < photo.x + photo.width + 10; ++x) {
        screen.Set(x, photo.y - 10, Color{120, 120, 120});
        screen.Set(x, photo.y - 9, Color{120, 120, 120});
        screen.Set(x, photo.y + photo.height + 8, Color{120, 120, 120});
        screen.Set(x, photo.y + photo.height + 9, Color{120, 120, 120});
    }
    for (int y = photo.y - 10; y < photo.y + photo.height + 10; ++y) {
        screen.Set(photo.x - 10, y, Color{120, 120, 120});
        screen.Set(photo.x - 9, y, Color{120, 120, 120});
        screen.Set(photo.x + photo.width + 8, y, Color{120, 120, 120});
        screen.Set(photo.x + photo.width + 9, y, Color{120, 120, 120});
    }
    // Sky: the canvas tone itself, with sparse disconnected debris.
    for (int y = photo.y; y < photo.y + 220; ++y)
        for (int x = photo.x; x < photo.x + photo.width; ++x) screen.Set(x, y, Color{51, 51, 51});
    for (int speck = 0; speck < 40; ++speck)
        screen.FillRect(IntRect{130 + (speck * 37) % 460, 90 + (speck * 53) % 190, 3, 3},
                        Color{30, 30, 30});
    // Forest: solid dark noise.
    screen.AddPhoto(IntRect{photo.x, photo.y + 220, photo.width, photo.height - 220}, 0xABCDEFu);

    const auto candidates = DetectPhotoRegions(screen.View());
    CHECK(AnyCandidateNear(candidates, photo, 12));
    // The truncated lower-half hole must have been retired by the
    // confirmed full rectangle.
    const IntRect half{photo.x, photo.y + 220, photo.width, photo.height - 220};
    CHECK_FALSE(AnyCandidateNear(candidates, half, 12));
}

TEST_CASE("Detector orders candidates largest first and respects the cap") {
    TestScreen screen(800, 600);
    screen.AddPhoto(IntRect{40, 40, 500, 400}, 0x11111111u);
    screen.AddPhoto(IntRect{600, 60, 160, 120}, 0x22222222u);

    const auto candidates = DetectPhotoRegions(screen.View(), {}, 1.0f, 1);
    REQUIRE(candidates.size() == 1);
    CHECK(NearlyEqual(candidates.front().rect, IntRect{40, 40, 500, 400}, 3));
}

}  // namespace sidescopes
