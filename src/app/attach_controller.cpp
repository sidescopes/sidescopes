#include "app/attach_controller.h"

#include <algorithm>
#include <utility>

namespace sidescopes {

bool AttachController::attached() const
{
    return !m_windows.empty();
}

std::size_t AttachController::trackedCount() const
{
    return m_windows.size();
}

std::vector<uint64_t> AttachController::trackedIdentities() const
{
    std::vector<uint64_t> identities;
    identities.reserve(m_windows.size());
    for (const TrackedWindow& window : m_windows) {
        identities.push_back(window.identity);
    }

    return identities;
}

bool AttachController::tracks(uint64_t identity) const
{
    return find(identity) != nullptr;
}

uint64_t AttachController::activeIdentity() const
{
    return m_activeIdentity;
}

std::string AttachController::activeApplicationName() const
{
    const TrackedWindow* active = find(m_activeIdentity);

    return active ? active->applicationName : std::string();
}

RegionOfInterest AttachController::attach(uint64_t identity, int64_t ownerPid, std::string applicationName,
                                          AttachWindowRect windowRect, AttachDisplayRect display,
                                          const RegionOfInterest& absoluteRegion)
{
    TrackedWindow* window = find(identity);
    if (!window) {
        m_windows.push_back(TrackedWindow{});
        window = &m_windows.back();
        window->identity = identity;
    }
    window->ownerPid = ownerPid;
    window->applicationName = std::move(applicationName);
    setRelativeFromAbsolute(*window, absoluteRegion, windowRect, display);
    m_activeIdentity = identity;

    return toAbsolute(*window, windowRect, display);
}

RegionOfInterest AttachController::editRegion(const RegionOfInterest& newAbsoluteRegion, AttachWindowRect windowRect,
                                              AttachDisplayRect display)
{
    TrackedWindow* active = find(m_activeIdentity);
    if (!active) {
        return newAbsoluteRegion;
    }

    setRelativeFromAbsolute(*active, newAbsoluteRegion, windowRect, display);

    return toAbsolute(*active, windowRect, display);
}

AttachDecision AttachController::observe(const std::vector<TrackedWindowObservation>& windows,
                                         std::optional<uint64_t> focusedWindow)
{
    AttachDecision decision;
    if (m_windows.empty()) {
        return decision;
    }

    pruneClosed(windows, decision);
    if (m_windows.empty()) {
        decision.detachedAll = true;
        m_activeIdentity = 0;

        return decision;
    }

    m_activeIdentity = focusedWindow && find(*focusedWindow) ? *focusedWindow : 0;
    updateRegion(windows, decision);

    return decision;
}

void AttachController::remove(uint64_t identity)
{
    const auto matches = [identity](const TrackedWindow& window) { return window.identity == identity; };
    m_windows.erase(std::remove_if(m_windows.begin(), m_windows.end(), matches), m_windows.end());
    if (m_activeIdentity == identity) {
        m_activeIdentity = 0;
    }
}

void AttachController::detachAll()
{
    m_windows.clear();
    m_activeIdentity = 0;
}

AttachController::TrackedWindow* AttachController::find(uint64_t identity)
{
    for (TrackedWindow& window : m_windows) {
        if (window.identity == identity) {
            return &window;
        }
    }

    return nullptr;
}

const AttachController::TrackedWindow* AttachController::find(uint64_t identity) const
{
    for (const TrackedWindow& window : m_windows) {
        if (window.identity == identity) {
            return &window;
        }
    }

    return nullptr;
}

void AttachController::setRelativeFromAbsolute(TrackedWindow& window, const RegionOfInterest& absoluteRegion,
                                               const AttachWindowRect& windowRect, const AttachDisplayRect& display)
{
    if (windowRect.width <= 0.0 || windowRect.height <= 0.0 || display.width <= 0.0 || display.height <= 0.0) {
        window.relativeLeft = 0.0;
        window.relativeTop = 0.0;
        window.relativeRight = 1.0;
        window.relativeBottom = 1.0;

        return;
    }

    const double leftDesktop = display.originX + absoluteRegion.leftPercent / 100.0 * display.width;
    const double topDesktop = display.originY + absoluteRegion.topPercent / 100.0 * display.height;
    const double rightDesktop = display.originX + absoluteRegion.rightPercent / 100.0 * display.width;
    const double bottomDesktop = display.originY + absoluteRegion.bottomPercent / 100.0 * display.height;

    window.relativeLeft = std::clamp((leftDesktop - windowRect.x) / windowRect.width, 0.0, 1.0);
    window.relativeTop = std::clamp((topDesktop - windowRect.y) / windowRect.height, 0.0, 1.0);
    window.relativeRight = std::clamp((rightDesktop - windowRect.x) / windowRect.width, 0.0, 1.0);
    window.relativeBottom = std::clamp((bottomDesktop - windowRect.y) / windowRect.height, 0.0, 1.0);
}

RegionOfInterest AttachController::toAbsolute(const TrackedWindow& window, const AttachWindowRect& windowRect,
                                              const AttachDisplayRect& display)
{
    RegionOfInterest region;
    if (display.width <= 0.0 || display.height <= 0.0) {
        return region;
    }

    const double leftDesktop = windowRect.x + window.relativeLeft * windowRect.width;
    const double topDesktop = windowRect.y + window.relativeTop * windowRect.height;
    const double rightDesktop = windowRect.x + window.relativeRight * windowRect.width;
    const double bottomDesktop = windowRect.y + window.relativeBottom * windowRect.height;

    region.leftPercent = std::clamp((leftDesktop - display.originX) / display.width * 100.0, 0.0, 100.0);
    region.topPercent = std::clamp((topDesktop - display.originY) / display.height * 100.0, 0.0, 100.0);
    region.rightPercent = std::clamp((rightDesktop - display.originX) / display.width * 100.0, 0.0, 100.0);
    region.bottomPercent = std::clamp((bottomDesktop - display.originY) / display.height * 100.0, 0.0, 100.0);

    return region;
}

void AttachController::pruneClosed(const std::vector<TrackedWindowObservation>& windows, AttachDecision& decision)
{
    for (const TrackedWindowObservation& observation : windows) {
        if (observation.windowRect.has_value()) {
            continue;
        }
        const auto matches = [&](const TrackedWindow& window) { return window.identity == observation.identity; };
        const auto removed = std::remove_if(m_windows.begin(), m_windows.end(), matches);
        if (removed != m_windows.end()) {
            m_windows.erase(removed, m_windows.end());
            ++decision.closedCount;
        }
    }
}

void AttachController::updateRegion(const std::vector<TrackedWindowObservation>& windows, AttachDecision& decision)
{
    const TrackedWindow* active = find(m_activeIdentity);
    if (!active) {
        return;
    }

    for (const TrackedWindowObservation& observation : windows) {
        if (observation.identity != m_activeIdentity || !observation.windowRect || observation.minimized) {
            continue;
        }
        decision.activeIdentity = m_activeIdentity;
        decision.activeOwnerPid = active->ownerPid;
        decision.activeDisplayId = observation.displayId;
        decision.activeRect = observation.windowRect;
        decision.region = toAbsolute(*active, *observation.windowRect, observation.display);

        return;
    }

    // The focused window is tracked but not visible right now (a race with a
    // minimize): not active.
    m_activeIdentity = 0;
}

}  // namespace sidescopes
