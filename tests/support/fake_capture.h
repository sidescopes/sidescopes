#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/frame_mailbox.h"
#include "platform/screen_capture.h"

namespace sidescopes::test {

// A scripted stand-in for the platform capture backend: the permission
// verdict, target list, and start outcome are set by the test, and every
// start/stop is counted so the restart policy can be observed without a real
// desktop. It also holds onto the status callback so a test can fire it, the
// way a dying stream does from a backend thread.
class FakeCaptureSource : public ScreenCaptureSource
{
public:
    CapturePermission permission = CapturePermission::Granted;
    std::vector<CaptureTarget> targets;
    bool startSucceeds = true;

    int permissionRequests = 0;
    int startCount = 0;
    int stopCount = 0;
    uint32_t lastStartedDisplay = 0;
    int lastFramesPerSecond = 0;

    CapturePermission requestPermission() override
    {
        ++permissionRequests;

        return permission;
    }

    std::vector<CaptureTarget> listTargets() override
    {
        return targets;
    }

    bool start(const CaptureTarget& target, int maxFramesPerSecond, FrameMailbox&) override
    {
        ++startCount;
        lastStartedDisplay = target.displayId;
        lastFramesPerSecond = maxFramesPerSecond;

        return startSucceeds;
    }

    void stop() override
    {
        ++stopCount;
    }

    void setStatusCallback(StatusCallback callback) override
    {
        m_statusCallback = std::move(callback);
    }

    void fireStatus(const std::string& message)
    {
        if (m_statusCallback) {
            m_statusCallback(message);
        }
    }

private:
    StatusCallback m_statusCallback;
};

inline CaptureTarget makeTarget(uint32_t displayId, std::string description)
{
    CaptureTarget target;
    target.identifier = "id-" + std::to_string(displayId);
    target.description = std::move(description);
    target.displayId = displayId;
    target.widthPoints = 1920;
    target.heightPoints = 1080;

    return target;
}

}  // namespace sidescopes::test
