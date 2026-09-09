// Windows' built-in detector runs once per picker request. Its async calls
// complete on a private MTA thread, so callers may use any apartment type.
#define NOMINMAX
#include "platform/face_detection.h"

#include <MemoryBuffer.h>
#include <unknwn.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.FaceAnalysis.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

#include "core/diagnostics.h"

namespace sidescopes {
namespace {

constexpr double MinimumFacePoints = 72.0;
constexpr std::size_t MaximumFaces = 8;

using winrt::Windows::Graphics::Imaging::BitmapBufferAccessMode;
using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
using winrt::Windows::Graphics::Imaging::SoftwareBitmap;
using winrt::Windows::Media::FaceAnalysis::FaceDetector;

class FaceApartment
{
public:
    FaceApartment()
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }

    ~FaceApartment()
    {
        // Release factories before COM can unload their DLLs. Each picker
        // call must work without any other apartment keeping them alive.
        winrt::clear_factory_cache();
        winrt::uninit_apartment();
    }

    FaceApartment(const FaceApartment&) = delete;
    FaceApartment& operator=(const FaceApartment&) = delete;
};

bool readableFrame(const FrameView& frame, float pixelsPerPoint)
{
    return frame.pixels && frame.width > 0 && frame.height > 0 && frame.width <= frame.strideBytes / 4 &&
           std::isfinite(pixelsPerPoint) && pixelsPerPoint > 0.0f &&
           (frame.format == PixelFormat::Bgra8 || frame.format == PixelFormat::Argb2101010);
}

// Rec.709 luma stays on the source scale until the final eight-bit conversion.
template <typename Pixels>
void writeGrayRows(const FrameView& frame, uint8_t* data, int32_t startIndex, int32_t stride)
{
    for (int y = 0; y < frame.height; ++y) {
        const uint8_t* source = frame.rawPixelAt(0, y);
        uint8_t* out = data + startIndex + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < frame.width; ++x, source += 4) {
            const Sample sample = Pixels::read(source);
            const int luma = (54 * sample.r + 183 * sample.g + 19 * sample.b) >> 8;
            out[x] = static_cast<uint8_t>(levelIn<Pixels, WholeLevelBits>(luma));
        }
    }
}

SoftwareBitmap grayBitmapFromFrame(const FrameView& frame)
{
    SoftwareBitmap bitmap(BitmapPixelFormat::Gray8, frame.width, frame.height);
    const auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
    const auto plane = buffer.GetPlaneDescription(0);
    const auto reference = buffer.CreateReference();
    const auto access = reference.as<::Windows::Foundation::IMemoryBufferByteAccess>();
    uint8_t* data = nullptr;
    uint32_t capacity = 0;
    winrt::check_hresult(access->GetBuffer(&data, &capacity));
    if (!data || plane.StartIndex < 0 || plane.Stride < frame.width ||
        static_cast<uint64_t>(plane.StartIndex) + static_cast<uint64_t>(frame.height - 1) * plane.Stride + frame.width >
            capacity) {
        throw winrt::hresult_error(E_UNEXPECTED);
    }
    if (frame.format == PixelFormat::Argb2101010) {
        writeGrayRows<Argb2101010Pixels>(frame, data, plane.StartIndex, plane.Stride);
    } else {
        writeGrayRows<Bgra8Pixels>(frame, data, plane.StartIndex, plane.Stride);
    }
    reference.Close();
    buffer.Close();
    return bitmap;
}

std::vector<IntRect> detectInApartment(const FrameView& frame, float pixelsPerPoint)
{
    const SoftwareBitmap bitmap = grayBitmapFromFrame(frame);
    const FaceDetector detector = FaceDetector::CreateAsync().get();
    const auto detected = detector.DetectFacesAsync(bitmap).get();
    std::vector<IntRect> faces;
    const double minimumPixels = MinimumFacePoints * pixelsPerPoint;
    for (const auto& face : detected) {
        const auto box = face.FaceBox();
        if (box.X >= static_cast<uint32_t>(frame.width) || box.Y >= static_cast<uint32_t>(frame.height)) {
            continue;
        }
        const IntRect rect{static_cast<int>(box.X), static_cast<int>(box.Y),
                           static_cast<int>(std::min(box.Width, static_cast<uint32_t>(frame.width) - box.X)),
                           static_cast<int>(std::min(box.Height, static_cast<uint32_t>(frame.height) - box.Y))};
        if (rect.width >= minimumPixels && rect.height >= minimumPixels) {
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

FaceDetectionResult detectOnOwnApartment(const FrameView& frame, float pixelsPerPoint)
{
    try {
        const FaceApartment apartment;
        if (!FaceDetector::IsSupported()) {
            return {FaceDetectionStatus::Unsupported, {}};
        }
        auto faces = detectInApartment(frame, pixelsPerPoint);
        SS_DIAG(FaceDetection, "face_detection completed faces=%zu", faces.size());
        return {FaceDetectionStatus::Completed, std::move(faces)};
    } catch (const winrt::hresult_error& error) {
        SS_DIAG(FaceDetection, "face_detection failed hresult=0x%08x", static_cast<uint32_t>(error.code()));
    } catch (...) {
        diagEmit(DiagChannel::FaceDetection, "face_detection failed");
    }
    return {};
}

bool querySupport()
{
    try {
        const FaceApartment apartment;
        return FaceDetector::IsSupported();
    } catch (...) {
        return false;
    }
}

}  // namespace

bool supportsFaceDetection()
{
    // Do not build a throwaway model at startup. Only the capability check
    // runs here, off the interface thread; the first picker loads the model.
    enum
    {
        Unknown = -2,
        Querying = -1,
        Unsupported = 0,
        Supported = 1
    };

    static std::atomic<int> supported{Unknown};
    int known = supported.load();
    if (known == Unknown && supported.compare_exchange_strong(known, Querying)) {
        try {
            std::thread worker([] { supported.store(querySupport() ? Supported : Unsupported); });
            try {
                worker.detach();
            } catch (...) {
                worker.join();
            }
        } catch (const std::exception&) {
            supported.store(Unknown);
            diagEmit(DiagChannel::FaceDetection, "face_support worker creation failed");
        }
    }
    return supported.load() == Supported;
}

FaceDetectionResult detectFaces(const FrameView& frame, float pixelsPerPoint)
{
    if (!readableFrame(frame, pixelsPerPoint)) {
        return {};
    }
    FaceDetectionResult result;
    try {
        // Joining also keeps the caller-owned frame alive until WinRT has
        // finished reading it. A new picker never retains previous image data.
        const std::jthread worker([&] { result = detectOnOwnApartment(frame, pixelsPerPoint); });
    } catch (const std::exception&) {
        diagEmit(DiagChannel::FaceDetection, "face_detection failed to create worker");
    }
    return result;
}

}  // namespace sidescopes
