#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/frame_mailbox.h"

namespace sidescopes {

enum class CapturePermission { Granted, Denied };

// A capture target is a whole display. Arbitrary-rectangle regions are
// always cropped app-side: the least capable backend (Linux portals, where
// the user picks a target and the app cannot request rectangles) defines the
// contract, and richer backends may crop at the source only as an invisible
// optimization.
struct CaptureTarget {
    std::string identifier;   // backend-specific, stable while connected
    std::string description;  // human-readable
    int width_points = 0;
    int height_points = 0;
};

// Implemented per platform. Frames arrive in the mailbox from a
// backend-owned thread. Status messages (including stream death — capture is
// a service that can die at any time, e.g. on lock screen) arrive via the
// callback, possibly from any thread; the application restarts on failure.
class ScreenCaptureSource {
public:
    using StatusCallback = std::function<void(const std::string& message)>;

    virtual ~ScreenCaptureSource() = default;

    // May prompt the user on first call.
    virtual CapturePermission RequestPermission() = 0;

    virtual std::vector<CaptureTarget> ListTargets() = 0;

    virtual bool Start(const CaptureTarget& target, int max_frames_per_second,
                       FrameMailbox& mailbox) = 0;
    virtual void Stop() = 0;

    virtual void SetStatusCallback(StatusCallback callback) = 0;
};

std::unique_ptr<ScreenCaptureSource> CreateScreenCaptureSource();

}  // namespace sidescopes
