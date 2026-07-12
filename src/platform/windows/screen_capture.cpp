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

class DxgiScreenCaptureSource final : public ScreenCaptureSource {
public:
    ~DxgiScreenCaptureSource() override { Stop(); }

    CapturePermission RequestPermission() override {
        // Reading the desktop needs no user consent on Windows.
        return CapturePermission::Granted;
    }

    std::vector<CaptureTarget> ListTargets() override {
        std::vector<CaptureTarget> targets;
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return targets;

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT adapter_index = 0;
             factory->EnumAdapters1(adapter_index, &adapter) != DXGI_ERROR_NOT_FOUND;
             ++adapter_index) {
            ComPtr<IDXGIOutput> output;
            for (UINT output_index = 0;
                 adapter->EnumOutputs(output_index, &output) != DXGI_ERROR_NOT_FOUND;
                 ++output_index) {
                DXGI_OUTPUT_DESC description{};
                if (FAILED(output->GetDesc(&description)) || !description.AttachedToDesktop)
                    continue;
                CaptureTarget target;
                target.identifier =
                    std::to_string(adapter_index) + ":" + std::to_string(output_index);
                char name[64];
                const int written = WideCharToMultiByte(CP_UTF8, 0, description.DeviceName, -1,
                                                        name, sizeof(name), nullptr, nullptr);
                target.description = written > 0 ? name : "Display";
                target.display_id = DisplayIdFromDeviceName(description.DeviceName);
                const RECT& rect = description.DesktopCoordinates;
                target.width_points = static_cast<int>(rect.right - rect.left);
                target.height_points = static_cast<int>(rect.bottom - rect.top);
                targets.push_back(std::move(target));
            }
        }
        return targets;
    }

    bool Start(const CaptureTarget& target, int max_frames_per_second,
               FrameMailbox& mailbox) override {
        Stop();
        UINT adapter_index = 0;
        UINT output_index = 0;
        const auto separator = target.identifier.find(':');
        if (separator == std::string::npos) return false;
        adapter_index = static_cast<UINT>(std::strtoul(target.identifier.c_str(), nullptr, 10));
        output_index =
            static_cast<UINT>(std::strtoul(target.identifier.c_str() + separator + 1, nullptr, 10));

        stop_requested_.store(false);
        worker_ = std::thread([this, adapter_index, output_index, max_frames_per_second, &mailbox] {
            CaptureLoop(adapter_index, output_index, max_frames_per_second, mailbox);
        });
        return true;
    }

    void Stop() override {
        stop_requested_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    void SetStatusCallback(StatusCallback callback) override {
        status_callback_ = std::move(callback);
    }

private:
    void ReportStatus(const std::string& message) {
        if (status_callback_) status_callback_(message);
    }

    void CaptureLoop(UINT adapter_index, UINT output_index, int max_frames_per_second,
                     FrameMailbox& mailbox) {
        ComPtr<IDXGIFactory1> factory;
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput> output;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapters1(adapter_index, &adapter)) ||
            FAILED(adapter->EnumOutputs(output_index, &output))) {
            ReportStatus("capture target disappeared");
            return;
        }
        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output.As(&output1))) {
            ReportStatus("output duplication is unavailable");
            return;
        }

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                     D3D11_SDK_VERSION, &device, nullptr, &context))) {
            ReportStatus("could not create a capture device");
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
        if (FAILED(duplicated)) duplicated = output1->DuplicateOutput(device.Get(), &duplication);
        if (FAILED(duplicated)) {
            ReportStatus("could not duplicate the display");
            return;
        }
        DXGI_OUTDUPL_DESC duplication_description{};
        duplication->GetDesc(&duplication_description);
        if (duplication_description.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            ReportStatus("unsupported capture format (HDR display?)");
            return;
        }
        // Rotated outputs deliver rotated rows: colors would survive but
        // the waveform axes and the region mapping would not.
        if (duplication_description.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
            duplication_description.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
            ReportStatus("rotated displays are not supported yet");
            return;
        }

        ComPtr<ID3D11Texture2D> staging;
        FrameBuffer buffer;
        uint64_t sequence = 0;
        const auto minimum_interval = std::chrono::microseconds(
            max_frames_per_second > 0 ? 1000000 / max_frames_per_second : 0);
        auto last_publish = std::chrono::steady_clock::now() - minimum_interval;

        while (!stop_requested_.load()) {
            DXGI_OUTDUPL_FRAME_INFO info{};
            ComPtr<IDXGIResource> resource;
            const HRESULT acquired = duplication->AcquireNextFrame(100, &info, &resource);
            if (acquired == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (acquired == DXGI_ERROR_ACCESS_LOST) {
                ReportStatus("capture access lost");
                return;
            }
            if (FAILED(acquired)) {
                ReportStatus("capture failed");
                return;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - last_publish < minimum_interval) {
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
                    D3D11_TEXTURE2D_DESC staging_current{};
                    staging->GetDesc(&staging_current);
                    if (staging_current.Width != description.Width ||
                        staging_current.Height != description.Height ||
                        staging_current.Format != description.Format)
                        staging.Reset();
                }
                if (!staging) {
                    D3D11_TEXTURE2D_DESC staging_description = description;
                    staging_description.Usage = D3D11_USAGE_STAGING;
                    staging_description.BindFlags = 0;
                    staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    staging_description.MiscFlags = 0;
                    if (FAILED(device->CreateTexture2D(&staging_description, nullptr, &staging))) {
                        duplication->ReleaseFrame();
                        ReportStatus("could not create a staging texture");
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

                    buffer.stride_bytes = stride;
                    buffer.width = width;
                    buffer.height = height;
                    buffer.color_space = ColorSpaceHint::Srgb;
                    buffer.sequence = ++sequence;
                    buffer = mailbox.Publish(std::move(buffer));
                    last_publish = now;
                }
            }
            duplication->ReleaseFrame();
        }
    }

    std::thread worker_;
    std::atomic<bool> stop_requested_{false};
    StatusCallback status_callback_;
};

}  // namespace

std::unique_ptr<ScreenCaptureSource> CreateScreenCaptureSource() {
    return std::make_unique<DxgiScreenCaptureSource>();
}

}  // namespace sidescopes
