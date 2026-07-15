// Windows screen capture via DXGI output duplication. Compiles on every
// push via CI; runtime behavior awaits the Windows port proper.
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
#include <cstring>
#include <thread>
#include <utility>

#include "platform/windows/display_identity.h"

namespace sidescopes {
namespace {

using Microsoft::WRL::ComPtr;

class DxgiScreenCaptureSource final : public ScreenCaptureSource
{
public:
    ~DxgiScreenCaptureSource() override
    {
        stop();
    }

    CapturePermission requestPermission()
    {
        // Reading the desktop needs no user consent on Windows.
        return CapturePermission::Granted;
    }

    std::vector<CaptureTarget> listTargets()
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

    bool start(const CaptureTarget& target, int maxFramesPerSecond, FrameMailbox& mailbox)
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
        m_worker = std::thread([this, adapterIndex, outputIndex, maxFramesPerSecond, &mailbox] {
            captureLoop(adapterIndex, outputIndex, maxFramesPerSecond, mailbox);
        });
        return true;
    }

    void stop()
    {
        m_stopRequested.store(true);
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    void setStatusCallback(StatusCallback callback)
    {
        m_statusCallback = std::move(callback);
    }

private:
    void reportStatus(const std::string& message)
    {
        if (m_statusCallback) {
            m_statusCallback(message);
        }
    }

    void captureLoop(UINT adapterIndex, UINT outputIndex, int maxFramesPerSecond, FrameMailbox& mailbox)
    {
        ComPtr<IDXGIFactory1> factory;
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput> output;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapters1(adapterIndex, &adapter)) ||
            FAILED(adapter->EnumOutputs(outputIndex, &output))) {
            reportStatus("capture target disappeared");
            return;
        }
        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output.As(&output1))) {
            reportStatus("output duplication is unavailable");
            return;
        }

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                     &device, nullptr, &context))) {
            reportStatus("could not create a capture device");
            return;
        }
        // The engines assume 8-bit BGRA. On an HDR display plain
        // DuplicateOutput delivers half-float scRGB, so ask for BGRA
        // explicitly where the system can tone-map for us (Windows 10
        // 1703+); older systems fall back to the plain call, whose format
        // is verified below and refused loudly rather than read as
        // garbage.
        ComPtr<IDXGIOutputDuplication> duplication;
        ComPtr<IDXGIOutput5> output5;
        HRESULT duplicated = E_NOINTERFACE;
        if (SUCCEEDED(output.As(&output5))) {
            const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
            duplicated = output5->DuplicateOutput1(device.Get(), 0, 1, formats, &duplication);
        }
        if (FAILED(duplicated)) {
            duplicated = output1->DuplicateOutput(device.Get(), &duplication);
        }
        if (FAILED(duplicated)) {
            reportStatus("could not duplicate the display");
            return;
        }
        DXGI_OUTDUPL_DESC duplicationDescription{};
        duplication->GetDesc(&duplicationDescription);
        if (duplicationDescription.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            reportStatus("unsupported capture format (HDR display?)");
            return;
        }
        // Rotated outputs deliver rotated rows: colors would survive but
        // the waveform axes and the region mapping would not.
        if (duplicationDescription.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
            duplicationDescription.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
            reportStatus("rotated displays are not supported yet");
            return;
        }

        ComPtr<ID3D11Texture2D> staging;
        FrameBuffer buffer;
        uint64_t sequence = 0;
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

            DXGI_OUTDUPL_FRAME_INFO info{};
            ComPtr<IDXGIResource> resource;
            const HRESULT acquired = duplication->AcquireNextFrame(100, &info, &resource);
            if (acquired == DXGI_ERROR_WAIT_TIMEOUT) {
                continue;
            }
            if (acquired == DXGI_ERROR_ACCESS_LOST) {
                reportStatus("capture access lost");
                return;
            }
            if (FAILED(acquired)) {
                reportStatus("capture failed");
                return;
            }

            // A zero present time is a metadata-only delivery - the mouse
            // moved, the image did not. Copying the unchanged desktop just
            // for the analysis hash to discard it would bill a full-screen
            // copy to every cursor twitch.
            if (info.LastPresentTime.QuadPart == 0) {
                duplication->ReleaseFrame();
                continue;
            }

            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(resource.As(&texture))) {
                D3D11_TEXTURE2D_DESC description{};
                texture->GetDesc(&description);
                // A resolution change usually kills the stream with
                // ACCESS_LOST, but a driver may also start delivering
                // different-size frames in place; copying those into the
                // old staging texture would misbehave silently.
                if (staging) {
                    D3D11_TEXTURE2D_DESC stagingCurrent{};
                    staging->GetDesc(&stagingCurrent);
                    if (stagingCurrent.Width != description.Width || stagingCurrent.Height != description.Height ||
                        stagingCurrent.Format != description.Format) {
                        staging.Reset();
                    }
                }
                if (!staging) {
                    D3D11_TEXTURE2D_DESC stagingDescription = description;
                    stagingDescription.Usage = D3D11_USAGE_STAGING;
                    stagingDescription.BindFlags = 0;
                    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    stagingDescription.MiscFlags = 0;
                    if (FAILED(device->CreateTexture2D(&stagingDescription, nullptr, &staging))) {
                        duplication->ReleaseFrame();
                        reportStatus("could not create a staging texture");
                        return;
                    }
                }
                context->CopyResource(staging.Get(), texture.Get());

                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                    const int width = static_cast<int>(description.Width);
                    const int height = static_cast<int>(description.Height);
                    const int stride = width * 4;
                    buffer.data.resize(static_cast<std::size_t>(stride) * height);
                    const auto* source = static_cast<const uint8_t*>(mapped.pData);
                    for (int row = 0; row < height; ++row) {
                        std::memcpy(buffer.data.data() + static_cast<std::size_t>(row) * stride,
                                    source + static_cast<std::size_t>(row) * mapped.RowPitch,
                                    static_cast<std::size_t>(stride));
                    }
                    context->Unmap(staging.Get(), 0);

                    buffer.strideBytes = stride;
                    buffer.width = width;
                    buffer.height = height;
                    buffer.colorSpace = ColorSpaceHint::Srgb;
                    buffer.sequence = ++sequence;
                    buffer = mailbox.publish(std::move(buffer));
                    lastPublish = now;
                }
            }
            duplication->ReleaseFrame();
        }
    }

    std::thread m_worker;
    std::atomic<bool> m_stopRequested{false};
    StatusCallback m_statusCallback;
};

}  // namespace

std::unique_ptr<ScreenCaptureSource> createScreenCaptureSource()
{
    return std::make_unique<DxgiScreenCaptureSource>();
}

}  // namespace sidescopes
