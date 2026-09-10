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

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>
#include <system_error>
#include <thread>
#include <utility>

#include "core/diagnostics.h"
#include "core/parallel_for.h"
#include "core/scrgb.h"
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
    ComPtr<ID3D11Texture2D> staging;
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
// chunk writes only its own rows.
void convertScrgbRows(FrameCopyState& state, const D3D11_MAPPED_SUBRESOURCE& mapped, int width, int height)
{
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* target = state.buffer.data.data();
    const ScrgbToDisplayCodes& codes = state.scrgb;
    const std::size_t targetStride = static_cast<std::size_t>(width) * 4;
    runParallelChunks(parallelChunkCount(height, ScrgbRowsPerChunk), height,
                      [&](int, int rowBegin, int rowEnd) noexcept {
                          for (int row = rowBegin; row < rowEnd; ++row) {
                              codes.convertRow(source + static_cast<std::size_t>(row) * mapped.RowPitch,
                                               target + static_cast<std::size_t>(row) * targetStride, width);
                          }
                      });
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

private:
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
            const AcquireResult acquired = acquireFrame(duplication, resource);
            if (acquired == AcquireResult::Fatal) {
                return;
            }
            if (acquired == AcquireResult::Retry) {
                continue;
            }
            const AcquiredFrame acquiredFrame{*duplication};
            state.stamp.receivedSeconds = frameClockSeconds();

            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(resource.As(&texture))) {
                const FrameOutcome outcome = copyFrame(setup, texture.Get(), state, mailbox);
                if (outcome == FrameOutcome::Fatal) {
                    return;
                }
                if (outcome == FrameOutcome::Published) {
                    lastPublish = now;
                }
            }
        }
    }

    // Acquires the next changed frame. Timeouts and metadata-only
    // deliveries ask the caller to retry; a lost stream is fatal. On
    // Frame, the caller owns the matching ReleaseFrame.
    AcquireResult acquireFrame(IDXGIOutputDuplication* duplication, ComPtr<IDXGIResource>& resource)
    {
        DXGI_OUTDUPL_FRAME_INFO info{};
        const HRESULT acquired = duplication->AcquireNextFrame(100, &info, &resource);
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

    // Ensures the staging texture matches the incoming frame. A resolution
    // change usually kills the stream with ACCESS_LOST, but a driver may
    // also start delivering different-size frames in place; copying those
    // into the old staging texture would misbehave silently. Reports and
    // returns false only when a needed texture cannot be created.
    bool ensureStaging(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& frameDescription,
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
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.BindFlags = 0;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDescription.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&stagingDescription, nullptr, &staging))) {
            reportStatus("could not create a staging texture");
            return false;
        }
        return true;
    }

    // Copies one acquired frame into a CPU buffer and publishes it. A BGRA8
    // desktop is copied as it is. An scRGB desktop - what duplication delivers
    // while Windows composes in HDR or with Auto Color Management - is
    // normalised to the display's SDR white level and packed as ten-bit codes,
    // so SDR content reads the same codes in every composition mode and
    // nothing brighter than 80 nits clips. Returns Fatal when the frame cannot
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
        if (!ensureStaging(setup.device.Get(), description, state.staging)) {
            return FrameOutcome::Fatal;
        }
        setup.context->CopyResource(state.staging.Get(), texture);

        const int width = static_cast<int>(description.Width);
        const int height = static_cast<int>(description.Height);
        const int stride = width * 4;  // both layouts the scopes read are four bytes per pixel
        double whiteNits = ScrgbWhiteNits;
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(setup.context->Map(state.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                reportStatus("could not map capture frame");
                return FrameOutcome::Fatal;
            }
            const MappedTexture mappedTexture{*setup.context.Get(), *state.staging.Get()};
            state.buffer.sizeTo(static_cast<std::size_t>(stride) * height);
            if (scrgb) {
                // Read per frame: the user can move the SDR brightness slider
                // at any time, and nothing in the stream announces it.
                whiteNits = sdrWhiteNits(setup.colorTarget);
                state.scrgb.setSdrWhiteNits(whiteNits);
                convertScrgbRows(state, mapped, width, height);
            } else {
                copyBgra8Rows(state.buffer, mapped, stride, height);
            }
        }

        stampFrame(state, width, height, scrgb ? PixelFormat::Argb2101010 : PixelFormat::Bgra8);
        logDelivery(state.buffer.format, whiteNits);
        state.buffer = mailbox.publish(std::move(state.buffer));
        return FrameOutcome::Published;
    }

    // Describes the delivery in the recycled buffer. Every field is written on
    // every frame, never only on the ones that changed: the buffer comes back
    // from the mailbox holding the previous delivery's answers, and one left
    // alone mislabels these pixels.
    void stampFrame(FrameCopyState& state, int width, int height, PixelFormat format)
    {
        state.buffer.strideBytes = width * 4;
        state.buffer.width = width;
        state.buffer.height = height;
        state.buffer.colorSpace = ColorSpaceHint::Srgb;
        state.buffer.format = format;
        state.buffer.sourceX = 0;
        state.buffer.sourceY = 0;
        state.buffer.sourceWidth = width;
        state.buffer.sourceHeight = height;
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
    uint64_t m_sequence = 0;  // worker-owned; stop joins before the next start
    std::atomic<bool> m_stopRequested{false};
    StatusCallback m_statusCallback;
    // The delivery this recording has been told about, read on the capture
    // thread and forgotten whenever a recording opens.
    DiagOnChange<DeliveryDescription> m_loggedDelivery{DiagChannel::Perf};
};

}  // namespace

std::unique_ptr<ScreenCaptureSource> createScreenCaptureSource()
{
    return std::make_unique<DxgiScreenCaptureSource>();
}

}  // namespace sidescopes
