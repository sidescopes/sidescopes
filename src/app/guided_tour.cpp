#include "app/guided_tour.h"

#include <utility>

namespace sidescopes {

GuidedTour::GuidedTour(std::vector<TourStep> steps)
    : m_steps(std::move(steps))
{
}

void GuidedTour::start()
{
    if (m_steps.empty()) {
        return;
    }
    m_at = 0;
}

void GuidedTour::advance()
{
    if (!running()) {
        return;
    }
    ++m_at;
    if (m_at >= count()) {
        // Seen through to the end, which settles it exactly as skipping does:
        // both mean "do not open this by yourself again".
        m_at = -1;
        m_settled = true;
    }
}

void GuidedTour::skip()
{
    m_at = -1;
    m_settled = true;
}

const TourStep* GuidedTour::current() const
{
    if (!running()) {
        return nullptr;
    }

    return &m_steps[static_cast<std::size_t>(m_at)];
}

bool GuidedTour::onLastStep() const
{
    return running() && m_at == count() - 1;
}

void GuidedTour::restoreSettled(bool settled)
{
    m_settled = settled;
    if (!settled) {
        start();
    }
}

}  // namespace sidescopes
