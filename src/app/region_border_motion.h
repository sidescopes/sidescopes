#pragma once

#include <array>
#include <optional>

namespace sidescopes {

struct RegionOfInterest;

/// Animates presentation geometry without changing the region used for analysis.
/// Targets must have finite, strictly ordered edges; time must be finite.
class RegionBorderMotion
{
public:
    /// Advances the previous target before accepting the next target.
    [[nodiscard]] RegionOfInterest update(const RegionOfInterest& target, double now, bool animate);
    /// Discards motion so the next update starts at its target.
    void reset();
    /// Whether presentation updates are still needed to reach the target.
    [[nodiscard]] bool active() const;

private:
    void advance(double seconds);
    void settle();
    [[nodiscard]] RegionOfInterest region() const;

    std::array<double, 4> m_position{};
    std::array<double, 4> m_velocity{};
    std::array<double, 4> m_target{};
    std::optional<double> m_time;
    bool m_active = false;
};

}  // namespace sidescopes
