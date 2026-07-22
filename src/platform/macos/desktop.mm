#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include "platform/desktop.h"
#include "platform/focus_resolution.h"

#include <algorithm>
#include <cstdlib>

namespace sidescopes {

namespace {

std::vector<DesktopWindow> onScreenWindowsUpToLayer(uint32_t displayId, int maxLayer)
{
    std::vector<DesktopWindow> windows;
    const CGRect displayBounds = CGDisplayBounds(displayId);
    const pid_t ownPid = getpid();

    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
                                                 kCGNullWindowID);
    if (!list) {
        return windows;
    }

    const CFIndex count = CFArrayGetCount(list);
    for (CFIndex index = 0; index < count; ++index) {
        NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(list, index);
        // Layer 0 is ordinary application windows; low positive layers are
        // floating panels; everything above is chrome (menu bar, dock,
        // overlays - including our own).
        const int layer = [info[(__bridge NSString*)kCGWindowLayer] intValue];
        if (layer < 0 || layer > maxLayer) {
            continue;
        }
        if ([info[(__bridge NSString*)kCGWindowOwnerPID] intValue] == ownPid) {
            continue;
        }
        // Invisible system helper windows (accessibility warnings and the
        // like) sit at layer zero with zero alpha; the window list reports
        // them as if they were real.
        NSNumber* alpha = info[(__bridge NSString*)kCGWindowAlpha];
        if (alpha && alpha.doubleValue < 0.05) {
            continue;
        }

        CGRect bounds = CGRectZero;
        CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)info[(__bridge NSString*)kCGWindowBounds],
                                               &bounds);
        if (bounds.size.width < 64 || bounds.size.height < 64) {
            continue;
        }
        if (!CGRectIntersectsRect(bounds, displayBounds)) {
            continue;
        }

        DesktopWindow window;
        window.x = bounds.origin.x;
        window.y = bounds.origin.y;
        window.width = bounds.size.width;
        window.height = bounds.size.height;
        NSString* owner = info[(__bridge NSString*)kCGWindowOwnerName];
        window.application = owner ? owner.UTF8String : "";
        window.windowIdentity = [info[(__bridge NSString*)kCGWindowNumber] unsignedLongLongValue];
        window.ownerPid = [info[(__bridge NSString*)kCGWindowOwnerPID] longLongValue];
        windows.push_back(std::move(window));
    }
    CFRelease(list);
    return windows;
}

}  // namespace

std::vector<DesktopWindow> onScreenWindows(uint32_t displayId)
{
    return onScreenWindowsUpToLayer(displayId, 0);
}

std::vector<DesktopWindow> attachCandidateWindows(uint32_t displayId)
{
    // A key panel floats a few levels up (a Quick Look preview rides at
    // the floating level while it holds the keyboard), and pinning must
    // still find it rather than the window underneath it.
    return onScreenWindowsUpToLayer(displayId, 8);
}

std::optional<WindowGeometry> windowGeometry(uint64_t identity)
{
    const CGWindowID windowId = static_cast<CGWindowID>(identity);
    const void* ids[1] = {reinterpret_cast<const void*>(static_cast<uintptr_t>(windowId))};
    CFArrayRef idArray = CFArrayCreate(kCFAllocatorDefault, ids, 1, nullptr);
    if (!idArray) {
        return std::nullopt;
    }

    // Description-from-array queries a specific window regardless of whether it
    // is on screen, so a minimized window still comes back (with
    // kCGWindowIsOnscreen absent or false); a closed one drops out entirely.
    CFArrayRef list = CGWindowListCreateDescriptionFromArray(idArray);
    CFRelease(idArray);
    if (!list) {
        return std::nullopt;
    }

    std::optional<WindowGeometry> geometry;
    if (CFArrayGetCount(list) > 0) {
        // The window still exists: present, so not closed. Its bounds are
        // best-effort (ignored by the caller while minimized anyway).
        NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(list, 0);
        WindowGeometry result;
        CGRect bounds = CGRectZero;
        CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)info[(__bridge NSString*)kCGWindowBounds],
                                               &bounds);
        result.x = bounds.origin.x;
        result.y = bounds.origin.y;
        result.width = bounds.size.width;
        result.height = bounds.size.height;
        // kCGWindowIsOnscreen is present only while the window is on screen, so
        // its absence - not merely a false value - is how a minimized (or
        // other-space) window reads.
        NSNumber* onscreen = info[(__bridge NSString*)kCGWindowIsOnscreen];
        result.minimized = onscreen == nil || !onscreen.boolValue;
        NSString* title = info[(__bridge NSString*)kCGWindowName];
        result.title = title ? title.UTF8String : "";
        geometry = result;
    }
    CFRelease(list);

    return geometry;
}

namespace {

// The qualifying on-screen windows, front to back: layer 0, visible
// alpha, at least picker-sized. A TRACKED window qualifies at any layer:
// macOS shifts a Quick Look panel's level away from zero the moment it
// becomes key (probe-verified), and dropping the very window the user
// attached would lose its region on every click into it.
std::vector<OrderedWindow> qualifyingWindows(const std::vector<uint64_t>& tracked)
{
    std::vector<OrderedWindow> windows;
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
                                                 kCGNullWindowID);
    if (!list) {
        return windows;
    }
    const CFIndex count = CFArrayGetCount(list);
    for (CFIndex index = 0; index < count; ++index) {
        NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(list, index);
        const uint64_t identity = [info[(__bridge NSString*)kCGWindowNumber] unsignedLongLongValue];
        const bool trackedWindow = std::find(tracked.begin(), tracked.end(), identity) != tracked.end();
        if ([info[(__bridge NSString*)kCGWindowLayer] intValue] != 0 && !trackedWindow) {
            continue;
        }
        NSNumber* alpha = info[(__bridge NSString*)kCGWindowAlpha];
        if (alpha && alpha.doubleValue < 0.05) {
            continue;
        }
        CGRect bounds = CGRectZero;
        CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)info[(__bridge NSString*)kCGWindowBounds],
                                               &bounds);
        if (bounds.size.width < 64 || bounds.size.height < 64) {
            continue;
        }
        windows.push_back({identity, [info[(__bridge NSString*)kCGWindowOwnerPID] longLongValue], bounds.origin.x,
                           bounds.origin.y, bounds.size.width, bounds.size.height});
    }
    CFRelease(list);

    return windows;
}

}  // namespace

std::optional<uint64_t> focusedWindowForTracking(int64_t applicationPid, const std::vector<uint64_t>& tracked)
{
    return resolveTrackedFocus(qualifyingWindows(tracked), applicationPid, tracked);
}

void raiseWindow(uint64_t, int64_t ownerPid)
{
    NSRunningApplication* application =
        [NSRunningApplication runningApplicationWithProcessIdentifier:static_cast<pid_t>(ownerPid)];
    if (!application) {
        return;
    }
    // All-windows activation is the closest public thing to raising one
    // window of another application: the picked window comes up above other
    // applications (spike-verified from a frontmost caller), and the editor
    // takes the keyboard - which a fresh pick is about to use anyway.
    [application activateWithOptions:NSApplicationActivateAllWindows];
}

std::optional<DesktopPoint> globalCursorPosition()
{
    CGEventRef event = CGEventCreate(nullptr);
    if (!event) {
        return std::nullopt;
    }
    const CGPoint location = CGEventGetLocation(event);
    CFRelease(event);
    return DesktopPoint{location.x, location.y};
}

std::optional<DisplayGeometry> geometryOfDisplay(uint32_t displayId)
{
    const CGRect bounds = CGDisplayBounds(displayId);
    if (CGRectIsEmpty(bounds)) {
        return std::nullopt;
    }
    return DisplayGeometry{bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height};
}

std::string displayName(uint32_t displayId)
{
    for (NSScreen* screen in NSScreen.screens) {
        NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
        if (number.unsignedIntValue == displayId) {
            return screen.localizedName.UTF8String;
        }
    }

    return "Display";
}

std::optional<uint32_t> displayAtPoint(DesktopPoint point)
{
    CGDirectDisplayID display = 0;
    uint32_t matches = 0;
    if (CGGetDisplaysWithPoint(CGPointMake(point.x, point.y), 1, &display, &matches) != kCGErrorSuccess ||
        matches == 0) {
        return std::nullopt;
    }
    return display;
}

std::optional<uint32_t> displayUnderCursor()
{
    const auto cursor = globalCursorPosition();
    if (!cursor) {
        return std::nullopt;
    }
    return displayAtPoint(*cursor);
}

// sampleScreenColorAsync (declared in desktop.h) samples the screen color at
// a point; it lives in screen_capture.mm because it uses ScreenCaptureKit.

std::string preferencesFilePath()
{
    NSString* home = NSHomeDirectory();
    const std::string base = home.length > 0 ? home.UTF8String : ".";
    return base + "/Library/Application Support/SideScopes/preferences.txt";
}

ModifierState currentModifiers()
{
    // NSEvent.modifierFlags mirrors the application's own event stream,
    // which is precisely what a system overlay starves - it reads stuck
    // in the very situation this exists to heal. The Quartz event source
    // reports the session's real key state regardless of who received
    // the events.
    const CGEventFlags flags = CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);
    ModifierState state;
    state.shift = (flags & kCGEventFlagMaskShift) != 0;
    state.control = (flags & kCGEventFlagMaskControl) != 0;
    state.option = (flags & kCGEventFlagMaskAlternate) != 0;
    state.command = (flags & kCGEventFlagMaskCommand) != 0;
    return state;
}

bool platformHidesWindowOnCommandW()
{
    return true;
}

bool platformMinimizesWindowOnControlW()
{
    return false;
}

bool platformQuitsOnControlQ()
{
    return false;
}

void hideApplication()
{
    [NSApp hide:nil];
}

bool applicationHidden()
{
    return NSApp.hidden;
}

namespace {

// The strip and margins a grab lands on when the user is about to move or
// resize the window rather than click inside it: the standard title bar
// plus the resize edges, in desktop points. The edge band reaches OUTSIDE
// the frame too - macOS resize grips extend past it, and a hide that waits
// for the first geometry change is already too late.
constexpr double MotionChromeTopPoints = 30.0;
constexpr double MotionChromeEdgePoints = 8.0;

id g_motionMonitor = nil;
uint64_t g_motionIdentity = 0;
bool g_motionGrip = false;
WindowGeometry g_motionLastRect;
std::function<void(WindowMotionSignal)> g_motionCallback;

// Global mouse locations arrive in Cocoa screen coordinates (bottom-left
// origin against the primary screen); window geometry speaks CG top-left.
DesktopPoint motionCursorDesktopPoint()
{
    const NSPoint location = [NSEvent mouseLocation];
    const double primaryHeight = NSMaxY([NSScreen screens].firstObject.frame);

    return DesktopPoint{location.x, primaryHeight - location.y};
}

bool motionPointInWindow(const DesktopPoint& point, const WindowGeometry& rect)
{
    return point.x >= rect.x && point.x <= rect.x + rect.width && point.y >= rect.y && point.y <= rect.y + rect.height;
}

// Inside the window or within the resize-grip ring just outside it: macOS
// resize grips reach past the frame, and a hide that waits for the first
// geometry change is already too late.
bool motionPointInGripRange(const DesktopPoint& point, const WindowGeometry& rect)
{
    return point.x >= rect.x - MotionChromeEdgePoints && point.x <= rect.x + rect.width + MotionChromeEdgePoints &&
           point.y >= rect.y - MotionChromeEdgePoints && point.y <= rect.y + rect.height + MotionChromeEdgePoints;
}

bool motionPointOnChrome(const DesktopPoint& point, const WindowGeometry& rect)
{
    if (!motionPointInWindow(point, rect)) {
        // The caller has already bounded the point to the grip range: the
        // ring outside the frame is the resize grip.
        return true;
    }
    if (point.y - rect.y <= MotionChromeTopPoints) {
        return true;
    }

    return point.x - rect.x <= MotionChromeEdgePoints || rect.x + rect.width - point.x <= MotionChromeEdgePoints ||
           rect.y + rect.height - point.y <= MotionChromeEdgePoints;
}

void motionHandleMouseDown()
{
    const auto rect = windowGeometry(g_motionIdentity);
    if (!rect || !motionPointInGripRange(motionCursorDesktopPoint(), *rect)) {
        return;
    }

    g_motionGrip = true;
    g_motionLastRect = *rect;
    g_motionCallback(WindowMotionSignal::GripDown);
    if (motionPointOnChrome(motionCursorDesktopPoint(), *rect)) {
        g_motionCallback(WindowMotionSignal::MotionImminent);
    }
}

void motionHandleMouseDragged()
{
    if (!g_motionGrip) {
        return;
    }
    const auto rect = windowGeometry(g_motionIdentity);
    if (rect && (rect->x != g_motionLastRect.x || rect->y != g_motionLastRect.y ||
                 rect->width != g_motionLastRect.width || rect->height != g_motionLastRect.height)) {
        g_motionLastRect = *rect;
        g_motionCallback(WindowMotionSignal::Moved);
    }
}

}  // namespace

void watchWindowMotion(uint64_t identity, int64_t, std::function<void(WindowMotionSignal)> callback)
{
    unwatchWindowMotion();
    g_motionIdentity = identity;
    g_motionCallback = std::move(callback);
    // Global monitors observe other applications' mouse events only - our
    // own border and interface never trigger them - and mouse masks need no
    // permission grant. Down/dragged/up excludes the high-rate buttonless
    // mouse-moved stream.
    const NSEventMask mask = NSEventMaskLeftMouseDown | NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp;
    g_motionMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:mask
                                                             handler:^(NSEvent* event) {
                                                               if (event.type == NSEventTypeLeftMouseDown) {
                                                                   motionHandleMouseDown();
                                                               } else if (event.type == NSEventTypeLeftMouseDragged) {
                                                                   motionHandleMouseDragged();
                                                               } else if (g_motionGrip) {
                                                                   g_motionGrip = false;
                                                                   g_motionCallback(WindowMotionSignal::GripUp);
                                                               }
                                                             }];
}

void unwatchWindowMotion()
{
    if (g_motionMonitor) {
        [NSEvent removeMonitor:g_motionMonitor];
        g_motionMonitor = nil;
    }
    g_motionIdentity = 0;
    g_motionGrip = false;
    g_motionCallback = nullptr;
}

void rememberApplicationWindow(void*)
{
    // macOS hides and shows application-wide; there is no window handle to
    // keep.
}

int64_t ownApplicationPid()
{
    return getpid();
}

int64_t foregroundApplicationPid()
{
    NSRunningApplication* front = [[NSWorkspace sharedWorkspace] frontmostApplication];

    return front ? front.processIdentifier : 0;
}

void openScreenRecordingSettings()
{
    NSURL* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"];
    [[NSWorkspace sharedWorkspace] openURL:url];
}

void openUrl(const char* url)
{
    NSURL* target = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
    if (target) {
        [[NSWorkspace sharedWorkspace] openURL:target];
    }
}

std::vector<std::string> interfaceFontFiles()
{
    return {"/System/Library/Fonts/HelveticaNeue.ttc", "/System/Library/Fonts/SFNS.ttf",
            "/System/Library/Fonts/Supplemental/Arial.ttf"};
}

std::vector<std::string> monospaceFontFiles()
{
    return {"/System/Library/Fonts/SFNSMono.ttf", "/System/Library/Fonts/Menlo.ttc",
            "/System/Library/Fonts/Supplemental/Courier New.ttf"};
}

bool captureExclusionDisabled()
{
    static const bool disabled = std::getenv("SIDESCOPES_NO_CAPTURE_EXCLUSION") != nullptr;

    return disabled;
}

bool captureVisibilityToggleSupported()
{
    // Screenshots always see the application here; only its own capture
    // stream excludes it, so there is nothing to toggle.
    return false;
}

void setCaptureVisibility(bool)
{
}

bool captureVisible()
{
    return true;
}

float monospaceFontScale()
{
    // SF Mono and Menlo already sit within a few percent of the interface
    // font's ink height at an equal em.
    return 1.0f;
}

void observeSystemWake(std::function<void()> callback)
{
    // Waking the display or unlocking the session can leave a capture
    // stream a zombie: it either stops delivering without an error, or a
    // retry that ran while the screen was locked started a stream bound
    // to the wrong session. Both look alive, so these events tell the
    // application to force a restart - cheap on a screen that was just
    // black.
    auto shared = std::make_shared<std::function<void()>>(std::move(callback));
    const auto observe = ^(NSNotification*) {
      (*shared)();
    };
    [[[NSWorkspace sharedWorkspace] notificationCenter] addObserverForName:NSWorkspaceScreensDidWakeNotification
                                                                    object:nil
                                                                     queue:[NSOperationQueue mainQueue]
                                                                usingBlock:observe];
    [[[NSWorkspace sharedWorkspace] notificationCenter] addObserverForName:NSWorkspaceSessionDidBecomeActiveNotification
                                                                    object:nil
                                                                     queue:[NSOperationQueue mainQueue]
                                                                usingBlock:observe];
    [[NSDistributedNotificationCenter defaultCenter] addObserverForName:@"com.apple.screenIsUnlocked"
                                                                 object:nil
                                                                  queue:[NSOperationQueue mainQueue]
                                                             usingBlock:observe];
    // Display topology changes (a monitor connected, removed, or given a
    // new resolution) can leave the stream running against a stale
    // configuration; a restart is cheap and re-resolves the display.
    [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidChangeScreenParametersNotification
                                                      object:nil
                                                       queue:[NSOperationQueue mainQueue]
                                                  usingBlock:observe];
}

namespace {

// The activation observer's token, kept so teardown can retire it.
id g_foregroundObserver = nil;

}  // namespace

void observeForegroundChanges(std::function<void()> callback)
{
    // Application activation is the notification macOS offers here, so a
    // switch between two windows of the same application raises nothing: the
    // per-tick focus poll still covers that case at its own latency. The
    // named cases - Cmd+Tab and a click into another application - are
    // application switches and arrive on this observer.
    unobserveForegroundChanges();
    auto shared = std::make_shared<std::function<void()>>(std::move(callback));
    g_foregroundObserver = [[[NSWorkspace sharedWorkspace] notificationCenter]
        addObserverForName:NSWorkspaceDidActivateApplicationNotification
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification*) {
                  (*shared)();
                }];
}

void unobserveForegroundChanges()
{
    if (g_foregroundObserver) {
        [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:g_foregroundObserver];
        g_foregroundObserver = nil;
    }
}

void observeEscapeWithoutKeyWindow(std::function<void()> callback)
{
    // Clicking the region border activates the application without
    // giving any window keyboard focus - the border refuses key status
    // by design, so grabbing it never pulls the keyboard out of the
    // editor. Key presses in that state reach the application object and
    // die there; a local monitor picks them up. Local monitors never see
    // other applications' input.
    auto shared = std::make_shared<std::function<void()>>(std::move(callback));
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                          handler:^NSEvent*(NSEvent* event) {
                                            if (event.keyCode == 53 && NSApp.keyWindow == nil) {
                                                (*shared)();
                                                return nil;  // consumed
                                            }
                                            return event;
                                          }];
}

}  // namespace sidescopes
