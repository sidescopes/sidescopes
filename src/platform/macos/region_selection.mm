#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>

#include "platform/region_selection.h"

// Borderless windows refuse key status unless overridden; the picker needs
// keyDown for ESC-to-cancel.
@interface SidescopesPickerWindow : NSWindow
@end
@implementation SidescopesPickerWindow
- (BOOL)canBecomeKeyWindow {
    return YES;
}
@end

@interface SidescopesPickerView : NSView
@property(nonatomic, assign) NSPoint dragStart;
@property(nonatomic, assign) NSPoint dragCurrent;
@property(nonatomic, assign) BOOL dragging;
@property(nonatomic, assign) BOOL picked;
@end

@implementation SidescopesPickerView

- (NSRect)selectionRect {
    return NSMakeRect(std::min(self.dragStart.x, self.dragCurrent.x),
                      std::min(self.dragStart.y, self.dragCurrent.y),
                      std::abs(self.dragCurrent.x - self.dragStart.x),
                      std::abs(self.dragCurrent.y - self.dragStart.y));
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [[NSColor colorWithWhite:0 alpha:0.35] setFill];
    NSRectFillUsingOperation(self.bounds, NSCompositingOperationSourceOver);

    if (self.dragging) {
        const NSRect selection = [self selectionRect];
        [[NSColor clearColor] setFill];
        NSRectFillUsingOperation(selection, NSCompositingOperationCopy);
        [[NSColor whiteColor] setStroke];
        NSBezierPath* border = [NSBezierPath bezierPathWithRect:selection];
        border.lineWidth = 1.5;
        [border stroke];
    }

    NSString* hint = @"Drag to select the scoped area  -  ESC to cancel";
    NSDictionary* attributes = @{
        NSForegroundColorAttributeName : [NSColor whiteColor],
        NSFontAttributeName : [NSFont systemFontOfSize:15 weight:NSFontWeightMedium],
    };
    const NSSize size = [hint sizeWithAttributes:attributes];
    [hint drawAtPoint:NSMakePoint((self.bounds.size.width - size.width) / 2,
                                  self.bounds.size.height - 60)
        withAttributes:attributes];
}

- (void)mouseDown:(NSEvent*)event {
    self.dragStart = [self convertPoint:event.locationInWindow fromView:nil];
    self.dragCurrent = self.dragStart;
    self.dragging = YES;
    self.needsDisplay = YES;
}

- (void)mouseDragged:(NSEvent*)event {
    self.dragCurrent = [self convertPoint:event.locationInWindow fromView:nil];
    self.needsDisplay = YES;
}

- (void)mouseUp:(NSEvent*)event {
    self.dragCurrent = [self convertPoint:event.locationInWindow fromView:nil];
    const NSRect selection = [self selectionRect];
    // Tiny drags are almost always accidents, not selections.
    self.picked = selection.size.width > 8 && selection.size.height > 8;
    [NSApp stopModalWithCode:self.picked ? NSModalResponseOK : NSModalResponseCancel];
}

- (void)keyDown:(NSEvent*)event {
    if (event.keyCode == 53) {  // ESC
        self.picked = NO;
        [NSApp stopModalWithCode:NSModalResponseCancel];
        return;
    }
    [super keyDown:event];
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

@end

// Double-stroked border (dark under light) so it reads on any background.
@interface SidescopesBorderView : NSView
@end
@implementation SidescopesBorderView
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect bounds = self.bounds;
    [[NSColor colorWithWhite:0 alpha:0.55] setStroke];
    NSBezierPath* outer = [NSBezierPath bezierPathWithRect:NSInsetRect(bounds, 0.5, 0.5)];
    outer.lineWidth = 1.0;
    [outer stroke];
    [[NSColor colorWithWhite:1 alpha:0.85] setStroke];
    NSBezierPath* inner = [NSBezierPath bezierPathWithRect:NSInsetRect(bounds, 2.0, 2.0)];
    inner.lineWidth = 2.0;
    [inner stroke];
}
@end

namespace sidescopes {
namespace {

// The border sits OUTSIDE the region so it never enters the scoped pixels.
constexpr double kBorderMargin = 4.0;

NSWindow* g_border_window = nil;

NSScreen* ScreenForDisplay(uint32_t display_id) {
    for (NSScreen* screen in NSScreen.screens) {
        NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
        if (number && number.unsignedIntValue == display_id) return screen;
    }
    return NSScreen.mainScreen;
}

}  // namespace

std::optional<RegionOfInterest> PickRegionOnDisplay(uint32_t display_id) {
    NSScreen* screen = ScreenForDisplay(display_id);

    SidescopesPickerWindow* overlay =
        [[SidescopesPickerWindow alloc] initWithContentRect:screen.frame
                                                  styleMask:NSWindowStyleMaskBorderless
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
    overlay.backgroundColor = NSColor.clearColor;
    overlay.opaque = NO;
    overlay.level = NSStatusWindowLevel + 1;
    overlay.collectionBehavior =
        NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;

    SidescopesPickerView* view =
        [[SidescopesPickerView alloc] initWithFrame:overlay.contentView.bounds];
    overlay.contentView = view;

    [overlay makeKeyAndOrderFront:nil];
    [NSCursor.crosshairCursor push];
    const NSModalResponse response = [NSApp runModalForWindow:overlay];
    [NSCursor pop];
    [overlay orderOut:nil];

    if (response != NSModalResponseOK || !view.picked) return std::nullopt;

    // View coordinates are bottom-left-origin; the region convention is
    // top-left-origin percentages.
    const NSRect selection = [view selectionRect];
    const NSSize size = screen.frame.size;
    RegionOfInterest region;
    region.left_percent = selection.origin.x / size.width * 100.0;
    region.right_percent = (selection.origin.x + selection.size.width) / size.width * 100.0;
    region.top_percent =
        (size.height - (selection.origin.y + selection.size.height)) / size.height * 100.0;
    region.bottom_percent = (size.height - selection.origin.y) / size.height * 100.0;
    return region;
}

void ShowRegionBorder(uint32_t display_id, const RegionOfInterest& region) {
    NSScreen* screen = ScreenForDisplay(display_id);
    const NSRect frame = screen.frame;

    const double left = frame.origin.x + region.left_percent / 100.0 * frame.size.width;
    const double right = frame.origin.x + region.right_percent / 100.0 * frame.size.width;
    const double bottom =
        frame.origin.y + (100.0 - region.bottom_percent) / 100.0 * frame.size.height;
    const double top = frame.origin.y + (100.0 - region.top_percent) / 100.0 * frame.size.height;
    const NSRect rect =
        NSMakeRect(left - kBorderMargin, bottom - kBorderMargin, (right - left) + 2 * kBorderMargin,
                   (top - bottom) + 2 * kBorderMargin);

    if (!g_border_window) {
        g_border_window = [[NSWindow alloc] initWithContentRect:rect
                                                      styleMask:NSWindowStyleMaskBorderless
                                                        backing:NSBackingStoreBuffered
                                                          defer:NO];
        g_border_window.backgroundColor = NSColor.clearColor;
        g_border_window.opaque = NO;
        g_border_window.hasShadow = NO;
        g_border_window.ignoresMouseEvents = YES;  // click-through, always
        g_border_window.level = NSStatusWindowLevel;
        g_border_window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                             NSWindowCollectionBehaviorFullScreenAuxiliary |
                                             NSWindowCollectionBehaviorStationary;
        g_border_window.contentView = [[SidescopesBorderView alloc] initWithFrame:NSZeroRect];
    }
    [g_border_window setFrame:rect display:YES];
    [g_border_window orderFrontRegardless];
}

void HideRegionBorder() {
    [g_border_window orderOut:nil];
}

}  // namespace sidescopes
