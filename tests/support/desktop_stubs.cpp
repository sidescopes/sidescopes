#include "desktop_stubs.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
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

extern "C" void glfwWaitEventsTimeout(double)
{
}

namespace sidescopes {

namespace {

test::DesktopStubs g_stubs;

}  // namespace

bool supportsWindowAttach()
{
    return true;
}

int64_t ownApplicationPid()
{
    return g_stubs.ownPid;
}

int64_t foregroundApplicationPid()
{
    return g_stubs.foregroundPid;
}

std::optional<uint64_t> focusedAttachedWindow(int64_t, const std::vector<uint64_t>&)
{
    return g_stubs.focusedWindow;
}

void watchWindowMotion(uint64_t identity, int64_t, std::function<void(WindowMotionSignal)> callback)
{
    g_stubs.watchedWindow = identity;
    g_stubs.windowMotion = std::move(callback);
}

void unwatchWindowMotion()
{
    g_stubs.watchedWindow = 0;
    g_stubs.windowMotion = {};
}

void raiseWindow(uint64_t identity, int64_t)
{
    g_stubs.raisedWindow = identity;
}

std::vector<DesktopWindow> onScreenWindows(uint32_t)
{
    return g_stubs.onScreenWindows;
}

std::vector<DesktopWindow> attachCandidateWindows(uint32_t)
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

std::optional<uint32_t> displayAtPoint(DesktopPoint point)
{
    g_stubs.lastDisplayPoint = point;

    return g_stubs.cursorDisplay;
}

std::optional<CapturedImage> captureDisplayImage(uint32_t)
{
    return g_stubs.displayImage;
}

void sampleScreenColorAsync(DesktopPoint, std::function<void(std::optional<FloatColor>)> callback)
{
    // The seam takes the callback by value because a real implementation hands
    // it to whatever reads the screen; taking ownership here mirrors that. It
    // allows a synchronous answer where the read is immediate, which is what
    // the tests use.
    ++g_stubs.screenSampleRequests;
    const std::function<void(std::optional<FloatColor>)> reader = std::move(callback);
    reader(g_stubs.screenSample);
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

// The context menu reads and writes the capture's own visibility, and offers
// the diagnostic log folder. Nothing under test asserts on them, so they are
// the plainest stubs that link: a flag and a discarded url.
void setCaptureVisibility(bool visible)
{
    g_stubs.captureVisible = visible;
}

bool captureVisible()
{
    return g_stubs.captureVisible;
}

bool captureVisibilityToggleSupported()
{
    return true;
}

bool platformHidesWindowOnCommandW()
{
    return g_stubs.hidesWindowOnCommandW;
}

bool platformMinimizesWindowOnControlW()
{
    return g_stubs.minimizesWindowOnControlW;
}

bool platformQuitsOnControlQ()
{
    return g_stubs.quitsOnControlQ;
}

void openUrl(const char*)
{
}

void unobserveSystemEvents()
{
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
    lastDisplayPoint.reset();
    displayImage.reset();
    faceDetectionSupported = false;
    faces.clear();
    applicationHidden = false;
    displayName = "Test display";
    captureVisible = false;
    hidesWindowOnCommandW = false;
    minimizesWindowOnControlW = false;
    quitsOnControlQ = false;
    ownPid = 100;
    foregroundPid = 0;
    focusedWindow.reset();
    watchedWindow = 0;
    raisedWindow = 0;
    windowMotion = {};
    screenSample.reset();
    screenSampleRequests = 0;
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
    if (view.pixels != nullptr && view.width > 0 && view.height > 0) {
        for (std::size_t byte = 0; byte < m_detected.firstPixel.size(); ++byte) {
            m_detected.firstPixel[byte] = view.pixels[byte];
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
