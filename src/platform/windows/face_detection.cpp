#include "platform/face_detection.h"

#include <cmath>
#include <exception>
#include <memory>
#include <utility>

#include "core/diagnostics.h"
#include "platform/windows/face_network.h"

namespace sidescopes {
namespace {

constexpr double MinimumFacePoints = 72.0;
constexpr int TrackingInputEdge = 320;
constexpr int PickerInputEdge = 1280;

bool readableFrame(const FrameView& frame, double minimumPixels)
{
    return frame.pixels && frame.width > 0 && frame.height > 0 && frame.strideBytes > 0 &&
           frame.width <= frame.strideBytes / 4 && std::isfinite(minimumPixels) && minimumPixels >= 0.0 &&
           (frame.format == PixelFormat::Bgra8 || frame.format == PixelFormat::Argb2101010);
}

class WindowsFaceSession final : public FaceDetectionSession
{
public:
    explicit WindowsFaceSession(int maximumInputEdge)
        : m_maximumInputEdge(maximumInputEdge)
    {
    }

    FaceDetectionResult detect(const FrameView& frame, double minimumPixels) override
    {
        if (!readableFrame(frame, minimumPixels)) {
            return {};
        }
        try {
            if (!m_network) {
                m_network = std::make_unique<FaceNetwork>();
            }
            auto faces = m_network->detect(frame, minimumPixels, m_maximumInputEdge);
            SS_DIAG(FaceLock, "face_detection completed faces=%zu", faces.size());
            return {FaceDetectionStatus::Completed, std::move(faces)};
        } catch (const std::exception&) {
            // A failed native operation may leave partially updated tensors.
            // Recreate that session on retry, preserving the caller's region.
            m_network.reset();
            diagEmit(DiagChannel::FaceLock, "face_detection failed");
            return {};
        }
    }

private:
    int m_maximumInputEdge;
    std::unique_ptr<FaceNetwork> m_network;
};

}  // namespace

std::unique_ptr<FaceDetectionSession> createFaceDetectionSession()
{
    return std::make_unique<WindowsFaceSession>(TrackingInputEdge);
}

bool supportsFaceDetection()
{
    return true;
}

std::vector<IntRect> detectFaces(const FrameView& frame, float pixelsPerPoint)
{
    // The picker sees a whole display. Keep more detail than the live session,
    // which searches a crop around the selected face, so small faces on a
    // high-density display can still meet the initial 72-point size floor.
    try {
        return WindowsFaceSession{PickerInputEdge}.detect(frame, MinimumFacePoints * pixelsPerPoint).faces;
    } catch (const std::exception&) {
        diagEmit(DiagChannel::FaceLock, "face_detection failed");
        return {};
    }
}

}  // namespace sidescopes
