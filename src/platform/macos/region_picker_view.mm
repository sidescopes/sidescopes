#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>

#include "platform/face_detection.h"
#include "platform/macos/region_picker_view.h"
#include "platform/region_geometry.h"
#include "platform/region_selection.h"

@implementation SidescopesPickerWindow

- (BOOL)canBecomeKeyWindow
{
    return YES;
}

@end

@implementation SidescopesPickerView

// Face mode is available even when no face was found: the honest answer is
// the empty overlay saying so, not a key that silently does nothing.
- (void)switchToMode:(sidescopes::RegionPickerMode)mode
{
    const BOOL draw = mode == sidescopes::RegionPickerMode::DrawGlobal;
    const BOOL faces = mode == sidescopes::RegionPickerMode::AttachFace;
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
    self.pickDragging = NO;
    self.constraintRect = NSZeroRect;
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
        // Transient indicators wear the warm tone: they are on screen for a
        // moment, and neutral grey vanished against bright content. Only
        // RESTING chrome must stay neutral beside the sampled pixels.
        [[NSColor colorWithSRGBRed:1.0 green:0.84 blue:0.55 alpha:0.95] setStroke];
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

// Attaching to a window or a face: the screen is dimmed and the candidate under
// the cursor washed with the system accent, the way the macOS screenshot
// interface highlights a window.
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
        if (self.facesMode) {
            // A face target is small and easy to miss, so it keeps the
            // accent wash; a hovered window stays natural - what shows is
            // exactly what the scopes get on the click.
            [[[NSColor systemBlueColor] colorWithAlphaComponent:0.25] setFill];
            NSRectFillUsingOperation(hovered.first, NSCompositingOperationSourceOver);
        }
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
        NSString* secondary = @"[A] attach to a window    [D] draw    [Esc] full screen";
        if (!m_suggestions.empty()) {
            [self drawBanner:@"Attach to a face" secondary:secondary preferCenter:NO];
        } else if (self.facesScanned) {
            // Scanned, nothing found: the honest verdict, centered and quiet.
            // Before the scan lands there is no banner - absence is not yet known.
            [self drawBanner:@"No faces found on this screen" secondary:secondary preferCenter:YES];
        }
    } else {
        [self drawBanner:@"Click a window or drag a region inside it"
               secondary:sidescopes::supportsFaceDetection() ? @"[F] attach to a face    [D] draw    [Esc] full screen"
                                                             : @"[D] draw    [Esc] full screen"
            preferCenter:NO];
    }
}

// Freeform drawing: a heavier dim, and the dragged rectangle punched clear.
// A constraint (the attached draw) spotlights the target window instead: a
// hard dim everywhere else, the window itself under the usual light veil,
// rimmed neutrally.
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
        // The settled border's light-dark alternation, so the live drag
        // reads on pure white and pure black alike.
        NSBezierPath* border = [NSBezierPath bezierPathWithRect:selection];
        border.lineWidth = 1.0;
        [[NSColor colorWithWhite:0.1 alpha:0.85] setStroke];
        [border stroke];
        const CGFloat dash[2] = {4.0, 4.0};
        [border setLineDash:dash count:2 phase:0];
        [[NSColor colorWithWhite:0.97 alpha:0.95] setStroke];
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
        secondary = @"[A] attach to a window    [F] attach to a face    [Esc] full screen";
    } else if (!m_windows.empty()) {
        secondary = @"[A] attach to a window    [Esc] full screen";
    }
    [self drawBanner:@"Drag to draw a region" secondary:secondary preferCenter:NO];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    if (self.pinMode) {
        [self drawPinModeOverlay];
    } else if (!self.drawMode && !self.pickDragging) {
        [self drawPickModeOverlay];
    } else {
        [self drawDrawModeOverlay];
    }
}

- (void)resetCursorRects
{
    // Window mode draws as readily as it clicks, so it wears the
    // crosshair too; only face mode is click-only.
    NSCursor* cursor = self.facesMode ? NSCursor.pointingHandCursor : NSCursor.crosshairCursor;
    if (self.pinMode) {
        cursor = sidescopes::g_pinCursor ? sidescopes::g_pinCursor : NSCursor.crosshairCursor;
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
    if (!self.drawMode && !self.pickDragging) {
        [self dragInPickMode:event];
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

// A drag in window mode draws an attached region within the window under
// the drag's start; the spotlight and the clamp follow that window for the
// whole gesture.
- (void)dragInPickMode:(NSEvent*)event
{
    if (self.facesMode) {
        return;
    }
    self.dragCurrent = [self convertPoint:event.locationInWindow fromView:nil];
    if (std::abs(self.dragCurrent.x - self.dragStart.x) <= 4 && std::abs(self.dragCurrent.y - self.dragStart.y) <= 4) {
        return;
    }
    const NSInteger target = [self suggestionAtPoint:self.dragStart];
    self.constraintRect = target >= 0 ? m_suggestions[target].first : NSZeroRect;
    self.constraintLabel = target >= 0 ? [NSString stringWithUTF8String:m_suggestions[target].second.c_str()] : @"";
    self.pickDragging = YES;
    self.dragging = YES;
    self.needsDisplay = YES;
}

// The end of a window-mode drag: a real rectangle confirms as the drawn
// attached region; a stray micro-drag keeps the picker open.
- (void)finishPickDrag:(NSPoint)point
{
    self.dragCurrent = point;
    const NSRect selection = [self selectionRect];
    self.pickDragging = NO;
    self.dragging = NO;
    if (selection.size.width > 8 && selection.size.height > 8) {
        self.picked = YES;
        self.confirmedRect = selection;
        self.finished = YES;
    } else {
        self.constraintRect = NSZeroRect;
        self.needsDisplay = YES;
    }
}

- (void)mouseUp:(NSEvent*)event
{
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    if (self.pinMode) {
        self.dragCurrent = point;
        const NSRect selection = [self selectionRect];
        if (self.dragging && selection.size.width > 8 && selection.size.height > 8) {
            self.pinnedSample = selection;
            self.pinnedIsPoint = NO;
        } else {
            // A plain click pins the point itself, so the pinned color is
            // exactly what the live cursor readout showed; averaging a
            // patch here would fade pins taken over a small subject.
            // Dragging a rectangle is the explicit way to average a swatch.
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
        if (self.pickDragging) {
            [self finishPickDrag:point];
            return;
        }
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
            sidescopes::setRegionPickMode(sidescopes::RegionPickerMode::AttachWindow);
            return;
        }
        if (key == 'd' || key == 'D') {
            sidescopes::setRegionPickMode(sidescopes::RegionPickerMode::DrawGlobal);
            return;
        }
        if (key == 'f' || key == 'F') {
            sidescopes::setRegionPickMode(sidescopes::RegionPickerMode::AttachFace);
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
