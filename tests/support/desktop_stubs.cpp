#include "desktop_stubs.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/frame.h"
#include "platform/desktop.h"
#include "platform/face_detection.h"

// The test binary links no windowing library, so the wake the controllers post
// when a background thread lands is answered here. The clock is frozen: a
// controller that waits on it must be driven with the wait already satisfied.
extern "C" void glfwPostEmptyEvent(void)
{
}

extern "C" double glfwGetTime(void)
{
    return 0.0;
}

namespace sidescopes {

namespace {

test::DesktopStubs g_stubs;

}  // namespace

std::vector<DesktopWindow> onScreenWindows(uint32_t)
{
    return g_stubs.onScreenWindows;
}

std::optional<WindowGeometry> windowGeometry(uint64_t)
{
    return g_stubs.windowGeometry;
}

std::optional<DesktopPoint> globalCursorPosition()
{
    return g_stubs.cursor;
}

std::optional<DisplayGeometry> geometryOfDisplay(uint32_t)
{
    return g_stubs.displayGeometry;
}

std::optional<uint32_t> displayAtPoint(DesktopPoint)
{
    return g_stubs.cursorDisplay;
}

std::optional<CapturedImage> captureDisplayImage(uint32_t)
{
    return g_stubs.displayImage;
}

bool applicationHidden()
{
    return g_stubs.applicationHidden;
}

std::string displayName(uint32_t)
{
    return g_stubs.displayName;
}

bool supportsFaceDetection()
{
    return g_stubs.faceDetectionSupported;
}

std::vector<IntRect> detectFaces(const FrameView& view, float pixelsPerPoint)
{
    g_stubs.recordDetection(view, pixelsPerPoint);

    return g_stubs.faces;
}

namespace test {

void DesktopStubs::reset()
{
    displayGeometry.reset();
    windowGeometry.reset();
    onScreenWindows.clear();
    cursor.reset();
    cursorDisplay.reset();
    displayImage.reset();
    faceDetectionSupported = false;
    faces.clear();
    applicationHidden = false;
    displayName = "Test display";
    const std::lock_guard lock(m_mutex);
    m_detected = DetectorCall{};
}

void DesktopStubs::recordDetection(const FrameView& view, float pixelsPerPoint)
{
    const std::lock_guard lock(m_mutex);
    ++m_detected.calls;
    m_detected.width = view.width;
    m_detected.height = view.height;
    m_detected.pixelsPerPoint = pixelsPerPoint;
    m_detected.firstPixel = {};
    if (view.bgra != nullptr && view.width > 0 && view.height > 0) {
        for (std::size_t byte = 0; byte < m_detected.firstPixel.size(); ++byte) {
            m_detected.firstPixel[byte] = view.bgra[byte];
        }
    }
}

DetectorCall DesktopStubs::detectorCall() const
{
    const std::lock_guard lock(m_mutex);

    return m_detected;
}

DesktopStubs& desktopStubs()
{
    return g_stubs;
}

}  // namespace test

}  // namespace sidescopes
