// Windows desktop services. Compiles on every push via CI; runtime behavior
// awaits the Windows port proper. Coordinates are virtual-screen pixels with
// a top-left origin, the platform's own device-independent convention.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "platform/desktop.h"

#include <dwmapi.h>
#include <windows.h>
// shellapi.h (ShellExecuteW) depends on the windows.h types above; the
// comment keeps clang-format from sorting it ahead of them.
#include <shellapi.h>

#include <string>
#include <vector>

#include "platform/windows/display_identity.h"
#include "platform/windows/wide_strings.h"

namespace sidescopes {
namespace {

struct MonitorLookup
{
    uint32_t wantedId = 0;
    HMONITOR monitor = nullptr;
    RECT rect{};
};

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT rect, LPARAM context)
{
    auto* lookup = reinterpret_cast<MonitorLookup*>(context);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) && displayIdFromDeviceName(info.szDevice) == lookup->wantedId) {
        lookup->monitor = monitor;
        lookup->rect = *rect;
        return FALSE;
    }
    return TRUE;
}

std::optional<MonitorLookup> findMonitor(uint32_t displayId)
{
    MonitorLookup lookup;
    lookup.wantedId = displayId;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&lookup));
    if (!lookup.monitor) {
        return std::nullopt;
    }
    return lookup;
}

std::string applicationNameOfWindow(HWND window)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return {};
    }
    wchar_t path[MAX_PATH];
    DWORD length = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameW(process, 0, path, &length)) {
        std::wstring full(path, length);
        const auto slash = full.find_last_of(L"\\/");
        std::wstring base = slash == std::wstring::npos ? full : full.substr(slash + 1);
        const auto dot = base.find_last_of(L'.');
        if (dot != std::wstring::npos) {
            base.resize(dot);
        }
        name = utf8FromWide(base.c_str(), static_cast<int>(base.size()));
    }
    CloseHandle(process);
    return name;
}

struct WindowCollector
{
    HMONITOR monitor = nullptr;
    DWORD ownProcess = 0;
    std::vector<DesktopWindow>* windows = nullptr;
};

BOOL CALLBACK collectWindow(HWND window, LPARAM context)
{
    auto* collector = reinterpret_cast<WindowCollector*>(context);
    if (!IsWindowVisible(window)) {
        return TRUE;
    }
    if (GetWindowTextLengthW(window) == 0) {
        return TRUE;
    }
    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) {
        return TRUE;
    }

    // Minimized and suspended-store windows stay visible to EnumWindows but
    // are cloaked from the desktop.
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == collector->ownProcess) {
        return TRUE;
    }

    if (MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != collector->monitor) {
        return TRUE;
    }

    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        GetWindowRect(window, &frame);
    }
    const double width = static_cast<double>(frame.right) - frame.left;
    const double height = static_cast<double>(frame.bottom) - frame.top;
    if (width < 64.0 || height < 64.0) {
        return TRUE;
    }

    DesktopWindow entry;
    entry.x = static_cast<double>(frame.left);
    entry.y = static_cast<double>(frame.top);
    entry.width = width;
    entry.height = height;
    entry.application = applicationNameOfWindow(window);
    collector->windows->push_back(std::move(entry));
    return TRUE;
}

}  // namespace

std::vector<DesktopWindow> onScreenWindows(uint32_t displayId)
{
    std::vector<DesktopWindow> windows;
    const auto monitor = findMonitor(displayId);
    if (!monitor) {
        return windows;
    }

    WindowCollector collector;
    collector.monitor = monitor->monitor;
    collector.ownProcess = GetCurrentProcessId();
    collector.windows = &windows;
    // EnumWindows walks top-level windows in z-order, frontmost first,
    // matching the contract.
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&collector));
    return windows;
}

std::optional<DesktopPoint> globalCursorPosition()
{
    POINT point{};
    if (!GetCursorPos(&point)) {
        return std::nullopt;
    }
    return DesktopPoint{static_cast<double>(point.x), static_cast<double>(point.y)};
}

std::optional<DisplayGeometry> geometryOfDisplay(uint32_t displayId)
{
    const auto monitor = findMonitor(displayId);
    if (!monitor) {
        return std::nullopt;
    }
    return DisplayGeometry{static_cast<double>(monitor->rect.left), static_cast<double>(monitor->rect.top),
                           static_cast<double>(monitor->rect.right) - monitor->rect.left,
                           static_cast<double>(monitor->rect.bottom) - monitor->rect.top};
}

std::optional<uint32_t> displayAtPoint(DesktopPoint point)
{
    const POINT at{static_cast<LONG>(point.x), static_cast<LONG>(point.y)};
    HMONITOR monitor = MonitorFromPoint(at, MONITOR_DEFAULTTONULL);
    if (!monitor) {
        return std::nullopt;
    }
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return std::nullopt;
    }
    const uint32_t displayId = displayIdFromDeviceName(info.szDevice);
    if (displayId == 0) {
        return std::nullopt;
    }
    return displayId;
}

std::optional<uint32_t> displayUnderCursor()
{
    POINT point{};
    if (!GetCursorPos(&point)) {
        return std::nullopt;
    }
    return displayAtPoint(DesktopPoint{static_cast<double>(point.x), static_cast<double>(point.y)});
}

namespace {

std::string applicationDataDirectory()
{
    wchar_t appdata[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return ".";
    }
    return utf8FromWide(appdata, static_cast<int>(length));
}

}  // namespace

std::string preferencesFilePath()
{
    return applicationDataDirectory() + "\\SideScopes\\preferences.txt";
}

ModifierState currentModifiers()
{
    ModifierState state;
    state.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    state.control = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    state.option = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    state.command = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
    return state;
}

bool platformHidesWindowOnCommandW()
{
    return false;
}

bool platformMinimizesWindowOnControlW()
{
    return true;
}

bool platformQuitsOnControlQ()
{
    return true;
}

void hideApplication()
{
    // Windows dismisses through minimize; there is no application-wide
    // hide to invoke.
}

void openScreenRecordingSettings()
{
    // Reading the desktop needs no permission on Windows.
}

void openUrl(const char* url)
{
    const std::wstring target = wideFromUtf8(url);
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::vector<std::string> interfaceFontFiles()
{
    wchar_t windowsDirectory[MAX_PATH];
    const UINT length = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    const std::string base = utf8FromWide(windowsDirectory, static_cast<int>(length));
    return {base + "\\Fonts\\segoeui.ttf", base + "\\Fonts\\arial.ttf"};
}

std::vector<std::string> monospaceFontFiles()
{
    wchar_t windowsDirectory[MAX_PATH];
    const UINT length = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    const std::string base = utf8FromWide(windowsDirectory, static_cast<int>(length));
    return {base + "\\Fonts\\consola.ttf", base + "\\Fonts\\cour.ttf"};
}

bool captureExclusionDisabled()
{
    static const bool disabled = [] {
        wchar_t value[2];
        return GetEnvironmentVariableW(L"SIDESCOPES_NO_CAPTURE_EXCLUSION", value, 2) > 0;
    }();

    return disabled;
}

float monospaceFontScale()
{
    // Consolas' digit ink runs about a fifth taller than Segoe UI's at an
    // equal em; this factor reproduces the balance of the macOS pairing.
    return 0.866f;
}

void observeSystemWake(std::function<void()>)
{
    // Duplication dies loudly on lock and wake (access lost) and the
    // application's retry loop rebuilds it from scratch, so there is
    // nothing to observe here.
}

void observeEscapeWithoutKeyWindow(std::function<void()>)
{
    // The border window carries WS_EX_NOACTIVATE: interacting with it
    // never activates the application, so the active-but-focusless state
    // this seam exists for cannot occur here.
}

void sampleScreenColorAsync(DesktopPoint point, std::function<void(std::optional<FloatColor>)> callback)
{
    // GDI reads any monitor of the virtual screen synchronously.
    // CAPTUREBLT includes other applications' layered windows (tooltips,
    // notification toasts) the way the eye sees them; this application's
    // own overlays stay out through their capture affinity.
    constexpr int side = 3;
    const int left = static_cast<int>(point.x) - side / 2;
    const int top = static_cast<int>(point.y) - side / 2;

    HDC screen = GetDC(nullptr);
    if (!screen) {
        callback(std::nullopt);
        return;
    }
    std::optional<FloatColor> color;
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = side;
    info.bmiHeader.biHeight = -side;  // top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = memory ? CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (bitmap) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        if (BitBlt(memory, 0, 0, side, side, screen, left, top, SRCCOPY | CAPTUREBLT)) {
            const auto* pixels = static_cast<const uint8_t*>(bits);
            double sumR = 0;
            double sumG = 0;
            double sumB = 0;
            for (int index = 0; index < side * side; ++index) {
                sumB += pixels[index * 4 + 0];
                sumG += pixels[index * 4 + 1];
                sumR += pixels[index * 4 + 2];
            }
            constexpr double count = static_cast<double>(side) * side;
            color = FloatColor{static_cast<float>(sumR / count), static_cast<float>(sumG / count),
                               static_cast<float>(sumB / count)};
        }
        SelectObject(memory, previous);
        DeleteObject(bitmap);
    }
    if (memory) {
        DeleteDC(memory);
    }
    ReleaseDC(nullptr, screen);
    callback(color);
}

}  // namespace sidescopes
