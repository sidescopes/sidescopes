#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>

#include "platform/region_selection.h"

// Picker modes, ordered like the macOS screenshot toolbar: entire screen,
// pick a detected rectangle (its window-selection idiom), draw by hand
// (its portion-selection idiom).
typedef NS_ENUM(NSInteger, SidescopesPickerMode) {
    SidescopesPickerModeFull = 0,
    SidescopesPickerModePick = 1,
    SidescopesPickerModeDraw = 2,
};

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
@property(nonatomic, assign) SidescopesPickerMode mode;
@property(nonatomic, assign) NSPoint dragStart;
@property(nonatomic, assign) NSPoint dragCurrent;
@property(nonatomic, assign) BOOL dragging;
@property(nonatomic, assign) SidescopesDragEdges adjusting;
@property(nonatomic, assign) NSRect adjustAnchor;
@property(nonatomic, assign) BOOL picked;
@property(nonatomic, assign) NSInteger hoveredSuggestion;
@property(nonatomic, assign) BOOL hasRemembered;
@property(nonatomic, assign) NSRect rememberedRect;
@property(nonatomic, assign) NSRect confirmedRect;
@property(nonatomic, strong) NSView* toolbar;
@end

@implementation SidescopesPickerView

- (void)setPickerMode:(SidescopesPickerMode)mode {
    if (self.mode == mode) return;
    self.mode = mode;
    self.hoveredSuggestion = -1;
    self.dragging = NO;
    self.adjusting = SidescopesDragEdgeNone;
    [self.window invalidateCursorRectsForView:self];
    self.needsDisplay = YES;
}

- (void)modeChanged:(NSSegmentedControl*)sender {
    [self setPickerMode:static_cast<SidescopesPickerMode>(sender.selectedSegment)];
    // The control keeps key focus after a click; hand it back so ESC and
    // the mode keys keep working.
    [self.window makeFirstResponder:self];
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

- (void)drawLabel:(NSString*)label atPoint:(NSPoint)point size:(CGFloat)size {
    NSDictionary* attributes = @{
        NSForegroundColorAttributeName : [NSColor whiteColor],
        NSFontAttributeName : [NSFont systemFontOfSize:size weight:NSFontWeightMedium],
    };
    [label drawAtPoint:point withAttributes:attributes];
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSString* hint = @"";
    if (self.mode == SidescopesPickerModeFull) {
        [[[NSColor systemBlueColor] colorWithAlphaComponent:0.18] setFill];
        NSRectFillUsingOperation(self.bounds, NSCompositingOperationSourceOver);
        hint = @"Click to scope the entire screen  -  Esc to cancel";
    } else if (self.mode == SidescopesPickerModePick) {
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
            [self drawLabel:[NSString stringWithUTF8String:hovered.second.c_str()]
                    atPoint:NSMakePoint(hovered.first.origin.x + 6, NSMaxY(hovered.first) - 20)
                       size:12];
        }
        hint = suggestions_.empty() ? @"No areas detected  -  Esc to cancel"
                                    : @"Click a detected area  -  Esc to cancel";
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
        hint =
            self.hasRemembered
                ? @"Drag to select an area, or adjust and click the current one  -  Esc to cancel"
                : @"Drag to select an area  -  Esc to cancel";
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
    if (self.mode == SidescopesPickerModeDraw) {
        [self addCursorRect:self.bounds cursor:NSCursor.crosshairCursor];
        if (self.hasRemembered)
            [self addCursorRect:self.rememberedRect cursor:NSCursor.openHandCursor];
    } else {
        [self addCursorRect:self.bounds cursor:NSCursor.pointingHandCursor];
    }
    if (self.toolbar) [self addCursorRect:self.toolbar.frame cursor:NSCursor.arrowCursor];
}

- (void)mouseMoved:(NSEvent*)event {
    if (self.mode != SidescopesPickerModePick) return;
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
    if (self.mode == SidescopesPickerModeDraw) {
        self.adjusting = [self adjustZoneAtPoint:self.dragStart];
        self.adjustAnchor = self.rememberedRect;
    }
    self.needsDisplay = YES;
}

- (void)mouseDragged:(NSEvent*)event {
    if (self.mode != SidescopesPickerModeDraw) return;
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
    if (!self.dragging && (std::abs(dx) > 4 || std::abs(dy) > 4)) {
        self.dragging = YES;
        self.toolbar.hidden = YES;
    }
    self.needsDisplay = YES;
}

- (void)mouseUp:(NSEvent*)event {
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    if (self.mode == SidescopesPickerModeFull) {
        self.picked = YES;
        self.confirmedRect = self.bounds;
        [NSApp stopModalWithCode:NSModalResponseOK];
        return;
    }
    if (self.mode == SidescopesPickerModePick) {
        const NSInteger hovered = [self suggestionAtPoint:point];
        if (hovered < 0) return;  // a miss keeps the picker open
        self.picked = YES;
        self.confirmedRect = suggestions_[hovered].first;
        [NSApp stopModalWithCode:NSModalResponseOK];
        return;
    }
    self.dragCurrent = point;
    if (self.dragging) {
        const NSRect selection = [self selectionRect];
        self.dragging = NO;
        self.toolbar.hidden = NO;
        if (selection.size.width > 8 && selection.size.height > 8) {
            self.picked = YES;
            self.confirmedRect = selection;
            [NSApp stopModalWithCode:NSModalResponseOK];
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
            [NSApp stopModalWithCode:NSModalResponseOK];
            return;
        }
        self.needsDisplay = YES;
    }
}

- (void)keyDown:(NSEvent*)event {
    if (event.keyCode == 53) {  // ESC
        self.picked = NO;
        [NSApp stopModalWithCode:NSModalResponseCancel];
        return;
    }
    if (event.keyCode == 36 || event.keyCode == 76) {  // Return / Enter
        if (self.mode == SidescopesPickerModeFull) {
            self.picked = YES;
            self.confirmedRect = self.bounds;
            [NSApp stopModalWithCode:NSModalResponseOK];
            return;
        }
        if (self.mode == SidescopesPickerModeDraw && self.hasRemembered) {
            self.picked = YES;
            self.confirmedRect = self.rememberedRect;
            [NSApp stopModalWithCode:NSModalResponseOK];
            return;
        }
    }
    NSString* keys = event.charactersIgnoringModifiers;
    if (keys.length == 1) {
        const unichar key = [keys characterAtIndex:0];
        if (key >= '1' && key <= '3') {
            const auto mode = static_cast<SidescopesPickerMode>(key - '1');
            [self setPickerMode:mode];
            if ([self.toolbar.subviews.firstObject isKindOfClass:NSSegmentedControl.class])
                [static_cast<NSSegmentedControl*>(self.toolbar.subviews.firstObject)
                    setSelectedSegment:mode];
            return;
        }
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

// Remembered across invocations, like the screenshot interface remembers
// its capture mode. Negative until the picker has run once.
NSInteger g_last_picker_mode = -1;

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

NSImage* ToolbarSymbol(NSString* name, NSString* description) {
    NSImage* image = [NSImage imageWithSystemSymbolName:name accessibilityDescription:description];
    [image setTemplate:YES];
    return image;
}

// The screenshot-style mode toolbar: a blurred pill with one segment per
// mode, bottom-center of the overlay.
NSView* BuildModeToolbar(SidescopesPickerView* view, NSRect overlay_bounds,
                         SidescopesPickerMode mode) {
    NSSegmentedControl* segments =
        [NSSegmentedControl segmentedControlWithImages:@[
            ToolbarSymbol(@"macwindow", @"Entire screen"),
            ToolbarSymbol(@"rectangle.on.rectangle", @"Detected area"),
            ToolbarSymbol(@"rectangle.dashed", @"Drawn area"),
        ]
                                          trackingMode:NSSegmentSwitchTrackingSelectOne
                                                target:view
                                                action:@selector(modeChanged:)];
    [segments setToolTip:@"Scope the entire screen (1)" forSegment:0];
    [segments setToolTip:@"Pick a detected area (2)" forSegment:1];
    [segments setToolTip:@"Draw an area (3)" forSegment:2];
    segments.segmentStyle = NSSegmentStyleSeparated;
    segments.selectedSegment = mode;
    [segments sizeToFit];

    const CGFloat padding = 10.0;
    const NSSize content = segments.frame.size;
    NSVisualEffectView* pill = [[NSVisualEffectView alloc]
        initWithFrame:NSMakeRect((overlay_bounds.size.width - content.width) / 2 - padding, 48,
                                 content.width + 2 * padding, content.height + 2 * padding)];
    pill.material = NSVisualEffectMaterialHUDWindow;
    pill.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    pill.state = NSVisualEffectStateActive;
    pill.wantsLayer = YES;
    pill.layer.cornerRadius = (content.height + 2 * padding) / 2;
    pill.layer.masksToBounds = YES;
    segments.frame = NSMakeRect(padding, padding, content.width, content.height);
    [pill addSubview:segments];
    return pill;
}

}  // namespace

std::optional<RegionOfInterest> PickRegionOnDisplay(
    uint32_t display_id, const std::vector<SuggestedRegion>& suggestions,
    const std::optional<RegionOfInterest>& current_region) {
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

    SidescopesPickerMode mode =
        suggestions.empty() ? SidescopesPickerModeDraw : SidescopesPickerModePick;
    if (g_last_picker_mode >= 0) mode = static_cast<SidescopesPickerMode>(g_last_picker_mode);
    if (mode == SidescopesPickerModePick && suggestions.empty()) mode = SidescopesPickerModeDraw;
    view.mode = mode;

    view.toolbar = BuildModeToolbar(view, overlay.contentView.bounds, mode);
    [view addSubview:view.toolbar];
    overlay.contentView = view;
    overlay.acceptsMouseMovedEvents = YES;

    // Force the app frontmost so the overlay owns the mouse for the whole
    // interaction; otherwise clicks can activate whatever is behind it.
    [NSApp activateIgnoringOtherApps:YES];
    [overlay makeKeyAndOrderFront:nil];
    [overlay makeFirstResponder:view];
    const NSModalResponse response = [NSApp runModalForWindow:overlay];
    g_last_picker_mode = view.mode;
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
