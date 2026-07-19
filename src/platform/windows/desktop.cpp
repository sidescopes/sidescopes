// Windows desktop services. Compiles on every push via CI; runtime behavior
// awaits the Windows port proper. Coordinates are virtual-screen pixels with
// a top-left origin, the platform's own device-independent convention.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "platform/desktop.h"

#include <dwmapi.h>
#include <windows.h>

#include "platform/focus_resolution.h"
// shellapi.h (ShellExecuteW) depends on the windows.h types above; the
// comment keeps clang-format from sorting it ahead of them.
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "platform/windows/display_identity.h"
#include "platform/windows/wide_strings.h"

// Windows 10 2004; absent from older SDKs.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

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
    entry.windowIdentity = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(window));
    entry.ownerPid = static_cast<int64_t>(processId);
    collector->windows->push_back(std::move(entry));
    return TRUE;
}

// The main application window, remembered so hideApplication can target it:
// Windows has no application-wide hide.
HWND g_mainWindow = nullptr;

HWND windowFromIdentity(uint64_t identity)
{
    return reinterpret_cast<HWND>(static_cast<uintptr_t>(identity));
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

std::vector<DesktopWindow> attachCandidateWindows(uint32_t displayId)
{
    // No level shifts on Windows: the ordinary window list is the answer.
    return onScreenWindows(displayId);
}

std::optional<WindowGeometry> windowGeometry(uint64_t identity)
{
    HWND window = windowFromIdentity(identity);
    if (!IsWindow(window)) {
        return std::nullopt;
    }

    WindowGeometry geometry;
    geometry.minimized = IsIconic(window) != 0;
    RECT frame{};
    // The extended frame bounds match onScreenWindows; the plain window rect is
    // the fallback where the compositor cannot answer.
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        GetWindowRect(window, &frame);
    }
    geometry.x = static_cast<double>(frame.left);
    geometry.y = static_cast<double>(frame.top);
    geometry.width = static_cast<double>(frame.right) - frame.left;
    geometry.height = static_cast<double>(frame.bottom) - frame.top;
    wchar_t title[256];
    const int titleLength = GetWindowTextW(window, title, 256);
    if (titleLength > 0) {
        geometry.title = utf8FromWide(title, titleLength);
    }

    return geometry;
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

std::string displayName(uint32_t displayId)
{
    const auto monitor = findMonitor(displayId);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor->monitor, &info)) {
        return "Display";
    }
    // "\\.\DISPLAY2" -> "Display 2": the friendly product name would
    // need the display-config API; the device ordinal is honest and stable.
    const std::wstring device(info.szDevice);
    const std::size_t digits = device.find_first_of(L"0123456789");
    if (digits == std::wstring::npos) {
        return "Display";
    }
    std::string name = "Display ";
    for (std::size_t index = digits; index < device.size(); ++index) {
        name.push_back(static_cast<char>(device[index]));
    }

    return name;
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
    // No application-wide hide exists here, so this hides the remembered
    // main window. The platform's own dismissal chord (Ctrl+W) goes through
    // minimize instead and never reaches this.
    if (g_mainWindow) {
        ShowWindow(g_mainWindow, SW_HIDE);
    }
}

bool applicationHidden()
{
    // Dismissal is minimizing here; the caller reads the iconified state off
    // its own window.
    return false;
}

namespace {

std::function<void()> g_foregroundCallback;

// The shell surfaces that take the foreground mid focus switch: the alt-tab
// and task-view hosts and the staging window foreground changes pass
// through. The user works in none of them, so none may reroute the region.
bool isFocusTransitionSurface(HWND window)
{
    wchar_t className[64];
    const int length = GetClassNameW(window, className, 64);
    if (length <= 0) {
        return false;
    }

    return wcscmp(className, L"XamlExplorerHostIslandWindow") == 0 ||  // Windows 11 alt-tab and task view
           wcscmp(className, L"MultitaskingViewFrame") == 0 ||         // Windows 10 alt-tab and task view
           wcscmp(className, L"ForegroundStaging") == 0 ||             // transient staging between switches
           wcscmp(className, L"TaskSwitcherWnd") == 0 ||               // classic alt-tab
           wcscmp(className, L"TaskSwitcherOverlayWnd") == 0;
}

// The switcher's acrylic backdrop samples the desktop through a capture,
// and a capture-excluded window is a hole in that sample the compositor
// fills unstably - the covered part of the window flickers. While a
// switcher surface holds the foreground the main window rejoins captures;
// the exclusion returns with the next real foreground.
void updateCaptureExclusion(HWND foreground)
{
    if (!g_mainWindow || captureExclusionDisabled()) {
        return;
    }
    SetWindowDisplayAffinity(g_mainWindow, isFocusTransitionSurface(foreground) ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE);
}

void CALLBACK foregroundWinEvent(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD)
{
    updateCaptureExclusion(hwnd);
    if (g_foregroundCallback) {
        g_foregroundCallback();
    }
}

}  // namespace

void observeForegroundChanges(std::function<void()> callback)
{
    g_foregroundCallback = std::move(callback);
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, foregroundWinEvent, 0, 0,
                    WINEVENT_OUTOFCONTEXT);
}

namespace {

HWINEVENTHOOK g_motionMoveSizeHook = nullptr;
HWINEVENTHOOK g_motionLocationHook = nullptr;
HWND g_motionWindow = nullptr;
std::function<void(WindowMotionSignal)> g_motionCallback;

void CALLBACK motionWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD)
{
    if (hwnd != g_motionWindow || idObject != OBJID_WINDOW || !g_motionCallback) {
        return;
    }
    if (event == EVENT_SYSTEM_MOVESIZESTART) {
        g_motionCallback(WindowMotionSignal::GripDown);
        g_motionCallback(WindowMotionSignal::MotionImminent);
    } else if (event == EVENT_SYSTEM_MOVESIZEEND) {
        g_motionCallback(WindowMotionSignal::GripUp);
    } else if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        g_motionCallback(WindowMotionSignal::Moved);
    }
}

}  // namespace

void watchWindowMotion(uint64_t identity, int64_t ownerPid, std::function<void(WindowMotionSignal)> callback)
{
    unwatchWindowMotion();
    g_motionWindow = windowFromIdentity(identity);
    g_motionCallback = std::move(callback);
    // Out-of-context hooks scoped to the owning process deliver on this
    // thread whenever it pumps messages - the idle wait included, so the
    // border reacts to the grab, not to the next frame. The move-size pair
    // brackets the user's drag loop; location changes also cover
    // programmatic moves the loop never sees.
    const DWORD process = static_cast<DWORD>(ownerPid);
    g_motionMoveSizeHook = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, nullptr,
                                           motionWinEvent, process, 0, WINEVENT_OUTOFCONTEXT);
    g_motionLocationHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                                           motionWinEvent, process, 0, WINEVENT_OUTOFCONTEXT);
}

void unwatchWindowMotion()
{
    if (g_motionMoveSizeHook) {
        UnhookWinEvent(g_motionMoveSizeHook);
        g_motionMoveSizeHook = nullptr;
    }
    if (g_motionLocationHook) {
        UnhookWinEvent(g_motionLocationHook);
        g_motionLocationHook = nullptr;
    }
    g_motionWindow = nullptr;
    g_motionCallback = nullptr;
}

namespace {

// The qualifying top-level windows, front to back, for the shared focus
// rule: visible and not minimized.
std::vector<OrderedWindow> orderedTopLevelWindows()
{
    std::vector<OrderedWindow> windows;
    for (HWND handle = GetTopWindow(nullptr); handle; handle = GetWindow(handle, GW_HWNDNEXT)) {
        if (!IsWindowVisible(handle) || IsIconic(handle)) {
            continue;
        }
        RECT rect{};
        if (!GetWindowRect(handle, &rect)) {
            continue;
        }
        DWORD ownerPid = 0;
        GetWindowThreadProcessId(handle, &ownerPid);
        windows.push_back({static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle)), static_cast<int64_t>(ownerPid),
                           static_cast<double>(rect.left), static_cast<double>(rect.top),
                           static_cast<double>(rect.right - rect.left), static_cast<double>(rect.bottom - rect.top)});
    }

    return windows;
}

}  // namespace

std::optional<uint64_t> focusedWindowForTracking(int64_t applicationPid, const std::vector<uint64_t>& tracked)
{
    return resolveTrackedFocus(orderedTopLevelWindows(), applicationPid, tracked);
}

void raiseWindow(uint64_t identity, int64_t)
{
    HWND window = windowFromIdentity(identity);
    // The picker holds the foreground when this runs, so the system honors
    // handing it to the picked window directly.
    if (IsWindow(window)) {
        SetForegroundWindow(window);
    }
}

void rememberApplicationWindow(void* nativeWindow)
{
    g_mainWindow = static_cast<HWND>(nativeWindow);
}

int64_t ownApplicationPid()
{
    return static_cast<int64_t>(GetCurrentProcessId());
}

int64_t foregroundApplicationPid()
{
    HWND foreground = GetForegroundWindow();
    if (!foreground || isFocusTransitionSurface(foreground)) {
        // A switch in flight is no verdict; reporting no foreground lets
        // the host hold the active window, as it does for the empty
        // foreground of a click's handoff.
        return 0;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);

    return static_cast<int64_t>(processId);
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
