#import <CoreVideo/CoreVideo.h>
#import <Vision/Vision.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "platform/face_detection.h"

namespace sidescopes {
namespace {

// Faces smaller than this (in points) are thumbnails, not scoping targets.
constexpr double MinimumFacePoints = 72.0;

// Whether this frame contains enough well-formed storage to give Vision. An
// eight-bit frame is wrapped directly; a deeper frame is converted below.
bool readableByVision(const FrameView& frame)
{
    return frame.pixels != nullptr && frame.width > 0 && frame.height > 0 && frame.width <= frame.strideBytes / 4 &&
           (frame.format == PixelFormat::Bgra8 || frame.format == PixelFormat::Argb2101010);
}

// Vision reports normalized rectangles from the bottom left. Convert one to
// the frame's top-left pixel convention, rejecting thumbnail-sized results.
bool validNormalizedBox(const CGRect box)
{
    return std::isfinite(box.origin.x) && std::isfinite(box.origin.y) && std::isfinite(box.size.width) &&
           std::isfinite(box.size.height) && box.size.width > 0.0 && box.size.height > 0.0;
}

std::optional<IntRect> faceRect(const CGRect box, const FrameView& frame, double minimumSize)
{
    if (!validNormalizedBox(box)) {
        return std::nullopt;
    }
    const double width = box.size.width * frame.width;
    const double height = box.size.height * frame.height;
    if (!std::isfinite(minimumSize) || minimumSize < 0.0 || width < minimumSize || height < minimumSize ||
        box.origin.x > 1.0 || box.origin.y > 1.0 || box.origin.x + box.size.width <= 0.0 ||
        box.origin.y + box.size.height <= 0.0) {
        return std::nullopt;
    }

    const double left = box.origin.x * frame.width;
    const double top = (1.0 - box.origin.y - box.size.height) * frame.height;
    IntRect rect;
    rect.x = static_cast<int>(std::lround(std::max(0.0, left)));
    rect.y = static_cast<int>(std::lround(std::max(0.0, top)));
    rect.width = static_cast<int>(std::lround(std::min(width, static_cast<double>(frame.width) - rect.x)));
    rect.height = static_cast<int>(std::lround(std::min(height, static_cast<double>(frame.height) - rect.y)));
    if (rect.width <= 0 || rect.height <= 0) {
        return std::nullopt;
    }
    return rect;
}

}  // namespace

bool supportsFaceDetection()
{
    return true;
}

namespace {

std::vector<IntRect> detectWithRequest(const FrameView& frame, double minimumSize,
                                       VNDetectFaceRectanglesRequest* request, bool& completed)
{
    std::vector<IntRect> faces;
    if (!request || !readableByVision(frame)) {
        return faces;
    }

    // Vision accepts eight-bit BGRA, while the normal macOS capture stream is
    // ten-bit. Preserve the zero-copy path for an eight-bit frame and convert
    // only the deeper stream. The owned copy outlives the synchronous request.
    std::vector<uint8_t> converted;
    const uint8_t* pixels = frame.pixels;
    std::size_t strideBytes = static_cast<std::size_t>(frame.strideBytes);
    if (frame.format != PixelFormat::Bgra8) {
        converted = copyAsBgra8(frame);
        if (converted.empty()) {
            return faces;
        }
        pixels = converted.data();
        strideBytes = static_cast<std::size_t>(frame.width) * 4;
    }

    CVPixelBufferRef buffer = nullptr;
    const CVReturn wrapped = CVPixelBufferCreateWithBytes(
        kCFAllocatorDefault, static_cast<size_t>(frame.width), static_cast<size_t>(frame.height),
        kCVPixelFormatType_32BGRA, const_cast<uint8_t*>(pixels), strideBytes, nullptr, nullptr, nullptr, &buffer);
    if (wrapped != kCVReturnSuccess || !buffer) {
        return faces;
    }
    const std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)> ownedBuffer(
        buffer, CVPixelBufferRelease);

    VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithCVPixelBuffer:buffer options:@{}];
    NSError* error = nil;
    const BOOL performed = [handler performRequests:@[ request ] error:&error];
    if (performed && !error) {
        completed = true;
        for (VNFaceObservation* observation in request.results) {
            if (const std::optional<IntRect> rect = faceRect(observation.boundingBox, frame, minimumSize)) {
                faces.push_back(*rect);
            }
        }
    }

    std::sort(faces.begin(), faces.end(), [](const IntRect& a, const IntRect& b) {
        return static_cast<int64_t>(a.width) * a.height > static_cast<int64_t>(b.width) * b.height;
    });
    constexpr std::size_t MaximumFaces = 8;
    if (faces.size() > MaximumFaces) {
        faces.resize(MaximumFaces);
    }
    return faces;
}

}  // namespace

FaceDetectionResult detectFaces(const FrameView& frame, float pixelsPerPoint)
{
    if (!std::isfinite(pixelsPerPoint) || pixelsPerPoint <= 0.0f || !readableByVision(frame)) {
        return {};
    }
    try {
        @autoreleasepool {
            VNDetectFaceRectanglesRequest* request = [[VNDetectFaceRectanglesRequest alloc] init];
            bool completed = false;
            auto faces = detectWithRequest(frame, MinimumFacePoints * pixelsPerPoint, request, completed);
            return {completed ? FaceDetectionStatus::Completed : FaceDetectionStatus::Failed, std::move(faces)};
        }
    } catch (const std::exception&) {
        return {};
    }
}

}  // namespace sidescopes
