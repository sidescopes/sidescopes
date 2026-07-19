#include "platform/focus_resolution.h"

#include <algorithm>

namespace sidescopes {
namespace {

bool overlaps(const OrderedWindow& a, const OrderedWindow& b)
{
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

// Whether some window listed above covers more than half of @p target:
// the tracked window is genuinely behind a sibling, not merely
// list-ordered below it the way panels are.
bool windowBuried(const std::vector<OrderedWindow>& windows, std::size_t target)
{
    const OrderedWindow& bounds = windows[target];
    const double area = bounds.width * bounds.height;
    for (std::size_t index = 0; index < target; ++index) {
        const double left = std::max(windows[index].x, bounds.x);
        const double right = std::min(windows[index].x + windows[index].width, bounds.x + bounds.width);
        const double top = std::max(windows[index].y, bounds.y);
        const double bottom = std::min(windows[index].y + windows[index].height, bounds.y + bounds.height);
        if (right > left && bottom > top && area > 0.0 && (right - left) * (bottom - top) > area * 0.5) {
            return true;
        }
    }

    return false;
}

}  // namespace

std::optional<uint64_t> resolveTrackedFocus(const std::vector<OrderedWindow>& windows, int64_t applicationPid,
                                            const std::vector<uint64_t>& tracked)
{
    const auto isTracked = [&tracked](uint64_t identity) {
        return std::find(tracked.begin(), tracked.end(), identity) != tracked.end();
    };
    std::optional<std::size_t> first;
    std::optional<std::size_t> trackedOwn;
    for (std::size_t index = 0; index < windows.size(); ++index) {
        if (windows[index].ownerPid != applicationPid) {
            continue;
        }
        if (!first) {
            first = index;
        }
        if (isTracked(windows[index].identity)) {
            trackedOwn = index;
            break;
        }
    }
    if (!first) {
        return std::nullopt;
    }
    // A tracked window of another application above the foreground one and
    // overlapping it wins: a preview panel rendered by a helper process.
    for (std::size_t index = 0; index < *first; ++index) {
        if (isTracked(windows[index].identity) && overlaps(windows[index], windows[*first])) {
            return windows[index].identity;
        }
    }
    // A tracked window of the foreground application itself wins even when
    // listed deeper - the window list's order and the visual stacking
    // disagree for panels (Quick Look) - unless something above genuinely
    // buries it.
    if (trackedOwn && *trackedOwn != *first && !windowBuried(windows, *trackedOwn)) {
        return windows[*trackedOwn].identity;
    }

    return windows[*first].identity;
}

}  // namespace sidescopes
