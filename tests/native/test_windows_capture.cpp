#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <catch2/catch_test_macros.hpp>
#include <cwchar>
#include <functional>

namespace {

// Only this executable redirects DisplayConfig. Both production translation
// units below retain their normal control flow and use a real D3D11 device.
int g_sizeCalls = 0;
int g_queryCalls = 0;
int g_smallBuffers = 0;
LONG g_sizeResult = ERROR_SUCCESS;
LONG g_infoResult = ERROR_SUCCESS;
UINT32 g_whiteLevel = 3000;

LONG testBufferSizes(UINT32, UINT32* paths, UINT32* modes)
{
    ++g_sizeCalls;
    *paths = 1;
    *modes = 1;
    return g_sizeResult;
}

LONG testDisplayConfig(UINT32, UINT32* paths, DISPLAYCONFIG_PATH_INFO* path, UINT32*, DISPLAYCONFIG_MODE_INFO*,
                       DISPLAYCONFIG_TOPOLOGY_ID*)
{
    ++g_queryCalls;
    if (g_queryCalls <= g_smallBuffers) {
        return ERROR_INSUFFICIENT_BUFFER;
    }
    *paths = 1;
    *path = {};
    path->targetInfo.id = 7;
    return ERROR_SUCCESS;
}

LONG testDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_HEADER* header)
{
    if (g_infoResult != ERROR_SUCCESS) {
        return g_infoResult;
    }
    if (header->type == DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME) {
        auto* source = reinterpret_cast<DISPLAYCONFIG_SOURCE_DEVICE_NAME*>(header);
        wcscpy_s(source->viewGdiDeviceName, L"\\\\.\\DISPLAY1");
        return ERROR_SUCCESS;
    }
    auto* white = reinterpret_cast<DISPLAYCONFIG_SDR_WHITE_LEVEL*>(header);
    white->SDRWhiteLevel = g_whiteLevel;
    return ERROR_SUCCESS;
}

void resetDisplayConfig()
{
    g_sizeCalls = g_queryCalls = g_smallBuffers = 0;
    g_sizeResult = g_infoResult = ERROR_SUCCESS;
    g_whiteLevel = 3000;
}

}  // namespace

#define GetDisplayConfigBufferSizes testBufferSizes
#define QueryDisplayConfig testDisplayConfig
#define DisplayConfigGetDeviceInfo testDeviceInfo
// Compile the production implementation with fault-injected display configuration calls.
#include "platform/windows/advanced_color.cpp"  // NOLINT(bugprone-suspicious-include)
#undef DisplayConfigGetDeviceInfo
#undef QueryDisplayConfig
#undef GetDisplayConfigBufferSizes

// Exercise the production capture loop through the test-only friend accessor.
#include "platform/windows/screen_capture.cpp"  // NOLINT(bugprone-suspicious-include)

namespace sidescopes {
namespace {

using namespace std::chrono_literals;

struct DxgiCaptureTestAccess
{
    static void capture(DxgiScreenCaptureSource& source, const DuplicationSetup& setup, FrameMailbox& mailbox)
    {
        source.captureFrames(setup, 0, mailbox, FrameStamp{12, 34, 0.0});
    }
};

struct ScriptedDuplication final : IDXGIOutputDuplication
{
    ULONG refs = 1;
    unsigned acquisitions = 0;
    unsigned releases = 0;
    UINT lastTimeout = 0;
    std::function<HRESULT(unsigned, DXGI_OUTDUPL_FRAME_INFO*, IDXGIResource**)> acquire;
    std::function<void()> release;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override
    {
        *result = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IDXGIOutputDuplication)) {
            *result = this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        return --refs;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override
    {
        return E_NOTIMPL;
    }

    void STDMETHODCALLTYPE GetDesc(DXGI_OUTDUPL_DESC* description) override
    {
        *description = {};
    }

    HRESULT STDMETHODCALLTYPE AcquireNextFrame(UINT timeout, DXGI_OUTDUPL_FRAME_INFO* info,
                                               IDXGIResource** result) override
    {
        lastTimeout = timeout;
        *info = {};
        *result = nullptr;
        return acquire(acquisitions++, info, result);
    }

    HRESULT STDMETHODCALLTYPE GetFrameDirtyRects(UINT, RECT*, UINT*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetFrameMoveRects(UINT, DXGI_OUTDUPL_MOVE_RECT*, UINT*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetFramePointerShape(UINT, void*, UINT*, DXGI_OUTDUPL_POINTER_SHAPE_INFO*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE MapDesktopSurface(DXGI_MAPPED_RECT*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE UnMapDesktopSurface() override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE ReleaseFrame() override
    {
        ++releases;
        if (release) {
            release();
        }
        return S_OK;
    }
};

DuplicationSetup makeSetup(ScriptedDuplication& duplication)
{
    DuplicationSetup setup;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                        &setup.device, nullptr, &setup.context)));
    setup.duplication = &duplication;
    setup.colorTarget = ColorTarget{0, 0, 7, true};
    return setup;
}

ComPtr<ID3D11Texture2D> makeTexture(const DuplicationSetup& setup, UINT width, UINT height, bool half = false)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = half ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    const UINT stride = width * (half ? 8 : 4);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(stride) * height);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            auto* pixel =
                bytes.data() + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * (half ? 8 : 4);
            if (half) {
                const uint16_t values[] = {0x3E00, 0x3E00, 0x3E00, 0x3C00};  // linear 1.5 at white 3.0
                std::memcpy(pixel, values, sizeof values);
            } else {
                pixel[0] = 30;
                pixel[1] = static_cast<uint8_t>(20 + y);
                pixel[2] = static_cast<uint8_t>(10 + x);
                pixel[3] = 255;
            }
        }
    }
    const D3D11_SUBRESOURCE_DATA data{bytes.data(), stride, stride * height};
    ComPtr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(setup.device->CreateTexture2D(&description, &data, &texture)));
    return texture;
}

HRESULT deliver(ID3D11Texture2D* texture, DXGI_OUTDUPL_FRAME_INFO* info, IDXGIResource** resource, bool mouse = false)
{
    info->LastPresentTime.QuadPart = mouse ? 0 : 1;
    return texture->QueryInterface(IID_PPV_ARGS(resource));
}

double checkCrop(FrameMailbox& mailbox, const IntRect& rect, uint64_t sequence, int width = 8, int height = 4)
{
    const auto frame = mailbox.takeLatest(0ms);
    REQUIRE(frame);
    CHECK(frame->width == rect.width);
    CHECK(frame->height == rect.height);
    CHECK(frame->sourceX == rect.x);
    CHECK(frame->sourceY == rect.y);
    CHECK(frame->sourceWidth == width);
    CHECK(frame->sourceHeight == height);
    CHECK(frame->sequence == sequence);
    CHECK(frame->stamp.captureEpoch == 12);
    CHECK(frame->stamp.displayId == 34);
    for (int y = 0; y < rect.height; ++y) {
        for (int x = 0; x < rect.width; ++x) {
            const auto* pixel =
                frame->data.data() + static_cast<std::size_t>(y) * frame->strideBytes + static_cast<std::size_t>(x) * 4;
            CHECK(pixel[0] == 30);
            CHECK(pixel[1] == 20 + rect.y + y);
            CHECK(pixel[2] == 10 + rect.x + x);
            CHECK(pixel[3] == 255);
        }
    }
    return frame->stamp.receivedSeconds;
}

TEST_CASE("Crop changes publish an owned desktop through timeouts and mouse updates")
{
    ScriptedDuplication duplication;
    const auto setup = makeSetup(duplication);
    const auto texture = makeTexture(setup, 8, 4);
    DxgiScreenCaptureSource source;
    FrameMailbox mailbox;
    source.narrowTo(IntRect{0, 0, 2, 2});
    double received = 0.0;
    duplication.release = [&] {
        if (duplication.releases == 1) {
            received = checkCrop(mailbox, {0, 0, 2, 2}, 1);
            // The acquired surface may be reused after release. Poison it to
            // prove that every later crop reads our owned copy instead.
            const std::array<uint8_t, 128> poison{};
            setup.context->UpdateSubresource(texture.Get(), 0, nullptr, poison.data(), 32, 128);
            source.narrowTo(IntRect{5, 2, 3, 2});
        }
    };
    duplication.acquire = [&](unsigned step, auto* info, auto** resource) {
        switch (step) {
        case 0:
            return deliver(texture.Get(), info, resource);
        case 1:
            CHECK(duplication.lastTimeout == 0);
            return DXGI_ERROR_WAIT_TIMEOUT;
        case 2:
            CHECK(checkCrop(mailbox, {5, 2, 3, 2}, 2) == received);
            source.narrowTo(std::nullopt);
            return DXGI_ERROR_WAIT_TIMEOUT;
        case 3:
            CHECK(checkCrop(mailbox, {0, 0, 8, 4}, 3) == received);
            source.narrowTo(IntRect{2, 1, 2, 2});
            return deliver(texture.Get(), info, resource, true);
        case 4:
            CHECK(checkCrop(mailbox, {2, 1, 2, 2}, 4) == received);
            source.narrowTo(IntRect{2, 1, 2, 2});
            return DXGI_ERROR_WAIT_TIMEOUT;
        default:
            CHECK_FALSE(mailbox.takeLatest(0ms));
            return DXGI_ERROR_ACCESS_LOST;
        }
    };
    DxgiCaptureTestAccess::capture(source, setup, mailbox);
    CHECK(duplication.acquisitions == 6);
    CHECK(duplication.releases == 2);
}

TEST_CASE("A new desktop replaces the saved image and its dimensions")
{
    ScriptedDuplication duplication;
    const auto setup = makeSetup(duplication);
    const auto first = makeTexture(setup, 8, 4);
    const auto second = makeTexture(setup, 10, 6);
    DxgiScreenCaptureSource source;
    FrameMailbox mailbox;
    source.narrowTo(IntRect{6, 2, 4, 4});
    duplication.release = [&] {
        if (duplication.releases == 1) {
            (void)checkCrop(mailbox, {6, 2, 2, 2}, 1);
            source.narrowTo(IntRect{5, 1, 5, 5});
        }
    };
    duplication.acquire = [&](unsigned step, auto* info, auto** resource) {
        if (step == 0) {
            CHECK_FALSE(mailbox.takeLatest(0ms));
            return DXGI_ERROR_WAIT_TIMEOUT;
        }
        if (step == 1) {
            return deliver(first.Get(), info, resource);
        }
        if (step == 2) {
            // Change the crop as well as the desktop. The new frame must win
            // immediately; publishing a saved crop here would starve video
            // whenever the user drags faster than the capture cadence.
            CHECK_FALSE(mailbox.takeLatest(0ms));
            CHECK(duplication.lastTimeout == 0);
            return deliver(second.Get(), info, resource);
        }
        (void)checkCrop(mailbox, {5, 1, 5, 5}, 2, 10, 6);
        return DXGI_ERROR_ACCESS_LOST;
    };
    DxgiCaptureTestAccess::capture(source, setup, mailbox);
    CHECK(duplication.releases == 2);
}

TEST_CASE("scRGB metadata errors publish no assumed-white pixels and can recover")
{
    for (const LONG result : {ERROR_GEN_FAILURE, ERROR_SUCCESS}) {
        resetDisplayConfig();
        g_infoResult = result;
        ScriptedDuplication duplication;
        const auto setup = makeSetup(duplication);
        const auto texture = makeTexture(setup, 8, 4, true);
        DxgiScreenCaptureSource source;
        FrameMailbox mailbox;
        std::string status;
        source.setStatusCallback([&](const std::string& message) { status = message; });
        duplication.acquire = [&](unsigned step, auto* info, auto** resource) {
            return step == 0 ? deliver(texture.Get(), info, resource) : DXGI_ERROR_ACCESS_LOST;
        };
        DxgiCaptureTestAccess::capture(source, setup, mailbox);
        const auto frame = mailbox.takeLatest(0ms);
        if (result == ERROR_SUCCESS) {
            REQUIRE(frame);
            CHECK(frame->format == PixelFormat::Argb2101010);
            const uint32_t expected = 0xC0000000u | 752u << 20 | 752u << 10 | 752u;
            uint32_t pixel = 0;
            std::memcpy(&pixel, frame->data.data(), sizeof pixel);
            CHECK(pixel == expected);
        } else {
            CHECK_FALSE(frame);
            CHECK(status == "could not read the display SDR white level");
        }
        CHECK(duplication.releases == 1);
    }
}

TEST_CASE("Display target lookup retries a topology growth and bounds repeated races")
{
    for (const int races : {0, 1, 2, 3}) {
        resetDisplayConfig();
        g_smallBuffers = races;
        const auto target = findColorTarget(L"\\\\.\\DISPLAY1");
        CHECK(target.found == (races < 3));
        CHECK(g_queryCalls == (races < 3 ? races + 1 : 3));
        CHECK(g_sizeCalls == g_queryCalls);
        if (target.found) {
            CHECK(target.id == 7);
            CHECK(sdrWhiteNits(target) == 240.0);
        }
    }
    resetDisplayConfig();
    g_sizeResult = ERROR_ACCESS_DENIED;
    CHECK_FALSE(findColorTarget(L"\\\\.\\DISPLAY1").found);
    CHECK(g_queryCalls == 0);
    resetDisplayConfig();
    g_whiteLevel = 0;
    CHECK_FALSE(sdrWhiteNits(ColorTarget{0, 0, 7, true}));
}

}  // namespace
}  // namespace sidescopes
