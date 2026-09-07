#include "app/region_border_motion.h"

#include <cmath>

#include "core/analysis_worker.h"

namespace sidescopes {
namespace {

constexpr double AngularRate = 40.0;
constexpr double SettleDistance = 0.002;
constexpr double MaximumGap = 0.25;

}  // namespace

RegionOfInterest RegionBorderMotion::update(const RegionOfInterest& target, double now, bool animate)
{
    const std::array<double, 4> next{target.leftPercent, target.topPercent, target.rightPercent, target.bottomPercent};
    if (!m_time || !animate || now < *m_time || now - *m_time > MaximumGap) {
        m_position = next;
        m_velocity.fill(0.0);
    } else {
        // Newly received geometry did not exist during the elapsed interval.
        advance(now - *m_time);
    }
    m_time = now;
    m_target = next;
    settle();
    return region();
}

void RegionBorderMotion::advance(double seconds)
{
    if (seconds <= 0.0) {
        return;
    }
    const double decay = std::exp(-AngularRate * seconds);
    for (std::size_t edge = 0; edge < m_position.size(); ++edge) {
        const double error = m_position[edge] - m_target[edge];
        const double slope = m_velocity[edge] + AngularRate * error;
        m_position[edge] = m_target[edge] + (error + slope * seconds) * decay;
        m_velocity[edge] = (m_velocity[edge] - AngularRate * slope * seconds) * decay;
    }
}

void RegionBorderMotion::settle()
{
    for (std::size_t edge = 0; edge < m_position.size(); ++edge) {
        if (std::abs(m_position[edge] - m_target[edge]) >= SettleDistance ||
            std::abs(m_velocity[edge]) / AngularRate >= SettleDistance) {
            m_active = true;
            return;
        }
    }
    m_position = m_target;
    m_velocity.fill(0.0);
    m_active = false;
}

RegionOfInterest RegionBorderMotion::region() const
{
    return {m_position[0], m_position[1], m_position[2], m_position[3]};
}

void RegionBorderMotion::reset()
{
    m_time.reset();
    m_velocity.fill(0.0);
    m_active = false;
}

bool RegionBorderMotion::active() const
{
    return m_active;
}

}  // namespace sidescopes
