#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/frame_mailbox.h"

namespace sidescopes {

enum class CapturePermission
{
    Granted,
    Denied
};

// A capture target is a whole display. Arbitrary-rectangle regions are
// always cropped app-side: the least capable backend (Linux portals, where
// the user picks a target and the app cannot request rectangles) defines the
// contract, and richer backends may crop at the source only as an invisible
// optimization.
struct CaptureTarget
{
    std::string identifier;   // backend-specific, stable while connected
    std::string description;  // human-readable
    // The same display identity the desktop services speak
    // (GeometryOfDisplay, OnScreenWindows, the region overlays). Capture
    // backends and desktop services enumerate displays through different
    // APIs; this field is the bridge, resolved by the backend.
    uint32_t displayId = 0;
    int widthPoints = 0;
    int heightPoints = 0;
};

// Implemented per platform. Frames arrive in the mailbox from a
// backend-owned thread. Status messages (including stream death — capture is
// a service that can die at any time, e.g. on lock screen) arrive via the
// callback, possibly from any thread; the application restarts on failure.
class ScreenCaptureSource
{
public:
    using StatusCallback = std::function<void(const std::string& message)>;

    virtual ~ScreenCaptureSource() = default;

    // May prompt the user on first call.
    virtual CapturePermission requestPermission() = 0;

    virtual std::vector<CaptureTarget> listTargets() = 0;

    virtual bool start(const CaptureTarget& target, int maxFramesPerSecond, FrameMailbox& mailbox) = 0;
    virtual void stop() = 0;

    virtual void setStatusCallback(StatusCallback callback) = 0;
};

std::unique_ptr<ScreenCaptureSource> createScreenCaptureSource();

}  // namespace sidescopes
