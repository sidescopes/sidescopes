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

@interface SidescopesPickerView : NSView {
@public
    // Suggested rectangles in view coordinates, with their labels.
    std::vector<std::pair<NSRect, std::string>> suggestions_;
}
@property(nonatomic, assign) NSPoint dragStart;
@property(nonatomic, assign) NSPoint dragCurrent;
@property(nonatomic, assign) NSPoint hover;
@property(nonatomic, assign) BOOL dragging;
@property(nonatomic, assign) BOOL picked;
@property(nonatomic, assign) NSInteger hoveredSuggestion;
@property(nonatomic, assign) NSRect confirmedRect;
@end

@implementation SidescopesPickerView

// The smallest suggestion under the cursor wins, so a photo canvas beats
// the window that contains it.
- (NSInteger)suggestionAtPoint:(NSPoint)point {
    NSInteger best = -1;
    CGFloat best_area = CGFLOAT_MAX;
    for (NSUInteger index = 0; index < suggestions_.size(); ++index) {
        const NSRect rect = suggestions_[index].first;
        if (!NSPointInRect(point, rect)) continue;
        const CGFloat area = rect.size.width * rect.size.height;
        if (area < best_area) {
            best_area = area;
            best = static_cast<NSInteger>(index);
        }
    }
    return best;
}

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
    } else {
        // Only the suggestion under the cursor is shown: it punches through
        // the dimming and carries its label. Outlining every candidate at
        // once (including occluded background windows) was just clutter.
        if (self.hoveredSuggestion >= 0 &&
            self.hoveredSuggestion < static_cast<NSInteger>(suggestions_.size())) {
            const auto& hovered = suggestions_[self.hoveredSuggestion];
            [[NSColor clearColor] setFill];
            NSRectFillUsingOperation(hovered.first, NSCompositingOperationCopy);
            // The window server treats fully transparent window pixels as
            // click-through: a truly clear punch would send the confirming
            // click to the application underneath (which is exactly where
            // the user clicks). Five percent black is visually nothing but
            // keeps every pixel hit-testable.
            [[NSColor colorWithWhite:0 alpha:0.05] setFill];
            NSRectFillUsingOperation(hovered.first, NSCompositingOperationSourceOver);
            [[NSColor whiteColor] setStroke];
            NSBezierPath* border = [NSBezierPath bezierPathWithRect:hovered.first];
            border.lineWidth = 2.0;
            [border stroke];
            NSString* label = [NSString stringWithUTF8String:hovered.second.c_str()];
            NSDictionary* label_attributes = @{
                NSForegroundColorAttributeName : [NSColor whiteColor],
                NSFontAttributeName : [NSFont systemFontOfSize:12 weight:NSFontWeightMedium],
            };
            [label drawAtPoint:NSMakePoint(hovered.first.origin.x + 6, NSMaxY(hovered.first) - 20)
                withAttributes:label_attributes];
        }
    }

    NSString* hint = suggestions_.empty()
                         ? @"Drag to select the scoped area  -  ESC to cancel"
                         : @"Click a highlighted area or drag to select  -  ESC to cancel";
    NSDictionary* attributes = @{
        NSForegroundColorAttributeName : [NSColor whiteColor],
        NSFontAttributeName : [NSFont systemFontOfSize:15 weight:NSFontWeightMedium],
    };
    const NSSize size = [hint sizeWithAttributes:attributes];
    [hint drawAtPoint:NSMakePoint((self.bounds.size.width - size.width) / 2,
                                  self.bounds.size.height - 60)
        withAttributes:attributes];
}

- (void)mouseMoved:(NSEvent*)event {
    self.hover = [self convertPoint:event.locationInWindow fromView:nil];
    const NSInteger hovered = [self suggestionAtPoint:self.hover];
    if (hovered != self.hoveredSuggestion) {
        self.hoveredSuggestion = hovered;
        self.needsDisplay = YES;
    }
}

- (void)mouseDown:(NSEvent*)event {
    self.dragStart = [self convertPoint:event.locationInWindow fromView:nil];
    self.dragCurrent = self.dragStart;
    self.needsDisplay = YES;
}

- (void)mouseDragged:(NSEvent*)event {
    self.dragCurrent = [self convertPoint:event.locationInWindow fromView:nil];
    // A real drag only starts after a few points of travel, so a click on a
    // suggestion never flashes a tiny manual selection.
    if (!self.dragging && (fabs(self.dragCurrent.x - self.dragStart.x) > 4 ||
                           fabs(self.dragCurrent.y - self.dragStart.y) > 4))
        self.dragging = YES;
    self.needsDisplay = YES;
}

- (void)mouseUp:(NSEvent*)event {
    self.dragCurrent = [self convertPoint:event.locationInWindow fromView:nil];
    if (self.dragging) {
        const NSRect selection = [self selectionRect];
        self.picked = selection.size.width > 8 && selection.size.height > 8;
        self.confirmedRect = selection;
    } else {
        const NSInteger hovered = [self suggestionAtPoint:[self convertPoint:event.locationInWindow
                                                                    fromView:nil]];
        self.picked = hovered >= 0;
        if (self.picked) self.confirmedRect = suggestions_[hovered].first;
    }
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

// Without this, the first click on the overlay is swallowed as a
// window-activation gesture and never reaches the view - the symptom was
// clicks falling through to the app behind while only ESC worked.
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
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

std::optional<RegionOfInterest> PickRegionOnDisplay(
    uint32_t display_id, const std::vector<SuggestedRegion>& suggestions) {
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
    view.hoveredSuggestion = -1;
    // Percent (top-left origin) to view coordinates (bottom-left origin).
    const NSSize view_size = screen.frame.size;
    for (const SuggestedRegion& suggestion : suggestions) {
        const RegionOfInterest& region = suggestion.region;
        const NSRect rect =
            NSMakeRect(region.left_percent / 100.0 * view_size.width,
                       (100.0 - region.bottom_percent) / 100.0 * view_size.height,
                       (region.right_percent - region.left_percent) / 100.0 * view_size.width,
                       (region.bottom_percent - region.top_percent) / 100.0 * view_size.height);
        view->suggestions_.emplace_back(rect, suggestion.label);
    }
    overlay.contentView = view;
    overlay.acceptsMouseMovedEvents = YES;

    // Force the app frontmost so the overlay owns the mouse for the whole
    // interaction; otherwise clicks can activate whatever is behind it.
    [NSApp activateIgnoringOtherApps:YES];
    [overlay makeKeyAndOrderFront:nil];
    [overlay makeFirstResponder:view];
    [NSCursor.crosshairCursor push];
    const NSModalResponse response = [NSApp runModalForWindow:overlay];
    [NSCursor pop];
    [overlay orderOut:nil];

    if (response != NSModalResponseOK || !view.picked) return std::nullopt;

    // View coordinates are bottom-left-origin; the region convention is
    // top-left-origin percentages.
    const NSRect selection = view.confirmedRect;
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
