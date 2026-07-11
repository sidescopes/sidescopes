#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>

#include "platform/region_selection.h"

// Which edges a border drag adjusts; Move relocates the whole region.
typedef NS_OPTIONS(NSUInteger, SidescopesDragEdges) {
    SidescopesDragEdgeNone = 0,
    SidescopesDragEdgeLeft = 1 << 0,
    SidescopesDragEdgeRight = 1 << 1,
    SidescopesDragEdgeBottom = 1 << 2,
    SidescopesDragEdgeTop = 1 << 3,
    SidescopesDragMove = 1 << 4,
};

namespace sidescopes {
namespace {

// The border strokes sit this far OUTSIDE the region so they never enter
// the scoped pixels.
constexpr double kBorderMargin = 4.0;
// The border window extends this far beyond the region: the grab ring for
// resizing, wide enough to hit with a cursor.
constexpr double kBorderPad = 14.0;
// The move tab above the top edge.
constexpr double kTabWidth = 40.0;
constexpr double kTabHeight = 12.0;
// Regions cannot shrink beyond this many points per side.
constexpr double kMinimumRegionSize = 24.0;

NSScreen* ScreenForDisplay(uint32_t display_id) {
    for (NSScreen* screen in NSScreen.screens) {
        NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
        if (number && number.unsignedIntValue == display_id) return screen;
    }
    return NSScreen.mainScreen;
}

RegionOfInterest RegionFromScreenRect(NSRect rect, NSRect screen_frame) {
    // Screen coordinates are bottom-left-origin; the region convention is
    // top-left-origin percentages.
    RegionOfInterest region;
    region.left_percent = (rect.origin.x - screen_frame.origin.x) / screen_frame.size.width * 100.0;
    region.right_percent =
        (rect.origin.x + rect.size.width - screen_frame.origin.x) / screen_frame.size.width * 100.0;
    region.top_percent =
        (screen_frame.origin.y + screen_frame.size.height - (rect.origin.y + rect.size.height)) /
        screen_frame.size.height * 100.0;
    region.bottom_percent = (screen_frame.origin.y + screen_frame.size.height - rect.origin.y) /
                            screen_frame.size.height * 100.0;
    return region;
}

// Shared edit state the application polls once per frame.
bool g_border_editing = false;
bool g_border_edit_changed = false;
RegionOfInterest g_border_edit_region;

}  // namespace
}  // namespace sidescopes

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
    // This application's own windows, in view coordinates: undimmed and
    // truly transparent, so clicks fall through to them.
    std::vector<NSRect> exclusions_;
}
@property(nonatomic, assign) BOOL drawMode;
@property(nonatomic, assign) NSPoint dragStart;
@property(nonatomic, assign) NSPoint dragCurrent;
@property(nonatomic, assign) BOOL dragging;
@property(nonatomic, assign) BOOL picked;
@property(nonatomic, assign) BOOL finished;
@property(nonatomic, assign) NSInteger hoveredSuggestion;
@property(nonatomic, assign) NSRect confirmedRect;
@end

@implementation SidescopesPickerView

- (void)switchToDrawMode:(BOOL)draw {
    if (self.drawMode == draw) return;
    if (!draw && suggestions_.empty()) return;  // nothing to pick from
    self.drawMode = draw;
    self.hoveredSuggestion = -1;
    self.dragging = NO;
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

// A punched hole must keep a whisper of alpha when it should stay
// clickable: the window server treats fully transparent window pixels as
// click-through. The picker uses both behaviors deliberately - 5% black
// for its own click targets, true zero alpha over this application's
// windows so clicks reach them.
- (void)punchRect:(NSRect)rect {
    [[NSColor clearColor] setFill];
    NSRectFillUsingOperation(rect, NSCompositingOperationCopy);
    [[NSColor colorWithWhite:0 alpha:0.05] setFill];
    NSRectFillUsingOperation(rect, NSCompositingOperationSourceOver);
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
        hint = @"Click a detected area  -  D to draw instead  -  Esc resets to full screen";
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
        }
        hint = suggestions_.empty() ? @"Drag to select an area  -  Esc resets to full screen"
                                    : @"Drag to select an area  -  A to pick a detected one  -  "
                                      @"Esc resets to full screen";
    }

    if (!self.dragging) {
        NSDictionary* attributes = @{
            NSForegroundColorAttributeName : [NSColor whiteColor],
            NSFontAttributeName : [NSFont systemFontOfSize:15 weight:NSFontWeightMedium],
        };
        const NSSize size = [hint sizeWithAttributes:attributes];
        [hint drawAtPoint:NSMakePoint((self.bounds.size.width - size.width) / 2,
                                      self.bounds.size.height - 60)
            withAttributes:attributes];
    }

    // Last, so nothing re-adds alpha over them: this application's own
    // windows stay usable while the picker is up.
    [[NSColor clearColor] setFill];
    for (const NSRect& exclusion : exclusions_)
        NSRectFillUsingOperation(exclusion, NSCompositingOperationCopy);
}

- (void)resetCursorRects {
    [self addCursorRect:self.bounds
                 cursor:self.drawMode ? NSCursor.crosshairCursor : NSCursor.pointingHandCursor];
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
    self.needsDisplay = YES;
}

- (void)mouseDragged:(NSEvent*)event {
    if (!self.drawMode) return;
    self.dragCurrent = [self convertPoint:event.locationInWindow fromView:nil];
    // A real drag only starts after a few points of travel, so a stray
    // click never flashes a tiny manual selection.
    if (!self.dragging && (std::abs(self.dragCurrent.x - self.dragStart.x) > 4 ||
                           std::abs(self.dragCurrent.y - self.dragStart.y) > 4))
        self.dragging = YES;
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
    }
}

- (void)keyDown:(NSEvent*)event {
    if (event.keyCode == 53) {  // ESC
        self.picked = NO;
        self.finished = YES;
        return;
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

// The interactive region border: double-stroked so it reads on any
// background, with a grab ring outside the region for resizing and a tab
// above the top edge for moving. The interior is truly transparent, so
// the editor underneath keeps receiving clicks.
@interface SidescopesBorderView : NSView
@property(nonatomic, assign) SidescopesDragEdges dragZone;
@property(nonatomic, assign) NSPoint dragStartMouse;  // global screen coords
@property(nonatomic, assign) NSRect dragStartRegion;  // global screen coords
@end
@implementation SidescopesBorderView

// The region rectangle in view coordinates.
- (NSRect)regionRect {
    return NSInsetRect(self.bounds, sidescopes::kBorderPad, sidescopes::kBorderPad);
}

- (NSRect)tabRect {
    const NSRect region = [self regionRect];
    return NSMakeRect(NSMidX(region) - sidescopes::kTabWidth / 2,
                      NSMaxY(region) + sidescopes::kBorderMargin, sidescopes::kTabWidth,
                      sidescopes::kTabHeight);
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect region = [self regionRect];

    // The grab ring: a whisper of alpha keeps it hit-testable (safely
    // above the window server's threshold); the interior stays truly
    // transparent and therefore click-through.
    [[NSColor colorWithWhite:0 alpha:0.08] setFill];
    NSRectFillUsingOperation(self.bounds, NSCompositingOperationCopy);
    [[NSColor clearColor] setFill];
    NSRectFillUsingOperation(region, NSCompositingOperationCopy);

    const NSRect outer =
        NSInsetRect(region, -sidescopes::kBorderMargin, -sidescopes::kBorderMargin);
    // A dark-white-dark sandwich: the white line separates from dark
    // content, the dark casing separates it from light content, so the
    // border reads on any image.
    [[NSColor colorWithWhite:0 alpha:0.9] setStroke];
    NSBezierPath* casing_outer = [NSBezierPath bezierPathWithRect:NSInsetRect(outer, 0.5, 0.5)];
    casing_outer.lineWidth = 1.2;
    [casing_outer stroke];
    NSBezierPath* casing_inner = [NSBezierPath bezierPathWithRect:NSInsetRect(outer, 3.4, 3.4)];
    casing_inner.lineWidth = 1.2;
    [casing_inner stroke];
    [[NSColor colorWithWhite:1 alpha:0.95] setStroke];
    NSBezierPath* light = [NSBezierPath bezierPathWithRect:NSInsetRect(outer, 1.9, 1.9)];
    light.lineWidth = 1.8;
    [light stroke];

    // The move tab.
    NSBezierPath* tab = [NSBezierPath bezierPathWithRoundedRect:[self tabRect]
                                                        xRadius:4.0
                                                        yRadius:4.0];
    [[NSColor colorWithWhite:0 alpha:0.8] setFill];
    [tab fill];
    [[NSColor colorWithWhite:1 alpha:0.95] setStroke];
    tab.lineWidth = 1.2;
    [tab stroke];
    // Grip dots so the tab reads as a handle.
    [[NSColor colorWithWhite:1 alpha:0.9] setFill];
    const NSRect tab_rect = [self tabRect];
    for (int dot = -1; dot <= 1; ++dot) {
        const NSRect dot_rect =
            NSMakeRect(NSMidX(tab_rect) + dot * 6.0 - 1.25, NSMidY(tab_rect) - 1.25, 2.5, 2.5);
        [[NSBezierPath bezierPathWithOvalInRect:dot_rect] fill];
    }
}

- (SidescopesDragEdges)zoneAtPoint:(NSPoint)point {
    if (NSPointInRect(point, [self tabRect])) return SidescopesDragMove;
    const NSRect region = [self regionRect];
    if (NSPointInRect(point, region)) return SidescopesDragEdgeNone;  // click-through anyway
    SidescopesDragEdges edges = SidescopesDragEdgeNone;
    if (point.x < NSMinX(region)) edges |= SidescopesDragEdgeLeft;
    if (point.x > NSMaxX(region)) edges |= SidescopesDragEdgeRight;
    if (point.y < NSMinY(region)) edges |= SidescopesDragEdgeBottom;
    if (point.y > NSMaxY(region)) edges |= SidescopesDragEdgeTop;
    // Near a corner, an edge grab also takes the perpendicular edge, so
    // corners resize diagonally.
    const CGFloat corner = 18.0;
    const bool horizontal = edges & (SidescopesDragEdgeLeft | SidescopesDragEdgeRight);
    const bool vertical = edges & (SidescopesDragEdgeTop | SidescopesDragEdgeBottom);
    if (horizontal && !vertical) {
        if (point.y > NSMaxY(region) - corner)
            edges |= SidescopesDragEdgeTop;
        else if (point.y < NSMinY(region) + corner)
            edges |= SidescopesDragEdgeBottom;
    } else if (vertical && !horizontal) {
        if (point.x > NSMaxX(region) - corner)
            edges |= SidescopesDragEdgeRight;
        else if (point.x < NSMinX(region) + corner)
            edges |= SidescopesDragEdgeLeft;
    }
    return edges;
}

- (void)resetCursorRects {
    const NSRect region = [self regionRect];
    const NSRect bounds = self.bounds;
    [self addCursorRect:[self tabRect] cursor:NSCursor.openHandCursor];
    [self addCursorRect:NSMakeRect(bounds.origin.x, region.origin.y, sidescopes::kBorderPad,
                                   region.size.height)
                 cursor:NSCursor.resizeLeftRightCursor];
    [self addCursorRect:NSMakeRect(NSMaxX(region), region.origin.y, sidescopes::kBorderPad,
                                   region.size.height)
                 cursor:NSCursor.resizeLeftRightCursor];
    [self addCursorRect:NSMakeRect(region.origin.x, bounds.origin.y, region.size.width,
                                   sidescopes::kBorderPad)
                 cursor:NSCursor.resizeUpDownCursor];
    [self addCursorRect:NSMakeRect(region.origin.x, NSMaxY(region), region.size.width,
                                   sidescopes::kBorderPad)
                 cursor:NSCursor.resizeUpDownCursor];
}

- (void)mouseDown:(NSEvent*)event {
    const NSPoint local = [self convertPoint:event.locationInWindow fromView:nil];
    self.dragZone = [self zoneAtPoint:local];
    if (self.dragZone == SidescopesDragEdgeNone) return;
    self.dragStartMouse = NSEvent.mouseLocation;
    self.dragStartRegion =
        NSInsetRect(self.window.frame, sidescopes::kBorderPad, sidescopes::kBorderPad);
    sidescopes::g_border_editing = true;
}

- (void)mouseDragged:(NSEvent*)event {
    (void)event;
    if (self.dragZone == SidescopesDragEdgeNone) return;
    const NSPoint mouse = NSEvent.mouseLocation;
    const CGFloat dx = mouse.x - self.dragStartMouse.x;
    const CGFloat dy = mouse.y - self.dragStartMouse.y;
    NSRect rect = self.dragStartRegion;
    if (self.dragZone & SidescopesDragMove) {
        rect.origin.x += dx;
        rect.origin.y += dy;
    } else {
        if (self.dragZone & SidescopesDragEdgeLeft) {
            const CGFloat limit = NSMaxX(self.dragStartRegion) - sidescopes::kMinimumRegionSize;
            const CGFloat left = std::min(NSMinX(self.dragStartRegion) + dx, limit);
            rect.size.width = NSMaxX(self.dragStartRegion) - left;
            rect.origin.x = left;
        }
        if (self.dragZone & SidescopesDragEdgeRight)
            rect.size.width =
                std::max(sidescopes::kMinimumRegionSize, self.dragStartRegion.size.width + dx);
        if (self.dragZone & SidescopesDragEdgeBottom) {
            const CGFloat limit = NSMaxY(self.dragStartRegion) - sidescopes::kMinimumRegionSize;
            const CGFloat bottom = std::min(NSMinY(self.dragStartRegion) + dy, limit);
            rect.size.height = NSMaxY(self.dragStartRegion) - bottom;
            rect.origin.y = bottom;
        }
        if (self.dragZone & SidescopesDragEdgeTop)
            rect.size.height =
                std::max(sidescopes::kMinimumRegionSize, self.dragStartRegion.size.height + dy);
    }
    NSScreen* screen = self.window.screen ? self.window.screen : NSScreen.mainScreen;
    sidescopes::g_border_edit_region = sidescopes::RegionFromScreenRect(rect, screen.frame);
    sidescopes::g_border_edit_changed = true;
}

- (void)mouseUp:(NSEvent*)event {
    (void)event;
    self.dragZone = SidescopesDragEdgeNone;
    sidescopes::g_border_editing = false;
}

// The scoped editor is usually the active application; without this, the
// first grab of the border is swallowed as an activation gesture.
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}

@end

namespace sidescopes {
namespace {

NSWindow* g_border_window = nil;
SidescopesPickerWindow* g_picker_window = nil;
SidescopesPickerView* g_picker_view = nil;
NSSize g_picker_size = {0, 0};

NSRect RegionToViewRect(const RegionOfInterest& region, NSSize view_size) {
    // Percent (top-left origin) to view coordinates (bottom-left origin).
    return NSMakeRect(region.left_percent / 100.0 * view_size.width,
                      (100.0 - region.bottom_percent) / 100.0 * view_size.height,
                      (region.right_percent - region.left_percent) / 100.0 * view_size.width,
                      (region.bottom_percent - region.top_percent) / 100.0 * view_size.height);
}

}  // namespace

bool BeginRegionPick(uint32_t display_id, const std::vector<SuggestedRegion>& suggestions,
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
    // This application's own visible windows stay reachable through the
    // overlay: the toolbar keeps working while picking.
    for (NSWindow* window in NSApp.windows) {
        if (window == overlay || window == g_border_window || !window.isVisible) continue;
        const NSRect frame = window.frame;
        view->exclusions_.push_back(NSMakeRect(frame.origin.x - screen.frame.origin.x,
                                               frame.origin.y - screen.frame.origin.y,
                                               frame.size.width, frame.size.height));
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

    const auto region_from_view = [](NSRect rect, NSSize size) {
        RegionOfInterest region;
        region.left_percent = rect.origin.x / size.width * 100.0;
        region.right_percent = (rect.origin.x + rect.size.width) / size.width * 100.0;
        region.top_percent =
            (size.height - (rect.origin.y + rect.size.height)) / size.height * 100.0;
        region.bottom_percent = (size.height - rect.origin.y) / size.height * 100.0;
        return region;
    };

    if (g_picker_view.finished) {
        poll.finished = true;
        if (g_picker_view.picked)
            poll.confirmed = region_from_view(g_picker_view.confirmedRect, g_picker_size);
        [g_picker_window orderOut:nil];
        g_picker_window = nil;
        g_picker_view = nil;
        return poll;
    }

    if (!g_picker_view.drawMode) {
        const NSInteger hovered = g_picker_view.hoveredSuggestion;
        if (hovered >= 0 && hovered < static_cast<NSInteger>(g_picker_view->suggestions_.size()))
            poll.preview =
                region_from_view(g_picker_view->suggestions_[hovered].first, g_picker_size);
    } else if (g_picker_view.dragging) {
        const NSRect selection = [g_picker_view selectionRect];
        if (selection.size.width > 8 && selection.size.height > 8)
            poll.preview = region_from_view(selection, g_picker_size);
    }
    return poll;
}

void CancelRegionPick() {
    if (!g_picker_view) return;
    g_picker_view.picked = NO;
    g_picker_view.finished = YES;
}

void SetRegionPickMode(RegionPickerMode mode) {
    if (!g_picker_view) return;
    [g_picker_view switchToDrawMode:(mode == RegionPickerMode::Draw)];
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
        NSMakeRect(left - kBorderPad, bottom - kBorderPad, (right - left) + 2 * kBorderPad,
                   (top - bottom) + 2 * kBorderPad);

    if (!g_border_window) {
        g_border_window = [[NSWindow alloc] initWithContentRect:rect
                                                      styleMask:NSWindowStyleMaskBorderless
                                                        backing:NSBackingStoreBuffered
                                                          defer:NO];
        g_border_window.backgroundColor = NSColor.clearColor;
        g_border_window.opaque = NO;
        g_border_window.hasShadow = NO;
        // The interior is truly transparent and therefore click-through;
        // only the grab ring and tab take the mouse. ignoresMouseEvents is
        // deliberately never set: an explicit assignment - even to NO -
        // switches AppKit off its per-pixel alpha hit-testing, and the
        // whole window starts swallowing clicks.
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
    [g_border_window invalidateCursorRectsForView:g_border_window.contentView];
    [g_border_window orderFrontRegardless];
}

void HideRegionBorder() {
    [g_border_window orderOut:nil];
}

RegionBorderEdit PollRegionBorderEdit() {
    RegionBorderEdit edit;
    edit.editing = g_border_editing;
    if (g_border_edit_changed) {
        edit.region = g_border_edit_region;
        g_border_edit_changed = false;
    }
    return edit;
}

}  // namespace sidescopes
