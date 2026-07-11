#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>

#include "platform/face_detection.h"
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

// The border window extends this far beyond the region: the grab band,
// wide enough to hit with a cursor, slim enough to stay unobtrusive.
constexpr double kBorderPad = 12.0;
// Within this distance of a region corner, a grab resizes; the rest of
// the band moves the region (Shift-drag resizes a single edge).
constexpr double kCornerZone = 22.0;
// Half-length of the edge-midpoint handle's grab zone along its edge.
constexpr double kMidpointZone = 22.0;
// Visible size of the square resize handles on the measurement line.
constexpr double kHandleSize = 9.0;
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
    // The active suggestion list in view coordinates, with labels - the
    // windows or the faces, depending on the mode.
    std::vector<std::pair<NSRect, std::string>> suggestions_;
    std::vector<std::pair<NSRect, std::string>> windows_;
    std::vector<std::pair<NSRect, std::string>> faces_;
    // This application's own windows, in view coordinates: undimmed and
    // truly transparent, so clicks fall through to them.
    std::vector<NSRect> exclusions_;
}
// NO = suggestion picking (windows or faces), YES = drag to draw.
@property(nonatomic, assign) BOOL drawMode;
// In picking mode: whether the face list is active instead of windows.
@property(nonatomic, assign) BOOL facesMode;
@property(nonatomic, assign) NSPoint dragStart;
@property(nonatomic, assign) NSPoint dragCurrent;
@property(nonatomic, assign) BOOL dragging;
@property(nonatomic, assign) BOOL picked;
@property(nonatomic, assign) BOOL finished;
@property(nonatomic, assign) NSInteger hoveredSuggestion;
@property(nonatomic, assign) NSRect confirmedRect;
@end

@implementation SidescopesPickerView

// 0 = pick a window, 1 = draw, 2 = pick a face. Face mode is offered
// even when no face was found: the honest answer is the empty overlay
// saying so, not a key that silently does nothing.
- (void)switchToMode:(int)mode {
    const BOOL draw = mode == 1;
    const BOOL faces = mode == 2;
    if (self.drawMode == draw && self.facesMode == faces) return;
    self.drawMode = draw;
    self.facesMode = faces;
    suggestions_ = faces ? faces_ : windows_;
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

// The mode instruction: a primary line and a bracketed-keys line on a
// dark pill, placed where this application's own windows do not cover
// it - they float above the overlay, and a message half-hidden behind
// the scope window reads as a glitch.
- (void)drawBanner:(NSString*)primary secondary:(NSString*)secondary preferCenter:(BOOL)center {
    NSDictionary* primary_attributes = @{
        NSForegroundColorAttributeName : [NSColor whiteColor],
        NSFontAttributeName : [NSFont systemFontOfSize:22 weight:NSFontWeightSemibold],
    };
    NSDictionary* secondary_attributes = @{
        NSForegroundColorAttributeName : [NSColor colorWithWhite:1 alpha:0.75],
        NSFontAttributeName : [NSFont systemFontOfSize:14 weight:NSFontWeightRegular],
    };
    const NSSize primary_size = [primary sizeWithAttributes:primary_attributes];
    const NSSize secondary_size = [secondary sizeWithAttributes:secondary_attributes];
    const CGFloat width = std::max(primary_size.width, secondary_size.width) + 48;
    const CGFloat height = primary_size.height + secondary_size.height + 30;
    const CGFloat x = (self.bounds.size.width - width) / 2;
    const CGFloat top_y = self.bounds.size.height - height - 80;
    const CGFloat center_y = (self.bounds.size.height - height) / 2;
    const CGFloat low_y = self.bounds.size.height * 0.22;
    const CGFloat candidates[3] = {center ? center_y : top_y, center ? top_y : center_y, low_y};
    NSRect banner = NSMakeRect(x, candidates[0], width, height);
    for (const CGFloat candidate : candidates) {
        const NSRect probe = NSMakeRect(x, candidate, width, height);
        BOOL covered = NO;
        for (const NSRect& exclusion : exclusions_) {
            if (NSIntersectsRect(NSInsetRect(probe, -12, -12), exclusion)) {
                covered = YES;
                break;
            }
        }
        if (!covered) {
            banner = probe;
            break;
        }
    }
    [[NSColor colorWithWhite:0 alpha:0.55] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:banner xRadius:12 yRadius:12] fill];
    [primary drawAtPoint:NSMakePoint(banner.origin.x + (width - primary_size.width) / 2,
                                     NSMaxY(banner) - primary_size.height - 10)
          withAttributes:primary_attributes];
    [secondary drawAtPoint:NSMakePoint(banner.origin.x + (width - secondary_size.width) / 2,
                                       banner.origin.y + 10)
            withAttributes:secondary_attributes];
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    if (!self.drawMode) {
        [[NSColor colorWithWhite:0 alpha:0.2] setFill];
        NSRectFillUsingOperation(self.bounds, NSCompositingOperationSourceOver);
        if (self.facesMode) {
            // Faces are few and easy to miss: every found face is outlined
            // up front, so the answer is visible before any hovering. The
            // hovered one still gets the full accent treatment below.
            [[NSColor colorWithWhite:1 alpha:0.85] setStroke];
            for (NSInteger i = 0; i < static_cast<NSInteger>(suggestions_.size()); ++i) {
                if (i == self.hoveredSuggestion) continue;
                [self punchRect:suggestions_[i].first];
                NSBezierPath* outline = [NSBezierPath bezierPathWithRect:suggestions_[i].first];
                outline.lineWidth = 1.5;
                [outline stroke];
            }
        }
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
        if (self.facesMode) {
            [self drawBanner:suggestions_.empty() ? @"No faces found on this screen"
                                                  : @"Click a face"
                   secondary:@"[A] pick a window    [D] draw    [Esc] full screen"
                preferCenter:suggestions_.empty()];
        } else {
            [self drawBanner:@"Click a window"
                   secondary:sidescopes::SupportsFaceDetection()
                                 ? @"[F] pick a face    [D] draw    [Esc] full screen"
                                 : @"[D] draw    [Esc] full screen"
                preferCenter:NO];
        }
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
        if (!self.dragging) {
            NSString* secondary = @"[Esc] full screen";
            if (!windows_.empty() && sidescopes::SupportsFaceDetection())
                secondary = @"[A] pick a window    [F] pick a face    [Esc] full screen";
            else if (!windows_.empty())
                secondary = @"[A] pick a window    [Esc] full screen";
            [self drawBanner:@"Drag to select an area" secondary:secondary preferCenter:NO];
        }
    }
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
            [self switchToMode:0];
            return;
        }
        if (key == 'd' || key == 'D') {
            [self switchToMode:1];
            return;
        }
        if (key == 'f' || key == 'F') {
            [self switchToMode:2];
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

// The interactive region border. The band outside the region is muted
// hazard tape; grabbing it MOVES the region - moving is the frequent
// operation, so it owns the long edges - while the corners resize, and
// Shift-dragging an edge resizes just that edge. The interior is truly
// transparent, so the editor underneath keeps receiving clicks. Cursors
// come from an always-active tracking area: cursor rects only work in
// the key window, and this window never takes key.
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

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect region = [self regionRect];

    // The whole grab band is the hazard tape, muted so it stays calm
    // beside the photograph while its own light-dark alternation keeps it
    // visible on any content. The interior is never painted and therefore
    // stays click-through.
    NSBezierPath* ring_clip = [NSBezierPath bezierPathWithRect:self.bounds];
    [ring_clip appendBezierPathWithRect:region];
    ring_clip.windingRule = NSWindingRuleEvenOdd;
    [NSGraphicsContext saveGraphicsState];
    [ring_clip addClip];
    [[NSColor colorWithWhite:0.1 alpha:0.45] setFill];
    NSRectFillUsingOperation(self.bounds, NSCompositingOperationCopy);
    NSBezierPath* stripes = [NSBezierPath bezierPath];
    stripes.lineWidth = 4.0;
    const NSRect bounds = self.bounds;
    for (CGFloat x = NSMinX(bounds) - bounds.size.height; x < NSMaxX(bounds); x += 10.0) {
        [stripes moveToPoint:NSMakePoint(x, NSMinY(bounds))];
        [stripes lineToPoint:NSMakePoint(x + bounds.size.height, NSMaxY(bounds))];
    }
    [[NSColor colorWithWhite:0.9 alpha:0.45] setStroke];
    [stripes stroke];
    [NSGraphicsContext restoreGraphicsState];

    // A narrow bright line marks the exact measured edge.
    [[NSColor colorWithWhite:0.97 alpha:0.9] setStroke];
    NSBezierPath* edge = [NSBezierPath bezierPathWithRect:NSInsetRect(region, -0.9, -0.9)];
    edge.lineWidth = 1.4;
    [edge stroke];

    // Eight resize handles - corners and edge midpoints - the universal
    // selection idiom, so the affordance explains itself. They live in the
    // middle of the hazard band, entirely outside the region: the scoped
    // pixels stay untouched, and bright squares on the muted stripes read
    // like fittings on a frame.
    const CGFloat out = sidescopes::kBorderPad / 2;
    const auto handle = [&](CGFloat x, CGFloat y) {
        const NSRect square =
            NSMakeRect(x - sidescopes::kHandleSize / 2, y - sidescopes::kHandleSize / 2,
                       sidescopes::kHandleSize, sidescopes::kHandleSize);
        [[NSColor colorWithWhite:0.97 alpha:0.95] setFill];
        NSBezierPath* fill = [NSBezierPath bezierPathWithRoundedRect:square xRadius:2 yRadius:2];
        [fill fill];
        [[NSColor colorWithWhite:0.1 alpha:0.7] setStroke];
        fill.lineWidth = 1.0;
        [fill stroke];
    };
    handle(NSMinX(region) - out, NSMinY(region) - out);
    handle(NSMidX(region), NSMinY(region) - out);
    handle(NSMaxX(region) + out, NSMinY(region) - out);
    handle(NSMinX(region) - out, NSMidY(region));
    handle(NSMaxX(region) + out, NSMidY(region));
    handle(NSMinX(region) - out, NSMaxY(region) + out);
    handle(NSMidX(region), NSMaxY(region) + out);
    handle(NSMaxX(region) + out, NSMaxY(region) + out);
}

// Eight handles, no modifier: the corners resize both axes, the edge
// midpoints resize their edge, and the rest of the band moves. The
// visible handles say which is which - a modifier key never could.
- (SidescopesDragEdges)zoneAtPoint:(NSPoint)point {
    const NSRect region = [self regionRect];
    if (NSPointInRect(point, region)) return SidescopesDragEdgeNone;  // click-through anyway
    const BOOL near_left = point.x < NSMinX(region) + sidescopes::kCornerZone;
    const BOOL near_right = point.x > NSMaxX(region) - sidescopes::kCornerZone;
    const BOOL near_bottom = point.y < NSMinY(region) + sidescopes::kCornerZone;
    const BOOL near_top = point.y > NSMaxY(region) - sidescopes::kCornerZone;
    if ((near_left || near_right) && (near_top || near_bottom)) {
        SidescopesDragEdges edges = SidescopesDragEdgeNone;
        edges |= near_left ? SidescopesDragEdgeLeft : SidescopesDragEdgeRight;
        edges |= near_bottom ? SidescopesDragEdgeBottom : SidescopesDragEdgeTop;
        return edges;
    }
    const BOOL mid_x = std::abs(point.x - NSMidX(region)) <= sidescopes::kMidpointZone;
    const BOOL mid_y = std::abs(point.y - NSMidY(region)) <= sidescopes::kMidpointZone;
    if (mid_x && point.y > NSMaxY(region)) return SidescopesDragEdgeTop;
    if (mid_x && point.y < NSMinY(region)) return SidescopesDragEdgeBottom;
    if (mid_y && point.x < NSMinX(region)) return SidescopesDragEdgeLeft;
    if (mid_y && point.x > NSMaxX(region)) return SidescopesDragEdgeRight;
    return SidescopesDragMove;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea* area in self.trackingAreas) [self removeTrackingArea:area];
    NSTrackingArea* tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                     NSTrackingActiveAlways | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:tracking];
}

- (void)applyCursorForZone:(SidescopesDragEdges)zone {
    if (zone == SidescopesDragEdgeNone) {
        [NSCursor.arrowCursor set];
        return;
    }
    if (zone & SidescopesDragMove) {
        [NSCursor.openHandCursor set];
        return;
    }
    const BOOL horizontal = (zone & (SidescopesDragEdgeLeft | SidescopesDragEdgeRight)) != 0;
    const BOOL vertical = (zone & (SidescopesDragEdgeTop | SidescopesDragEdgeBottom)) != 0;
    if (horizontal && vertical) {
        if (@available(macOS 15.0, *)) {
            const BOOL rising =
                ((zone & SidescopesDragEdgeLeft) != 0) == ((zone & SidescopesDragEdgeBottom) != 0);
            [[NSCursor frameResizeCursorFromPosition:rising ? NSCursorFrameResizePositionBottomLeft
                                                            : NSCursorFrameResizePositionBottomRight
                                        inDirections:NSCursorFrameResizeDirectionsAll] set];
        } else {
            [NSCursor.crosshairCursor set];
        }
        return;
    }
    [horizontal ? NSCursor.resizeLeftRightCursor : NSCursor.resizeUpDownCursor set];
}

- (void)mouseMoved:(NSEvent*)event {
    if (self.dragZone != SidescopesDragEdgeNone) return;  // drag owns the cursor
    const NSPoint local = [self convertPoint:event.locationInWindow fromView:nil];
    [self applyCursorForZone:[self zoneAtPoint:local]];
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    if (self.dragZone == SidescopesDragEdgeNone) [NSCursor.arrowCursor set];
}

- (void)mouseDown:(NSEvent*)event {
    const NSPoint local = [self convertPoint:event.locationInWindow fromView:nil];
    self.dragZone = [self zoneAtPoint:local];
    if (self.dragZone == SidescopesDragEdgeNone) return;
    if (self.dragZone & SidescopesDragMove) [NSCursor.closedHandCursor set];
    self.dragStartMouse = NSEvent.mouseLocation;
    self.dragStartRegion =
        NSInsetRect(self.window.frame, sidescopes::kBorderPad, sidescopes::kBorderPad);
    sidescopes::g_border_editing = true;
}

- (void)mouseDragged:(NSEvent*)event {
    (void)event;
    if (self.dragZone == SidescopesDragEdgeNone) return;
    if (self.dragZone & SidescopesDragMove) [NSCursor.closedHandCursor set];
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
    if (self.dragZone & SidescopesDragMove) [NSCursor.openHandCursor set];
    self.dragZone = SidescopesDragEdgeNone;
    sidescopes::g_border_editing = false;
    const NSPoint local = [self convertPoint:event.locationInWindow fromView:nil];
    [self applyCursorForZone:[self zoneAtPoint:local]];
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
// This application's own windows are raised above the overlay for the
// pick, with their previous levels remembered for the teardown: real
// window compositing keeps their rounded corners and click handling,
// where punching rectangular holes in the dimming could not.
std::vector<std::pair<NSWindow*, NSInteger>> g_raised_windows;

std::vector<NSRect> OwnWindowExclusions(NSWindow* overlay, NSPoint screen_origin) {
    std::vector<NSRect> exclusions;
    for (NSWindow* window in NSApp.windows) {
        if (window == overlay || window == g_border_window || !window.isVisible) continue;
        const NSRect frame = window.frame;
        exclusions.push_back(NSMakeRect(frame.origin.x - screen_origin.x,
                                        frame.origin.y - screen_origin.y, frame.size.width,
                                        frame.size.height));
    }
    return exclusions;
}

NSRect RegionToViewRect(const RegionOfInterest& region, NSSize view_size) {
    // Percent (top-left origin) to view coordinates (bottom-left origin).
    return NSMakeRect(region.left_percent / 100.0 * view_size.width,
                      (100.0 - region.bottom_percent) / 100.0 * view_size.height,
                      (region.right_percent - region.left_percent) / 100.0 * view_size.width,
                      (region.bottom_percent - region.top_percent) / 100.0 * view_size.height);
}

}  // namespace

bool BeginRegionPick(uint32_t display_id, const std::vector<SuggestedRegion>& windows,
                     const std::vector<SuggestedRegion>& faces, RegionPickerMode initial_mode) {
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
    for (const SuggestedRegion& suggestion : windows) {
        view->windows_.emplace_back(RegionToViewRect(suggestion.region, view_size),
                                    suggestion.label);
    }
    for (const SuggestedRegion& suggestion : faces) {
        view->faces_.emplace_back(RegionToViewRect(suggestion.region, view_size), suggestion.label);
    }
    // This application's own visible windows float above the overlay for
    // the duration: they stay undimmed and clickable by ordinary window
    // compositing, corners and all, and follow their own movement with no
    // repainting on our side. The rectangles still feed the banner
    // placement, which avoids sitting beneath them.
    for (NSWindow* window in NSApp.windows) {
        if (window == overlay || window == g_border_window || !window.isVisible) continue;
        g_raised_windows.emplace_back(window, window.level);
        window.level = overlay.level + 1;
    }
    view->exclusions_ = OwnWindowExclusions(overlay, screen.frame.origin);
    if (initial_mode == RegionPickerMode::Draw ||
        (initial_mode == RegionPickerMode::PickWindows && windows.empty())) {
        view.drawMode = YES;
    } else if (initial_mode == RegionPickerMode::PickFaces) {
        view.facesMode = YES;
        view->suggestions_ = view->faces_;
    } else {
        view->suggestions_ = view->windows_;
    }
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

    // The banner dodges this application's own windows; their rectangles
    // refresh on a gentle cadence - nothing visual tracks them anymore,
    // so per-frame repaints would be waste.
    static double last_exclusion_refresh = 0.0;
    const double now = CFAbsoluteTimeGetCurrent();
    if (now - last_exclusion_refresh > 0.2) {
        last_exclusion_refresh = now;
        std::vector<NSRect> exclusions =
            OwnWindowExclusions(g_picker_window, g_picker_window.frame.origin);
        if (exclusions.size() != g_picker_view->exclusions_.size() ||
            !std::equal(exclusions.begin(), exclusions.end(), g_picker_view->exclusions_.begin(),
                        [](const NSRect& a, const NSRect& b) { return NSEqualRects(a, b); })) {
            g_picker_view->exclusions_ = std::move(exclusions);
            g_picker_view.needsDisplay = YES;
        }
    }

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
        for (const auto& [window, level] : g_raised_windows) window.level = level;
        g_raised_windows.clear();
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
    [g_picker_view switchToMode:(mode == RegionPickerMode::Draw        ? 1
                                 : mode == RegionPickerMode::PickFaces ? 2
                                                                       : 0)];
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
