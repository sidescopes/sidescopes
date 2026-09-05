#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "app/region_geometry.h"
#include "boundary_input.h"
#include "boundary_properties.h"
#include "core/frame.h"
#include "platform/region_geometry.h"

namespace sidescopes::test {
namespace {

void checkIntersection(BoundaryInput& input)
{
    const IntRect rect{input.integer(), input.integer(), input.integer(), input.integer()};
    const int width = input.integer();
    const int height = input.integer();
    const IntRect clipped = rect.clampedTo(width, height);
    const auto clippedAgain = clipped.clampedTo(width, height);
    requireBoundary(clipped.empty() ? clippedAgain.empty() : clipped == clippedAgain,
                    "geometry: intersection is not idempotent");
    const IntRect frame{0, 0, width, height};
    const std::array xs{input.integer(), rect.x, clipped.x, width, 0, -1, std::numeric_limits<int>::max()};
    const std::array ys{input.integer(), rect.y, clipped.y, height, 0, -1, std::numeric_limits<int>::min()};
    for (int x : xs) {
        for (int y : ys) {
            requireBoundary(clipped.contains(x, y) == (rect.contains(x, y) && frame.contains(x, y)),
                            "geometry: intersection changes point membership");
        }
    }
    FrameView view;
    view.sourceX = input.integer();
    view.sourceY = input.integer();
    const auto local = view.fromDisplay(rect);
    if (static_cast<int64_t>(rect.x) - view.sourceX >= std::numeric_limits<int>::min() &&
        static_cast<int64_t>(rect.x) - view.sourceX <= std::numeric_limits<int>::max()) {
        requireBoundary(static_cast<int64_t>(local.x) + view.sourceX == rect.x,
                        "geometry: display translation loses a representable coordinate");
    }
    requireBoundary(local.width == rect.width && local.height == rect.height,
                    "geometry: display translation resizes the region");
}

bool near(double a, double b)
{
    return std::abs(a - b) <= 2e-10 * std::max({1.0, std::abs(a), std::abs(b)});
}

void checkConversions(BoundaryInput& input)
{
    // Finite desktop coordinates and positive display sizes are the platform
    // conversion contract. Invalid frame layouts are exercised separately.
    const int width = 1 + static_cast<int>(input.word(4) % 1048576);
    const int height = 1 + static_cast<int>(input.word(4) % 1048576);
    const double x = static_cast<double>(input.integer()) / 16.0;
    const double y = static_cast<double>(input.integer()) / 16.0;
    const LocalRect original{x, y, 1.0 + input.byte(), 1.0 + input.byte()};
    const auto region = regionFromLocalRect(original, width, height);
    const auto restored = localRectFromRegion(region, width, height);
    const double tolerance = 16 * std::numeric_limits<double>::epsilon() *
                             std::max({1.0, std::abs(x), std::abs(y), original.width, original.height});
    requireBoundary(std::abs(original.x - restored.x) <= tolerance && std::abs(original.y - restored.y) <= tolerance &&
                        std::abs(original.width - restored.width) <= tolerance &&
                        std::abs(original.height - restored.height) <= tolerance,
                    "geometry: local/percent roundtrip drifts");
    const auto lock = lockRectFromPercent(region, width, height);
    const auto percent = percentFromLockRect(lock, width, height);
    requireBoundary(near(region.leftPercent, percent.leftPercent) && near(region.topPercent, percent.topPercent) &&
                        near(region.rightPercent, percent.rightPercent) &&
                        near(region.bottomPercent, percent.bottomPercent),
                    "geometry: lock/percent roundtrip drifts");
    const auto fitted = rectClampedWithin(original, width, height);
    requireBoundary(
        fitted.x >= 0 && fitted.y >= 0 && fitted.x + fitted.width <= width && fitted.y + fitted.height <= height,
        "geometry: fitted rectangle escapes its display");
}

void checkOcclusion(BoundaryInput& input)
{
    std::vector<LocalRect> windows;
    const unsigned count = input.byte() % 8;
    for (unsigned index = 0; index < count; ++index) {
        windows.push_back({static_cast<double>(input.byte() % 12) - 4, static_cast<double>(input.byte() % 12) - 4,
                           static_cast<double>(input.byte() % 7), static_cast<double>(input.byte() % 7)});
    }
    const auto fractions = visibleFractions(windows);
    std::vector<unsigned> visible(count, 0);
    // An independent unit-cell oracle: every integer rectangle either covers
    // the complete cell or none of it. No union-area algorithm is duplicated.
    for (int x = -4; x < 14; ++x) {
        for (int y = -4; y < 14; ++y) {
            const auto hit = topmostVisibleWindowAt(windows, x + 0.5, y + 0.5);
            if (hit) {
                ++visible[*hit];
            }
        }
    }
    for (unsigned index = 0; index < count; ++index) {
        const double area = windows[index].width * windows[index].height;
        const double expected = area > 0 ? visible[index] / area : 0.0;
        requireBoundary(near(fractions[index], expected), "geometry: visible area disagrees with unit-cell coverage");
    }
}

}  // namespace

void checkGeometryProperties(std::span<const uint8_t> bytes)
{
    BoundaryInput input{bytes};
    checkIntersection(input);
    checkConversions(input);
    checkOcclusion(input);
}

}  // namespace sidescopes::test
