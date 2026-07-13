#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <vector>

#include "modules/module_registry.h"

namespace sidescopes {
namespace {

// A solid 75% red frame, BGRA.
std::vector<uint8_t> SolidRed(int width, int height)
{
    std::vector<uint8_t> data(static_cast<std::size_t>(width) * height * 4, 0);
    for (std::size_t pixel = 0; pixel < data.size(); pixel += 4) {
        data[pixel + 2] = 191;
        data[pixel + 3] = 255;
    }

    return data;
}

}  // namespace

TEST_CASE("Registry serves the vectorscope through the module boundary")
{
    ModuleRegistry& registry = BuiltinModules();
    const RegisteredScope* scope = registry.FindScope("org.sidescopes.vectorscope");
    REQUIRE(scope != nullptr);
    CHECK(scope->descriptor->letter == 'V');
    CHECK(scope->descriptor->param_count == 4);

    ScopeInstance instance = registry.CreateInstance("org.sidescopes.vectorscope");
    REQUIRE(instance.Valid());

    // Defaults through the boundary must match the engine's: BT.709
    // puts 75% red at bin (109, 43).
    const std::vector<uint8_t> pixels = SolidRed(8, 8);
    const SsFrameView frame{pixels.data(), 8 * 4, 8, 8, SS_COLOR_SPACE_SRGB, 1};
    REQUIRE(instance.accumulate(frame, SsRect{0, 0, 8, 8}));
    const SsImageView image = instance.image();
    REQUIRE(image.width == 256);
    int bestX = -1, bestY = -1, best = 0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const uint8_t* rgba = image.rgba + (static_cast<std::size_t>(y) * image.width + x) * 4;
            const int sum = rgba[0] + rgba[1] + rgba[2];
            if (sum > best) {
                best = sum;
                bestX = x;
                bestY = y;
            }
        }
    }
    CHECK(bestX == 109);
    CHECK(bestY == 43);

    // Reconfigure to BT.601 through parameters: the peak moves to (100, 43).
    std::vector<SsParamValue> values{{"matrix", 0.0}};
    REQUIRE(instance.configure(values));
    REQUIRE(instance.accumulate(frame, SsRect{0, 0, 8, 8}));
    const SsImageView after = instance.image();
    best = 0;
    for (int y = 0; y < after.height; ++y) {
        for (int x = 0; x < after.width; ++x) {
            const uint8_t* rgba = after.rgba + (static_cast<std::size_t>(y) * after.width + x) * 4;
            const int sum = rgba[0] + rgba[1] + rgba[2];
            if (sum > best) {
                best = sum;
                bestX = x;
                bestY = y;
            }
        }
    }
    CHECK(bestX == 100);
    CHECK(bestY == 43);

    // Overlays and markers arrive as declarative data.
    CHECK(instance.Graticule().size() > 8);
    const std::vector<SsMarker> markers = instance.Markers(SsColor{191.0f, 0.0f, 0.0f});
    REQUIRE(markers.size() == 1);
    CHECK(markers[0].kind == SS_MARKER_POINT);
    CHECK(markers[0].channel_mask == 0x7u);

    // The adaptive-image extension resizes the display grid.
    const auto* adaptive = static_cast<const SsAdaptiveImageExtension*>(instance.GetExtension(AdaptiveImageExtension));
    REQUIRE(adaptive != nullptr);
}

}  // namespace sidescopes
