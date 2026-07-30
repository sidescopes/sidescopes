// Screen capture on Linux. The targets are real - the connected outputs, so
// the interface speaks true display names and geometry - but the stream
// itself is not built yet: start() declines with a status naming that, and
// the controller's retry loop keeps the page honest. The planned backend is
// the ScreenCast portal feeding PipeWire, per the workbench design note.

#include "platform/screen_capture.h"

#include "platform/linux/x11_displays.h"

namespace sidescopes {
namespace {

class LinuxScreenCapture final : public ScreenCaptureSource
{
public:
    CapturePermission requestPermission() override
    {
        // Consent on Linux is asked by the portal dialog at stream start,
        // not granted ahead of it; there is nothing to request here.
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
            targets.push_back(target);
        }
        return targets;
    }

    bool start(const CaptureTarget&, int, FrameMailbox&) override
    {
        if (m_status) {
            m_status("screen capture is not built on Linux yet");
        }
        return false;
    }

    void stop() override
    {
    }

    void setStatusCallback(StatusCallback callback) override
    {
        m_status = std::move(callback);
    }

private:
    StatusCallback m_status;
};

}  // namespace

std::unique_ptr<ScreenCaptureSource> createScreenCaptureSource()
{
    return std::make_unique<LinuxScreenCapture>();
}

}  // namespace sidescopes
