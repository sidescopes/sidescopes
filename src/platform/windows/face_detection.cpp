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
// Each apartment clears its cached activation factories before COM can unload
// their DLLs. Detection does not depend on another part of the process keeping
// an apartment alive.

#include "platform/face_detection.h"

#include <MemoryBuffer.h>
#include <unknwn.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.FaceAnalysis.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <new>
#include <system_error>
#include <thread>
#include <vector>

#include "core/diagnostics.h"

namespace sidescopes {
namespace {

// Faces smaller than this (in points) are thumbnails, not scoping targets.
constexpr double MinimumFacePoints = 72.0;

using winrt::Windows::Graphics::Imaging::BitmapBufferAccessMode;
using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
using winrt::Windows::Graphics::Imaging::SoftwareBitmap;
using winrt::Windows::Media::FaceAnalysis::FaceDetector;

class FaceApartment
{
public:
    FaceApartment()
    {
        winrt::init_apartment();
    }

    ~FaceApartment()
    {
        winrt::clear_factory_cache();
        winrt::uninit_apartment();
    }

    FaceApartment(const FaceApartment&) = delete;
    FaceApartment& operator=(const FaceApartment&) = delete;
};

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
    const FaceApartment apartment;
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
    constexpr std::size_t MaximumFaces = 8;
    if (faces.size() > MaximumFaces) {
        faces.resize(MaximumFaces);
    }
    return faces;
}

// Detection errors produce an empty result; the log retains the reason so
// callers can distinguish detector failure from an image containing no faces.
std::vector<IntRect> detectWithDiagnostics(const FrameView& frame, float pixelsPerPoint)
{
    try {
        std::vector<IntRect> faces = detectOnOwnApartment(frame, pixelsPerPoint);
        SS_DIAG(FaceLock, "face_detection completed faces=%zu", faces.size());
        return faces;
    } catch (const winrt::hresult_error& error) {
        SS_DIAG(FaceLock, "face_detection failed hresult=0x%08x", static_cast<uint32_t>(error.code()));
    } catch (...) {
        SS_DIAG(FaceLock, "face_detection failed with an unexpected exception");
    }
    return {};
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
        std::thread worker;
        try {
            worker = std::thread([] {
                bool answer = false;
                try {
                    const FaceApartment apartment;
                    answer = FaceDetector::IsSupported();
                } catch (...) {
                    answer = false;
                }
                supported.store(answer ? 1 : 0);
                diagEmit(DiagChannel::FaceLock,
                         answer ? "face_support completed supported=1" : "face_support completed supported=0");
            });
        } catch (const std::bad_alloc&) {
            querying.store(false);
            diagEmit(DiagChannel::FaceLock, "face_support worker allocation failed");
            return false;
        } catch (const std::system_error&) {
            querying.store(false);
            diagEmit(DiagChannel::FaceLock, "face_support worker creation failed");
            return false;
        }
        try {
            worker.detach();
        } catch (const std::system_error&) {
            // A started query still owns its captures: finish it before the
            // joinable worker leaves scope if detaching is unavailable.
            worker.join();
        }
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
    if (!frame.pixels || frame.width <= 0 || frame.height <= 0 || frame.width > frame.strideBytes / 4) {
        return faces;
    }
    if (!supportsFaceDetection()) {
        return faces;
    }

    std::thread worker;
    try {
        worker = std::thread([&] { faces = detectWithDiagnostics(frame, pixelsPerPoint); });
    } catch (const std::bad_alloc&) {
        diagEmit(DiagChannel::FaceLock, "face_detection worker allocation failed");
        return faces;
    } catch (const std::system_error&) {
        diagEmit(DiagChannel::FaceLock, "face_detection worker creation failed");
        return faces;
    }
    worker.join();
    return faces;
}

}  // namespace sidescopes
