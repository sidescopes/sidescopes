#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>

#include "platform/region_selection.h"

// Where a mouse-down lands relative to the remembered selection: on a
// resize handle (encoded by which edges it drags), inside (move), or
// elsewhere (start a fresh rectangle).
typedef NS_OPTIONS(NSUInteger, SidescopesDragEdges) {
    SidescopesDragEdgeNone = 0,
    SidescopesDragEdgeLeft = 1 << 0,
    SidescopesDragEdgeRight = 1 << 1,
    SidescopesDragEdgeBottom = 1 << 2,
    SidescopesDragEdgeTop = 1 << 3,
    SidescopesDragMove = 1 << 4,
};

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
@property(nonatomic, assign) BOOL drawMode;
@property(nonatomic, assign) NSPoint dragStart;
@property(nonatomic, assign) NSPoint dragCurrent;
@property(nonatomic, assign) BOOL dragging;
@property(nonatomic, assign) SidescopesDragEdges adjusting;
@property(nonatomic, assign) NSRect adjustAnchor;
@property(nonatomic, assign) BOOL picked;
@property(nonatomic, assign) BOOL finished;
@property(nonatomic, assign) NSInteger hoveredSuggestion;
@property(nonatomic, assign) BOOL hasRemembered;
@property(nonatomic, assign) NSRect rememberedRect;
@property(nonatomic, assign) NSRect confirmedRect;
@end

@implementation SidescopesPickerView

- (void)switchToDrawMode:(BOOL)draw {
    if (self.drawMode == draw) return;
    if (!draw && suggestions_.empty()) return;  // nothing to pick from
    self.drawMode = draw;
    self.hoveredSuggestion = -1;
    self.dragging = NO;
    self.adjusting = SidescopesDragEdgeNone;
    [self.window invalidateCursorRectsForView:self];
    self.needsDisplay = YES;
}

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

- (SidescopesDragEdges)adjustZoneAtPoint:(NSPoint)point {
    if (!self.hasRemembered) return SidescopesDragEdgeNone;
    const CGFloat grip = 8.0;
    const NSRect rect = self.rememberedRect;
    if (!NSPointInRect(point, NSInsetRect(rect, -grip, -grip))) return SidescopesDragEdgeNone;
    SidescopesDragEdges edges = SidescopesDragEdgeNone;
    if (std::abs(point.x - NSMinX(rect)) <= grip) edges |= SidescopesDragEdgeLeft;
    if (std::abs(point.x - NSMaxX(rect)) <= grip) edges |= SidescopesDragEdgeRight;
    if (std::abs(point.y - NSMinY(rect)) <= grip) edges |= SidescopesDragEdgeBottom;
    if (std::abs(point.y - NSMaxY(rect)) <= grip) edges |= SidescopesDragEdgeTop;
    if (edges == SidescopesDragEdgeNone && NSPointInRect(point, rect)) edges = SidescopesDragMove;
    return edges;
}

// A punched hole must keep a whisper of alpha: the window server treats
// fully transparent window pixels as click-through, and the confirming
// click would land on the application underneath.
- (void)punchRect:(NSRect)rect {
    [[NSColor clearColor] setFill];
    NSRectFillUsingOperation(rect, NSCompositingOperationCopy);
    [[NSColor colorWithWhite:0 alpha:0.05] setFill];
    NSRectFillUsingOperation(rect, NSCompositingOperationSourceOver);
}

- (void)drawHandlesAroundRect:(NSRect)rect {
    const CGFloat half = 3.5;
    const CGFloat xs[3] = {NSMinX(rect), NSMidX(rect), NSMaxX(rect)};
    const CGFloat ys[3] = {NSMinY(rect), NSMidY(rect), NSMaxY(rect)};
    for (int ix = 0; ix < 3; ++ix)
        for (int iy = 0; iy < 3; ++iy) {
            if (ix == 1 && iy == 1) continue;
            const NSRect handle = NSMakeRect(xs[ix] - half, ys[iy] - half, 2 * half, 2 * half);
            [[NSColor whiteColor] setFill];
            NSRectFill(handle);
            [[NSColor colorWithWhite:0 alpha:0.6] setStroke];
            [[NSBezierPath bezierPathWithRect:handle] stroke];
        }
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSString* hint = @"";
    if (!self.drawMode) {
        [[NSColor colorWithWhite:0 alpha:0.2] setFill];
        NSRectFillUsingOperation(self.bounds, NSCompositingOperationSourceOver);
        if (self.hoveredSuggestion >= 0 &&
            self.hoveredSuggestion < static_cast<NSInteger>(suggestions_.size())) {
            // Only the rectangle under the cursor is shown, washed with the
            // system accent like window selection in the macOS screenshot
            // interface. Outlining every candidate at once was clutter.
            const auto& hovered = suggestions_[self.hoveredSuggestion];
            [self punchRect:hovered.first];
            [[[NSColor systemBlueColor] colorWithAlphaComponent:0.25] setFill];
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
        hint = @"Click a detected area  -  D to draw instead  -  Esc to cancel";
    } else {
        [[NSColor colorWithWhite:0 alpha:0.35] setFill];
        NSRectFillUsingOperation(self.bounds, NSCompositingOperationSourceOver);
        if (self.dragging) {
            const NSRect selection = [self selectionRect];
            [self punchRect:selection];
            [[NSColor whiteColor] setStroke];
            NSBezierPath* border = [NSBezierPath bezierPathWithRect:selection];
            border.lineWidth = 1.5;
            [border stroke];
        } else if (self.hasRemembered) {
            const NSRect rect = self.rememberedRect;
            [self punchRect:rect];
            [[NSColor whiteColor] setStroke];
            NSBezierPath* border = [NSBezierPath bezierPathWithRect:rect];
            border.lineWidth = 1.5;
            [border stroke];
            [self drawHandlesAroundRect:rect];
        }
        hint = suggestions_.empty()
                   ? @"Drag to select an area  -  Esc to cancel"
                   : @"Drag to select an area  -  A to pick a detected one  -  Esc to cancel";
    }

    if (!self.dragging && self.adjusting == SidescopesDragEdgeNone) {
        NSDictionary* attributes = @{
            NSForegroundColorAttributeName : [NSColor whiteColor],
            NSFontAttributeName : [NSFont systemFontOfSize:15 weight:NSFontWeightMedium],
        };
        const NSSize size = [hint sizeWithAttributes:attributes];
        [hint drawAtPoint:NSMakePoint((self.bounds.size.width - size.width) / 2,
                                      self.bounds.size.height - 60)
            withAttributes:attributes];
    }
}

- (void)resetCursorRects {
    if (self.drawMode) {
        [self addCursorRect:self.bounds cursor:NSCursor.crosshairCursor];
        if (self.hasRemembered)
            [self addCursorRect:self.rememberedRect cursor:NSCursor.openHandCursor];
    } else {
        [self addCursorRect:self.bounds cursor:NSCursor.pointingHandCursor];
    }
}

- (void)mouseMoved:(NSEvent*)event {
    if (self.drawMode) return;
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    const NSInteger hovered = [self suggestionAtPoint:point];
    if (hovered != self.hoveredSuggestion) {
        self.hoveredSuggestion = hovered;
        self.needsDisplay = YES;
    }
}

- (void)mouseDown:(NSEvent*)event {
    self.dragStart = [self convertPoint:event.locationInWindow fromView:nil];
    self.dragCurrent = self.dragStart;
    if (self.drawMode) {
        self.adjusting = [self adjustZoneAtPoint:self.dragStart];
        self.adjustAnchor = self.rememberedRect;
    }
    self.needsDisplay = YES;
}

- (void)mouseDragged:(NSEvent*)event {
    if (!self.drawMode) return;
    self.dragCurrent = [self convertPoint:event.locationInWindow fromView:nil];
    const CGFloat dx = self.dragCurrent.x - self.dragStart.x;
    const CGFloat dy = self.dragCurrent.y - self.dragStart.y;
    if (self.adjusting != SidescopesDragEdgeNone) {
        NSRect rect = self.adjustAnchor;
        if (self.adjusting & SidescopesDragMove) {
            rect.origin.x += dx;
            rect.origin.y += dy;
        } else {
            const CGFloat minimum = 16.0;
            if (self.adjusting & SidescopesDragEdgeLeft) {
                const CGFloat limit = NSMaxX(self.adjustAnchor) - minimum;
                const CGFloat left = std::min(NSMinX(self.adjustAnchor) + dx, limit);
                rect.size.width = NSMaxX(self.adjustAnchor) - left;
                rect.origin.x = left;
            }
            if (self.adjusting & SidescopesDragEdgeRight)
                rect.size.width = std::max(minimum, self.adjustAnchor.size.width + dx);
            if (self.adjusting & SidescopesDragEdgeBottom) {
                const CGFloat limit = NSMaxY(self.adjustAnchor) - minimum;
                const CGFloat bottom = std::min(NSMinY(self.adjustAnchor) + dy, limit);
                rect.size.height = NSMaxY(self.adjustAnchor) - bottom;
                rect.origin.y = bottom;
            }
            if (self.adjusting & SidescopesDragEdgeTop)
                rect.size.height = std::max(minimum, self.adjustAnchor.size.height + dy);
        }
        self.rememberedRect = rect;
        [self.window invalidateCursorRectsForView:self];
        self.needsDisplay = YES;
        return;
    }
    // A real drag only starts after a few points of travel, so a click on
    // the remembered rectangle never flashes a tiny manual selection.
    if (!self.dragging && (std::abs(dx) > 4 || std::abs(dy) > 4)) self.dragging = YES;
    self.needsDisplay = YES;
}

- (void)mouseUp:(NSEvent*)event {
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    if (!self.drawMode) {
        const NSInteger hovered = [self suggestionAtPoint:point];
        if (hovered < 0) return;  // a miss keeps the picker open
        self.picked = YES;
        self.confirmedRect = suggestions_[hovered].first;
        self.finished = YES;
        return;
    }
    self.dragCurrent = point;
    if (self.dragging) {
        const NSRect selection = [self selectionRect];
        self.dragging = NO;
        if (selection.size.width > 8 && selection.size.height > 8) {
            self.picked = YES;
            self.confirmedRect = selection;
            self.finished = YES;
        } else {
            self.needsDisplay = YES;
        }
        return;
    }
    if (self.adjusting != SidescopesDragEdgeNone) {
        const BOOL moved = !NSEqualRects(self.rememberedRect, self.adjustAnchor);
        const BOOL inside = (self.adjusting & SidescopesDragMove) != 0;
        self.adjusting = SidescopesDragEdgeNone;
        // A plain click inside confirms; releasing an adjustment keeps the
        // picker open for further tuning (Return also confirms).
        if (!moved && inside) {
            self.picked = YES;
            self.confirmedRect = self.rememberedRect;
            self.finished = YES;
            return;
        }
        self.needsDisplay = YES;
    }
}

- (void)keyDown:(NSEvent*)event {
    if (event.keyCode == 53) {  // ESC
        self.picked = NO;
        self.finished = YES;
        return;
    }
    if (event.keyCode == 36 || event.keyCode == 76) {  // Return / Enter
        if (self.drawMode && self.hasRemembered) {
            self.picked = YES;
            self.confirmedRect = self.rememberedRect;
            self.finished = YES;
            return;
        }
    }
    NSString* keys = event.charactersIgnoringModifiers;
    if (keys.length == 1) {
        const unichar key = [keys characterAtIndex:0];
        if (key == 'a' || key == 'A') {
            [self switchToDrawMode:NO];
            return;
        }
        if (key == 'd' || key == 'D') {
            [self switchToDrawMode:YES];
            return;
        }
    }
    [super keyDown:event];
}

// Whatever the user is currently indicating, for the live scope preview.
- (BOOL)previewRect:(NSRect*)rect {
    if (!self.drawMode) {
        if (self.hoveredSuggestion >= 0 &&
            self.hoveredSuggestion < static_cast<NSInteger>(suggestions_.size())) {
            *rect = suggestions_[self.hoveredSuggestion].first;
            return YES;
        }
        return NO;
    }
    if (self.dragging) {
        const NSRect selection = [self selectionRect];
        if (selection.size.width > 8 && selection.size.height > 8) {
            *rect = selection;
            return YES;
        }
        return NO;
    }
    if (self.hasRemembered) {
        *rect = self.rememberedRect;
        return YES;
    }
    return NO;
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

NSRect RegionToViewRect(const RegionOfInterest& region, NSSize view_size) {
    // Percent (top-left origin) to view coordinates (bottom-left origin).
    return NSMakeRect(region.left_percent / 100.0 * view_size.width,
                      (100.0 - region.bottom_percent) / 100.0 * view_size.height,
                      (region.right_percent - region.left_percent) / 100.0 * view_size.width,
                      (region.bottom_percent - region.top_percent) / 100.0 * view_size.height);
}

}  // namespace

namespace {

SidescopesPickerWindow* g_picker_window = nil;
SidescopesPickerView* g_picker_view = nil;
NSSize g_picker_size = {0, 0};

RegionOfInterest RegionFromViewRect(NSRect rect, NSSize size) {
    // View coordinates are bottom-left-origin; the region convention is
    // top-left-origin percentages.
    RegionOfInterest region;
    region.left_percent = rect.origin.x / size.width * 100.0;
    region.right_percent = (rect.origin.x + rect.size.width) / size.width * 100.0;
    region.top_percent = (size.height - (rect.origin.y + rect.size.height)) / size.height * 100.0;
    region.bottom_percent = (size.height - rect.origin.y) / size.height * 100.0;
    return region;
}

}  // namespace

bool BeginRegionPick(uint32_t display_id, const std::vector<SuggestedRegion>& suggestions,
                     const std::optional<RegionOfInterest>& current_region,
                     RegionPickerMode initial_mode) {
    if (g_picker_view) return false;  // one picker at a time
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
    const NSSize view_size = screen.frame.size;
    for (const SuggestedRegion& suggestion : suggestions) {
        view->suggestions_.emplace_back(RegionToViewRect(suggestion.region, view_size),
                                        suggestion.label);
    }
    if (current_region) {
        view.hasRemembered = YES;
        view.rememberedRect = RegionToViewRect(*current_region, view_size);
    }
    view.drawMode = initial_mode == RegionPickerMode::Draw || suggestions.empty();
    overlay.contentView = view;
    overlay.acceptsMouseMovedEvents = YES;

    // Force the app frontmost so the overlay owns the mouse for the whole
    // interaction; otherwise clicks can activate whatever is behind it.
    [NSApp activateIgnoringOtherApps:YES];
    [overlay makeKeyAndOrderFront:nil];
    [overlay makeFirstResponder:view];

    g_picker_window = overlay;
    g_picker_view = view;
    g_picker_size = view_size;
    return true;
}

RegionPickPoll PollRegionPick() {
    RegionPickPoll poll;
    if (!g_picker_view) return poll;
    poll.active = true;

    if (g_picker_view.finished) {
        poll.finished = true;
        if (g_picker_view.picked)
            poll.confirmed = RegionFromViewRect(g_picker_view.confirmedRect, g_picker_size);
        [g_picker_window orderOut:nil];
        g_picker_window = nil;
        g_picker_view = nil;
        return poll;
    }

    NSRect preview;
    if ([g_picker_view previewRect:&preview])
        poll.preview = RegionFromViewRect(preview, g_picker_size);
    return poll;
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
        // One level below the scope window: both float above Quick Look,
        // but the border must never cover the scopes. Sharing a level would
        // leave their order to whoever ordered front last.
        g_border_window.level = NSStatusWindowLevel - 1;
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
