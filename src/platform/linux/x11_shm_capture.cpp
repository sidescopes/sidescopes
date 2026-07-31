// Screen capture on a pure X11 session: MIT-SHM reads of the root window at
// the quality level's cadence, the direct path the X server offers with no
// portal, no PipeWire, and no consent dialog. The frame layout, the source
// stamp, and the narrow-to-region contract are the same ones the macOS and
// Windows backends fill, so nothing downstream tells the platforms apart.
//
// Delivery is poll-and-diff rather than damage-driven: XShmGetImage of the
// root always returns the true composited pixels on every window manager and
// compositor, where XDamage on the root reports differently across them. The
// grab happens every tick; the publish only when the pixels changed, so a
// static screen wakes no analysis pass. The grab's own cost (a server-side
// copy into shared memory) is the price of that robustness, and narrowing to a
// region shrinks it to the region. XDamage is a possible future cut to be
// measured on real compositor hardware, not guessed at here.

#include "platform/linux/x11_shm_capture.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "platform/linux/x11_displays.h"
#include "platform/linux/x11_error_guard.h"
#include "platform/linux/x11_pixels.h"
#include "platform/linux/x11_shm_geometry.h"

namespace sidescopes {
namespace {

/// What the last published frame was, so a tick can tell whether it has
/// anything new to send: the pixels (for the change test), the source stamp
/// (so a region MOVED to identical-looking pixels still republishes its new
/// origin), and the running sequence number.
struct PublishState
{
    std::vector<uint8_t> bytes;
    GrabRect grab;
    uint64_t sequence = 0;
};

/// Whether two grabs cover the same part of the display - same crop origin and
/// same display extents. Pixels can match across a region move; the stamp must
/// not be carried forward when it does.
bool sameSource(const GrabRect& a, const GrabRect& b)
{
    return a.sourceX == b.sourceX && a.sourceY == b.sourceY && a.sourceWidth == b.sourceWidth &&
           a.sourceHeight == b.sourceHeight;
}

/// Owns the capture thread's own X connection, closed when the loop ends. It
/// is declared before the shared-memory surface so it OUTLIVES it: the
/// surface's destructor detaches its segment from this connection, so closing
/// the connection first would leave XShmDetach reading freed memory.
struct DisplayHandle
{
    explicit DisplayHandle(Display* display)
        : handle(display)
    {
    }

    DisplayHandle(const DisplayHandle&) = delete;
    DisplayHandle& operator=(const DisplayHandle&) = delete;

    ~DisplayHandle()
    {
        if (handle != nullptr) {
            XCloseDisplay(handle);
        }
    }

    Display* handle = nullptr;
};

/// A shared-memory XImage of a chosen size, recreated when the grab size
/// changes (a narrow to a region, or back). Owns the segment and the image
/// together so neither can outlive the other.
class ShmSurface
{
public:
    ShmSurface(Display* display, Visual* visual, int depth)
        : m_display(display),
          m_visual(visual),
          m_depth(depth)
    {
    }

    ShmSurface(const ShmSurface&) = delete;
    ShmSurface& operator=(const ShmSurface&) = delete;

    ~ShmSurface()
    {
        release();
    }

    /// Ensures the surface is @p width by @p height, recreating it when the
    /// size changed. @return The image to grab into, or null when shared memory
    /// could not be set up (the caller then uses the plain path).
    XImage* ensure(int width, int height)
    {
        if (m_image != nullptr && m_image->width == width && m_image->height == height) {
            return m_image;
        }
        release();
        return create(width, height);
    }

private:
    XImage* create(int width, int height)
    {
        m_image = XShmCreateImage(m_display, m_visual, static_cast<unsigned int>(m_depth), ZPixmap, nullptr, &m_info,
                                  static_cast<unsigned int>(width), static_cast<unsigned int>(height));
        if (m_image == nullptr) {
            return nullptr;
        }
        m_info.shmid =
            shmget(IPC_PRIVATE, static_cast<std::size_t>(m_image->bytes_per_line) * m_image->height, IPC_CREAT | 0600);
        if (m_info.shmid == -1) {
            return abandon();
        }
        m_info.shmaddr = static_cast<char*>(shmat(m_info.shmid, nullptr, 0));
        if (m_info.shmaddr == reinterpret_cast<char*>(-1)) {
            // Attach to this process failed, so nothing will ever detach it and
            // drop the segment; remove it here or it leaks kernel-side.
            shmctl(m_info.shmid, IPC_RMID, nullptr);
            return abandon();
        }
        m_image->data = m_info.shmaddr;
        m_info.readOnly = False;
        // Marked for destruction the moment it is attached, BEFORE the server
        // attach that may fail: the segment then survives only as long as an
        // attachment holds it and is freed on the last detach - so the release()
        // path after a refused XShmAttach (a remote display) cannot leak it, and
        // neither can a crash. shmdt in release() is what finally drops it.
        shmctl(m_info.shmid, IPC_RMID, nullptr);
        if (!attach()) {
            return abandon();
        }
        m_attached = true;

        return m_image;
    }

    bool attach()
    {
        // The server refuses a shared-memory attach on a remote display with a
        // BadAccess, which the shared non-fatal handler catches; without it the
        // default handler would exit the application. failed() drains the reply
        // so the verdict is the attach's own, not a later request's.
        const X11ErrorGuard guard(m_display);
        const bool asked = XShmAttach(m_display, &m_info) != 0;

        return asked && !guard.failed();
    }

    /// Tears down a half-built surface after a failed step and returns null,
    /// so create() reports "no shared memory" in one place.
    XImage* abandon()
    {
        release();

        return nullptr;
    }

    void release()
    {
        if (m_attached) {
            XShmDetach(m_display, &m_info);
            m_attached = false;
        }
        if (m_image != nullptr) {
            XDestroyImage(m_image);
            m_image = nullptr;
        }
        if (m_info.shmaddr != nullptr && m_info.shmaddr != reinterpret_cast<char*>(-1)) {
            shmdt(m_info.shmaddr);
            m_info.shmaddr = nullptr;
        }
    }

    Display* m_display = nullptr;
    Visual* m_visual = nullptr;
    int m_depth = 0;
    XImage* m_image = nullptr;
    XShmSegmentInfo m_info{};
    bool m_attached = false;
};

/// Copies an XImage into the tight BGRA buffer @p out, sizing it first. The
/// common truecolor visual is a per-row memcpy; anything else unpacks per
/// pixel through the channel masks. Alpha is forced opaque - the scopes ignore
/// it, and BGRx leaves the fourth byte undefined.
void toBgra(const XImage* image, int width, int height, std::vector<uint8_t>& out)
{
    const int stride = width * 4;
    out.resize(static_cast<std::size_t>(stride) * height);
    if (isDirectBgraLayout(image)) {
        for (int row = 0; row < height; ++row) {
            std::memcpy(out.data() + static_cast<std::size_t>(row) * stride,
                        image->data + static_cast<std::size_t>(row) * image->bytes_per_line,
                        static_cast<std::size_t>(stride));
        }
        return;
    }
    for (int py = 0; py < height; ++py) {
        uint8_t* rowOut = out.data() + static_cast<std::size_t>(py) * stride;
        for (int px = 0; px < width; ++px) {
            uint8_t* pixel = rowOut + static_cast<std::size_t>(px) * 4;
            unpackPixelBgra(image, XGetPixel(const_cast<XImage*>(image), px, py), pixel[0], pixel[1], pixel[2]);
            pixel[3] = 255;
        }
    }
}

/// The display the target names, or nothing when it is no longer connected.
/// Read through the desktop services' shared connection, so it is resolved on
/// the main thread at start() and handed to the capture thread - never called
/// from the capture thread, which owns a connection of its own and must not
/// touch this one (Xlib serialises nothing between threads).
std::optional<DisplayGeometry> displayGeometryOf(uint32_t displayId)
{
    for (const LinuxDisplay& candidate : connectedDisplays()) {
        if (candidate.id == displayId) {
            return candidate.geometry;
        }
    }

    return std::nullopt;
}

class X11ShmScreenCapture final : public ScreenCaptureSource
{
public:
    ~X11ShmScreenCapture() override
    {
        stop();
    }

    CapturePermission requestPermission() override
    {
        // The X server hands its own clients the screen; there is nothing to
        // ask for and no dialog to raise.
        return CapturePermission::Granted;
    }

    std::vector<CaptureTarget> listTargets() override
    {
        std::vector<CaptureTarget> targets;
        for (const LinuxDisplay& display : connectedDisplays()) {
            CaptureTarget target;
            target.identifier = std::to_string(display.id);
            target.description = display.name;
            target.displayId = display.id;
            target.widthPoints = static_cast<int>(display.geometry.widthPoints);
            target.heightPoints = static_cast<int>(display.geometry.heightPoints);
            targets.push_back(std::move(target));
        }

        return targets;
    }

    bool start(const CaptureTarget& target, int maxFramesPerSecond, FrameMailbox& mailbox) override
    {
        stop();
        // The display's geometry is resolved here, on the main thread, through
        // the desktop services' shared X connection; the capture thread must
        // never touch that connection, so it is snapshotted and handed over. A
        // resolution change under the stream makes the grab fail, which ends
        // the stream and the controller restarts it, re-reading the geometry.
        const std::optional<DisplayGeometry> geometry = displayGeometryOf(target.displayId);
        if (!geometry) {
            report("display unavailable");
            return false;
        }
        m_geometry = *geometry;
        m_stopRequested.store(false);
        m_worker = std::thread([this, maxFramesPerSecond, &mailbox] { captureLoop(maxFramesPerSecond, mailbox); });

        return true;
    }

    void narrowTo(const std::optional<IntRect>& rect) override
    {
        std::lock_guard lock(m_cropMutex);
        m_crop = rect;
    }

    void stop() override
    {
        m_stopRequested.store(true);
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    void setStatusCallback(StatusCallback callback) override
    {
        m_status = std::move(callback);
    }

private:
    void report(const std::string& message)
    {
        if (m_status) {
            m_status(message);
        }
    }

    std::optional<IntRect> currentCrop()
    {
        std::lock_guard lock(m_cropMutex);

        return m_crop;
    }

    /// The capture thread: its own X connection, a shared-memory surface, and
    /// a poll-and-diff loop paced to the cadence. Everything it owns lives and
    /// dies here, so nothing crosses to the main thread but the published
    /// frame and the status callback.
    void captureLoop(int maxFramesPerSecond, FrameMailbox& mailbox)
    {
        // The connection holder is declared first so it is destroyed last -
        // after the surface, whose destructor detaches its segment from this
        // connection.
        const DisplayHandle connection(XOpenDisplay(nullptr));
        Display* display = connection.handle;
        if (display == nullptr) {
            report("capture could not open the X display");
            return;
        }
        // The process-wide non-fatal error handler, installed before any X call
        // on this thread: a stray protocol error (a shared-memory attach the
        // server refuses, a read across a display disconnect) is then caught
        // rather than exiting the whole application through the default handler.
        ensureX11ErrorHandler();
        const int screen = DefaultScreen(display);
        ShmSurface surface(display, DefaultVisual(display, screen), DefaultDepth(display, screen));
        const ::Window root = RootWindow(display, screen);
        const auto interval = std::chrono::duration<double>(1.0 / std::max(maxFramesPerSecond, 1));
        FrameBuffer buffer;
        std::vector<uint8_t> latest;
        PublishState published;
        while (!m_stopRequested.load()) {
            const auto tickStart = std::chrono::steady_clock::now();
            if (!grabAndPublish(display, root, surface, mailbox, buffer, latest, published)) {
                break;
            }
            std::this_thread::sleep_until(tickStart + std::chrono::duration_cast<std::chrono::nanoseconds>(interval));
        }
        mailbox.returnStorage(std::move(buffer));
    }

    /// One tick: grab the current rectangle, and publish it only when the
    /// pixels differ from the last frame sent. @return False on a fatal grab
    /// failure (the display vanished), which ends the loop and the stream, so
    /// the controller's status callback reports the stop.
    bool grabAndPublish(Display* display, ::Window root, ShmSurface& surface, FrameMailbox& mailbox,
                        FrameBuffer& buffer, std::vector<uint8_t>& latest, PublishState& published)
    {
        const GrabRect grab = computeGrab(m_geometry, currentCrop());
        // The shared-memory surface is reused across ticks; the plain fallback
        // allocates a fresh image this tick and is freed below. Which one it is
        // decides ownership, so it is tracked here rather than inferred later.
        XImage* image = surface.ensure(grab.width, grab.height);
        const bool ownsImage = image == nullptr;
        if (image != nullptr) {
            if (XShmGetImage(display, root, image, grab.rootX, grab.rootY, AllPlanes) == 0) {
                report("capture read failed");
                return false;
            }
        } else {
            image = XGetImage(display, root, grab.rootX, grab.rootY, static_cast<unsigned int>(grab.width),
                              static_cast<unsigned int>(grab.height), AllPlanes, ZPixmap);
            if (image == nullptr) {
                report("capture read failed");
                return false;
            }
        }
        toBgra(image, grab.width, grab.height, latest);
        if (ownsImage) {
            XDestroyImage(image);
        }
        publishIfChanged(mailbox, buffer, grab, latest, published);

        return true;
    }

    static void publishIfChanged(FrameMailbox& mailbox, FrameBuffer& buffer, const GrabRect& grab,
                                 std::vector<uint8_t>& latest, PublishState& published)
    {
        // A static screen sends nothing, the same as the damage-driven
        // backends: an unchanged grab is dropped before it wakes the analysis.
        // But a region moved to identical-looking pixels still carries a new
        // source stamp, so the stamp is part of the "unchanged" test.
        if (latest == published.bytes && sameSource(grab, published.grab)) {
            return;
        }
        buffer.sizeTo(latest.size());
        std::memcpy(buffer.data.data(), latest.data(), latest.size());
        buffer.strideBytes = grab.width * 4;
        buffer.width = grab.width;
        buffer.height = grab.height;
        buffer.colorSpace = ColorSpaceHint::Srgb;
        buffer.format = PixelFormat::Bgra8;
        buffer.sequence = ++published.sequence;
        buffer.sourceX = grab.sourceX;
        buffer.sourceY = grab.sourceY;
        buffer.sourceWidth = grab.sourceWidth;
        buffer.sourceHeight = grab.sourceHeight;
        buffer = mailbox.publish(std::move(buffer));
        published.bytes = latest;
        published.grab = grab;
    }

    std::thread m_worker;
    std::atomic<bool> m_stopRequested{true};
    // The target display's geometry, snapshotted on the main thread in start()
    // and read only by the capture thread thereafter.
    DisplayGeometry m_geometry;
    std::mutex m_cropMutex;
    std::optional<IntRect> m_crop;
    StatusCallback m_status;
};

}  // namespace

std::unique_ptr<ScreenCaptureSource> createX11ShmScreenCaptureSource()
{
    return std::make_unique<X11ShmScreenCapture>();
}

}  // namespace sidescopes
