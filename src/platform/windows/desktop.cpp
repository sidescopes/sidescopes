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

namespace sidescopes {
namespace {

struct MonitorLookup {
    uint32_t wanted_index = 0;
    uint32_t current_index = 0;
    HMONITOR monitor = nullptr;
    RECT rect{};
};

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT rect, LPARAM context) {
    auto* lookup = reinterpret_cast<MonitorLookup*>(context);
    if (lookup->current_index == lookup->wanted_index) {
        lookup->monitor = monitor;
        lookup->rect = *rect;
        return FALSE;
    }
    ++lookup->current_index;
    return TRUE;
}

std::optional<MonitorLookup> FindMonitor(uint32_t display_id) {
    MonitorLookup lookup;
    lookup.wanted_index = display_id;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&lookup));
    if (!lookup.monitor) return std::nullopt;
    return lookup;
}

std::string Utf8FromWide(const wchar_t* wide, int wide_length) {
    if (wide_length <= 0) return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, wide, wide_length, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, wide_length, utf8.data(), size, nullptr, nullptr);
    return utf8;
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

void OpenScreenRecordingSettings() {
    // Reading the desktop needs no permission on Windows.
}

}  // namespace sidescopes
