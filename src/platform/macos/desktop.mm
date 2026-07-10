#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <cstdlib>

#include "platform/desktop.h"

namespace sidescopes {

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

}  // namespace sidescopes
