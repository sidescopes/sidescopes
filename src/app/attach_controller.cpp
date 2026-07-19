#include "app/attach_controller.h"

#include <algorithm>
#include <cmath>
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
    setStoredFromAbsolute(*window, absoluteRegion, windowRect, display);
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

    setStoredFromAbsolute(*active, newAbsoluteRegion, windowRect, display);

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

    updateTracked(windows);
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

void AttachController::setStoredFromAbsolute(TrackedWindow& window, const RegionOfInterest& absoluteRegion,
                                             const AttachWindowRect& windowRect, const AttachDisplayRect& display)
{
    window.lastWindowRect = windowRect;
    window.observedOnce = true;
    if (windowRect.width <= 0.0 || windowRect.height <= 0.0 || display.width <= 0.0 || display.height <= 0.0) {
        window.left = windowRect.x;
        window.top = windowRect.y;
        window.right = windowRect.x + windowRect.width;
        window.bottom = windowRect.y + windowRect.height;

        return;
    }

    // The stored region lives inside its window.
    window.left = std::clamp(display.originX + absoluteRegion.leftPercent / 100.0 * display.width, windowRect.x,
                             windowRect.x + windowRect.width);
    window.right = std::clamp(display.originX + absoluteRegion.rightPercent / 100.0 * display.width, windowRect.x,
                              windowRect.x + windowRect.width);
    window.top = std::clamp(display.originY + absoluteRegion.topPercent / 100.0 * display.height, windowRect.y,
                            windowRect.y + windowRect.height);
    window.bottom = std::clamp(display.originY + absoluteRegion.bottomPercent / 100.0 * display.height, windowRect.y,
                               windowRect.y + windowRect.height);
}

void AttachController::bindStoredToWindow(TrackedWindow& window, const AttachWindowRect& windowRect)
{
    if (!window.observedOnce) {
        window.lastWindowRect = windowRect;
        window.observedOnce = true;

        return;
    }
    const AttachWindowRect& last = window.lastWindowRect;
    const bool sameSize =
        std::abs(windowRect.width - last.width) < 0.5 && std::abs(windowRect.height - last.height) < 0.5;
    if (sameSize) {
        // A move: the region rides along exactly.
        const double dx = windowRect.x - last.x;
        const double dy = windowRect.y - last.y;
        window.left += dx;
        window.right += dx;
        window.top += dy;
        window.bottom += dy;
    } else {
        // A resize: the region stays glued to the screen; an arriving edge
        // pushes it just enough to stay inside, per axis, permanently. The
        // push never changes the stored size - a window smaller than the
        // region only clips the emitted mapping, elastically.
        const double width = window.right - window.left;
        const double height = window.bottom - window.top;
        const double pushableWidth = std::min(width, windowRect.width);
        const double pushableHeight = std::min(height, windowRect.height);
        const double newLeft = std::clamp(window.left, windowRect.x, windowRect.x + windowRect.width - pushableWidth);
        const double newTop = std::clamp(window.top, windowRect.y, windowRect.y + windowRect.height - pushableHeight);
        window.left = newLeft;
        window.right = newLeft + width;
        window.top = newTop;
        window.bottom = newTop + height;
    }
    window.lastWindowRect = windowRect;
}

namespace {

bool sameRect(const AttachWindowRect& a, const AttachWindowRect& b)
{
    return std::abs(a.x - b.x) < 0.5 && std::abs(a.y - b.y) < 0.5 && std::abs(a.width - b.width) < 0.5 &&
           std::abs(a.height - b.height) < 0.5;
}

}  // namespace

void AttachController::updateTracked(const std::vector<TrackedWindowObservation>& windows)
{
    for (const TrackedWindowObservation& observation : windows) {
        TrackedWindow* window = find(observation.identity);
        if (window == nullptr || !observation.windowRect) {
            continue;
        }
        if (observation.minimized) {
            window->settling = true;

            continue;
        }
        if (window->settling) {
            // The re-entry animation's rectangles rebaseline silently;
            // two identical observations mean the window has landed.
            if (sameRect(window->lastWindowRect, *observation.windowRect)) {
                window->settling = false;
            } else {
                window->lastWindowRect = *observation.windowRect;
            }

            continue;
        }
        bindStoredToWindow(*window, *observation.windowRect);
    }
}

RegionOfInterest AttachController::toAbsolute(const TrackedWindow& window, const AttachWindowRect& windowRect,
                                              const AttachDisplayRect& display)
{
    RegionOfInterest region;
    if (display.width <= 0.0 || display.height <= 0.0) {
        return region;
    }

    // Elastic clip: only the emitted mapping is bounded by the window, so a
    // squeezed region re-expands when the window grows back.
    const double left = std::max(window.left, windowRect.x);
    const double right = std::min(window.right, windowRect.x + windowRect.width);
    const double top = std::max(window.top, windowRect.y);
    const double bottom = std::min(window.bottom, windowRect.y + windowRect.height);

    region.leftPercent = std::clamp((left - display.originX) / display.width * 100.0, 0.0, 100.0);
    region.topPercent = std::clamp((top - display.originY) / display.height * 100.0, 0.0, 100.0);
    region.rightPercent = std::clamp((right - display.originX) / display.width * 100.0, 0.0, 100.0);
    region.bottomPercent = std::clamp((bottom - display.originY) / display.height * 100.0, 0.0, 100.0);

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
        decision.activeTitle = observation.title;
        decision.region = toAbsolute(*active, *observation.windowRect, observation.display);

        return;
    }

    // The focused window is tracked but not visible right now (a race with a
    // minimize): not active.
    m_activeIdentity = 0;
}

}  // namespace sidescopes
