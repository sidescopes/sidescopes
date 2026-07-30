// Face detection through libfacedetection (3-clause BSD), fetched and built
// for Linux only.
//
// macOS detects faces with Vision and Windows with WinRT's FaceDetector, both
// part of the system. Linux ships nothing equivalent, so the picker's face
// mode would be missing on the one platform whose users have no alternative.
// libfacedetection is the owner-approved stand-in: a self-contained C++ CNN
// whose weights are compiled-in source, so a detection needs no model file, no
// OpenCV and no network, and the dependency is a compile-time one that reaches
// no further than this file.

#include "platform/face_detection.h"

#include <facedetectcnn.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace sidescopes {
namespace {

// Faces smaller than this (in points) are thumbnails, not scoping targets.
// The floor both other platforms apply, kept identical so a face picked here
// is the same face picked there.
constexpr double MinimumFacePoints = 72.0;

// The most faces reported, as on macOS.
constexpr std::size_t MaximumFaces = 8;

// The detector's own confidence, on the 0..100 scale its result buffer
// carries. Its internal cut is 0.2, deliberately generous so that a caller
// can choose its own; 0.9 is what the project's own example applies before
// drawing a box. A screen holds far more face-like structure than a
// photograph does - avatars, thumbnails, faces inside a filmstrip - so the
// generous end of that range would offer the picker regions it should never
// suggest.
constexpr int16_t MinimumConfidencePercent = 90;

// One face as the result buffer carries it: five shorts at the head of a
// sixteen-short record whose remainder holds landmarks this does not use.
struct DetectedFace
{
    int16_t confidence = 0;
    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 0;
    int16_t height = 0;
};

static_assert(sizeof(DetectedFace) == 5 * sizeof(int16_t), "the record is copied out of the buffer verbatim");

// Whether the detector can be handed this frame. Its input is three
// interleaved eight-bit channels, which the conversion below builds from BGRA
// bytes; a ten-bit frame's packed words would convert to a plausible image
// made of the wrong bits, and no capture backend on this platform produces
// one.
bool readableByDetector(const FrameView& frame)
{
    return frame.pixels != nullptr && frame.format == PixelFormat::Bgra8 && frame.width > 0 && frame.height > 0;
}

// The frame's pixels as the three interleaved channels the detector reads,
// rows packed. Copied rather than viewed because the fourth byte per pixel
// has to go, and because the detector's input is not const.
std::vector<uint8_t> bgrFromFrame(const FrameView& frame)
{
    const std::size_t rowBytes = static_cast<std::size_t>(frame.width) * 3;
    std::vector<uint8_t> bgr(rowBytes * static_cast<std::size_t>(frame.height));
    for (int py = 0; py < frame.height; ++py) {
        const uint8_t* source = frame.rawPixelAt(0, py);
        uint8_t* out = bgr.data() + static_cast<std::size_t>(py) * rowBytes;
        for (int px = 0; px < frame.width; ++px, source += 4, out += 3) {
            out[0] = source[0];
            out[1] = source[1];
            out[2] = source[2];
        }
    }

    return bgr;
}

// How many faces the buffer's leading count claims, bounded by the most the
// library can have written into it.
int faceCount(const std::vector<uint8_t>& buffer)
{
    int count = 0;
    std::memcpy(&count, buffer.data(), sizeof(count));

    return std::clamp(count, 0, FACEDETECTION_RESULT_MAX_FACES);
}

DetectedFace faceAt(const std::vector<uint8_t>& buffer, int index)
{
    const std::size_t stride = FACEDETECTION_RESULT_STRIDE_SHORTS * sizeof(int16_t);
    DetectedFace face;
    std::memcpy(&face, buffer.data() + sizeof(int) + static_cast<std::size_t>(index) * stride, sizeof(face));

    return face;
}

// The faces worth offering, largest first and at most MaximumFaces of them.
// The boxes are the detector's own, unpadded; only the frame's edges move
// one, since a box may hang off the side of the image it was found in.
std::vector<IntRect> facesFromBuffer(const std::vector<uint8_t>& buffer, const FrameView& frame, double minimumSize)
{
    std::vector<IntRect> faces;
    const int count = faceCount(buffer);
    for (int index = 0; index < count; ++index) {
        const DetectedFace face = faceAt(buffer, index);
        if (face.confidence < MinimumConfidencePercent) {
            continue;
        }
        if (face.width < minimumSize || face.height < minimumSize) {
            continue;
        }
        const IntRect rect = IntRect{face.x, face.y, face.width, face.height}.clampedTo(frame.width, frame.height);
        if (!rect.empty()) {
            faces.push_back(rect);
        }
    }

    std::sort(faces.begin(), faces.end(), [](const IntRect& a, const IntRect& b) {
        return static_cast<int64_t>(a.width) * a.height > static_cast<int64_t>(b.width) * b.height;
    });
    if (faces.size() > MaximumFaces) {
        faces.resize(MaximumFaces);
    }

    return faces;
}

}  // namespace

bool supportsFaceDetection()
{
    return true;
}

void warmFaceDetection()
{
    // Nothing, deliberately, as on the other two platforms. The model's
    // parameters are built on the first detection, and doing that at startup
    // would charge every session for a face pick most never make.
}

std::vector<IntRect> detectFaces(const FrameView& frame, float pixelsPerPoint)
{
    const double minimumSize = MinimumFacePoints * static_cast<double>(pixelsPerPoint);
    if (!readableByDetector(frame) || frame.width < minimumSize || frame.height < minimumSize) {
        // A frame narrower or shorter than the floor can hold no face this
        // would report, so the pass is skipped rather than run for an answer
        // already known to be empty.
        return {};
    }

    std::vector<uint8_t> bgr = bgrFromFrame(frame);
    std::vector<uint8_t> buffer(FACEDETECTION_RESULT_BUFFER_SIZE, uint8_t{0});

    // The model's parameters are built into file-scope state on the first
    // call, so two detections must not start at once. They are rare - a
    // picker opening, a face lock re-acquiring - and each is a single pass,
    // so serializing them costs nothing a caller can see.
    static std::mutex detectorMutex;
    const std::lock_guard<std::mutex> lock(detectorMutex);
    if (facedetect_cnn(buffer.data(), bgr.data(), frame.width, frame.height, frame.width * 3) == nullptr) {
        return {};
    }

    return facesFromBuffer(buffer, frame, minimumSize);
}

}  // namespace sidescopes
