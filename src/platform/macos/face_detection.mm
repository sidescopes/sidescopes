#import <CoreVideo/CoreVideo.h>
#import <Vision/Vision.h>

#include <algorithm>
#include <cmath>
#include <optional>

#include "platform/face_detection.h"

namespace sidescopes {
namespace {

// Faces smaller than this (in points) are thumbnails, not scoping targets.
constexpr double MinimumFacePoints = 72.0;

// Whether this frame contains enough well-formed storage to give Vision. An
// eight-bit frame is wrapped directly; a deeper frame is converted below.
bool readableByVision(const FrameView& frame)
{
    return frame.pixels != nullptr && frame.width > 0 && frame.height > 0 && frame.strideBytes >= frame.width * 4;
}

// Vision reports normalized rectangles from the bottom left. Convert one to
// the frame's top-left pixel convention, rejecting thumbnail-sized results.
std::optional<IntRect> faceRect(const CGRect box, const FrameView& frame, double minimumSize)
{
    const double width = box.size.width * frame.width;
    const double height = box.size.height * frame.height;
    if (width < minimumSize || height < minimumSize) {
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

void warmFaceDetection()
{
    // Nothing, deliberately. This used to run a throwaway request at startup
    // so that Vision's model was loaded before the picker could ask for it.
    // Measured, that saves nothing: the picker's first opening takes the same
    // 250 ms either way, across four interleaved pairs. What the warm request
    // does cost is 13.5 MB of the 44.5 MB an application with no region
    // holds, and it charges that to every session including the many that
    // never look for a face. The model is loaded by the first real request
    // instead, which is the first time the picker opens.
}

std::vector<IntRect> detectFaces(const FrameView& frame, float pixelsPerPoint)
{
    std::vector<IntRect> faces;
    if (!readableByVision(frame)) {
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

    VNDetectFaceRectanglesRequest* request = [[VNDetectFaceRectanglesRequest alloc] init];
    VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithCVPixelBuffer:buffer options:@{}];
    NSError* error = nil;
    const BOOL performed = [handler performRequests:@[ request ] error:&error];
    if (performed && !error) {
        const double minimumSize = MinimumFacePoints * pixelsPerPoint;
        for (VNFaceObservation* observation in request.results) {
            if (const std::optional<IntRect> rect = faceRect(observation.boundingBox, frame, minimumSize)) {
                faces.push_back(*rect);
            }
        }
    }
    CVPixelBufferRelease(buffer);

    std::sort(faces.begin(), faces.end(), [](const IntRect& a, const IntRect& b) {
        return static_cast<int64_t>(a.width) * a.height > static_cast<int64_t>(b.width) * b.height;
    });
    constexpr std::size_t MaximumFaces = 8;
    if (faces.size() > MaximumFaces) {
        faces.resize(MaximumFaces);
    }
    return faces;
}

}  // namespace sidescopes
