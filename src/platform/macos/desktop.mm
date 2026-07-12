#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <cstdlib>

#include "platform/desktop.h"

namespace sidescopes {

std::vector<DesktopWindow> OnScreenWindows(uint32_t display_id) {
    std::vector<DesktopWindow> windows;
    const CGRect display_bounds = CGDisplayBounds(display_id);
    const pid_t own_pid = getpid();

    CFArrayRef list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list) return windows;

    const CFIndex count = CFArrayGetCount(list);
    for (CFIndex index = 0; index < count; ++index) {
        NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(list, index);
        // Layer 0 is ordinary application windows; everything else is
        // chrome (menu bar, dock, overlays - including our own).
        if ([info[(__bridge NSString*)kCGWindowLayer] intValue] != 0) continue;
        if ([info[(__bridge NSString*)kCGWindowOwnerPID] intValue] == own_pid) continue;
        // Invisible system helper windows (accessibility warnings and the
        // like) sit at layer zero with zero alpha; the window list reports
        // them as if they were real.
        NSNumber* alpha = info[(__bridge NSString*)kCGWindowAlpha];
        if (alpha && alpha.doubleValue < 0.05) continue;

        CGRect bounds = CGRectZero;
        CGRectMakeWithDictionaryRepresentation(
            (__bridge CFDictionaryRef)info[(__bridge NSString*)kCGWindowBounds], &bounds);
        if (bounds.size.width < 64 || bounds.size.height < 64) continue;
        if (!CGRectIntersectsRect(bounds, display_bounds)) continue;

        DesktopWindow window;
        window.x = bounds.origin.x;
        window.y = bounds.origin.y;
        window.width = bounds.size.width;
        window.height = bounds.size.height;
        NSString* owner = info[(__bridge NSString*)kCGWindowOwnerName];
        window.application = owner ? owner.UTF8String : "";
        windows.push_back(std::move(window));
    }
    CFRelease(list);
    return windows;
}

std::optional<DesktopPoint> GlobalCursorPosition() {
    CGEventRef event = CGEventCreate(nullptr);
    if (!event) return std::nullopt;
    const CGPoint location = CGEventGetLocation(event);
    CFRelease(event);
    return DesktopPoint{location.x, location.y};
}

std::optional<DisplayGeometry> GeometryOfDisplay(uint32_t display_id) {
    const CGRect bounds = CGDisplayBounds(display_id);
    if (CGRectIsEmpty(bounds)) return std::nullopt;
    return DisplayGeometry{bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height};
}

std::optional<uint32_t> DisplayAtPoint(DesktopPoint point) {
    CGDirectDisplayID display = 0;
    uint32_t matches = 0;
    if (CGGetDisplaysWithPoint(CGPointMake(point.x, point.y), 1, &display, &matches) !=
            kCGErrorSuccess ||
        matches == 0)
        return std::nullopt;
    return display;
}

std::optional<uint32_t> DisplayUnderCursor() {
    const auto cursor = GlobalCursorPosition();
    if (!cursor) return std::nullopt;
    return DisplayAtPoint(*cursor);
}

std::string PreferencesFilePath() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") +
           "/Library/Application Support/SideScopes/preferences.txt";
}

ModifierState CurrentModifiers() {
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

bool PlatformClosesWindowOnCommandW() {
    return true;
}

void OpenScreenRecordingSettings() {
    NSURL* url = [NSURL
        URLWithString:
            @"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"];
    [[NSWorkspace sharedWorkspace] openURL:url];
}

std::vector<std::string> InterfaceFontFiles() {
    return {"/System/Library/Fonts/HelveticaNeue.ttc", "/System/Library/Fonts/SFNS.ttf",
            "/System/Library/Fonts/Supplemental/Arial.ttf"};
}

void ObserveSystemWake(std::function<void()> callback) {
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
    [[[NSWorkspace sharedWorkspace] notificationCenter]
        addObserverForName:NSWorkspaceScreensDidWakeNotification
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:observe];
    [[[NSWorkspace sharedWorkspace] notificationCenter]
        addObserverForName:NSWorkspaceSessionDidBecomeActiveNotification
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:observe];
    [[NSDistributedNotificationCenter defaultCenter]
        addObserverForName:@"com.apple.screenIsUnlocked"
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:observe];
    // Display topology changes (a monitor connected, removed, or given a
    // new resolution) can leave the stream running against a stale
    // configuration; a restart is cheap and re-resolves the display.
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSApplicationDidChangeScreenParametersNotification
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:observe];
}

void ObserveEscapeWithoutKeyWindow(std::function<void()> callback) {
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
