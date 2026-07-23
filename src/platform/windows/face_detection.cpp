// Face detection via WinRT's in-box FaceDetector - no dependency beyond
// the Windows SDK. Compiles on every push via CI; runtime behavior awaits
// the Windows machine.
//
// The WinRT calls here block on their async results, which is forbidden
// on an STA thread - and the application's main thread is one (GLFW
// initializes OLE for drag and drop). Every detection therefore runs to
// completion on its own short-lived MTA thread and the caller joins it,
// which keeps the seam synchronous without ever blocking inside the
// caller's apartment.

#include "platform/face_detection.h"

#include <MemoryBuffer.h>
#include <unknwn.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.FaceAnalysis.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace sidescopes {
namespace {

// Faces smaller than this (in points) are thumbnails, not scoping targets.
constexpr double MinimumFacePoints = 72.0;

using winrt::Windows::Graphics::Imaging::BitmapBufferAccessMode;
using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
using winrt::Windows::Graphics::Imaging::SoftwareBitmap;
using winrt::Windows::Media::FaceAnalysis::FaceDetector;

// Rec.709 luma, fixed-point x256: the detector consumes grayscale.
inline uint8_t luma709(int r, int g, int b)
{
    return static_cast<uint8_t>((54 * r + 183 * g + 19 * b) >> 8);
}

SoftwareBitmap grayBitmapFromFrame(const FrameView& frame)
{
    SoftwareBitmap bitmap(BitmapPixelFormat::Gray8, frame.width, frame.height);
    {
        const auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
        const auto plane = buffer.GetPlaneDescription(0);
        const auto reference = buffer.CreateReference();
        const auto access = reference.as<::Windows::Foundation::IMemoryBufferByteAccess>();
        uint8_t* data = nullptr;
        uint32_t capacity = 0;
        winrt::check_hresult(access->GetBuffer(&data, &capacity));
        for (int py = 0; py < frame.height; ++py) {
            const uint8_t* source = frame.pixelAt(0, py);
            uint8_t* out = data + plane.StartIndex + static_cast<std::size_t>(py) * plane.Stride;
            for (int px = 0; px < frame.width; ++px, source += 4) {
                out[px] = luma709(source[2], source[1], source[0]);
            }
        }
        reference.Close();
        buffer.Close();
    }
    return bitmap;
}

std::vector<IntRect> detectOnOwnApartment(const FrameView& frame, float pixelsPerPoint)
{
    std::vector<IntRect> faces;
    winrt::init_apartment();
    {
        const SoftwareBitmap bitmap = grayBitmapFromFrame(frame);
        const FaceDetector detector = FaceDetector::CreateAsync().get();
        const auto detected = detector.DetectFacesAsync(bitmap).get();

        const double minimumPixels = MinimumFacePoints * pixelsPerPoint;
        for (const auto& face : detected) {
            const auto box = face.FaceBox();
            if (box.Width < minimumPixels || box.Height < minimumPixels) {
                continue;
            }
            faces.push_back(IntRect{static_cast<int>(box.X), static_cast<int>(box.Y), static_cast<int>(box.Width),
                                    static_cast<int>(box.Height)});
        }
        std::sort(faces.begin(), faces.end(), [](const IntRect& a, const IntRect& b) {
            return static_cast<int64_t>(a.width) * a.height > static_cast<int64_t>(b.width) * b.height;
        });
    }
    winrt::uninit_apartment();
    return faces;
}

}  // namespace

bool supportsFaceDetection()
{
    // Queried once, off the caller's thread: the WinRT support check
    // itself needs an apartment, and this function is called from the
    // interface loop. The face action appears as soon as the answer is
    // known - a moment after startup.
    static std::atomic<int> supported{-1};
    static std::atomic<bool> querying{false};
    const int known = supported.load();
    if (known >= 0) {
        return known == 1;
    }
    if (!querying.exchange(true)) {
        std::thread([] {
            bool answer = false;
            try {
                winrt::init_apartment();
                answer = FaceDetector::IsSupported();
                winrt::uninit_apartment();
            } catch (...) {
                answer = false;
            }
            supported.store(answer ? 1 : 0);
        }).detach();
    }
    return false;
}

void warmFaceDetection()
{
    // The first FaceDetector construction loads the model; pay that cost
    // in the background at startup, the way the macOS warmer does.
    std::thread([] {
        try {
            winrt::init_apartment();
            // The temporary releases before the apartment closes.
            FaceDetector::CreateAsync().get();
            winrt::uninit_apartment();
        } catch (...) {  // NOLINT(bugprone-empty-catch): warm-up failure is retried by real detection
        }
    }).detach();
}

std::vector<IntRect> detectFaces(const FrameView& frame, float pixelsPerPoint)
{
    std::vector<IntRect> faces;
    if (!frame.bgra || frame.width <= 0 || frame.height <= 0) {
        return faces;
    }
    if (!supportsFaceDetection()) {
        return faces;
    }

    std::thread worker([&] {
        try {
            faces = detectOnOwnApartment(frame, pixelsPerPoint);
        } catch (...) {
            faces.clear();  // an honest empty answer beats a crash
        }
    });
    worker.join();
    return faces;
}

}  // namespace sidescopes
