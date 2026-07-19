#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace sidescopes {

/// One on-screen window as the focus routing sees it: identity, owning
/// process, and bounds, listed front to back. Each platform harvests
/// these; the resolution rule below is shared and unit-tested, so the
/// behaviours the harvest cannot control - helper-process previews,
/// panels whose list order disagrees with their visual stacking - are
/// locked by tests instead of rediscovered.
struct OrderedWindow
{
    uint64_t identity = 0;
    int64_t ownerPid = 0;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

/// The window the focus routing should treat as focused, given the
/// qualifying on-screen windows front to back. The foreground
/// application's first-listed window is the baseline; a TRACKED window
/// wins over it from another process when it overlaps it from above,
/// and from the same process even when listed deeper - the window
/// list's order and the visual stacking disagree for panels (Quick
/// Look) - unless some window above covers more than half of it.
/// An application with nothing on screen resolves to nothing.
[[nodiscard]] std::optional<uint64_t> resolveTrackedFocus(const std::vector<OrderedWindow>& windows,
                                                          int64_t applicationPid, const std::vector<uint64_t>& tracked);

}  // namespace sidescopes
