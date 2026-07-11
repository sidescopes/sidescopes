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

std::string PreferencesFilePath() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") +
           "/Library/Application Support/SideScopes/preferences.txt";
}

std::string AppRegionsFilePath() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") +
           "/Library/Application Support/SideScopes/app_regions.txt";
}

}  // namespace sidescopes
