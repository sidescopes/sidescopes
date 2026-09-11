#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/frame_mailbox.h"

namespace sidescopes {

enum class CapturePermission
{
    Granted,
    Denied
};

/// A capture target is a whole display. Arbitrary-rectangle regions are
/// always cropped app-side: the least capable backend (Linux portals, where
/// the user picks a target and the app cannot request rectangles) defines the
/// contract, and richer backends may crop at the source only as an invisible
/// optimization.
struct CaptureTarget
{
    std::string identifier;   ///< Backend-specific, stable while connected.
    std::string description;  ///< Human-readable.
    /// The same display identity the desktop services speak
    /// (geometryOfDisplay, onScreenWindows, the region overlays). Capture
    /// backends and desktop services enumerate displays through different
    /// APIs; this field is the bridge, resolved by the backend.
    uint32_t displayId = 0;
    int widthPoints = 0;
    int heightPoints = 0;
};

/// Implemented per platform. Frames arrive in the mailbox from a
/// backend-owned thread. Status messages (including stream death — capture is
/// a service that can die at any time, e.g. on lock screen) arrive via the
/// callback, possibly from any thread; the application restarts on failure.
class ScreenCaptureSource
{
public:
    using StatusCallback = std::function<void(const std::string& message)>;

    virtual ~ScreenCaptureSource() = default;

    /// May prompt the user on first call.
    [[nodiscard]] virtual CapturePermission requestPermission() = 0;

    virtual std::vector<CaptureTarget> listTargets() = 0;

    /// Starts delivering @p target's frames into the mailbox; false on failure.
    /// Each delivery retains @p captureEpoch from this call, including callbacks
    /// already in flight when a replacement stream starts.
    [[nodiscard]] virtual bool start(const CaptureTarget& target, int maxFramesPerSecond, FrameMailbox& mailbox,
                                     uint64_t captureEpoch = 0) = 0;

    virtual void stop() = 0;

    virtual void setStatusCallback(StatusCallback callback) = 0;

    /// Selects whether the next stream should retain HDR luminance. Called
    /// only while stopped. Unsupported sources continue SDR capture and
    /// leave the optional luminance plane absent.
    virtual void setHdrEnabled(bool enabled)
    {
        (void)enabled;
    }

    /// Asks the running stream to deliver only @p rect of the display, in display
    /// pixels, or the whole display when nothing is passed. Frames report which
    /// part of the display they carry either way, so narrowing never moves what a
    /// scope reads; see FrameView.
    ///
    /// Best-effort by design: a backend that cannot narrow keeps delivering the
    /// whole display, which is correct rather than merely tolerable - the region
    /// is still resolved against the display, so only the cost is unimproved. That
    /// is the default. macOS narrows the stream itself; Windows, whose duplication
    /// has no source rectangle, duplicates the whole display and copies only the
    /// rectangle out of it.
    virtual void narrowTo(const std::optional<IntRect>& rect)
    {
        (void)rect;
    }
};

/// Creates the platform's screen-capture source.
[[nodiscard]] std::unique_ptr<ScreenCaptureSource> createScreenCaptureSource();

}  // namespace sidescopes
