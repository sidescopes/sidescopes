#include "app/face_motion.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {

void FaceMotion::reset(FaceLockState crop)
{
    m_crop = crop;
    pause();
}

void FaceMotion::pause()
{
    m_sourceSeconds.reset();
}

FaceAnchor FaceMotion::current() const
{
    return m_crop.lastAnchor;
}

FaceAnchor FaceMotion::follow(FaceAnchor target, double sourceSeconds)
{
    const double dt = m_sourceSeconds ? sourceSeconds - *m_sourceSeconds : 0.0;
    auto& anchor = m_crop.lastAnchor;
    if (m_sourceSeconds && dt <= 0.0) {
        return anchor;
    }
    if (!m_sourceSeconds || dt > 0.20) {
        anchor = target;
    } else if (dt > 0.0) {
        const auto before = face_lock::mapRegion(m_crop, anchor);
        const auto after = face_lock::mapRegion(m_crop, target);
        const double residual =
            std::max({std::abs(after.left - before.left), std::abs(after.top - before.top),
                      std::abs(after.right - before.right), std::abs(after.bottom - before.bottom)}) /
            target.width;
        // Small detector changes need damping; a deliberate pan or zoom
        // needs a prompt response. This curve has no velocity prediction,
        // frame queue or overshoot. A capture source may withhold the final
        // unchanged image, so cap the remaining offset at 2% of face width
        // rather than promise convergence from frames that may never arrive.
        constexpr double TimeConstant = 0.15;
        constexpr double Reach = 0.04;
        constexpr double MaximumResidual = 0.02;
        const double relative = residual / Reach;
        const double alpha = std::max(-std::expm1(-dt / TimeConstant * (1.0 + relative * relative)),
                                      residual > MaximumResidual ? 1.0 - MaximumResidual / residual : 0.0);
        anchor = {anchor.centerX + alpha * (target.centerX - anchor.centerX),
                  anchor.centerY + alpha * (target.centerY - anchor.centerY),
                  anchor.width + alpha * (target.width - anchor.width)};
    }
    m_sourceSeconds = sourceSeconds;
    return anchor;
}

}  // namespace sidescopes
