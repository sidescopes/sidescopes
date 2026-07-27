// Face detection via WinRT's in-box FaceDetector - no dependency beyond
// the Windows SDK.
//
// The WinRT calls here block on their async results, which an STA thread
// must never do, and the main thread is in no apartment this application
// creates: GLFW initializes no OLE, taking drops through WM_DROPFILES.
// Every detection therefore runs to completion on its own short-lived MTA
// thread and the caller joins it, which keeps the seam synchronous without
// blocking inside the caller's apartment.
//
// Those threads open and close an apartment around each call, which is only
// safe while something else in the process keeps COM alive. The last
// CoUninitialize unloads the activation DLL, and the cached WinRT factory
// then points into unmapped memory, so the next activation faults. Measured
// on Windows: the application never loses it, and a console program built
// from these sources does.

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

// Rec.709 luma, fixed-point x256: the detector consumes grayscale. Returned on
// the scale its arguments came in on - a ten-bit pixel yields a ten-bit luma -
// so that narrowing to a byte happens once, after the conversion, rather than
// wrapping here.
inline int luma709(int r, int g, int b)
{
    return (54 * r + 183 * g + 19 * b) >> 8;
}

// Writes the frame's rows into @p data as Gray8. Compiled per pixel layout,
// the same way the scope accumulate paths are, so the inner loop holds no test
// of the format. Unlike Vision on macOS the detector here is handed a bitmap
// this converts, never the frame's own memory, so any layout can be read.
template <typename Pixels>
void writeGrayRows(const FrameView& frame, uint8_t* data, int32_t startIndex, int32_t stride)
{
    for (int py = 0; py < frame.height; ++py) {
        const uint8_t* source = frame.rawPixelAt(0, py);
        uint8_t* out = data + startIndex + static_cast<std::size_t>(py) * stride;
        for (int px = 0; px < frame.width; ++px, source += 4) {
            const Sample sample = Pixels::read(source);
            out[px] = static_cast<uint8_t>(levelIn<Pixels, WholeLevelBits>(
                luma709(static_cast<int>(sample.r), static_cast<int>(sample.g), static_cast<int>(sample.b))));
        }
    }
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
        if (frame.format == PixelFormat::Argb2101010) {
            writeGrayRows<Argb2101010Pixels>(frame, data, plane.StartIndex, plane.Stride);
        } else {
            writeGrayRows<Bgra8Pixels>(frame, data, plane.StartIndex, plane.Stride);
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
    // Nothing, deliberately, as on macOS. Building a throwaway FaceDetector
    // at startup charges 8.6 MB of private memory to every session - a fifth
    // of what the application holds at rest - including the many that never
    // look for a face. It buys about 40 ms on a first face pick: a first
    // detector takes 46-53 ms against 8-12 ms once the model is loaded. The
    // first real detection loads it instead.
}

std::vector<IntRect> detectFaces(const FrameView& frame, float pixelsPerPoint)
{
    std::vector<IntRect> faces;
    if (!frame.pixels || frame.width <= 0 || frame.height <= 0) {
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
