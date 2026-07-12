// Windows desktop services. Compiles on every push via CI; runtime behavior
// awaits the Windows port proper. Coordinates are virtual-screen pixels with
// a top-left origin, the platform's own device-independent convention.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "platform/desktop.h"

#include <dwmapi.h>
#include <windows.h>

#include <string>
#include <vector>

#include "platform/windows/display_identity.h"
#include "platform/windows/wide_strings.h"

namespace sidescopes {
namespace {

struct MonitorLookup {
    uint32_t wanted_id = 0;
    HMONITOR monitor = nullptr;
    RECT rect{};
};

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT rect, LPARAM context) {
    auto* lookup = reinterpret_cast<MonitorLookup*>(context);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) &&
        DisplayIdFromDeviceName(info.szDevice) == lookup->wanted_id) {
        lookup->monitor = monitor;
        lookup->rect = *rect;
        return FALSE;
    }
    return TRUE;
}

std::optional<MonitorLookup> FindMonitor(uint32_t display_id) {
    MonitorLookup lookup;
    lookup.wanted_id = display_id;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&lookup));
    if (!lookup.monitor) return std::nullopt;
    return lookup;
}

std::string ApplicationNameOfWindow(HWND window) {
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return {};
    wchar_t path[MAX_PATH];
    DWORD length = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameW(process, 0, path, &length)) {
        std::wstring full(path, length);
        const auto slash = full.find_last_of(L"\\/");
        std::wstring base = slash == std::wstring::npos ? full : full.substr(slash + 1);
        const auto dot = base.find_last_of(L'.');
        if (dot != std::wstring::npos) base.resize(dot);
        name = Utf8FromWide(base.c_str(), static_cast<int>(base.size()));
    }
    CloseHandle(process);
    return name;
}

struct WindowCollector {
    HMONITOR monitor = nullptr;
    DWORD own_process = 0;
    std::vector<DesktopWindow>* windows = nullptr;
};

BOOL CALLBACK CollectWindow(HWND window, LPARAM context) {
    auto* collector = reinterpret_cast<WindowCollector*>(context);
    if (!IsWindowVisible(window)) return TRUE;
    if (GetWindowTextLengthW(window) == 0) return TRUE;
    const LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (ex_style & WS_EX_TOOLWINDOW) return TRUE;

    // Minimized and suspended-store windows stay visible to EnumWindows but
    // are cloaked from the desktop.
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0)
        return TRUE;

    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == collector->own_process) return TRUE;

    if (MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != collector->monitor) return TRUE;

    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
        GetWindowRect(window, &frame);
    const double width = static_cast<double>(frame.right) - frame.left;
    const double height = static_cast<double>(frame.bottom) - frame.top;
    if (width < 64.0 || height < 64.0) return TRUE;

    DesktopWindow entry;
    entry.x = static_cast<double>(frame.left);
    entry.y = static_cast<double>(frame.top);
    entry.width = width;
    entry.height = height;
    entry.application = ApplicationNameOfWindow(window);
    collector->windows->push_back(std::move(entry));
    return TRUE;
}

}  // namespace

std::vector<DesktopWindow> OnScreenWindows(uint32_t display_id) {
    std::vector<DesktopWindow> windows;
    const auto monitor = FindMonitor(display_id);
    if (!monitor) return windows;

    WindowCollector collector;
    collector.monitor = monitor->monitor;
    collector.own_process = GetCurrentProcessId();
    collector.windows = &windows;
    // EnumWindows walks top-level windows in z-order, frontmost first,
    // matching the contract.
    EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&collector));
    return windows;
}

std::optional<DesktopPoint> GlobalCursorPosition() {
    POINT point{};
    if (!GetCursorPos(&point)) return std::nullopt;
    return DesktopPoint{static_cast<double>(point.x), static_cast<double>(point.y)};
}

std::optional<DisplayGeometry> GeometryOfDisplay(uint32_t display_id) {
    const auto monitor = FindMonitor(display_id);
    if (!monitor) return std::nullopt;
    return DisplayGeometry{static_cast<double>(monitor->rect.left),
                           static_cast<double>(monitor->rect.top),
                           static_cast<double>(monitor->rect.right) - monitor->rect.left,
                           static_cast<double>(monitor->rect.bottom) - monitor->rect.top};
}

std::optional<uint32_t> DisplayAtPoint(DesktopPoint point) {
    const POINT at{static_cast<LONG>(point.x), static_cast<LONG>(point.y)};
    HMONITOR monitor = MonitorFromPoint(at, MONITOR_DEFAULTTONULL);
    if (!monitor) return std::nullopt;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return std::nullopt;
    const uint32_t display_id = DisplayIdFromDeviceName(info.szDevice);
    if (display_id == 0) return std::nullopt;
    return display_id;
}

std::optional<uint32_t> DisplayUnderCursor() {
    POINT point{};
    if (!GetCursorPos(&point)) return std::nullopt;
    return DisplayAtPoint(DesktopPoint{static_cast<double>(point.x), static_cast<double>(point.y)});
}

namespace {

std::string ApplicationDataDirectory() {
    wchar_t appdata[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return ".";
    return Utf8FromWide(appdata, static_cast<int>(length));
}

}  // namespace

std::string PreferencesFilePath() {
    return ApplicationDataDirectory() + "\\SideScopes\\preferences.txt";
}

ModifierState CurrentModifiers() {
    ModifierState state;
    state.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    state.control = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    state.option = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    state.command =
        (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
    return state;
}

void OpenScreenRecordingSettings() {
    // Reading the desktop needs no permission on Windows.
}

std::vector<std::string> InterfaceFontFiles() {
    wchar_t windows_directory[MAX_PATH];
    const UINT length = GetWindowsDirectoryW(windows_directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    const std::string base = Utf8FromWide(windows_directory, static_cast<int>(length));
    return {base + "\\Fonts\\segoeui.ttf", base + "\\Fonts\\arial.ttf"};
}

void ObserveSystemWake(std::function<void()>) {
    // Duplication dies loudly on lock and wake (access lost) and the
    // application's retry loop rebuilds it from scratch, so there is
    // nothing to observe here.
}

void ObserveEscapeWithoutKeyWindow(std::function<void()>) {
    // The border window carries WS_EX_NOACTIVATE: interacting with it
    // never activates the application, so the active-but-focusless state
    // this seam exists for cannot occur here.
}

void SampleScreenColorAsync(DesktopPoint point,
                            std::function<void(std::optional<FloatColor>)> callback) {
    // GDI reads any monitor of the virtual screen synchronously.
    // CAPTUREBLT includes other applications' layered windows (tooltips,
    // notification toasts) the way the eye sees them; this application's
    // own overlays stay out through their capture affinity.
    constexpr int kSide = 3;
    const int left = static_cast<int>(point.x) - kSide / 2;
    const int top = static_cast<int>(point.y) - kSide / 2;

    HDC screen = GetDC(nullptr);
    if (!screen) {
        callback(std::nullopt);
        return;
    }
    std::optional<FloatColor> color;
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = kSide;
    info.bmiHeader.biHeight = -kSide;  // top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap =
        memory ? CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (bitmap) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        if (BitBlt(memory, 0, 0, kSide, kSide, screen, left, top, SRCCOPY | CAPTUREBLT)) {
            const auto* pixels = static_cast<const uint8_t*>(bits);
            double sum_r = 0;
            double sum_g = 0;
            double sum_b = 0;
            for (int index = 0; index < kSide * kSide; ++index) {
                sum_b += pixels[index * 4 + 0];
                sum_g += pixels[index * 4 + 1];
                sum_r += pixels[index * 4 + 2];
            }
            constexpr double kCount = static_cast<double>(kSide) * kSide;
            color =
                FloatColor{static_cast<float>(sum_r / kCount), static_cast<float>(sum_g / kCount),
                           static_cast<float>(sum_b / kCount)};
        }
        SelectObject(memory, previous);
        DeleteObject(bitmap);
    }
    if (memory) DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    callback(color);
}

}  // namespace sidescopes
