#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/frame.h"
#include "platform/desktop.h"

namespace sidescopes::test {

// The desktop and face-detection seams the application controllers reach
// through, scripted in one place. Several suites drive controllers that share
// these seams, and a translation unit may define each of them only once, so
// the stubs live here rather than in whichever suite happened to need them
// first.

/// What one detector call was handed, so a caller that hands the detector a
/// cropped region can be judged on the crop itself: its size, its density, and
/// its first pixel, which over a frame whose pixels encode their coordinates
/// says where in that frame the crop was taken from. Written from the probe
/// threads the controllers detach, so it is read back under the same lock.
struct DetectorCall
{
    int calls = 0;
    int width = 0;
    int height = 0;
    float pixelsPerPoint = 0.0f;
    std::array<uint8_t, 4> firstPixel{};
};

/// The scripted desktop: what it reports and what the detector finds. One
/// instance serves the whole test binary, so every suite touching it resets
/// it first.
class DesktopStubs
{
public:
    std::optional<DisplayGeometry> displayGeometry;
    std::optional<WindowGeometry> windowGeometry;
    std::vector<DesktopWindow> onScreenWindows;
    std::optional<DesktopPoint> cursor;
    std::optional<uint32_t> cursorDisplay;
    std::optional<CapturedImage> displayImage;

    bool faceDetectionSupported = false;
    std::vector<IntRect> faces;

    /// Puts every answer back to its empty default and forgets what the
    /// detector was handed.
    void reset();

    /// Records one detector call; safe from a detached probe thread.
    void recordDetection(const FrameView& view, float pixelsPerPoint);

    /// What the detector was handed last, and how often it was called.
    [[nodiscard]] DetectorCall detectorCall() const;

private:
    mutable std::mutex m_mutex;
    DetectorCall m_detected;
};

/// The one scripted desktop the stubs answer from.
[[nodiscard]] DesktopStubs& desktopStubs();

}  // namespace sidescopes::test
