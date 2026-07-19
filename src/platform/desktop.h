#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/frame.h"

namespace sidescopes {

// Small desktop services the app needs outside capture itself.

struct DesktopPoint
{
    double x = 0.0;
    double y = 0.0;
};

struct DisplayGeometry
{
    double originX = 0.0;  ///< Global desktop coordinates, top-left origin.
    double originY = 0.0;
    double widthPoints = 0.0;
    double heightPoints = 0.0;
};

/// An on-screen window, front-to-back, in global desktop points (top-left
/// origin). Only ordinary application windows are reported - no menu bar,
/// dock, or overlay layers - and never this application's own windows.
struct DesktopWindow
{
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    std::string application;
    /// The window's platform identity, stable for its lifetime: the
    /// kCGWindowNumber on macOS, the HWND (as a uintptr_t) on Windows. Window
    /// attach binds to it and follows it through windowGeometry.
    uint64_t windowIdentity = 0;
    /// The process id of the application that owns the window, so the attach
    /// visibility rule can tell that application's own child windows from a
    /// switch to a different application.
    int64_t ownerPid = 0;
};

/// A window's current on-screen rectangle plus whether it is minimized, in the
/// same global desktop points (top-left origin) onScreenWindows reports.
/// @c title is the window's current title - the filename in most editors -
/// refreshed on every query so a label wearing it stays honest; empty when
/// the window has none (macOS also requires the screen-recording permission
/// this application already holds).
struct WindowGeometry
{
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool minimized = false;
    std::string title;
};

/// Windows currently visible on the given display, frontmost first. Window
/// geometry comes from the window server and needs no capture permission.
[[nodiscard]] std::vector<DesktopWindow> onScreenWindows(uint32_t displayId);

/// The current geometry of the window with @p identity (a DesktopWindow's
/// windowIdentity), or nothing when it no longer exists - the caller reads
/// that as the window having closed. A minimized window is still reported,
/// flagged, so the attach coupling can hide the scopes rather than crop the
/// desktop behind it.
[[nodiscard]] std::optional<WindowGeometry> windowGeometry(uint64_t identity);

/// Global cursor position in desktop points. Reading the position requires no
/// special permission on any supported platform.
[[nodiscard]] std::optional<DesktopPoint> globalCursorPosition();

/// The geometry of the display with @p displayId, if it exists.
[[nodiscard]] std::optional<DisplayGeometry> geometryOfDisplay(uint32_t displayId);

/// The display containing the given desktop point, if any.
[[nodiscard]] std::optional<uint32_t> displayAtPoint(DesktopPoint point);

/// The display the cursor is on right now. Photographers park the cursor
/// near the editor, so this is where region picking should open.
[[nodiscard]] std::optional<uint32_t> displayUnderCursor();

/// Preferences file location in the platform's convention.
std::string preferencesFilePath();

/// Opens the operating system's screen-recording permission pane, as deep
/// as the platform allows. A no-op where capture needs no permission.
void openScreenRecordingSettings();

/// Opens a URL in the user's default browser. Fire-and-forget: there is
/// no result to report and any failure is silent.
void openUrl(const char* url);

/// The keyboard modifiers as the operating system sees them right now.
/// System overlays (the macOS screenshot interface, for one) swallow
/// key-up events, leaving event-driven modifier tracking stuck; polling
/// the ground truth lets the application heal itself.
struct ModifierState
{
    bool shift = false;
    bool control = false;
    bool option = false;
    bool command = false;
};

/// The modifier keys as the operating system currently reports them.
[[nodiscard]] ModifierState currentModifiers();

/// Whether Command+W dismisses the window while the application keeps
/// running (macOS single-window convention: the Dock icon stays, a click
/// there brings the window back; Cmd+Q owns the real quit).
[[nodiscard]] bool platformHidesWindowOnCommandW();

/// Whether Ctrl+W minimizes (Windows: a non-destructive dismissal that
/// mirrors the macOS story, with the taskbar as the way back; the title
/// bar and Alt+F4 keep owning the real close).
[[nodiscard]] bool platformMinimizesWindowOnControlW();

/// Whether Ctrl+Q quits (Windows, where Alt+F4 is the only system
/// affordance and frequent quit-and-relaunch deserves a lighter chord).
/// On macOS the application menu's Cmd+Q already owns quitting.
[[nodiscard]] bool platformQuitsOnControlQ();

/// Hides the whole application the platform's way (macOS: the same
/// system hide as Cmd+H, so the Dock click restores every window
/// natively). Windows, which has no application-wide hide, hides the main
/// window remembered through rememberApplicationWindow.
void hideApplication();

/// Whether the application is currently hidden the hideApplication way
/// (macOS Cmd+W/Cmd+H). The region border must not be drawn while the
/// application itself is out of sight. Always false where hiding is
/// minimizing instead - the caller checks the window's iconified state
/// separately.
[[nodiscard]] bool applicationHidden();

/// A signal from watchWindowMotion about the watched window. The grip pair
/// brackets a possible user drag; the motion pair means the border must react
/// this instant, not at the next frame.
enum class WindowMotionSignal
{
    /// The primary button went down inside the window - a drag may follow.
    GripDown,
    /// The grip landed on the window's chrome (title strip, resize edge on
    /// macOS; the system move-size loop on Windows): a move or resize is
    /// starting, before any geometry has changed.
    MotionImminent,
    /// The window's rectangle actually changed.
    Moved,
    /// The primary button released (macOS) or the move-size loop ended
    /// (Windows).
    GripUp,
};

/// Watches the window with @p identity, owned by the application with
/// @p ownerPid, for user moves and resizes, invoking @p callback on the main
/// thread the moment a signal happens - including while the event loop sits
/// in an idle wait, which is what polling can never do: the region border can
/// leave the screen before the first stale frame is composited, on any
/// machine speed. macOS listens to global mouse events (permission-free for
/// mouse); Windows hooks the move-size loop and window location changes.
/// Replaces any previous watch. Call from the main thread.
void watchWindowMotion(uint64_t identity, int64_t ownerPid, std::function<void(WindowMotionSignal)> callback);

/// Stops watching and drops the callback. Safe to call when nothing is
/// watched. Call from the main thread.
void unwatchWindowMotion();

/// Invokes the callback on the main thread whenever the foreground
/// application changes (macOS: the workspace activation notification;
/// Windows: a global foreground win-event hook), so the focus-routed region
/// logic reroutes immediately instead of waiting out an idle tick - a
/// Cmd+Tab must not leave the old border on screen for a beat. Install
/// once, from the main thread.
void observeForegroundChanges(std::function<void()> callback);

/// Like onScreenWindows, additionally admitting floating-level panels:
/// macOS lifts a key panel (a Quick Look preview) above the ordinary
/// window layer, and pinning a region must still find the window visually
/// under it. Identical to onScreenWindows on Windows.
[[nodiscard]] std::vector<DesktopWindow> attachCandidateWindows(uint32_t displayId);

/// The display's user-facing name (macOS: the system's localized monitor
/// name; Windows: the device ordinal), worn by the global region's border
/// label.
[[nodiscard]] std::string displayName(uint32_t displayId);

/// The window the focus routing should treat as focused: the foreground
/// application's frontmost ordinary window - unless one of @p tracked sits
/// above it in z-order and overlaps it, in which case that tracked window
/// wins. A preview panel rendered by a helper process (macOS Quick Look)
/// stays focused while its host application holds the foreground.
[[nodiscard]] std::optional<uint64_t> focusedWindowForTracking(int64_t applicationPid,
                                                               const std::vector<uint64_t>& tracked);

/// Raises the window with @p identity, owned by the application with
/// @p ownerPid, after the user picked it: a partially obstructed pick would
/// otherwise wear a border around pixels that belong to whatever obstructs
/// it. Windows raises the exact window. macOS activates the owning
/// application with all windows brought forward - the picked window rises
/// above other applications, though not above a sibling of its own
/// application (no public per-window raise exists). Call while this
/// application is frontmost: activation hand-off is only honored then.
void raiseWindow(uint64_t identity, int64_t ownerPid);

/// Records this application's main window so the visibility seams can target
/// it. macOS drives hide and show application-wide and ignores @p nativeWindow;
/// Windows keeps the HWND. Called once, after the window and its graphics
/// backend exist.
void rememberApplicationWindow(void* nativeWindow);

/// This application's own process id, for the attach self-focus exception
/// (focusing SideScopes must never hide it). A seam because getpid is spelled
/// differently, and deprecated under /WX, on Windows.
[[nodiscard]] int64_t ownApplicationPid();

/// The process id of the frontmost application, so the attach coupling can
/// compare it against the attached window's owner and this application's own
/// pid. Zero when no application is frontmost.
[[nodiscard]] int64_t foregroundApplicationPid();

/// Absolute paths of system fonts suitable for the interface, best first;
/// the application loads the first one that exists.
[[nodiscard]] std::vector<std::string> interfaceFontFiles();

/// Absolute paths of fixed-width system fonts, best first, for values
/// whose glyphs must align - hex codes most of all.
[[nodiscard]] std::vector<std::string> monospaceFontFiles();

/// The em multiplier for the fixed-width companion font, chosen so its ink
/// weight matches the interface font's; 1.0 where the platform's pairing is
/// naturally balanced.
[[nodiscard]] float monospaceFontScale();

/// Whether SIDESCOPES_NO_CAPTURE_EXCLUSION is set in the environment. Test
/// harnesses set it so screenshot-driven tooling can see this application's
/// own windows; Windows' capture exclusion blanks them from every screenshot
/// API otherwise. Read once at first use.
[[nodiscard]] bool captureExclusionDisabled();

/// Invokes the callback - on the platform's main-thread event context -
/// whenever the display or the user session wakes up. Capture streams
/// survive those transitions as zombies that look alive but deliver
/// nothing, so the application restarts capture on the signal. A no-op on
/// platforms whose capture dies loudly instead (Windows duplication
/// reports access loss and the retry loop rebuilds it).
void observeSystemWake(std::function<void()> callback);

/// Invokes the callback when Escape is pressed while this application is
/// active but no window of it has keyboard focus. The region border never
/// takes focus - grabbing it must not pull the keyboard out of the editor
/// - so a border interaction can leave the application active yet deaf to
/// its own Escape shortcut; this closes that gap. A no-op on platforms
/// where the gap cannot occur (on Windows the border never activates the
/// application in the first place). The callback is invoked on the main
/// thread.
void observeEscapeWithoutKeyWindow(std::function<void()> callback);

/// Samples the averaged screen color around a desktop point on whatever
/// display it falls on, independent of the capture stream, so the cursor
/// readout works on every screen and not only the tracked one. This
/// application's own windows are excluded where the platform allows.
/// Asynchronous: the callback may fire on any thread, or synchronously
/// where reading the screen is immediate; it receives nothing when the
/// point cannot be read. Callers own the pacing.
void sampleScreenColorAsync(DesktopPoint point, std::function<void(std::optional<FloatColor>)> callback);

}  // namespace sidescopes
