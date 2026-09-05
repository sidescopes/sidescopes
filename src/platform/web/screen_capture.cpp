// The browser's capture source: a real one, fed by the host.
//
// A page cannot read another program's window, so there is no screen to
// capture and there never will be. What there IS, is pixels the host can
// hand over - the photograph the lab's visitor chose, or, in a browser
// extension, frames from tabCapture. Those go into the mailbox through the
// ordinary source, so the analysis, the region and the colour readout all
// reach them exactly as they reach a desktop's frames.
//
// That is the whole reason this is not a stub. A host that pushed pixels
// straight into its own analysis - which is what the lab did first - ends up
// carrying a private copy of the pipeline, and a private copy drifts.

#include "platform/screen_capture.h"

#include <cstring>
#include <memory>
#include <vector>

#include "platform/web/screen_capture_source.h"

namespace sidescopes {
namespace {

/// The running capture, or nothing. A page is single-threaded, so this needs
/// no lock: the host submits from the same thread the frame loop runs on.
class WebScreenCapture;
WebScreenCapture* g_running = nullptr;

class WebScreenCapture final : public ScreenCaptureSource
{
public:
    ~WebScreenCapture() override
    {
        if (g_running == this) {
            g_running = nullptr;
        }
    }

    CapturePermission requestPermission() override
    {
        // Granted, and it is not a pretence: the host supplies the pixels, so
        // there is nothing left to ask anyone for. The question the pane area
        // is really putting is "is there a reason to show a help page instead
        // of the scopes", and here there is not.
        return CapturePermission::Granted;
    }

    std::vector<CaptureTarget> listTargets() override
    {
        // One target, standing for whatever the host feeds in. Its size is
        // filled in by the first picture; a target listed with zeroes still
        // starts, because the host decides the dimensions, not a display.
        CaptureTarget target;
        target.identifier = "page";
        target.description = "This page";
        target.displayId = 0;
        target.widthPoints = m_width;
        target.heightPoints = m_height;

        return {target};
    }

    bool start(const CaptureTarget&, int, FrameMailbox& mailbox) override
    {
        m_mailbox = &mailbox;
        g_running = this;

        return true;
    }

    void stop() override
    {
        m_mailbox = nullptr;
        if (g_running == this) {
            g_running = nullptr;
        }
    }

    void setStatusCallback(StatusCallback) override
    {
        // Submission is synchronous; there is no background capture status.
    }

    void submit(const uint8_t* bgra, int width, int height)
    {
        if (m_mailbox == nullptr || bgra == nullptr || width <= 0 || height <= 0) {
            return;
        }
        m_width = width;
        m_height = height;

        const auto stride = static_cast<std::size_t>(width) * 4u;
        const std::size_t bytes = stride * static_cast<std::size_t>(height);
        m_buffer.sizeTo(bytes);
        std::memcpy(m_buffer.data.data(), bgra, bytes);
        m_buffer.strideBytes = static_cast<int>(stride);
        m_buffer.width = width;
        m_buffer.height = height;
        m_buffer.format = PixelFormat::Bgra8;
        m_buffer.colorSpace = ColorSpaceHint::Srgb;
        m_buffer.sequence = ++m_sequence;
        // The picture IS the display here, so it covers all of it. Stamped on
        // every frame rather than left alone, because buffers are recycled and
        // a field not written carries the last delivery's answer forward.
        m_buffer.sourceX = 0;
        m_buffer.sourceY = 0;
        m_buffer.sourceWidth = width;
        m_buffer.sourceHeight = height;

        // publish hands back storage to fill next time, as it does for every
        // desktop backend.
        m_buffer = m_mailbox->publish(std::move(m_buffer));
    }

private:
    FrameMailbox* m_mailbox = nullptr;
    FrameBuffer m_buffer;
    uint64_t m_sequence = 0;
    int m_width = 0;
    int m_height = 0;
};

}  // namespace

void submitCapturedPicture(const uint8_t* bgra, int width, int height)
{
    if (g_running != nullptr) {
        g_running->submit(bgra, width, height);
    }
}

std::unique_ptr<ScreenCaptureSource> createScreenCaptureSource()
{
    return std::make_unique<WebScreenCapture>();
}

}  // namespace sidescopes
