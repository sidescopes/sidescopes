#import <CoreVideo/CoreVideo.h>
#import <Vision/Vision.h>

#include <algorithm>
#include <cmath>

#include "platform/face_detection.h"

namespace sidescopes {
namespace {

// The detector's box hugs the features; skin judgment wants cheeks and
// forehead in the sample too.
constexpr double kPaddingFraction = 0.2;

// Faces smaller than this (in points) are thumbnails, not scoping targets.
constexpr double kMinimumFacePoints = 72.0;

}  // namespace

std::vector<IntRect> DetectFaces(const FrameView& frame, float pixels_per_point) {
    std::vector<IntRect> faces;
    if (!frame.bgra || frame.width <= 0 || frame.height <= 0) return faces;

    // The pixel buffer wraps the frame without copying; the frame outlives
    // the synchronous request below.
    CVPixelBufferRef buffer = nullptr;
    const CVReturn wrapped = CVPixelBufferCreateWithBytes(
        kCFAllocatorDefault, static_cast<size_t>(frame.width), static_cast<size_t>(frame.height),
        kCVPixelFormatType_32BGRA, const_cast<uint8_t*>(frame.bgra),
        static_cast<size_t>(frame.stride_bytes), nullptr, nullptr, nullptr, &buffer);
    if (wrapped != kCVReturnSuccess || !buffer) return faces;

    VNDetectFaceRectanglesRequest* request = [[VNDetectFaceRectanglesRequest alloc] init];
    VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithCVPixelBuffer:buffer
                                                                                  options:@{}];
    NSError* error = nil;
    const BOOL performed = [handler performRequests:@[ request ] error:&error];
    if (performed && !error) {
        const double minimum_size = kMinimumFacePoints * pixels_per_point;
        for (VNFaceObservation* observation in request.results) {
            // Normalized, bottom-left origin; the frame convention is
            // top-left pixels.
            const CGRect box = observation.boundingBox;
            const double width = box.size.width * frame.width;
            const double height = box.size.height * frame.height;
            if (width < minimum_size || height < minimum_size) continue;
            const double pad_x = width * kPaddingFraction;
            const double pad_y = height * kPaddingFraction;
            const double left = box.origin.x * frame.width - pad_x;
            const double top = (1.0 - box.origin.y - box.size.height) * frame.height - pad_y;
            IntRect rect;
            rect.x = static_cast<int>(std::lround(std::max(0.0, left)));
            rect.y = static_cast<int>(std::lround(std::max(0.0, top)));
            rect.width = static_cast<int>(std::lround(
                std::min(width + 2 * pad_x, static_cast<double>(frame.width) - rect.x)));
            rect.height = static_cast<int>(std::lround(
                std::min(height + 2 * pad_y, static_cast<double>(frame.height) - rect.y)));
            if (rect.width > 0 && rect.height > 0) faces.push_back(rect);
        }
    }
    CVPixelBufferRelease(buffer);

    std::sort(faces.begin(), faces.end(), [](const IntRect& a, const IntRect& b) {
        return static_cast<int64_t>(a.width) * a.height > static_cast<int64_t>(b.width) * b.height;
    });
    constexpr std::size_t kMaximumFaces = 8;
    if (faces.size() > kMaximumFaces) faces.resize(kMaximumFaces);
    return faces;
}

}  // namespace sidescopes
