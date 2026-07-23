#import <AppKit/AppKit.h>

#include <cstring>

#include "platform/icons.h"
#include "platform/macos/region_border_view.h"
#include "platform/macos/region_selection_geometry.h"
#include "platform/region_geometry.h"

// The drag-zone bits (ZoneLeft/Right/Top/Bottom/Move/Close) come from the
// shared region geometry; the overlay speaks them directly.

namespace sidescopes {

std::vector<BorderKeyPress> g_borderKeyPresses;
bool g_borderEditing = false;
bool g_borderEditChanged = false;
bool g_borderDismissed = false;
bool g_borderAttachToggled = false;
RegionOfInterest g_borderEditRegion;

}  // namespace sidescopes

@implementation SidescopesBorderPanel

- (BOOL)canBecomeKeyWindow
{
    return YES;
}

@end

@implementation SidescopesBorderView

// The border panel holds the keyboard after a click (key without
// activation); its keys route through the application's own shortcut map.
- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (void)keyDown:(NSEvent*)event
{
    if (event.keyCode == 53) {
        sidescopes::g_borderKeyPresses.push_back({std::string(), false, true});
        return;
    }
    NSString* characters = event.charactersIgnoringModifiers.uppercaseString;
    if (characters.length == 0) {
        [super keyDown:event];
        return;
    }
    const bool shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    sidescopes::g_borderKeyPresses.push_back({std::string(characters.UTF8String), shift, false});
}

// The region rectangle in view coordinates.
- (NSRect)regionRect
{
    NSRect rect = NSInsetRect(self.bounds, sidescopes::WindowPad, sidescopes::WindowPad);
    // The label strip rides above the band; the region keeps its bottom
    // anchor and every zone stays put.
    rect.size.height -= self.labelBand;
    return rect;
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

// The attach toggle lives at the label tab's fixed left end: the same
// spot whatever the label says, clear of every handle.
- (NSPoint)attachButtonCenter
{
    const NSRect region = [self regionRect];
    return NSMakePoint(NSMinX(region) - sidescopes::BorderPad + sidescopes::TabAttachZone / 2,
                       NSMaxY(region) + sidescopes::WindowPad + self.labelBand / 2);
}

// The whole grab band is the hazard tape, muted so it stays calm beside the
// photograph while its own light-dark alternation keeps it visible on any
// content. The interior is never painted and therefore stays click-through.
- (void)drawHazardBand:(NSRect)region
{
    const NSRect band = NSInsetRect(region, -sidescopes::BorderPad, -sidescopes::BorderPad);
    // The stripes stop short of the measured-edge ring: crossing it would read
    // as the band bleeding into the measured region.
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
    // An integer step index keeps the diagonal spacing exact; a CGFloat loop
    // counter would accumulate rounding across a wide view.
    const CGFloat start = NSMinX(bounds) - bounds.size.height;
    for (int step = 0; start + step * 10.0 < NSMaxX(bounds); ++step) {
        const CGFloat x = start + step * 10.0;
        [stripes moveToPoint:NSMakePoint(x, NSMinY(bounds))];
        [stripes lineToPoint:NSMakePoint(x + bounds.size.height, NSMaxY(bounds))];
    }
    [[NSColor colorWithWhite:0.9 alpha:0.45] setStroke];
    [stripes stroke];
    [NSGraphicsContext restoreGraphicsState];
}

// The measured edge is a filled ring, not a stroked line: it spans exactly from
// the region to the stripes with no geometry of its own to disagree. White
// dashes ride over the dark ring, so one tone survives any background - the
// screenshot tools' marching-ants idea, standing still.
- (void)drawMeasuredEdge:(NSRect)region
{
    const NSRect stripeHole = NSInsetRect(region, -sidescopes::EdgeRing, -sidescopes::EdgeRing);
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
    // Neutral greys only, both region kinds: any hue this close to the
    // sampled pixels would skew the eye's read of the photograph. The
    // label above the band is what tells an attached region apart.
    [[NSColor colorWithWhite:0.97 alpha:0.95] setStroke];
    [dashes stroke];
}

// The attached window's name rides a tab above the band: the attached region
// carries its own identification instead of the main window's toolbar doing
// it at a distance. The label strip the window grew makes room for it clear
// of the handles.
- (void)drawAttachedLabel:(NSRect)region
{
    if (self.attachedLabel.length == 0) {
        return;
    }
    // The tab hugs the text but never leaves the window: a title wider than
    // the region truncates at its tail, Preview-style, instead of being
    // clipped sharply at both ends by the window bounds.
    NSMutableParagraphStyle* style = [[NSMutableParagraphStyle alloc] init];
    style.lineBreakMode = NSLineBreakByTruncatingTail;
    NSDictionary* attributes = @{
        NSFontAttributeName : [NSFont systemFontOfSize:10],
        NSForegroundColorAttributeName : [NSColor colorWithWhite:0.97 alpha:0.95],
        NSParagraphStyleAttributeName : style,
    };
    const CGFloat padX = 6.0;
    const CGFloat padY = 2.0;
    const NSSize textSize = [self.attachedLabel sizeWithAttributes:attributes];
    // Flush with the band's outer left edge: the tab and the border read
    // as one left-aligned block.
    const CGFloat tabX = NSMinX([self regionRect]) - sidescopes::BorderPad;
    // The tab holds the attach toggle at its fixed left end, then the
    // text; left-aligned with the region's own corner. The shared layout
    // keeps both platforms' degradation on small regions identical.
    // The trim budget ends at the band's outer right edge, so a truncated
    // tab stays flush with the border block on both sides.
    const sidescopes::TabLayout layout = sidescopes::borderTabLayout(
        NSMaxX(region) + sidescopes::BorderPad - tabX, sidescopes::TabAttachZone, padX, textSize.width, 16.0);
    if (!layout.visible) {
        return;
    }
    const CGFloat textWidth = layout.textWidth;
    const CGFloat tabWidth = layout.tabWidth;
    const CGFloat centreY = NSMaxY(region) + sidescopes::WindowPad + self.labelBand / 2;
    const NSRect tab = NSMakeRect(tabX, centreY - textSize.height / 2 - padY, tabWidth, textSize.height + 2 * padY);
    NSBezierPath* plate = [NSBezierPath bezierPathWithRoundedRect:tab xRadius:4 yRadius:4];
    [[NSColor colorWithWhite:0.1 alpha:0.85] setFill];
    [plate fill];
    [[NSColor colorWithWhite:0.75 alpha:0.6] setStroke];
    plate.lineWidth = 1.0;
    [plate stroke];
    const NSRect text =
        NSMakeRect(tab.origin.x + sidescopes::TabAttachZone + padX, tab.origin.y + padY, textWidth, textSize.height);
    [self.attachedLabel drawInRect:text withAttributes:attributes];
}

// Eight handle dots - corners and edge midpoints - centered on the measurement
// line, the way the macOS screenshot selection wears them: small gray circles
// straddling the selection edge, costing only a few pixels of the sample's rim.
- (void)drawHandles:(NSRect)region
{
    const NSRect lane = region;
    const auto handle = [&](CGFloat x, CGFloat y) {
        const NSRect circle = NSMakeRect(x - sidescopes::HandleRadius, y - sidescopes::HandleRadius,
                                         sidescopes::HandleRadius * 2, sidescopes::HandleRadius * 2);
        NSBezierPath* dot = [NSBezierPath bezierPathWithOvalInRect:circle];
        [[NSColor colorWithWhite:0.78 alpha:1.0] setFill];
        [dot fill];
        // A dark rim beneath the near-white ring keeps the dot visible on a
        // bright sky; the ring matches the measurement line.
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
}

// The hover-revealed close button, in the handles' own visual language: a dark
// disc where the dots are light, so it reads as an action rather than a grip,
// with the same bright ring and an x.
- (void)drawCloseButton
{
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

// The icon set's pushpin at the tab's fixed left end, showing the
// STATE: the pin while the region is attached, the struck-through pin
// while it is global. Rasterized once from the embedded vector sources,
// so every platform draws the identical icons.
- (void)drawAttachButton
{
    static NSImage* icons[2] = {nil, nil};
    const int which = self.attachedRegion ? 1 : 0;
    if (!icons[which]) {
        const int pixels = 22;  // 11 points at the retina scale
        const std::vector<uint8_t> rgba =
            sidescopes::rasterizeIcon(self.attachedRegion ? sidescopes::Icon::Pin : sidescopes::Icon::PinOff, pixels);
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                        pixelsWide:pixels
                                                                        pixelsHigh:pixels
                                                                     bitsPerSample:8
                                                                   samplesPerPixel:4
                                                                          hasAlpha:YES
                                                                          isPlanar:NO
                                                                    colorSpaceName:NSDeviceRGBColorSpace
                                                                      bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
                                                                       bytesPerRow:static_cast<NSInteger>(pixels) * 4
                                                                      bitsPerPixel:32];
        std::memcpy(rep.bitmapData, rgba.data(), rgba.size());
        icons[which] = [[NSImage alloc] initWithSize:NSMakeSize(11, 11)];
        [icons[which] addRepresentation:rep];
    }
    const NSPoint center = [self attachButtonCenter];
    [icons[which] drawInRect:NSMakeRect(center.x - 5.5, center.y - 5.5, 11, 11)
                    fromRect:NSZeroRect
                   operation:NSCompositingOperationSourceOver
                    fraction:1.0];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect region = [self regionRect];
    [self drawHazardBand:region];
    [self drawMeasuredEdge:region];
    [self drawAttachedLabel:region];
    [self drawHandles:region];
    if ([self closeVisible]) {
        [self drawCloseButton];
    }
    [self drawAttachButton];
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
    {
        const NSPoint center = [self attachButtonCenter];
        const CGFloat dx = point.x - center.x;
        const CGFloat dy = point.y - center.y;
        if (dx * dx + dy * dy <= sidescopes::CloseHitRadius * sidescopes::CloseHitRadius) {
            return sidescopes::ZoneAttach;
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
    if (zone & (sidescopes::ZoneClose | sidescopes::ZoneAttach)) {
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
    if (zone & sidescopes::ZoneClose) {
        self.closePressed = YES;
        return;
    }
    if (zone & sidescopes::ZoneAttach) {
        self.attachPressed = YES;
        return;
    }
    self.dragZone = zone;
    if (self.dragZone & sidescopes::ZoneMove) {
        [NSCursor.closedHandCursor set];
    }
    // Key without activation: the click focuses the region, so Escape and
    // the letter shortcuts work right after a border interaction, and the
    // cursor becomes ours to shape - while the editor's window order and
    // menu bar stay untouched.
    [self.window makeKeyWindow];
    self.dragStartMouse = NSEvent.mouseLocation;
    NSRect startRegion = NSInsetRect(self.window.frame, sidescopes::WindowPad, sidescopes::WindowPad);
    // The label strip rides above the band: the window is that much taller
    // than the symmetric padding says, and reading the strip as region grew
    // every attached edit upward by its height.
    startRegion.size.height -= self.labelBand;
    self.dragStartRegion = startRegion;
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
    if (self.attachPressed) {
        self.attachPressed = NO;
        if ([self zoneAtPoint:local] & sidescopes::ZoneAttach) {
            sidescopes::g_borderAttachToggled = true;
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

@implementation SidescopesEditDimView

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSBezierPath* veil = [NSBezierPath bezierPathWithRect:self.bounds];
    [veil appendBezierPathWithRect:self.holeRect];
    veil.windingRule = NSWindingRuleEvenOdd;
    [[NSColor colorWithWhite:0 alpha:0.45] setFill];
    [veil fill];
    [[NSColor colorWithSRGBRed:1.0 green:0.84 blue:0.55 alpha:0.9] setStroke];
    NSBezierPath* rim = [NSBezierPath bezierPathWithRect:self.holeRect];
    rim.lineWidth = 1.5;
    [rim stroke];
}

@end
