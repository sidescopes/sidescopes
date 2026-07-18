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
// Extra window height above the band when the attached label is worn, so
// the name tab clears the handles instead of crowding the top-center one.
constexpr double LabelBand = 20.0;
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
std::vector<BorderKeyPress> g_borderKeyPresses;
bool g_borderEditing = false;
bool g_borderEditChanged = false;
bool g_borderDismissed = false;
RegionOfInterest g_borderEditRegion;

// The pin cursor's swatch color, pushed by the application once per
// frame; sampled from the capture stream so the swatch previews exactly
// what a click would pin.
std::optional<FloatColor> g_pinChipColor;

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
    std::vector<std::pair<NSRect, std::string>> m_suggestions;
    std::vector<std::pair<NSRect, std::string>> m_windows;
    std::vector<std::pair<NSRect, std::string>> m_faces;
    // This application's own windows, in view coordinates: undimmed and
    // truly transparent, so clicks fall through to them.
    std::vector<NSRect> m_exclusions;
}
// NO = suggestion picking (windows or faces), YES = drag to draw.
@property(nonatomic, assign) BOOL drawMode;
// The attached-draw constraint: an empty rect means unconstrained. The drag
// cannot leave the rect and everything outside it dims hard; the label
// names the target window's application on the banner.
@property(nonatomic, assign) NSRect constraintRect;
@property(nonatomic, copy) NSString* constraintLabel;
// In picking mode: whether the face list is active instead of windows.
@property(nonatomic, assign) BOOL facesMode;
// Color pinning: a click reports a point to sample, a drag an area to
// average, the region is never touched, and a cursor chip previews the
// sample. pinnedIsPoint says which of the two the pending pin is.
@property(nonatomic, assign) BOOL pinMode;
@property(nonatomic, assign) NSPoint pinnedPoint;
@property(nonatomic, assign) NSRect pinnedArea;
@property(nonatomic, assign) BOOL pinnedIsPoint;
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

// Face mode is offered even when no face was found: the honest answer is
// the empty overlay saying so, not a key that silently does nothing.
- (void)switchToMode:(sidescopes::RegionPickerMode)mode
{
    const BOOL draw = mode == sidescopes::RegionPickerMode::Draw;
    const BOOL faces = mode == sidescopes::RegionPickerMode::PickFaces;
    const BOOL pin = mode == sidescopes::RegionPickerMode::PinColor;
    if (self.drawMode == draw && self.facesMode == faces && self.pinMode == pin) {
        return;
    }
    self.drawMode = draw;
    self.facesMode = faces;
    self.pinMode = pin;
    m_suggestions = faces ? m_faces : m_windows;
    self.hoveredSuggestion = -1;
    self.dragging = NO;
    [self.window invalidateCursorRectsForView:self];
    self.needsDisplay = YES;
}

// The suggestion under the cursor. Windows are front-to-back, so the first
// one containing the point is the topmost there: the window that actually
// shows at that point wins over any window peeking out from behind it. Faces
// carry no depth, so the smallest box under the cursor wins for them instead.
- (NSInteger)suggestionAtPoint:(NSPoint)point
{
    NSInteger best = -1;
    CGFloat bestArea = CGFLOAT_MAX;
    for (NSUInteger index = 0; index < m_suggestions.size(); ++index) {
        const NSRect rect = m_suggestions[index].first;
        if (!NSPointInRect(point, rect)) {
            continue;
        }
        if (!self.facesMode) {
            return static_cast<NSInteger>(index);
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
    NSRect selection = NSMakeRect(rect.x, rect.y, rect.width, rect.height);
    if (!NSIsEmptyRect(self.constraintRect)) {
        // The attached draw cannot leave its window.
        selection = NSIntersectionRect(selection, self.constraintRect);
    }
    return selection;
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
        for (const NSRect& exclusion : m_exclusions) {
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

// No dim at all - judging a color through even a light wash misleads. Nothing
// is painted over the screen; pin-mode windows set ignoresMouseEvents
// explicitly, so the overlay still owns its clicks.
- (void)drawPinModeOverlay
{
    if (self.dragging) {
        // A two-tone frame: the white line rides a dark halo so one of the
        // tones survives any undimmed background.
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
        // Pinning is its own tool: no mode keys here and none of the region
        // modes lead back - crossing over midway would blur what a click means.
        [self drawBanner:@"Click or drag to pin a color"
               secondary:@"[Shift+click] pin and continue    [Esc] done"
            preferCenter:NO];
    }
}

// Window or face picking: the screen is dimmed and the candidate under the
// cursor washed with the system accent, the way the macOS screenshot interface
// highlights a window.
- (void)drawPickModeOverlay
{
    [[NSColor colorWithWhite:0 alpha:0.2] setFill];
    NSRectFillUsingOperation(self.bounds, NSCompositingOperationSourceOver);
    if (self.facesMode) {
        // Faces are few and easy to miss: every found face is outlined up
        // front, so the answer is visible before any hovering. The hovered one
        // still gets the full accent treatment below.
        [[NSColor colorWithWhite:1 alpha:0.85] setStroke];
        for (NSInteger i = 0; i < static_cast<NSInteger>(m_suggestions.size()); ++i) {
            if (i == self.hoveredSuggestion) {
                continue;
            }
            [self punchRect:m_suggestions[i].first];
            NSBezierPath* outline = [NSBezierPath bezierPathWithRect:m_suggestions[i].first];
            outline.lineWidth = 1.5;
            [outline stroke];
        }
    }
    if (self.hoveredSuggestion >= 0 && self.hoveredSuggestion < static_cast<NSInteger>(m_suggestions.size())) {
        const auto& hovered = m_suggestions[self.hoveredSuggestion];
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
        [self drawBanner:m_suggestions.empty() ? @"No faces found on this screen" : @"Click a face"
               secondary:@"[A] pick a window    [D] draw    [Esc] full screen"
            preferCenter:m_suggestions.empty()];
    } else {
        [self drawBanner:@"Click a window"
               secondary:sidescopes::supportsFaceDetection() ? @"[F] pick a face    [D] draw    [Esc] full screen"
                                                             : @"[D] draw    [Esc] full screen"
            preferCenter:NO];
    }
}

// Freeform drawing: a heavier dim, and the dragged rectangle punched clear.
// A constraint (the attached draw) spotlights the target window instead: a
// hard dim everywhere else, the window itself under the usual light veil,
// rimmed in the attached regions' warm tone.
- (void)drawDrawModeOverlay
{
    const BOOL constrained = !NSIsEmptyRect(self.constraintRect);
    [[NSColor colorWithWhite:0 alpha:constrained ? 0.55 : 0.35] setFill];
    NSRectFillUsingOperation(self.bounds, NSCompositingOperationSourceOver);
    if (constrained) {
        [self punchRect:self.constraintRect];
        [[NSColor colorWithSRGBRed:1.0 green:0.84 blue:0.55 alpha:0.9] setStroke];
        NSBezierPath* spotlight = [NSBezierPath bezierPathWithRect:self.constraintRect];
        spotlight.lineWidth = 1.5;
        [spotlight stroke];
    }
    if (self.dragging) {
        const NSRect selection = [self selectionRect];
        [self punchRect:selection];
        [[NSColor whiteColor] setStroke];
        NSBezierPath* border = [NSBezierPath bezierPathWithRect:selection];
        border.lineWidth = 1.5;
        [border stroke];
    }
    if (!self.dragging) {
        [self drawDrawModeBanner];
    }
}

// The draw overlay's instruction, with the attached draw naming its window.
- (void)drawDrawModeBanner
{
    if (!NSIsEmptyRect(self.constraintRect)) {
        NSString* primary = self.constraintLabel.length > 0
                                ? [NSString stringWithFormat:@"Draw a region in %@", self.constraintLabel]
                                : @"Draw a region in the window";
        [self drawBanner:primary secondary:@"[Esc] cancel" preferCenter:NO];

        return;
    }
    NSString* secondary = @"[Esc] full screen";
    if (!m_windows.empty() && sidescopes::supportsFaceDetection()) {
        secondary = @"[A] pick a window    [F] pick a face    [Esc] full screen";
    } else if (!m_windows.empty()) {
        secondary = @"[A] pick a window    [Esc] full screen";
    }
    [self drawBanner:@"Drag to select an area" secondary:secondary preferCenter:NO];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    if (self.pinMode) {
        [self drawPinModeOverlay];
    } else if (!self.drawMode) {
        [self drawPickModeOverlay];
    } else {
        [self drawDrawModeOverlay];
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
            self.pinnedIsPoint = NO;
        } else {
            // A plain click pins the point itself, so the pinned color is
            // exactly what the live cursor readout showed; averaging a
            // patch here would fade pins taken over a small subject.
            // Dragging an area is the explicit way to average a swatch.
            self.pinnedPoint = point;
            self.pinnedIsPoint = YES;
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
        self.confirmedRect = m_suggestions[hovered].first;
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
// Borderless panels refuse key status unless overridden; the border takes
// the keyboard on click - without activating the application - so Escape
// and the letter shortcuts work right after a border interaction.
@interface SidescopesBorderPanel : NSPanel
@end
@implementation SidescopesBorderPanel

- (BOOL)canBecomeKeyWindow
{
    return YES;
}

@end

@interface SidescopesBorderView : NSView
@property(nonatomic, assign) unsigned dragZone;       // a mask of sidescopes::ZoneBits
@property(nonatomic, assign) NSPoint dragStartMouse;  // global screen coords
@property(nonatomic, assign) NSRect dragStartRegion;  // global screen coords
@property(nonatomic, assign) BOOL closePressed;
// Non-empty for a window-attached region: the tracked application's name,
// worn as a small tab above the band, with the measurement dashes taking a
// warm tint - the subtle tell that this region belongs to a window.
@property(nonatomic, copy) NSString* attachedLabel;
// Extra top strip carrying the attached label, zero when unattached; the
// region math below subtracts it so every zone stays anchored to the band.
@property(nonatomic, assign) CGFloat labelBand;
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

// The whole grab band is the hazard tape, muted so it stays calm beside the
// photograph while its own light-dark alternation keeps it visible on any
// content. The interior is never painted and therefore stays click-through.
- (void)drawHazardBand:(NSRect)region
{
    const NSRect band = NSInsetRect(region, -sidescopes::BorderPad, -sidescopes::BorderPad);
    // The stripes stop short of the measured-edge ring: crossing it would read
    // as the band bleeding into the measured area.
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
    // The attached region's dashes take a warm tint - enough to tell the two
    // region kinds apart at a glance, calm enough to sit beside a photograph.
    if (self.attachedLabel.length > 0) {
        [[NSColor colorWithSRGBRed:1.0 green:0.84 blue:0.55 alpha:0.95] setStroke];
    } else {
        [[NSColor colorWithWhite:0.97 alpha:0.95] setStroke];
    }
    [dashes stroke];
}

// The tracked window's name rides a tab above the band: the attached region
// carries its own identification instead of the main window's toolbar doing
// it at a distance. The label strip the window grew makes room for it clear
// of the handles.
- (void)drawAttachedLabel:(NSRect)region
{
    if (self.attachedLabel.length == 0) {
        return;
    }
    NSDictionary* attributes = @{
        NSFontAttributeName : [NSFont systemFontOfSize:10],
        NSForegroundColorAttributeName : [NSColor colorWithWhite:0.97 alpha:0.95],
    };
    const NSSize textSize = [self.attachedLabel sizeWithAttributes:attributes];
    const NSPoint tabCentre = NSMakePoint(NSMidX(region), NSMaxY(region) + sidescopes::WindowPad + self.labelBand / 2);
    const NSRect tab = NSMakeRect(tabCentre.x - textSize.width / 2 - 6, tabCentre.y - textSize.height / 2 - 2,
                                  textSize.width + 12, textSize.height + 4);
    NSBezierPath* plate = [NSBezierPath bezierPathWithRoundedRect:tab xRadius:4 yRadius:4];
    [[NSColor colorWithWhite:0.1 alpha:0.85] setFill];
    [plate fill];
    [[NSColor colorWithSRGBRed:1.0 green:0.84 blue:0.55 alpha:0.6] setStroke];
    plate.lineWidth = 1.0;
    [plate stroke];
    [self.attachedLabel drawAtPoint:NSMakePoint(tab.origin.x + 6, tab.origin.y + 2) withAttributes:attributes];
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

// The attached-edit spotlight: a click-through veil that dims everything
// outside the tracked window while its border is being dragged, so the
// resize limit is visible - the attached draw's dress, without a picker.
@interface SidescopesEditDimView : NSView
@property(nonatomic, assign) NSRect holeRect;
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

namespace sidescopes {
namespace {

NSWindow* g_editDimWindow = nil;

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

// View coordinates are bottom-left origin; flip the rect's top edge into the
// shared geometry's top-left space, then delegate.
RegionOfInterest regionPercentFromViewRect(NSRect rect, NSSize size)
{
    const LocalRect local{rect.origin.x, flippedY(NSMaxY(rect), size.height), rect.size.width, rect.size.height};

    return regionFromLocalRect(local, size.width, size.height);
}

// The initial tool decides every overlay's mode: a window pick with nothing to
// pick anywhere opens as drawing, and the decision is global so every display
// shows the same mode.
struct PickerModes
{
    bool pin;
    bool draw;
    bool faces;
};

PickerModes computePickerModes(const std::vector<PickerDisplay>& displays, RegionPickerMode initialMode)
{
    bool anyWindows = false;
    for (const PickerDisplay& entry : displays) {
        anyWindows |= !entry.windows.empty();
    }
    const bool pin = initialMode == RegionPickerMode::PinColor;
    const bool draw = !pin && (initialMode == RegionPickerMode::Draw ||
                               (initialMode == RegionPickerMode::PickWindows && !anyWindows));
    const bool faces = initialMode == RegionPickerMode::PickFaces;

    return {pin, draw, faces};
}

// Hands the attached draw's spotlight to the overlay covering its display.
void applyPickConstraint(SidescopesPickerView* view, uint32_t displayId,
                         const std::optional<PickConstraint>& constraint, NSSize viewSize)
{
    if (!constraint || constraint->displayId != displayId) {
        return;
    }
    view.constraintRect = regionToViewRect(constraint->region, viewSize);
    view.constraintLabel = [NSString stringWithUTF8String:constraint->label.c_str()];
}

void addPickerOverlay(const PickerDisplay& entry, PickerModes modes, const std::optional<PickConstraint>& constraint)
{
    NSScreen* screen = nil;
    for (NSScreen* candidate in NSScreen.screens) {
        NSNumber* number = candidate.deviceDescription[@"NSScreenNumber"];
        if (number && number.unsignedIntValue == entry.displayId) {
            screen = candidate;
        }
    }
    if (!screen) {
        return;  // gone between enumeration and now
    }

    SidescopesPickerWindow* overlay = [[SidescopesPickerWindow alloc] initWithContentRect:screen.frame
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
        view->m_windows.emplace_back(regionToViewRect(suggestion.region, viewSize), suggestion.label);
    }
    for (const SuggestedRegion& suggestion : entry.faces) {
        view->m_faces.emplace_back(regionToViewRect(suggestion.region, viewSize), suggestion.label);
    }
    view.drawMode = modes.draw ? YES : NO;
    view.facesMode = modes.faces ? YES : NO;
    view.pinMode = modes.pin ? YES : NO;
    applyPickConstraint(view, entry.displayId, constraint, viewSize);
    if (!modes.draw && !modes.pin) {
        view->m_suggestions = modes.faces ? view->m_faces : view->m_windows;
    }
    overlay.contentView = view;
    overlay.acceptsMouseMovedEvents = YES;
    // Pin mode paints nothing over the screen, and an all-transparent window
    // would be click-through: the explicit assignment - even to NO - switches
    // the window server off its per-pixel transparency hit-testing, so the
    // overlay owns every click. The region border must never do this.
    if (modes.pin) {
        overlay.ignoresMouseEvents = NO;
    }

    g_pickerOverlays.push_back(PickerOverlay{overlay, view, entry.displayId, viewSize});
}

// This application's own visible windows float above the overlays for the
// duration: they stay undimmed and clickable by ordinary window compositing,
// and follow their own movement with no repainting on our side.
void raiseOwnWindows()
{
    for (NSWindow* window in NSApp.windows) {
        if (isPickerOverlayWindow(window) || window == g_borderWindow || !window.isVisible) {
            continue;
        }
        g_raisedWindows.emplace_back(window, window.level);
        window.level = NSStatusWindowLevel + 2;
    }
}

// Force the app frontmost so the overlays own the mouse for the whole
// interaction; the keyboard starts on the display under the cursor and follows
// clicks after.
void presentPickerOverlays()
{
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
}

// The banner dodges this application's own windows; their rectangles refresh on
// a gentle cadence, since nothing visual tracks them per-frame anymore.
void refreshPickerExclusions()
{
    static double lastExclusionRefresh = 0.0;
    const double now = CFAbsoluteTimeGetCurrent();
    if (now - lastExclusionRefresh <= 0.2) {
        return;
    }
    lastExclusionRefresh = now;
    for (PickerOverlay& overlay : g_pickerOverlays) {
        std::vector<NSRect> exclusions = ownWindowExclusions(overlay.window.frame.origin);
        if (exclusions.size() != overlay.view->m_exclusions.size() ||
            !std::equal(exclusions.begin(), exclusions.end(), overlay.view->m_exclusions.begin(),
                        [](const NSRect& a, const NSRect& b) { return NSEqualRects(a, b); })) {
            overlay.view->m_exclusions = std::move(exclusions);
            overlay.view.needsDisplay = YES;
        }
    }
}

// Any overlay finishing - a confirm there, or ESC anywhere - ends the pick on
// every display, tearing every overlay down. @return whether the pick finished.
bool pollPickerFinish(RegionPickPoll& poll)
{
    for (PickerOverlay& overlay : g_pickerOverlays) {
        if (!overlay.view.finished) {
            continue;
        }
        poll.finished = true;
        poll.displayId = overlay.displayId;
        if (overlay.view.picked) {
            poll.confirmed = regionPercentFromViewRect(overlay.view.confirmedRect, overlay.size);
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

        return true;
    }

    return false;
}

void collectPinnedResult(RegionPickPoll& poll)
{
    for (PickerOverlay& overlay : g_pickerOverlays) {
        if (!overlay.view.pinnedReady) {
            continue;
        }
        overlay.view.pinnedReady = NO;
        if (overlay.view.pinnedIsPoint) {
            // The view is bottom-left origin; flip the click's Y into the shared
            // top-left percent space, matching regionPercentFromViewRect.
            const NSPoint click = overlay.view.pinnedPoint;
            poll.pinnedPoint = DisplayPoint{click.x / overlay.size.width * 100.0,
                                            flippedY(click.y, overlay.size.height) / overlay.size.height * 100.0};
        } else {
            poll.pinnedArea = regionPercentFromViewRect(overlay.view.pinnedArea, overlay.size);
        }
        poll.pinnedKeepOpen = overlay.view.pinnedKeepOpen;
        poll.displayId = overlay.displayId;
        break;
    }
}

// The cursor is only ever on one display, so at most one overlay has something
// to preview.
void collectPreview(RegionPickPoll& poll)
{
    for (PickerOverlay& overlay : g_pickerOverlays) {
        if (overlay.view.pinMode) {
            break;  // pin modes never preview a region
        }
        if (!overlay.view.drawMode) {
            const NSInteger hovered = overlay.view.hoveredSuggestion;
            if (hovered >= 0 && hovered < static_cast<NSInteger>(overlay.view->m_suggestions.size())) {
                poll.preview = regionPercentFromViewRect(overlay.view->m_suggestions[hovered].first, overlay.size);
                poll.displayId = overlay.displayId;
                break;
            }
        } else if (overlay.view.dragging) {
            const NSRect selection = [overlay.view selectionRect];
            if (selection.size.width > 8 && selection.size.height > 8) {
                poll.preview = regionPercentFromViewRect(selection, overlay.size);
                poll.displayId = overlay.displayId;
                break;
            }
        }
    }
}

}  // namespace

bool beginRegionPick(const std::vector<PickerDisplay>& displays, RegionPickerMode initialMode,
                     const std::optional<PickConstraint>& constraint)
{
    if (!g_pickerOverlays.empty()) {
        return false;  // one picker at a time
    }

    const PickerModes modes = computePickerModes(displays, initialMode);
    for (const PickerDisplay& entry : displays) {
        addPickerOverlay(entry, modes, constraint);
    }
    if (g_pickerOverlays.empty()) {
        return false;
    }

    raiseOwnWindows();
    for (PickerOverlay& overlay : g_pickerOverlays) {
        overlay.view->m_exclusions = ownWindowExclusions(overlay.window.frame.origin);
    }
    presentPickerOverlays();

    return true;
}

RegionPickPoll pollRegionPick()
{
    RegionPickPoll poll;
    if (g_pickerOverlays.empty()) {
        return poll;
    }
    poll.active = true;
    // Mode flags come first: the finishing poll returns early below, and the
    // caller needs them to know a pin-mode finish never means a region change.
    // The overlays switch modes in lockstep; the front one speaks for all.
    poll.pinMode = g_pickerOverlays.front().view.pinMode;

    refreshPickerExclusions();
    if (pollPickerFinish(poll)) {
        return poll;
    }
    collectPinnedResult(poll);
    collectPreview(poll);

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
        [overlay.view switchToMode:mode];
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

// A NON-ACTIVATING panel: grabbing the border delivers the whole mouse
// sequence without activating this application - the macOS twin of the
// Windows border's WS_EX_NOACTIVATE - while a click still gives the panel
// KEY status, so the region owns the keyboard and the cursor exactly while
// the user works the border.
NSWindow* makeBorderWindow(NSRect rect)
{
    SidescopesBorderPanel* window = [[SidescopesBorderPanel alloc]
        initWithContentRect:rect
                  styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.backgroundColor = NSColor.clearColor;
    window.opaque = NO;
    window.hasShadow = NO;
    // The interior is truly transparent and therefore click-through; only
    // the grab ring and tab take the mouse. ignoresMouseEvents is
    // deliberately never set: an explicit assignment - even to NO - switches
    // AppKit off its per-pixel alpha hit-testing, and the whole window
    // starts swallowing clicks.
    // One level below the scope window: both float above Quick Look, but the
    // border must never cover the scopes. Sharing a level would leave their
    // order to whoever ordered front last.
    window.level = NSStatusWindowLevel - 1;
    window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorStationary;
    window.contentView = [[SidescopesBorderView alloc] initWithFrame:NSZeroRect];
    [window makeFirstResponder:window.contentView];
    // One cursor owner only: with key status possible, AppKit's cursor-rect
    // machinery would re-assert the arrow after every zone cursor the view
    // sets - a visible fight. The view's mouse-move handler is the sole
    // authority.
    [window disableCursorRects];

    return window;
}

void showRegionBorder(uint32_t displayId, const RegionOfInterest& region, const std::string& attachedLabel)
{
    NSScreen* screen = screenForDisplay(displayId);
    const NSRect frame = screen.frame;

    const double left = frame.origin.x + region.leftPercent / 100.0 * frame.size.width;
    const double right = frame.origin.x + region.rightPercent / 100.0 * frame.size.width;
    const double bottom = frame.origin.y + (100.0 - region.bottomPercent) / 100.0 * frame.size.height;
    const double top = frame.origin.y + (100.0 - region.topPercent) / 100.0 * frame.size.height;
    const NSRect rect = NSMakeRect(left - WindowPad, bottom - WindowPad, (right - left) + 2 * WindowPad,
                                   (top - bottom) + 2 * WindowPad);
    NSString* label = attachedLabel.empty() ? @"" : [NSString stringWithUTF8String:attachedLabel.c_str()];
    NSRect labelled = rect;
    if (label.length > 0) {
        labelled.size.height += LabelBand;
    }

    // The host reconciles every frame; an unchanged border must cost nothing.
    if (g_borderWindow && g_borderWindow.visible && NSEqualRects(g_borderWindow.frame, labelled)) {
        SidescopesBorderView* view = (SidescopesBorderView*)g_borderWindow.contentView;
        NSString* current = view.attachedLabel ? view.attachedLabel : @"";
        if ([current isEqualToString:label]) {
            return;
        }
        view.attachedLabel = label;
        view.needsDisplay = YES;

        return;
    }

    if (!g_borderWindow) {
        g_borderWindow = makeBorderWindow(rect);
    }
    SidescopesBorderView* view = (SidescopesBorderView*)g_borderWindow.contentView;
    NSString* current = view.attachedLabel ? view.attachedLabel : @"";
    if (![current isEqualToString:label]) {
        view.attachedLabel = label;
        view.needsDisplay = YES;
    }
    view.labelBand = label.length > 0 ? LabelBand : 0;
    [g_borderWindow setFrame:labelled display:YES];
    [g_borderWindow orderFrontRegardless];
}

void hideRegionBorder()
{
    [g_borderWindow orderOut:nil];
}

std::vector<BorderKeyPress> drainBorderKeyPresses()
{
    std::vector<BorderKeyPress> presses;
    presses.swap(g_borderKeyPresses);

    return presses;
}

void showAttachedEditDim(uint32_t displayId, const RegionOfInterest& windowRegion)
{
    NSScreen* screen = screenForDisplay(displayId);
    const NSRect frame = screen.frame;
    const double left = windowRegion.leftPercent / 100.0 * frame.size.width;
    const double right = windowRegion.rightPercent / 100.0 * frame.size.width;
    const double bottom = (100.0 - windowRegion.bottomPercent) / 100.0 * frame.size.height;
    const double top = (100.0 - windowRegion.topPercent) / 100.0 * frame.size.height;
    const NSRect hole = NSMakeRect(left, bottom, right - left, top - bottom);

    if (!g_editDimWindow) {
        // Non-activating and mouse-transparent: the veil informs, the border
        // and the editor keep every event.
        g_editDimWindow =
            [[NSPanel alloc] initWithContentRect:frame
                                       styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                         backing:NSBackingStoreBuffered
                                           defer:NO];
        g_editDimWindow.backgroundColor = NSColor.clearColor;
        g_editDimWindow.opaque = NO;
        g_editDimWindow.hasShadow = NO;
        g_editDimWindow.ignoresMouseEvents = YES;
        // One level below the border: the veil must never cover the handles.
        g_editDimWindow.level = NSStatusWindowLevel - 2;
        g_editDimWindow.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                             NSWindowCollectionBehaviorFullScreenAuxiliary |
                                             NSWindowCollectionBehaviorStationary;
        g_editDimWindow.contentView = [[SidescopesEditDimView alloc] initWithFrame:NSZeroRect];
    }

    SidescopesEditDimView* view = (SidescopesEditDimView*)g_editDimWindow.contentView;
    if (g_editDimWindow.visible && NSEqualRects(g_editDimWindow.frame, frame) && NSEqualRects(view.holeRect, hole)) {
        return;
    }
    view.holeRect = hole;
    view.needsDisplay = YES;
    [g_editDimWindow setFrame:frame display:YES];
    [g_editDimWindow orderFrontRegardless];
}

void hideAttachedEditDim()
{
    [g_editDimWindow orderOut:nil];
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
