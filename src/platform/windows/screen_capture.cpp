// Windows screen capture via DXGI output duplication.
//
// Duplication delivers a frame only when the desktop changes, which the
// mailbox design already assumes. Access loss (display mode changes, UAC
// desktops, lock screen) surfaces through the status callback and the
// application restarts capture, mirroring how stream death is handled on
// macOS.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "platform/screen_capture.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

#include "core/diagnostics.h"
#include "core/parallel_for.h"
#include "core/scrgb.h"
#include "platform/software_crop.h"
#include "platform/windows/advanced_color.h"
#include "platform/windows/display_identity.h"

namespace sidescopes {
namespace {

using Microsoft::WRL::ComPtr;

// The Direct3D objects a running duplication needs: the device and its
// immediate context for the staging copy, and the duplication itself.
struct DuplicationSetup
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    // Where the SDR white level of the duplicated display is read from while
    // the desktop composes in scRGB.
    ColorTarget colorTarget;
};

// State the capture loop carries across frames: the staging texture is
// reused until the frame dimensions change, the buffer's storage is
// recycled through the mailbox, the sequence counts published frames, and
// the scRGB conversion keeps its table for the white level last seen.
struct FrameCopyState
{
    // Duplication's texture is invalid after ReleaseFrame. Keep our own GPU
    // copy so a crop change can publish pixels without another desktop update.
    ComPtr<ID3D11Texture2D> desktop;
    ComPtr<ID3D11Texture2D> staging;
    std::optional<IntRect> publishedRect;
    FrameBuffer buffer;
    FrameStamp stamp;
    ScrgbToDisplayCodes scrgb;
};

// What a recording is told about a delivery: the layout the buffer carries
// and, for an scRGB desktop, the SDR white level it was normalised to.
struct DeliveryDescription
{
    PixelFormat format = PixelFormat::Bgra8;
    int whiteTenthNits = 0;

    bool operator==(const DeliveryDescription& other) const
    {
        return format == other.format && whiteTenthNits == other.whiteTenthNits;
    }
};

// Copies @p height rows of four-byte pixels out of a mapped staging texture
// into the buffer, dropping the texture's row padding.
void copyBgra8Rows(FrameBuffer& buffer, const D3D11_MAPPED_SUBRESOURCE& mapped, int stride, int height)
{
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    for (int row = 0; row < height; ++row) {
        std::memcpy(buffer.data.data() + static_cast<std::size_t>(row) * stride,
                    source + static_cast<std::size_t>(row) * mapped.RowPitch, static_cast<std::size_t>(stride));
    }
}

// Rows one thread converts before the work is worth splitting: three table
// reads per pixel are cheap enough that a small frame converts inline faster
// than it spawns.
constexpr int ScrgbRowsPerChunk = 128;

// Converts @p height rows of scRGB half floats into the buffer's packed
// ten-bit codes, splitting the rows across threads for a large frame. Each
// chunk writes only its own rows and its own counter. Returns how many pixels
// had a channel strictly above SDR white, which saturates at full scale.
int convertScrgbRows(FrameCopyState& state, const D3D11_MAPPED_SUBRESOURCE& mapped, int width, int height)
{
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* target = state.buffer.data.data();
    const ScrgbToDisplayCodes& codes = state.scrgb;
    const std::size_t targetStride = static_cast<std::size_t>(width) * 4;
    std::array<int, MaxParallelChunks> above{};
    runParallelChunks(
        parallelChunkCount(height, ScrgbRowsPerChunk), height, [&](int chunk, int rowBegin, int rowEnd) noexcept {
            int count = 0;
            for (int row = rowBegin; row < rowEnd; ++row) {
                count +=
                    codes.convertRow(source + static_cast<std::size_t>(row) * mapped.RowPitch,
                                     target + static_cast<std::size_t>(row) * targetStride, width,
                                     state.buffer.hdrLuminance.empty()
                                         ? nullptr
                                         : state.buffer.hdrLuminance.data() + static_cast<std::size_t>(row) * width);
            }
            above[static_cast<std::size_t>(chunk)] = count;
        });
    int total = 0;
    for (const int count : above) {
        total += count;
    }

    return total;
}

// The decade a share of pixels above SDR white falls in: none, under a tenth
// of a percent, under one, under ten, or more. A recording learns of the
// change of decade, not of every frame.
int headroomBucket(int abovePixels, double sharePercent)
{
    if (abovePixels == 0) {
        return 0;
    }
    if (sharePercent < 0.1) {
        return 1;
    }
    if (sharePercent < 1.0) {
        return 2;
    }

    return sharePercent < 10.0 ? 3 : 4;
}

struct AcquiredFrame
{
    IDXGIOutputDuplication& duplication;

    ~AcquiredFrame()
    {
        duplication.ReleaseFrame();
    }
};

struct MappedTexture
{
    ID3D11DeviceContext& context;
    ID3D11Texture2D& texture;

    ~MappedTexture()
    {
        context.Unmap(&texture, 0);
    }
};

// A frame either reached the mailbox or hit an error that ends the loop.
enum class FrameOutcome
{
    Published,
    Fatal,
};

// The outcome of asking duplication for the next changed frame: a fresh
// frame to process, a benign reason to retry, or a fatal loss of access.
enum class AcquireResult
{
    Frame,
    Retry,
    Fatal,
};

class DxgiScreenCaptureSource final : public ScreenCaptureSource
{
    friend struct DxgiCaptureTestAccess;

public:
    ~DxgiScreenCaptureSource() override
    {
        stop();
    }

    CapturePermission requestPermission() override
    {
        // Reading the desktop needs no user consent on Windows.
        return CapturePermission::Granted;
    }

    std::vector<CaptureTarget> listTargets() override
    {
        std::vector<CaptureTarget> targets;
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            return targets;
        }

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT adapterIndex = 0; factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND;
             ++adapterIndex) {
            ComPtr<IDXGIOutput> output;
            for (UINT outputIndex = 0; adapter->EnumOutputs(outputIndex, &output) != DXGI_ERROR_NOT_FOUND;
                 ++outputIndex) {
                DXGI_OUTPUT_DESC description{};
                if (FAILED(output->GetDesc(&description)) || !description.AttachedToDesktop) {
                    continue;
                }
                CaptureTarget target;
                target.identifier = std::to_string(adapterIndex) + ":" + std::to_string(outputIndex);
                char name[64];
                const int written =
                    WideCharToMultiByte(CP_UTF8, 0, description.DeviceName, -1, name, sizeof(name), nullptr, nullptr);
                target.description = written > 0 ? name : "Display";
                target.displayId = displayIdFromDeviceName(description.DeviceName);
                const RECT& rect = description.DesktopCoordinates;
                target.widthPoints = static_cast<int>(rect.right - rect.left);
                target.heightPoints = static_cast<int>(rect.bottom - rect.top);
                targets.push_back(std::move(target));
            }
        }
        return targets;
    }

    bool start(const CaptureTarget& target, int maxFramesPerSecond, FrameMailbox& mailbox,
               uint64_t captureEpoch) override
    {
        stop();
        UINT adapterIndex = 0;
        UINT outputIndex = 0;
        const auto separator = target.identifier.find(':');
        if (separator == std::string::npos) {
            return false;
        }
        adapterIndex = static_cast<UINT>(std::strtoul(target.identifier.c_str(), nullptr, 10));
        outputIndex = static_cast<UINT>(std::strtoul(target.identifier.c_str() + separator + 1, nullptr, 10));

        m_stopRequested.store(false);
        try {
            const FrameStamp stamp{captureEpoch, target.displayId, 0.0};
            m_worker = std::thread([this, adapterIndex, outputIndex, maxFramesPerSecond, &mailbox, stamp] {
                run(adapterIndex, outputIndex, maxFramesPerSecond, mailbox, stamp);
            });
            return true;
        } catch (const std::bad_alloc&) {
            reportStatus("capture worker allocation failed");
        } catch (const std::system_error&) {
            reportStatus("could not start the capture worker");
        }
        m_stopRequested.store(true);
        return false;
    }

    void setHdrEnabled(bool enabled) override
    {
        m_hdrEnabled = enabled;
    }

    void stop() override
    {
        m_stopRequested.store(true);
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    void setStatusCallback(StatusCallback callback) override
    {
        m_statusCallback = std::move(callback);
    }

    // The worker serves a changed crop from its owned desktop image even when
    // duplication has no new frame. Each publication describes one whole crop.
    void narrowTo(const std::optional<IntRect>& rect) override
    {
        const std::lock_guard lock(m_cropMutex);
        m_crop = rect;
    }

private:
    std::optional<IntRect> currentCrop()
    {
        const std::lock_guard lock(m_cropMutex);

        return m_crop;
    }

    void reportStatus(const char* message)
    {
        // An empty message still withdraws the stream if memory is too scarce
        // to store the explanation. The callback must run even in that case.
        std::string status;
        try {
            status = message;
        } catch (const std::bad_alloc&) {
            diagEmit(DiagChannel::Perf, "capture status allocation failed");
        }
        try {
            if (m_statusCallback) {
                m_statusCallback(status);
            }
        } catch (const std::bad_alloc&) {
            diagEmit(DiagChannel::Perf, "capture status callback allocation failed");
        } catch (const std::system_error&) {
            diagEmit(DiagChannel::Perf, "capture status callback unavailable");
        }
    }

    void run(UINT adapterIndex, UINT outputIndex, int maxFramesPerSecond, FrameMailbox& mailbox, FrameStamp stamp)
    {
        try {
            captureLoop(adapterIndex, outputIndex, maxFramesPerSecond, mailbox, stamp);
        } catch (const std::bad_alloc&) {
            reportStatus("capture frame allocation failed");
        } catch (const std::system_error&) {
            reportStatus("capture worker unavailable");
        }
        m_stopRequested.store(true);
    }

    void captureLoop(UINT adapterIndex, UINT outputIndex, int maxFramesPerSecond, FrameMailbox& mailbox,
                     FrameStamp stamp)
    {
        DuplicationSetup setup;
        if (!openDuplication(adapterIndex, outputIndex, setup)) {
            return;
        }
        captureFrames(setup, maxFramesPerSecond, mailbox, stamp);
    }

    void captureFrames(const DuplicationSetup& setup, int maxFramesPerSecond, FrameMailbox& mailbox, FrameStamp stamp)
    {
        IDXGIOutputDuplication* duplication = setup.duplication.Get();

        FrameCopyState state;
        state.stamp = stamp;
        const auto minimumInterval =
            std::chrono::microseconds(maxFramesPerSecond > 0 ? 1000000 / maxFramesPerSecond : 0);
        auto lastPublish = std::chrono::steady_clock::now() - minimumInterval;

        while (!m_stopRequested.load()) {
            // Pace before acquiring, not after: a frame acquired early and
            // dropped would be the freshest desktop there is, and when the
            // screen goes quiet right then, nothing else ever arrives - the
            // scopes would sit one frame stale. Sleeping first means every
            // frame actually acquired is published.
            const auto due = lastPublish + minimumInterval;
            const auto now = std::chrono::steady_clock::now();
            if (now < due) {
                std::this_thread::sleep_for(due - now);
                continue;
            }

            ComPtr<IDXGIResource> resource;
            // A changed crop must not wait for a desktop update, but first
            // check for one: continuous drags must not starve fresh video.
            const AcquireResult acquired = acquireFrame(duplication, resource, cropChanged(state) ? 0 : 100);
            if (acquired == AcquireResult::Fatal) {
                return;
            }
            if (acquired == AcquireResult::Retry) {
                if (cropChanged(state)) {
                    // This is a new crop of the last image. Keep that image's
                    // receipt time rather than claiming a fresher desktop.
                    if (copyFrame(setup, state.desktop.Get(), state, mailbox) == FrameOutcome::Fatal) {
                        return;
                    }
                    lastPublish = now;
                }
                continue;
            }
            const AcquiredFrame acquiredFrame{*duplication};
            state.stamp.receivedSeconds = frameClockSeconds();

            ComPtr<ID3D11Texture2D> texture;
            if (FAILED(resource.As(&texture))) {
                continue;
            }
            if (!retainDesktop(setup, texture.Get(), state.desktop)) {
                return;
            }
            if (copyFrame(setup, state.desktop.Get(), state, mailbox) == FrameOutcome::Fatal) {
                return;
            }
            lastPublish = now;
        }
    }

    bool cropChanged(const FrameCopyState& state)
    {
        if (!state.desktop) {
            return false;
        }
        D3D11_TEXTURE2D_DESC description{};
        state.desktop->GetDesc(&description);
        const IntRect wanted =
            softwareCropRect(currentCrop(), static_cast<int>(description.Width), static_cast<int>(description.Height));

        return state.publishedRect != wanted;
    }

    bool retainDesktop(const DuplicationSetup& setup, ID3D11Texture2D* texture, ComPtr<ID3D11Texture2D>& desktop)
    {
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (!ensureTexture(setup.device.Get(), description, D3D11_USAGE_DEFAULT, desktop)) {
            return false;
        }
        setup.context->CopyResource(desktop.Get(), texture);

        return true;
    }

    // Acquires the next changed frame. Timeouts and metadata-only
    // deliveries ask the caller to retry; a lost stream is fatal. On
    // Frame, the caller owns the matching ReleaseFrame.
    AcquireResult acquireFrame(IDXGIOutputDuplication* duplication, ComPtr<IDXGIResource>& resource, UINT timeout)
    {
        DXGI_OUTDUPL_FRAME_INFO info{};
        const HRESULT acquired = duplication->AcquireNextFrame(timeout, &info, &resource);
        if (acquired == DXGI_ERROR_WAIT_TIMEOUT) {
            return AcquireResult::Retry;
        }
        if (acquired == DXGI_ERROR_ACCESS_LOST) {
            reportStatus("capture access lost");
            return AcquireResult::Fatal;
        }
        if (FAILED(acquired)) {
            reportStatus("capture failed");
            return AcquireResult::Fatal;
        }

        // A zero present time is a metadata-only delivery - the mouse
        // moved, the image did not. Copying the unchanged desktop just for
        // the analysis hash to discard it would bill a full-screen copy to
        // every cursor twitch.
        if (info.LastPresentTime.QuadPart == 0) {
            duplication->ReleaseFrame();
            return AcquireResult::Retry;
        }
        return AcquireResult::Frame;
    }

    // Opens output duplication for the given adapter and output, preferring
    // explicit BGRA format request and falling back to plain duplication.
    // Reports the reason and returns false on any failure.
    bool openDuplication(UINT adapterIndex, UINT outputIndex, DuplicationSetup& setup)
    {
        ComPtr<IDXGIFactory1> factory;
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput> output;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapters1(adapterIndex, &adapter)) ||
            FAILED(adapter->EnumOutputs(outputIndex, &output))) {
            reportStatus("capture target disappeared");
            return false;
        }
        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output.As(&output1))) {
            reportStatus("output duplication is unavailable");
            return false;
        }

        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                     &setup.device, nullptr, &setup.context))) {
            reportStatus("could not create a capture device");
            return false;
        }
        // Ask for the desktop's own composition format: scRGB half floats
        // while it composes in HDR or with Auto Color Management, BGRA8
        // otherwise. Converting an scRGB desktop to BGRA8 in the duplication
        // itself clips everything above 80 nits, so the conversion happens in
        // copyFrame, which verifies the acquired texture either way. Plain
        // DuplicateOutput stays the fallback and always converts to BGRA8.
        ComPtr<IDXGIOutput5> output5;
        HRESULT duplicated = E_NOINTERFACE;
        if (SUCCEEDED(output.As(&output5))) {
            const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
            duplicated = output5->DuplicateOutput1(setup.device.Get(), 0, 2, formats, &setup.duplication);
        }
        if (FAILED(duplicated)) {
            duplicated = output1->DuplicateOutput(setup.device.Get(), &setup.duplication);
        }
        if (FAILED(duplicated)) {
            reportStatus("could not duplicate the display");
            return false;
        }
        DXGI_OUTPUT_DESC outputDescription{};
        if (SUCCEEDED(output->GetDesc(&outputDescription))) {
            setup.colorTarget = findColorTarget(outputDescription.DeviceName);
        }
        DXGI_OUTDUPL_DESC duplicationDescription{};
        setup.duplication->GetDesc(&duplicationDescription);
        // Rotated outputs deliver rotated rows: colors would survive but
        // the waveform axes and the region mapping would not.
        if (duplicationDescription.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
            duplicationDescription.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
            reportStatus("rotated displays are not supported yet");
            return false;
        }
        return true;
    }

    // Ensures an owned texture matches the incoming frame. A resolution
    // change usually kills the stream with ACCESS_LOST, but a driver may
    // also start delivering different-size frames in place; copying those
    // into the old staging texture would misbehave silently. Reports and
    // returns false only when a needed texture cannot be created.
    bool ensureTexture(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& frameDescription, D3D11_USAGE usage,
                       ComPtr<ID3D11Texture2D>& staging)
    {
        if (staging) {
            D3D11_TEXTURE2D_DESC stagingCurrent{};
            staging->GetDesc(&stagingCurrent);
            if (stagingCurrent.Width != frameDescription.Width || stagingCurrent.Height != frameDescription.Height ||
                stagingCurrent.Format != frameDescription.Format) {
                staging.Reset();
            }
        }
        if (staging) {
            return true;
        }
        D3D11_TEXTURE2D_DESC stagingDescription = frameDescription;
        stagingDescription.Usage = usage;
        stagingDescription.BindFlags = 0;
        stagingDescription.CPUAccessFlags = usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ : 0;
        stagingDescription.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&stagingDescription, nullptr, &staging))) {
            reportStatus("could not create a capture texture");
            return false;
        }
        return true;
    }

    // Copies one acquired frame into a CPU buffer and publishes it. A BGRA8
    // desktop is copied as it is. An scRGB desktop - what duplication delivers
    // while Windows composes in HDR or with Auto Color Management - is
    // normalised to the display's SDR white level and packed as ten-bit codes,
    // so SDR content up to that white level survives in every composition mode.
    // Returns Fatal when the frame cannot
    // be copied (the loop must end), Published otherwise.
    FrameOutcome copyFrame(const DuplicationSetup& setup, ID3D11Texture2D* texture, FrameCopyState& state,
                           FrameMailbox& mailbox)
    {
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        const bool scrgb = description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM && !scrgb) {
            reportStatus("unsupported capture format");
            return FrameOutcome::Fatal;
        }
        // Missing display metadata is not an 80-nit white. End this stream so
        // recovery resolves the current target again before showing any codes.
        // Read every delivery because the SDR brightness slider can change.
        const auto white = scrgb ? sdrWhiteNits(setup.colorTarget) : std::optional<double>{ScrgbWhiteNits};
        if (!white) {
            reportStatus("could not read the display SDR white level");
            return FrameOutcome::Fatal;
        }
        // Only the rows and columns the region needs leave the GPU; the frame
        // says which part of the display it carries.
        const IntRect rect =
            softwareCropRect(currentCrop(), static_cast<int>(description.Width), static_cast<int>(description.Height));
        if (!stageRegion(setup, texture, description, rect, state.staging)) {
            return FrameOutcome::Fatal;
        }

        const int stride = rect.width * 4;  // both layouts the scopes read are four bytes per pixel
        const double whiteNits = *white;
        int abovePixels = 0;
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(setup.context->Map(state.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                reportStatus("could not map capture frame");
                return FrameOutcome::Fatal;
            }
            const MappedTexture mappedTexture{*setup.context.Get(), *state.staging.Get()};
            state.buffer.sizeTo(static_cast<std::size_t>(stride) * rect.height);
            state.buffer.sizeHdrTo(scrgb && m_hdrEnabled ? static_cast<std::size_t>(rect.width) * rect.height : 0,
                                   whiteNits);
            if (scrgb) {
                state.scrgb.setSdrWhiteNits(whiteNits);
                abovePixels = convertScrgbRows(state, mapped, rect.width, rect.height);
            } else {
                copyBgra8Rows(state.buffer, mapped, stride, rect.height);
            }
        }

        stampFrame(state, rect, description, scrgb ? PixelFormat::Argb2101010 : PixelFormat::Bgra8);
        logDelivery(state.buffer.format, whiteNits);
        if (scrgb) {
            logHeadroom(abovePixels, rect.width * rect.height);
        }
        state.buffer = mailbox.publish(std::move(state.buffer));
        state.publishedRect = rect;
        return FrameOutcome::Published;
    }

    // Content brighter than SDR white saturates at full scale, where the
    // scopes cannot tell it from white; a recording is told what share of the
    // frame that was, whenever the share changes decade.
    void logHeadroom(int abovePixels, int pixels)
    {
        const double share = pixels > 0 ? 100.0 * abovePixels / pixels : 0.0;
        if (m_loggedHeadroom.shouldLog(headroomBucket(abovePixels, share))) {
            SS_DIAG(Perf, "capture pixels above sdr white %.2f%%", share);
        }
    }

    // Copies @p rect of the acquired texture into a CPU-readable staging
    // texture of that size, recreating the staging texture when the shape or
    // format changes. Reports and returns false only when it cannot be made.
    bool stageRegion(const DuplicationSetup& setup, ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& description,
                     const IntRect& rect, ComPtr<ID3D11Texture2D>& staging)
    {
        D3D11_TEXTURE2D_DESC shape = description;
        shape.Width = static_cast<UINT>(rect.width);
        shape.Height = static_cast<UINT>(rect.height);
        if (!ensureTexture(setup.device.Get(), shape, D3D11_USAGE_STAGING, staging)) {
            return false;
        }
        const D3D11_BOX box{static_cast<UINT>(rect.x),
                            static_cast<UINT>(rect.y),
                            0,
                            static_cast<UINT>(rect.x + rect.width),
                            static_cast<UINT>(rect.y + rect.height),
                            1};
        setup.context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, 0, &box);

        return true;
    }

    // Describes the delivery in the recycled buffer: @p rect of a display with
    // @p display's extents. Every field is written on every frame, never only
    // on the ones that changed: the buffer comes back from the mailbox holding
    // the previous delivery's answers, and one left alone mislabels these
    // pixels.
    void stampFrame(FrameCopyState& state, const IntRect& rect, const D3D11_TEXTURE2D_DESC& display, PixelFormat format)
    {
        state.buffer.strideBytes = rect.width * 4;
        state.buffer.width = rect.width;
        state.buffer.height = rect.height;
        state.buffer.colorSpace = ColorSpaceHint::Srgb;
        state.buffer.format = format;
        state.buffer.sourceX = rect.x;
        state.buffer.sourceY = rect.y;
        state.buffer.sourceWidth = static_cast<int>(display.Width);
        state.buffer.sourceHeight = static_cast<int>(display.Height);
        state.buffer.stamp = state.stamp;
        // Reusing a sequence after restart can collide with a module's cached
        // pixel pointer and return bins from the previous display. The producer
        // counter spans every stream owned by this source.
        state.buffer.sequence = ++m_sequence;
    }

    // Logged when the delivery changes rather than on every frame, and stated
    // afresh to every recording: a log switched on later still has to say what
    // depth is being delivered and which white level it was normalised to.
    void logDelivery(PixelFormat format, double whiteNits)
    {
        const DeliveryDescription delivery{format, static_cast<int>(std::lround(whiteNits * 10.0))};
        if (!m_loggedDelivery.shouldLog(delivery)) {
            return;
        }
        if (format == PixelFormat::Argb2101010) {
            SS_DIAG(Perf, "capture format 10-bit from scRGB, sdr white %.1f nits", whiteNits);
        } else {
            SS_DIAG(Perf, "capture format 8-bit");
        }
    }

    std::thread m_worker;
    bool m_hdrEnabled = false;  // changed only while the worker is stopped
    uint64_t m_sequence = 0;    // worker-owned; stop joins before the next start
    std::atomic<bool> m_stopRequested{false};
    StatusCallback m_statusCallback;
    // The delivery this recording has been told about, read on the capture
    // thread and forgotten whenever a recording opens.
    DiagOnChange<DeliveryDescription> m_loggedDelivery{DiagChannel::Perf};
    // The decade of the above-white share this recording has been told about.
    DiagOnChange<int> m_loggedHeadroom{DiagChannel::Perf};
    // The part of the display the copy is narrowed to, written by the
    // application thread and read once per frame by the capture thread.
    std::mutex m_cropMutex;
    std::optional<IntRect> m_crop;
};

}  // namespace

std::unique_ptr<ScreenCaptureSource> createScreenCaptureSource()
{
    return std::make_unique<DxgiScreenCaptureSource>();
}

}  // namespace sidescopes
