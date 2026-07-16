#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>

#include "platform/desktop.h"
#include "platform/face_detection.h"
#include "platform/region_geometry.h"
#include "platform/region_selection.h"

// The drag-zone bits (ZoneLeft/Right/Top/Bottom/Move/Close) come from the
// shared region geometry; the overlay speaks them directly.

namespace sidescopes {
namespace {

// The border window extends this far beyond the region: the grab band,
// wide enough to hit with a cursor, slim enough to stay unobtrusive.
constexpr double BorderPad = 12.0;
// The macOS-screenshot-style handle dots protrude past the band's outer
// edge; the window carries a transparent margin so they are not clipped.
constexpr double HandleRadius = 3.5;
// Thickness of the measured-edge ring between the region and the hazard
// stripes. The stripes' cutoff and the ring share this one constant, so
// they abut exactly at every display scale - two independently tuned
// numbers once left a hairline of bare transparency between them.
constexpr double EdgeRing = 1.0;
constexpr double HandleMargin = HandleRadius + 2.0;
constexpr double WindowPad = BorderPad + HandleMargin;
// Regions cannot shrink beyond this many points per side. The corner and
// edge-midpoint grab-zone sizes live with the shared geometry that reads
// them.
constexpr double MinimumRegionSize = 24.0;
// The hover-revealed close button: a badge on the band's outer corner,
// diagonally off the corner handle, so it visibly belongs to the region
// as a whole. Pulled inward a touch so the disc mostly rides the band;
// tiny regions still yield it to the resize zones.
constexpr double CloseRadius = 6.5;
constexpr double CloseHitRadius = 11.0;
constexpr double CloseCornerInset = 2.0;
constexpr double MinimumWidthForClose = 48.0;

NSScreen* screenForDisplay(uint32_t displayId)
{
    for (NSScreen* screen in NSScreen.screens) {
        NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
        if (number && number.unsignedIntValue == displayId) {
            return screen;
        }
    }
    return NSScreen.mainScreen;
}

// macOS screens and views are bottom-left origin; the shared region
// geometry is top-left. Flip a Y coordinate within a container of the given
// height (whose top edge sits at that height).
double flippedY(double y, double height)
{
    return height - y;
}

// Shared edit state the application polls once per frame.
bool g_borderEditing = false;
bool g_borderEditChanged = false;
bool g_borderDismissed = false;
RegionOfInterest g_borderEditRegion;

// The pin cursor's swatch color, pushed by the application once per
// frame; sampled from the capture stream so the swatch previews exactly
// what a click would pin.
std::optional<FloatColor> g_pinChipColor;

// The sample patch a pin-mode click averages, in points.
constexpr double PinSamplePoints = 14.0;

// The pin cursor: crosshair and preview swatch drawn into the CURSOR
// itself. A swatch painted into the overlay always trails the pointer
// by a composition frame - the cursor image rides its own zero-latency
// plane, so the swatch does too. The crosshair is a two-tone grey, the
// look the system crosshair only has over dimmed content.
constexpr double PinCursorHotspot = 12.0;  // crosshair center, points
constexpr double PinCursorArm = 8.0;
constexpr double PinCursorGap = 2.0;
constexpr double PinSwatchOffset = 7.0;  // from the hotspot, points
constexpr double PinSwatchSize = 13.0;

NSCursor* g_pinCursor = nil;

NSCursor* buildPinCursor(const std::optional<FloatColor>& color)
{
    const CGFloat side = PinCursorHotspot + PinSwatchOffset + PinSwatchSize + 2;
    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(side, side)];
    [image lockFocus];
    // Image coordinates are bottom-left; the hotspot NSCursor takes is
    // top-left. The crosshair centers on (hotspot, hotspot) from the
    // top, the swatch hangs below-right of it.
    const CGFloat centerX = PinCursorHotspot;
    const CGFloat centerY = side - PinCursorHotspot;
    const auto stroke = [&](CGFloat width, NSColor* tone) {
        NSBezierPath* arms = [NSBezierPath bezierPath];
        arms.lineWidth = width;
        [arms moveToPoint:NSMakePoint(centerX, centerY + PinCursorGap)];
        [arms lineToPoint:NSMakePoint(centerX, centerY + PinCursorArm)];
        [arms moveToPoint:NSMakePoint(centerX, centerY - PinCursorGap)];
        [arms lineToPoint:NSMakePoint(centerX, centerY - PinCursorArm)];
        [arms moveToPoint:NSMakePoint(centerX + PinCursorGap, centerY)];
        [arms lineToPoint:NSMakePoint(centerX + PinCursorArm, centerY)];
        [arms moveToPoint:NSMakePoint(centerX - PinCursorGap, centerY)];
        [arms lineToPoint:NSMakePoint(centerX - PinCursorArm, centerY)];
        [tone setStroke];
        [arms stroke];
    };
    stroke(3.2, [NSColor colorWithWhite:0.1 alpha:0.85]);
    stroke(1.5, [NSColor colorWithWhite:0.8 alpha:0.95]);
    if (color) {
        const NSRect swatch = NSMakeRect(centerX + PinSwatchOffset, centerY - PinSwatchOffset - PinSwatchSize,
                                         PinSwatchSize, PinSwatchSize);
        [[NSColor colorWithSRGBRed:color->r / 255.0 green:color->g / 255.0 blue:color->b / 255.0 alpha:1.0] setFill];
        NSRectFillUsingOperation(swatch, NSCompositingOperationCopy);
        NSBezierPath* rim = [NSBezierPath bezierPathWithRect:swatch];
        rim.lineWidth = 2.0;
        [[NSColor colorWithWhite:0.1 alpha:0.7] setStroke];
        [rim stroke];
        NSBezierPath* ring = [NSBezierPath bezierPathWithRect:swatch];
        ring.lineWidth = 1.0;
        [[NSColor colorWithWhite:0.97 alpha:0.95] setStroke];
        [ring stroke];
    }
    [image unlockFocus];
    return [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(PinCursorHotspot, PinCursorHotspot)];
}

}  // namespace
}  // namespace sidescopes

// Borderless windows refuse key status unless overridden; the picker needs
// keyDown for ESC-to-cancel.
@interface SidescopesPickerWindow : NSWindow
@end
@implementation SidescopesPickerWindow

- (BOOL)canBecomeKeyWindow
{
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
// Color pinning: clicks and drags report areas to average, the region
// is never touched, and a cursor chip previews the sample.
@property(nonatomic, assign) BOOL pinMode;
@property(nonatomic, assign) NSRect pinnedArea;
@property(nonatomic, assign) BOOL pinnedKeepOpen;
@property(nonatomic, assign) BOOL pinnedReady;
@property(nonatomic, assign) NSPoint dragStart;
@property(nonatomic, assign) NSPoint dragCurrent;
@property(nonatomic, assign) BOOL dragging;
@property(nonatomic, assign) BOOL picked;
@property(nonatomic, assign) BOOL finished;
@property(nonatomic, assign) NSInteger hoveredSuggestion;
@property(nonatomic, assign) NSRect confirmedRect;
@end

@implementation SidescopesPickerView

// 0 = pick a window, 1 = draw, 2 = pick a face, 3 = pin colors. Face
// mode is offered even when no face was found: the honest answer is the
// empty overlay saying so, not a key that silently does nothing.
- (void)switchToMode:(int)mode
{
    const BOOL draw = mode == 1;
    const BOOL faces = mode == 2;
    const BOOL pin = mode == 3;
    if (self.drawMode == draw && self.facesMode == faces && self.pinMode == pin) {
        return;
    }
    self.drawMode = draw;
    self.facesMode = faces;
    self.pinMode = pin;
    suggestions_ = faces ? faces_ : windows_;
    self.hoveredSuggestion = -1;
    self.dragging = NO;
    [self.window invalidateCursorRectsForView:self];
    self.needsDisplay = YES;
}

// The smallest suggestion under the cursor wins, so a photo canvas beats
// the window that contains it.
- (NSInteger)suggestionAtPoint:(NSPoint)point
{
    NSInteger best = -1;
    CGFloat bestArea = CGFLOAT_MAX;
    for (NSUInteger index = 0; index < suggestions_.size(); ++index) {
        const NSRect rect = suggestions_[index].first;
        if (!NSPointInRect(point, rect)) {
            continue;
        }
        const CGFloat area = rect.size.width * rect.size.height;
        if (area < bestArea) {
            bestArea = area;
            best = static_cast<NSInteger>(index);
        }
    }
    return best;
}

- (NSRect)selectionRect
{
    // Both drag points share the view's coordinate space, so the normalized
    // rectangle reads the same whichever origin convention it is viewed in -
    // no flip is needed here.
    const sidescopes::LocalRect rect =
        sidescopes::selectionRectFromDrag(self.dragStart.x, self.dragStart.y, self.dragCurrent.x, self.dragCurrent.y);
    return NSMakeRect(rect.x, rect.y, rect.width, rect.height);
}

// A punched hole must keep a whisper of alpha when it should stay
// clickable: the window server treats fully transparent window pixels as
// click-through. The picker uses both behaviors deliberately - 5% black
// for its own click targets, true zero alpha over this application's
// windows so clicks reach them.
- (void)punchRect:(NSRect)rect
{
    [[NSColor clearColor] setFill];
    NSRectFillUsingOperation(rect, NSCompositingOperationCopy);
    [[NSColor colorWithWhite:0 alpha:0.05] setFill];
    NSRectFillUsingOperation(rect, NSCompositingOperationSourceOver);
}

// The mode instruction: a primary line and a bracketed-keys line on a
// dark pill, placed where this application's own windows do not cover
// it - they float above the overlay, and a message half-hidden behind
// the scope window reads as a glitch.
- (void)drawBanner:(NSString*)primary secondary:(NSString*)secondary preferCenter:(BOOL)center
{
    NSDictionary* primaryAttributes = @{
        NSForegroundColorAttributeName : [NSColor whiteColor],
        NSFontAttributeName : [NSFont systemFontOfSize:22 weight:NSFontWeightSemibold],
    };
    NSDictionary* secondaryAttributes = @{
        NSForegroundColorAttributeName : [NSColor colorWithWhite:1 alpha:0.75],
        NSFontAttributeName : [NSFont systemFontOfSize:14 weight:NSFontWeightRegular],
    };
    const NSSize primarySize = [primary sizeWithAttributes:primaryAttributes];
    const NSSize secondarySize = [secondary sizeWithAttributes:secondaryAttributes];
    const CGFloat width = std::max(primarySize.width, secondarySize.width) + 48;
    const CGFloat height = primarySize.height + secondarySize.height + 30;
    const CGFloat x = (self.bounds.size.width - width) / 2;
    const CGFloat topY = self.bounds.size.height - height - 80;
    const CGFloat centerY = (self.bounds.size.height - height) / 2;
    const CGFloat lowY = self.bounds.size.height * 0.22;
    const CGFloat candidates[3] = {center ? centerY : topY, center ? topY : centerY, lowY};
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
    [primary drawAtPoint:NSMakePoint(banner.origin.x + (width - primarySize.width) / 2,
                                     NSMaxY(banner) - primarySize.height - 10)
          withAttributes:primaryAttributes];
    [secondary drawAtPoint:NSMakePoint(banner.origin.x + (width - secondarySize.width) / 2, banner.origin.y + 10)
            withAttributes:secondaryAttributes];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    if (self.pinMode) {
        // No dim at all - judging a color through even a light wash
        // misleads. Nothing is painted over the screen; the overlay owns
        // its clicks because pin-mode windows set ignoresMouseEvents
        // explicitly, which switches the window server off its per-pixel
        // transparency hit-testing.
        if (self.dragging) {
            // A two-tone frame: the white line rides a dark halo so one
            // of the tones survives any undimmed background.
            const NSRect selection = [self selectionRect];
            NSBezierPath* halo = [NSBezierPath bezierPathWithRect:selection];
            halo.lineWidth = 3.0;
            [[NSColor colorWithWhite:0.1 alpha:0.7] setStroke];
            [halo stroke];
            NSBezierPath* line = [NSBezierPath bezierPathWithRect:selection];
            line.lineWidth = 1.0;
            [[NSColor whiteColor] setStroke];
            [line stroke];
        }
        if (!self.dragging) {
            // Pinning is its own tool: no mode keys here and none of the
            // region modes lead back - crossing over midway would blur
            // what a click means.
            [self drawBanner:@"Click or drag to pin a color"
                   secondary:@"[Shift+click] pin and continue    [Esc] done"
                preferCenter:NO];
        }
        return;
    }
    if (!self.drawMode) {
        [[NSColor colorWithWhite:0 alpha:0.2] setFill];
        NSRectFillUsingOperation(self.bounds, NSCompositingOperationSourceOver);
        if (self.facesMode) {
            // Faces are few and easy to miss: every found face is outlined
            // up front, so the answer is visible before any hovering. The
            // hovered one still gets the full accent treatment below.
            [[NSColor colorWithWhite:1 alpha:0.85] setStroke];
            for (NSInteger i = 0; i < static_cast<NSInteger>(suggestions_.size()); ++i) {
                if (i == self.hoveredSuggestion) {
                    continue;
                }
                [self punchRect:suggestions_[i].first];
                NSBezierPath* outline = [NSBezierPath bezierPathWithRect:suggestions_[i].first];
                outline.lineWidth = 1.5;
                [outline stroke];
            }
        }
        if (self.hoveredSuggestion >= 0 && self.hoveredSuggestion < static_cast<NSInteger>(suggestions_.size())) {
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
            NSDictionary* labelAttributes = @{
                NSForegroundColorAttributeName : [NSColor whiteColor],
                NSFontAttributeName : [NSFont systemFontOfSize:12 weight:NSFontWeightMedium],
            };
            [label drawAtPoint:NSMakePoint(hovered.first.origin.x + 6, NSMaxY(hovered.first) - 20)
                withAttributes:labelAttributes];
        }
        if (self.facesMode) {
            [self drawBanner:suggestions_.empty() ? @"No faces found on this screen" : @"Click a face"
                   secondary:@"[A] pick a window    [D] draw    [Esc] full screen"
                preferCenter:suggestions_.empty()];
        } else {
            [self drawBanner:@"Click a window"
                   secondary:sidescopes::supportsFaceDetection() ? @"[F] pick a face    [D] draw    [Esc] full screen"
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
            if (!windows_.empty() && sidescopes::supportsFaceDetection()) {
                secondary = @"[A] pick a window    [F] pick a face    [Esc] full screen";
            } else if (!windows_.empty()) {
                secondary = @"[A] pick a window    [Esc] full screen";
            }
            [self drawBanner:@"Drag to select an area" secondary:secondary preferCenter:NO];
        }
    }
}

- (void)resetCursorRects
{
    NSCursor* cursor = NSCursor.pointingHandCursor;
    if (self.pinMode) {
        cursor = sidescopes::g_pinCursor ? sidescopes::g_pinCursor : NSCursor.crosshairCursor;
    } else if (self.drawMode) {
        cursor = NSCursor.crosshairCursor;
    }
    [self addCursorRect:self.bounds cursor:cursor];
}

- (void)mouseMoved:(NSEvent*)event
{
    // In pin mode the swatch rides the cursor image itself; motion needs
    // nothing from the view.
    if (self.pinMode) {
        return;
    }
    if (self.drawMode) {
        return;
    }
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    const NSInteger hovered = [self suggestionAtPoint:point];
    if (hovered != self.hoveredSuggestion) {
        self.hoveredSuggestion = hovered;
        self.needsDisplay = YES;
    }
}

- (void)mouseDown:(NSEvent*)event
{
    // The keyboard follows the click across displays: whichever overlay
    // was clicked last owns ESC and the mode keys.
    if (!self.window.keyWindow) {
        [self.window makeKeyAndOrderFront:nil];
    }
    self.dragStart = [self convertPoint:event.locationInWindow fromView:nil];
    self.dragCurrent = self.dragStart;
    self.needsDisplay = YES;
}

- (void)mouseDragged:(NSEvent*)event
{
    if (self.pinMode) {
        const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
        const NSRect previous = [self selectionRect];
        self.dragCurrent = point;
        if (!self.dragging && (std::abs(self.dragCurrent.x - self.dragStart.x) > 4 ||
                               std::abs(self.dragCurrent.y - self.dragStart.y) > 4)) {
            self.dragging = YES;
            self.needsDisplay = YES;  // full: the banner leaves
            return;
        }
        if (!self.dragging) {
            return;
        }
        NSRect changed = NSUnionRect(previous, [self selectionRect]);
        [self setNeedsDisplayInRect:NSInsetRect(changed, -4, -4)];
        return;
    }
    if (!self.drawMode) {
        return;
    }
    self.dragCurrent = [self convertPoint:event.locationInWindow fromView:nil];
    // A real drag only starts after a few points of travel, so a stray
    // click never flashes a tiny manual selection.
    if (!self.dragging &&
        (std::abs(self.dragCurrent.x - self.dragStart.x) > 4 || std::abs(self.dragCurrent.y - self.dragStart.y) > 4)) {
        self.dragging = YES;
    }
    self.needsDisplay = YES;
}

- (void)mouseUp:(NSEvent*)event
{
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    if (self.pinMode) {
        self.dragCurrent = point;
        const NSRect selection = [self selectionRect];
        if (self.dragging && selection.size.width > 8 && selection.size.height > 8) {
            self.pinnedArea = selection;
        } else {
            // A click pins a cursor-sized patch, not a pixel:
            // photographs are textured, and a point sample is a lottery
            // across the vectorscope cloud.
            self.pinnedArea =
                NSMakeRect(point.x - sidescopes::PinSamplePoints / 2, point.y - sidescopes::PinSamplePoints / 2,
                           sidescopes::PinSamplePoints, sidescopes::PinSamplePoints);
        }
        // The click's Shift carries the per-pin decision: pin and keep
        // picking, or pin and be done.
        self.pinnedKeepOpen = (event.modifierFlags & NSEventModifierFlagShift) != 0;
        self.pinnedReady = YES;
        if (self.dragging) {
            self.dragging = NO;
            self.needsDisplay = YES;  // full: the frame leaves, the banner returns
        }
        return;
    }
    if (!self.drawMode) {
        const NSInteger hovered = [self suggestionAtPoint:point];
        if (hovered < 0) {
            return;  // a miss keeps the picker open
        }
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

- (void)keyDown:(NSEvent*)event
{
    if (event.keyCode == 53) {  // ESC
        self.picked = NO;
        self.finished = YES;
        return;
    }
    // Pinning is its own tool: while it is up, only ESC speaks.
    if (self.pinMode) {
        return;
    }
    NSString* keys = event.charactersIgnoringModifiers;
    if (keys.length == 1) {
        // Modes switch on every display's overlay at once.
        const unichar key = [keys characterAtIndex:0];
        if (key == 'a' || key == 'A') {
            sidescopes::setRegionPickMode(sidescopes::RegionPickerMode::PickWindows);
            return;
        }
        if (key == 'd' || key == 'D') {
            sidescopes::setRegionPickMode(sidescopes::RegionPickerMode::Draw);
            return;
        }
        if (key == 'f' || key == 'F') {
            sidescopes::setRegionPickMode(sidescopes::RegionPickerMode::PickFaces);
            return;
        }
    }
    [super keyDown:event];
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

// Without this, the first click on the overlay is swallowed as a
// window-activation gesture and never reaches the view - the symptom was
// clicks falling through to the app behind while only ESC worked.
- (BOOL)acceptsFirstMouse:(NSEvent*)event
{
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
@property(nonatomic, assign) unsigned dragZone;       // a mask of sidescopes::ZoneBits
@property(nonatomic, assign) NSPoint dragStartMouse;  // global screen coords
@property(nonatomic, assign) NSRect dragStartRegion;  // global screen coords
@property(nonatomic, assign) BOOL closePressed;
@end
@implementation SidescopesBorderView

// The region rectangle in view coordinates.
- (NSRect)regionRect
{
    return NSInsetRect(self.bounds, sidescopes::WindowPad, sidescopes::WindowPad);
}

// Always visible while the border is up - hover-revealing it flickered
// on every band crossing, and crossing the band is what a cursor does
// all day. It still hides during drags and yields on tiny regions.
- (BOOL)closeVisible
{
    const NSRect region = [self regionRect];
    return self.dragZone == sidescopes::ZoneNone && region.size.width >= sidescopes::MinimumWidthForClose;
}

// On the band's outer corner, at forty-five degrees off the top-right
// handle dot - anchored to the corner rather than parked beside it.
- (NSPoint)closeCenter
{
    const NSRect region = [self regionRect];
    return NSMakePoint(NSMaxX(region) + sidescopes::BorderPad - sidescopes::CloseCornerInset,
                       NSMaxY(region) + sidescopes::BorderPad - sidescopes::CloseCornerInset + sidescopes::EdgeRing);
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect region = [self regionRect];

    // The whole grab band is the hazard tape, muted so it stays calm
    // beside the photograph while its own light-dark alternation keeps it
    // visible on any content. The interior is never painted and therefore
    // stays click-through.
    const NSRect band = NSInsetRect(region, -sidescopes::BorderPad, -sidescopes::BorderPad);
    // The stripes stop short of the measured-edge ring: crossing it
    // would read as the band bleeding into the measured area.
    const NSRect stripeHole = NSInsetRect(region, -sidescopes::EdgeRing, -sidescopes::EdgeRing);
    NSBezierPath* ringClip = [NSBezierPath bezierPathWithRect:band];
    [ringClip appendBezierPathWithRect:stripeHole];
    ringClip.windingRule = NSWindingRuleEvenOdd;
    [NSGraphicsContext saveGraphicsState];
    [ringClip addClip];
    [[NSColor colorWithWhite:0.1 alpha:0.45] setFill];
    NSRectFillUsingOperation(band, NSCompositingOperationCopy);
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

    // The measured edge is a filled ring, not a stroked line: it spans
    // exactly from the region to the stripes with no geometry of its own
    // to disagree. White dashes ride over the dark ring, so one of the
    // two tones survives any background - the screenshot tools'
    // marching-ants idea, standing still.
    NSBezierPath* edgeRing = [NSBezierPath bezierPathWithRect:stripeHole];
    [edgeRing appendBezierPathWithRect:region];
    edgeRing.windingRule = NSWindingRuleEvenOdd;
    [[NSColor colorWithWhite:0.1 alpha:0.85] setFill];
    [edgeRing fill];
    NSBezierPath* dashes =
        [NSBezierPath bezierPathWithRect:NSInsetRect(region, -sidescopes::EdgeRing / 2, -sidescopes::EdgeRing / 2)];
    dashes.lineWidth = sidescopes::EdgeRing;
    const CGFloat dash[2] = {4.0, 4.0};
    [dashes setLineDash:dash count:2 phase:0];
    [[NSColor colorWithWhite:0.97 alpha:0.95] setStroke];
    [dashes stroke];

    // Eight handle dots - corners and edge midpoints - centered on the
    // measurement line, the way the macOS screenshot selection wears
    // them: small gray circles straddling the selection edge. Round and
    // muted, they cost only a few pixels of the sample's rim.
    const NSRect lane = region;
    const auto handle = [&](CGFloat x, CGFloat y) {
        const NSRect circle = NSMakeRect(x - sidescopes::HandleRadius, y - sidescopes::HandleRadius,
                                         sidescopes::HandleRadius * 2, sidescopes::HandleRadius * 2);
        NSBezierPath* dot = [NSBezierPath bezierPathWithOvalInRect:circle];
        [[NSColor colorWithWhite:0.78 alpha:1.0] setFill];
        [dot fill];
        // A dark rim beneath the near-white ring keeps the dot visible on
        // a bright sky; the ring matches the measurement line, so the
        // dots and the line read as one instrument.
        [[NSColor colorWithWhite:0.1 alpha:0.7] setStroke];
        dot.lineWidth = 2.0;
        [dot stroke];
        [[NSColor colorWithWhite:0.97 alpha:0.95] setStroke];
        dot.lineWidth = 1.0;
        [dot stroke];
    };
    handle(NSMinX(lane), NSMinY(lane));
    handle(NSMidX(lane), NSMinY(lane));
    handle(NSMaxX(lane), NSMinY(lane));
    handle(NSMinX(lane), NSMidY(lane));
    handle(NSMaxX(lane), NSMidY(lane));
    handle(NSMinX(lane), NSMaxY(lane));
    handle(NSMidX(lane), NSMaxY(lane));
    handle(NSMaxX(lane), NSMaxY(lane));

    // The hover-revealed close button, in the handles' own visual
    // language: a dark disc where the dots are light, so it reads as an
    // action rather than a grip, with the same bright ring and an x.
    if ([self closeVisible]) {
        const NSPoint center = [self closeCenter];
        const NSRect disc = NSMakeRect(center.x - sidescopes::CloseRadius, center.y - sidescopes::CloseRadius,
                                       sidescopes::CloseRadius * 2, sidescopes::CloseRadius * 2);
        NSBezierPath* button = [NSBezierPath bezierPathWithOvalInRect:disc];
        [[NSColor colorWithWhite:0.1 alpha:0.85] setFill];
        [button fill];
        [[NSColor colorWithWhite:0.97 alpha:0.95] setStroke];
        button.lineWidth = 1.0;
        [button stroke];
        const CGFloat arm = sidescopes::CloseRadius - 3.7;
        NSBezierPath* cross = [NSBezierPath bezierPath];
        cross.lineWidth = 1.3;
        cross.lineCapStyle = NSLineCapStyleRound;
        [cross moveToPoint:NSMakePoint(center.x - arm, center.y - arm)];
        [cross lineToPoint:NSMakePoint(center.x + arm, center.y + arm)];
        [cross moveToPoint:NSMakePoint(center.x - arm, center.y + arm)];
        [cross lineToPoint:NSMakePoint(center.x + arm, center.y - arm)];
        [cross stroke];
    }
}

// Eight handles, no modifier: the corners resize both axes, the edge
// midpoints resize their edge, and the rest of the band moves. The
// visible handles say which is which - a modifier key never could.
- (unsigned)zoneAtPoint:(NSPoint)point
{
    const NSRect region = [self regionRect];
    if (NSPointInRect(point, region)) {
        return sidescopes::ZoneNone;  // click-through anyway
    }
    if ([self closeVisible]) {
        const NSPoint center = [self closeCenter];
        const CGFloat dx = point.x - center.x;
        const CGFloat dy = point.y - center.y;
        if (dx * dx + dy * dy <= sidescopes::CloseHitRadius * sidescopes::CloseHitRadius) {
            return sidescopes::ZoneClose;
        }
    }
    // View space is bottom-left origin; the shared zone tests are top-left.
    // Flip the region and the probe point into that space together - the
    // resulting zones (ZoneTop is the visually-top edge, and so on) carry
    // straight back.
    const CGFloat height = self.bounds.size.height;
    const sidescopes::LocalRect local{NSMinX(region), sidescopes::flippedY(NSMaxY(region), height), region.size.width,
                                      region.size.height};
    const double x = point.x;
    const double y = sidescopes::flippedY(point.y, height);
    const unsigned corner = sidescopes::cornerZoneAt(local, x, y, 1.0);
    if (corner != sidescopes::ZoneNone) {
        return corner;
    }
    return sidescopes::edgeOrMoveZoneAt(local, x, y, 1.0);
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    for (NSTrackingArea* area in self.trackingAreas) {
        [self removeTrackingArea:area];
    }
    NSTrackingArea* tracking =
        [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                     options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                             NSTrackingActiveAlways | NSTrackingInVisibleRect
                                       owner:self
                                    userInfo:nil];
    [self addTrackingArea:tracking];
}

- (void)applyCursorForZone:(unsigned)zone
{
    if (zone == sidescopes::ZoneNone) {
        [NSCursor.arrowCursor set];
        return;
    }
    if (zone & sidescopes::ZoneClose) {
        [NSCursor.pointingHandCursor set];
        return;
    }
    if (zone & sidescopes::ZoneMove) {
        [NSCursor.openHandCursor set];
        return;
    }
    const BOOL horizontal = (zone & (sidescopes::ZoneLeft | sidescopes::ZoneRight)) != 0;
    const BOOL vertical = (zone & (sidescopes::ZoneTop | sidescopes::ZoneBottom)) != 0;
    if (horizontal && vertical) {
        if (@available(macOS 15.0, *)) {
            const BOOL rising = ((zone & sidescopes::ZoneLeft) != 0) == ((zone & sidescopes::ZoneBottom) != 0);
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

- (void)mouseMoved:(NSEvent*)event
{
    if (self.dragZone != sidescopes::ZoneNone) {
        return;  // drag owns the cursor
    }
    const NSPoint local = [self convertPoint:event.locationInWindow fromView:nil];
    [self applyCursorForZone:[self zoneAtPoint:local]];
}

- (void)mouseExited:(NSEvent*)event
{
    (void)event;
    if (self.dragZone == sidescopes::ZoneNone) {
        [NSCursor.arrowCursor set];
    }
}

- (void)mouseDown:(NSEvent*)event
{
    const NSPoint local = [self convertPoint:event.locationInWindow fromView:nil];
    const unsigned zone = [self zoneAtPoint:local];
    if (zone == sidescopes::ZoneNone) {
        return;
    }
    // The band is mouse-only: its borderless window can never become
    // key, so a click here would otherwise strand the keyboard with no
    // key window at all - every application shortcut dead until the
    // scope window is clicked. Hand the keyboard to the application
    // window instead.
    for (NSWindow* candidate in NSApp.orderedWindows) {
        if (candidate == self.window) {
            continue;
        }
        if (candidate.canBecomeKeyWindow && candidate.visible) {
            [candidate makeKeyWindow];
            break;
        }
    }
    // Double-clicking anywhere on the band dismisses the region - the
    // fast path once the close button has taught the gesture's home.
    if (event.clickCount == 2) {
        self.closePressed = NO;
        sidescopes::g_borderDismissed = true;
        return;
    }
    if (zone & sidescopes::ZoneClose) {
        self.closePressed = YES;
        return;
    }
    self.dragZone = zone;
    if (self.dragZone & sidescopes::ZoneMove) {
        [NSCursor.closedHandCursor set];
    }
    self.dragStartMouse = NSEvent.mouseLocation;
    self.dragStartRegion = NSInsetRect(self.window.frame, sidescopes::WindowPad, sidescopes::WindowPad);
    sidescopes::g_borderEditing = true;
    self.needsDisplay = YES;  // the close button hides while dragging
}

- (void)mouseDragged:(NSEvent*)event
{
    (void)event;
    if (self.dragZone == sidescopes::ZoneNone) {
        return;
    }
    if (self.dragZone & sidescopes::ZoneMove) {
        [NSCursor.closedHandCursor set];
    }
    const NSPoint mouse = NSEvent.mouseLocation;
    const CGFloat dx = mouse.x - self.dragStartMouse.x;
    const CGFloat dy = mouse.y - self.dragStartMouse.y;
    NSScreen* screen = self.window.screen ? self.window.screen : NSScreen.mainScreen;
    const NSRect screenFrame = screen.frame;
    // Screen space is bottom-left origin; the shared clamp is top-left. Flip
    // the drag-start region's top edge into that space and negate the
    // vertical delta so ZoneTop/ZoneBottom move the visually-correct edges,
    // then read the clamped rectangle straight into a region.
    const sidescopes::LocalRect start{
        NSMinX(self.dragStartRegion) - screenFrame.origin.x,
        sidescopes::flippedY(NSMaxY(self.dragStartRegion) - screenFrame.origin.y, screenFrame.size.height),
        self.dragStartRegion.size.width, self.dragStartRegion.size.height};
    const sidescopes::LocalRect dragged =
        sidescopes::draggedRegionRect(self.dragZone, start, dx, -dy, sidescopes::MinimumRegionSize);
    sidescopes::g_borderEditRegion =
        sidescopes::regionFromLocalRect(dragged, screenFrame.size.width, screenFrame.size.height);
    sidescopes::g_borderEditChanged = true;
}

- (void)mouseUp:(NSEvent*)event
{
    const NSPoint local = [self convertPoint:event.locationInWindow fromView:nil];
    if (self.closePressed) {
        self.closePressed = NO;
        if ([self zoneAtPoint:local] & sidescopes::ZoneClose) {
            sidescopes::g_borderDismissed = true;
            return;
        }
    }
    if (self.dragZone & sidescopes::ZoneMove) {
        [NSCursor.openHandCursor set];
    }
    self.dragZone = sidescopes::ZoneNone;
    sidescopes::g_borderEditing = false;
    [self applyCursorForZone:[self zoneAtPoint:local]];
    self.needsDisplay = YES;
}

// The scoped editor is usually the active application; without this, the
// first grab of the border is swallowed as an activation gesture.
- (BOOL)acceptsFirstMouse:(NSEvent*)event
{
    (void)event;
    return YES;
}

@end

namespace sidescopes {
namespace {

NSWindow* g_borderWindow = nil;

// One overlay per display; a pick anywhere is a pick there.
struct PickerOverlay
{
    SidescopesPickerWindow* window = nil;
    SidescopesPickerView* view = nil;
    uint32_t displayId = 0;
    NSSize size = {0, 0};
};

std::vector<PickerOverlay> g_pickerOverlays;

// This application's own windows are raised above the overlays for the
// pick, with their previous levels remembered for the teardown: real
// window compositing keeps their rounded corners and click handling,
// where punching rectangular holes in the dimming could not.
std::vector<std::pair<NSWindow*, NSInteger>> g_raisedWindows;

bool isPickerOverlayWindow(NSWindow* window)
{
    for (const PickerOverlay& overlay : g_pickerOverlays) {
        if (overlay.window == window) {
            return true;
        }
    }
    return false;
}

std::vector<NSRect> ownWindowExclusions(NSPoint screenOrigin)
{
    std::vector<NSRect> exclusions;
    for (NSWindow* window in NSApp.windows) {
        if (isPickerOverlayWindow(window) || window == g_borderWindow || !window.isVisible) {
            continue;
        }
        const NSRect frame = window.frame;
        exclusions.push_back(NSMakeRect(frame.origin.x - screenOrigin.x, frame.origin.y - screenOrigin.y,
                                        frame.size.width, frame.size.height));
    }
    return exclusions;
}

NSRect regionToViewRect(const RegionOfInterest& region, NSSize viewSize)
{
    // Percent (top-left origin) to view coordinates (bottom-left origin): the
    // shared geometry gives a top-left rect; flip its bottom edge up.
    const LocalRect local = localRectFromRegion(region, viewSize.width, viewSize.height);
    return NSMakeRect(local.x, flippedY(local.y + local.height, viewSize.height), local.width, local.height);
}

}  // namespace

bool beginRegionPick(const std::vector<PickerDisplay>& displays, RegionPickerMode initialMode)
{
    if (!g_pickerOverlays.empty()) {
        return false;  // one picker at a time
    }

    // Window suggestions may be empty everywhere; a window pick with
    // nothing to pick opens as drawing, like before, but the decision is
    // global so every display shows the same mode.
    bool anyWindows = false;
    for (const PickerDisplay& entry : displays) {
        anyWindows |= !entry.windows.empty();
    }
    const bool pin = initialMode == RegionPickerMode::PinColor;
    const bool draw = !pin && (initialMode == RegionPickerMode::Draw ||
                               (initialMode == RegionPickerMode::PickWindows && !anyWindows));
    const bool faces = initialMode == RegionPickerMode::PickFaces;

    for (const PickerDisplay& entry : displays) {
        NSScreen* screen = nil;
        for (NSScreen* candidate in NSScreen.screens) {
            NSNumber* number = candidate.deviceDescription[@"NSScreenNumber"];
            if (number && number.unsignedIntValue == entry.displayId) {
                screen = candidate;
            }
        }
        if (!screen) {
            continue;  // gone between enumeration and now
        }

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

        SidescopesPickerView* view = [[SidescopesPickerView alloc] initWithFrame:overlay.contentView.bounds];
        view.hoveredSuggestion = -1;
        const NSSize viewSize = screen.frame.size;
        for (const SuggestedRegion& suggestion : entry.windows) {
            view->windows_.emplace_back(regionToViewRect(suggestion.region, viewSize), suggestion.label);
        }
        for (const SuggestedRegion& suggestion : entry.faces) {
            view->faces_.emplace_back(regionToViewRect(suggestion.region, viewSize), suggestion.label);
        }
        view.drawMode = draw ? YES : NO;
        view.facesMode = faces ? YES : NO;
        view.pinMode = pin ? YES : NO;
        if (!draw && !pin) {
            view->suggestions_ = faces ? view->faces_ : view->windows_;
        }
        overlay.contentView = view;
        overlay.acceptsMouseMovedEvents = YES;
        // Pin mode paints nothing over the screen, and an all-transparent
        // window would be click-through: the explicit assignment - even
        // to NO - switches the window server off its per-pixel
        // transparency hit-testing, so the overlay owns every click. The
        // region border must never do this; the picker wants exactly it.
        if (pin) {
            overlay.ignoresMouseEvents = NO;
        }

        g_pickerOverlays.push_back(PickerOverlay{overlay, view, entry.displayId, viewSize});
    }
    if (g_pickerOverlays.empty()) {
        return false;
    }

    // This application's own visible windows float above the overlays for
    // the duration: they stay undimmed and clickable by ordinary window
    // compositing, corners and all, and follow their own movement with no
    // repainting on our side. The rectangles still feed the banner
    // placement, which avoids sitting beneath them.
    for (NSWindow* window in NSApp.windows) {
        if (isPickerOverlayWindow(window) || window == g_borderWindow || !window.isVisible) {
            continue;
        }
        g_raisedWindows.emplace_back(window, window.level);
        window.level = NSStatusWindowLevel + 2;
    }
    for (PickerOverlay& overlay : g_pickerOverlays) {
        overlay.view->exclusions_ = ownWindowExclusions(overlay.window.frame.origin);
    }

    // Force the app frontmost so the overlays own the mouse for the whole
    // interaction; the keyboard starts on the display under the cursor -
    // that is where the user's attention is - and follows clicks after.
    [NSApp activateIgnoringOtherApps:YES];
    const uint32_t cursorDisplay = displayUnderCursor().value_or(0);
    PickerOverlay* keyOverlay = &g_pickerOverlays.front();
    for (PickerOverlay& overlay : g_pickerOverlays) {
        if (overlay.displayId == cursorDisplay) {
            keyOverlay = &overlay;
        }
    }
    for (PickerOverlay& overlay : g_pickerOverlays) {
        if (&overlay == keyOverlay) {
            continue;
        }
        [overlay.window orderFrontRegardless];
    }
    [keyOverlay->window makeKeyAndOrderFront:nil];
    [keyOverlay->window makeFirstResponder:keyOverlay->view];
    return true;
}

RegionPickPoll pollRegionPick()
{
    RegionPickPoll poll;
    if (g_pickerOverlays.empty()) {
        return poll;
    }
    poll.active = true;
    // Mode flags come first: the finishing poll returns early below, and
    // the caller needs them to know a pin-mode finish never means a
    // region change. The overlays switch modes in lockstep; the front
    // one speaks for all.
    poll.pinMode = g_pickerOverlays.front().view.pinMode;

    // The banner dodges this application's own windows; their rectangles
    // refresh on a gentle cadence - nothing visual tracks them anymore,
    // so per-frame repaints would be waste.
    static double lastExclusionRefresh = 0.0;
    const double now = CFAbsoluteTimeGetCurrent();
    if (now - lastExclusionRefresh > 0.2) {
        lastExclusionRefresh = now;
        for (PickerOverlay& overlay : g_pickerOverlays) {
            std::vector<NSRect> exclusions = ownWindowExclusions(overlay.window.frame.origin);
            if (exclusions.size() != overlay.view->exclusions_.size() ||
                !std::equal(exclusions.begin(), exclusions.end(), overlay.view->exclusions_.begin(),
                            [](const NSRect& a, const NSRect& b) { return NSEqualRects(a, b); })) {
                overlay.view->exclusions_ = std::move(exclusions);
                overlay.view.needsDisplay = YES;
            }
        }
    }

    const auto regionFromView = [](NSRect rect, NSSize size) {
        // View coordinates are bottom-left origin; flip the rect's top edge
        // into the shared geometry's top-left space, then delegate.
        const LocalRect local{rect.origin.x, flippedY(NSMaxY(rect), size.height), rect.size.width, rect.size.height};
        return regionFromLocalRect(local, size.width, size.height);
    };

    // Any overlay finishing - a confirm there, or ESC anywhere - ends the
    // pick on every display.
    for (PickerOverlay& overlay : g_pickerOverlays) {
        if (!overlay.view.finished) {
            continue;
        }
        poll.finished = true;
        poll.displayId = overlay.displayId;
        if (overlay.view.picked) {
            poll.confirmed = regionFromView(overlay.view.confirmedRect, overlay.size);
        }
        for (const auto& [window, level] : g_raisedWindows) {
            window.level = level;
        }
        g_raisedWindows.clear();
        for (PickerOverlay& each : g_pickerOverlays) {
            [each.window orderOut:nil];
        }
        g_pickerOverlays.clear();
        g_pinCursor = nil;
        return poll;
    }

    for (PickerOverlay& overlay : g_pickerOverlays) {
        if (!overlay.view.pinnedReady) {
            continue;
        }
        overlay.view.pinnedReady = NO;
        poll.pinnedArea = regionFromView(overlay.view.pinnedArea, overlay.size);
        poll.pinnedKeepOpen = overlay.view.pinnedKeepOpen;
        poll.displayId = overlay.displayId;
        break;
    }

    // The cursor is only ever on one display, so at most one overlay has
    // something to preview.
    for (PickerOverlay& overlay : g_pickerOverlays) {
        if (overlay.view.pinMode) {
            break;  // pin modes never preview a region
        }
        if (!overlay.view.drawMode) {
            const NSInteger hovered = overlay.view.hoveredSuggestion;
            if (hovered >= 0 && hovered < static_cast<NSInteger>(overlay.view->suggestions_.size())) {
                poll.preview = regionFromView(overlay.view->suggestions_[hovered].first, overlay.size);
                poll.displayId = overlay.displayId;
                break;
            }
        } else if (overlay.view.dragging) {
            const NSRect selection = [overlay.view selectionRect];
            if (selection.size.width > 8 && selection.size.height > 8) {
                poll.preview = regionFromView(selection, overlay.size);
                poll.displayId = overlay.displayId;
                break;
            }
        }
    }
    return poll;
}

void cancelRegionPick()
{
    if (g_pickerOverlays.empty()) {
        return;
    }
    g_pickerOverlays.front().view.picked = NO;
    g_pickerOverlays.front().view.finished = YES;
}

void setRegionPickMode(RegionPickerMode mode)
{
    // Region picking and color pinning are separate tools; a pick never
    // crosses between the families midway.
    const bool pin = mode == RegionPickerMode::PinColor;
    if (!g_pickerOverlays.empty() && (g_pickerOverlays.front().view.pinMode ? YES : NO) != (pin ? YES : NO)) {
        return;
    }
    for (PickerOverlay& overlay : g_pickerOverlays) {
        [overlay.view switchToMode:(mode == RegionPickerMode::Draw        ? 1
                                    : mode == RegionPickerMode::PickFaces ? 2
                                    : mode == RegionPickerMode::PinColor  ? 3
                                                                          : 0)];
    }
}

void setRegionPickChipColor(const std::optional<FloatColor>& color)
{
    const bool colorChanged =
        color.has_value() != g_pinChipColor.has_value() || (color && g_pinChipColor &&
                                                            (std::lround(color->r) != std::lround(g_pinChipColor->r) ||
                                                             std::lround(color->g) != std::lround(g_pinChipColor->g) ||
                                                             std::lround(color->b) != std::lround(g_pinChipColor->b)));
    g_pinChipColor = color;
    if (g_pickerOverlays.empty() || !g_pickerOverlays.front().view.pinMode) {
        return;
    }
    if (g_pinCursor && !colorChanged) {
        return;
    }
    g_pinCursor = buildPinCursor(color);
    // Re-arming the cursor rects makes AppKit apply the fresh cursor
    // wherever the pointer already stands.
    for (PickerOverlay& overlay : g_pickerOverlays) {
        if (!overlay.view.pinMode) {
            continue;
        }
        [overlay.window invalidateCursorRectsForView:overlay.view];
    }
}

void showRegionBorder(uint32_t displayId, const RegionOfInterest& region)
{
    NSScreen* screen = screenForDisplay(displayId);
    const NSRect frame = screen.frame;

    const double left = frame.origin.x + region.leftPercent / 100.0 * frame.size.width;
    const double right = frame.origin.x + region.rightPercent / 100.0 * frame.size.width;
    const double bottom = frame.origin.y + (100.0 - region.bottomPercent) / 100.0 * frame.size.height;
    const double top = frame.origin.y + (100.0 - region.topPercent) / 100.0 * frame.size.height;
    const NSRect rect = NSMakeRect(left - WindowPad, bottom - WindowPad, (right - left) + 2 * WindowPad,
                                   (top - bottom) + 2 * WindowPad);

    if (!g_borderWindow) {
        g_borderWindow = [[NSWindow alloc] initWithContentRect:rect
                                                     styleMask:NSWindowStyleMaskBorderless
                                                       backing:NSBackingStoreBuffered
                                                         defer:NO];
        g_borderWindow.backgroundColor = NSColor.clearColor;
        g_borderWindow.opaque = NO;
        g_borderWindow.hasShadow = NO;
        // The interior is truly transparent and therefore click-through;
        // only the grab ring and tab take the mouse. ignoresMouseEvents is
        // deliberately never set: an explicit assignment - even to NO -
        // switches AppKit off its per-pixel alpha hit-testing, and the
        // whole window starts swallowing clicks.
        // One level below the scope window: both float above Quick Look,
        // but the border must never cover the scopes. Sharing a level would
        // leave their order to whoever ordered front last.
        g_borderWindow.level = NSStatusWindowLevel - 1;
        g_borderWindow.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                            NSWindowCollectionBehaviorFullScreenAuxiliary |
                                            NSWindowCollectionBehaviorStationary;
        g_borderWindow.contentView = [[SidescopesBorderView alloc] initWithFrame:NSZeroRect];
    }
    [g_borderWindow setFrame:rect display:YES];
    [g_borderWindow invalidateCursorRectsForView:g_borderWindow.contentView];
    [g_borderWindow orderFrontRegardless];
}

void hideRegionBorder()
{
    [g_borderWindow orderOut:nil];
}

RegionBorderEdit pollRegionBorderEdit()
{
    RegionBorderEdit edit;
    edit.editing = g_borderEditing;
    edit.dismissed = g_borderDismissed;
    g_borderDismissed = false;
    if (g_borderEditChanged) {
        edit.region = g_borderEditRegion;
        g_borderEditChanged = false;
    }
    return edit;
}

}  // namespace sidescopes
