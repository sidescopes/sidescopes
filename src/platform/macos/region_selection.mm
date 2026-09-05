#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cmath>

#include "core/diagnostics.h"
#include "platform/desktop.h"
#include "platform/macos/region_border_view.h"
#include "platform/macos/region_picker_view.h"
#include "platform/macos/region_selection_geometry.h"
#include "platform/region_geometry.h"
#include "platform/region_selection.h"

namespace sidescopes {

NSCursor* g_pinCursor = nil;

namespace {

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

// The crosshair's four arms, drawn in one pass at one width and tone; the
// two-tone look comes from a wide dark pass under a narrow light one.
void strokePinCrosshair(CGFloat centerX, CGFloat centerY, CGFloat width, NSColor* tone)
{
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
}

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
    strokePinCrosshair(centerX, centerY, 3.2, [NSColor colorWithWhite:0.1 alpha:0.85]);
    strokePinCrosshair(centerX, centerY, 1.5, [NSColor colorWithWhite:0.8 alpha:0.95]);
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

NSWindow* g_editDimWindow = nil;

NSWindow* g_borderWindow = nil;

/// The border's entrance: a short fade with a slight outward settle onto
/// its place. Hiding stays instant - a stale border is wrong the moment it
/// is stale. The position never tweens - a moved target snaps.
/// g_borderTarget is the rect the border is heading to (the live frame
/// lags it mid-animation). The settle inset is absolute points, capped for
/// tiny regions, so the motion reads the same at every region size.
constexpr double BorderAppearSeconds = 0.12;
constexpr double BorderSettlePoints = 16.0;
NSRect g_borderTarget = {{0, 0}, {0, 0}};
/// The size the border band was last painted at. A snap that only moves the
/// window - the band content is identical - repositions without a redraw; only
/// a size change forces the band to draw again.
NSSize g_borderPaintedSize = {0, 0};

void snapBorderFrame(NSRect labelled)
{
    // The settle onto a moved place, at full opacity: the macOS twin of the
    // Windows present line, now carrying the same repaint flag - a pure move
    // repositions the drawn band, only a resize redraws it.
    const BOOL redraw = !NSEqualSizes(labelled.size, g_borderPaintedSize);
    g_borderPaintedSize = labelled.size;
    SS_DIAG(Border, "present pos=%ld,%ld size=%ldx%ld repaint=%d alpha=255", static_cast<long>(labelled.origin.x),
            static_cast<long>(labelled.origin.y), static_cast<long>(labelled.size.width),
            static_cast<long>(labelled.size.height), redraw ? 1 : 0);
    // A zero-duration group replaces any in-flight entrance animation on
    // BOTH properties. A direct setFrame loses to a running animator frame
    // animation - the window ends at the animation's stale target, and a
    // label strip that arrived meanwhile is clipped off the top, drawing
    // the region a strip short. display: rides the repaint flag: a same-size
    // move keeps the drawn band, so only a resize asks the view to redraw.
    [NSAnimationContext
        runAnimationGroup:^(NSAnimationContext* context) {
          context.duration = 0;
          g_borderWindow.animator.alphaValue = 1.0;
          [g_borderWindow.animator setFrame:labelled display:redraw];
        }
        completionHandler:nil];
}

void animateBorderAppear(NSRect labelled)
{
    g_borderTarget = labelled;
    // The entrance draws the band at its full size; a later same-size snap is
    // then a pure move.
    g_borderPaintedSize = labelled.size;
    // The entrance seam. The Windows border logs one advance line per
    // WM_TIMER tick of the fade; the macOS entrance is Core-Animation-driven
    // with no per-step callback, so the analog is one line at the start,
    // naming the frame the fade heads to and how long it runs.
    SS_DIAG(Border, "advance target=%ld,%ld,%ld,%ld duration=%.3f", static_cast<long>(NSMinX(labelled)),
            static_cast<long>(NSMaxY(labelled)), static_cast<long>(NSMaxX(labelled)),
            static_cast<long>(NSMinY(labelled)), BorderAppearSeconds);
    const double inset = std::min({BorderSettlePoints, labelled.size.width / 6.0, labelled.size.height / 6.0});
    const NSRect start = NSInsetRect(labelled, inset, inset);
    [NSAnimationContext
        runAnimationGroup:^(NSAnimationContext* context) {
          context.duration = 0;
          g_borderWindow.animator.alphaValue = 0.0;
        }
        completionHandler:nil];
    [g_borderWindow setFrame:start display:YES];
    [g_borderWindow orderFrontRegardless];
    [NSAnimationContext
        runAnimationGroup:^(NSAnimationContext* context) {
          context.duration = BorderAppearSeconds;
          context.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];
          [g_borderWindow.animator setFrame:labelled display:YES];
          g_borderWindow.animator.alphaValue = 1.0;
        }
        completionHandler:nil];
}

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

// The initial tool decides every overlay's mode: an attach with nothing to
// attach to anywhere opens as drawing, and the decision is global so every
// display shows the same mode.
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
    const bool draw = !pin && (initialMode == RegionPickerMode::DrawGlobal ||
                               (initialMode == RegionPickerMode::AttachWindow && !anyWindows));
    const bool faces = initialMode == RegionPickerMode::AttachFace;

    return {pin, draw, faces};
}

void addPickerOverlay(const PickerDisplay& entry, PickerModes modes)
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
    view.facesScanned = entry.facesScanned ? YES : NO;
    view.pinMode = modes.pin ? YES : NO;
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

// Ordering the key overlay out leaves this application active with no key
// window at all: AppKit promotes nothing in its place, and every keyboard
// shortcut is dead until the scope window is clicked. Hand the keyboard back
// explicitly, the way the border panel does after a click. The overlays and
// the chrome windows are skipped by name - the border can take key status, and
// it answers Escape differently from the scope window.
void handKeyboardToOwnWindow()
{
    for (NSWindow* candidate in NSApp.orderedWindows) {
        if (candidate == g_borderWindow || candidate == g_editDimWindow || isPickerOverlayWindow(candidate)) {
            continue;
        }
        if (candidate.canBecomeKeyWindow && candidate.visible) {
            [candidate makeKeyWindow];

            return;
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
        // Every dismissal reaches here - Escape, a confirming click or drag, a
        // pinned color, a tool switch - so the keyboard comes back on all of
        // them, not just the one that exposed it.
        handKeyboardToOwnWindow();
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
            poll.pinnedSample = regionPercentFromViewRect(overlay.view.pinnedSample, overlay.size);
        }
        poll.pinnedKeepOpen = overlay.view.pinnedKeepOpen;
        poll.displayId = overlay.displayId;
        break;
    }
}

// A held drag owns the preview; otherwise it follows the cursor's display.
void collectPreview(RegionPickPoll& poll)
{
    const auto dragged = std::find_if(g_pickerOverlays.begin(), g_pickerOverlays.end(),
                                      [](const PickerOverlay& overlay) { return overlay.view.dragging; });
    const auto activeDisplay =
        dragged != g_pickerOverlays.end() ? std::optional<uint32_t>(dragged->displayId) : displayUnderCursor();
    for (PickerOverlay& overlay : g_pickerOverlays) {
        if (overlay.view.pinMode) {
            return;
        }
        if (activeDisplay != overlay.displayId) {
            if (overlay.view.hoveredSuggestion >= 0) {
                overlay.view.hoveredSuggestion = -1;
                overlay.view.needsDisplay = YES;
            }
            continue;
        }
        if (overlay.view.dragging) {
            const NSRect selection = [overlay.view selectionRect];
            if (selection.size.width > 8 && selection.size.height > 8) {
                poll.preview = regionPercentFromViewRect(selection, overlay.size);
                poll.displayId = overlay.displayId;
            }
        } else if (!overlay.view.drawMode) {
            const NSInteger hovered = overlay.view.hoveredSuggestion;
            if (hovered >= 0 && hovered < static_cast<NSInteger>(overlay.view->m_suggestions.size())) {
                poll.preview = regionPercentFromViewRect(overlay.view->m_suggestions[hovered].first, overlay.size);
                poll.displayId = overlay.displayId;
            }
        }
    }
}

}  // namespace

bool beginRegionPick(const std::vector<PickerDisplay>& displays, RegionPickerMode initialMode)
{
    if (!g_pickerOverlays.empty()) {
        return false;  // one picker at a time
    }

    const PickerModes modes = computePickerModes(displays, initialMode);
    for (const PickerDisplay& entry : displays) {
        addPickerOverlay(entry, modes);
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
    SidescopesPickerView* front = g_pickerOverlays.front().view;
    poll.attachesToWindow = !front.pinMode && !front.drawMode && !front.facesMode;

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

void updatePickerFaces(uint32_t displayId, const std::vector<SuggestedRegion>& faces)
{
    for (PickerOverlay& overlay : g_pickerOverlays) {
        if (overlay.displayId != displayId) {
            continue;
        }
        SidescopesPickerView* view = overlay.view;
        view->m_faces.clear();
        for (const SuggestedRegion& suggestion : faces) {
            view->m_faces.emplace_back(regionToViewRect(suggestion.region, overlay.size), suggestion.label);
        }
        // The scan is done for this display now: an empty list is the honest
        // "none found", no longer "not yet scanned".
        view.facesScanned = YES;
        if (view.facesMode) {
            view->m_suggestions = view->m_faces;
            view.hoveredSuggestion = -1;
        }
        view.needsDisplay = YES;

        return;
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

void showRegionBorder(uint32_t displayId, const RegionOfInterest& region, const std::string& label,
                      RegionBinding binding)
{
    NSScreen* screen = screenForDisplay(displayId);
    const NSRect frame = screen.frame;

    const double left = frame.origin.x + region.leftPercent / 100.0 * frame.size.width;
    const double right = frame.origin.x + region.rightPercent / 100.0 * frame.size.width;
    const double bottom = frame.origin.y + (100.0 - region.bottomPercent) / 100.0 * frame.size.height;
    const double top = frame.origin.y + (100.0 - region.topPercent) / 100.0 * frame.size.height;
    const NSRect rect = NSMakeRect(left - WindowPad, bottom - WindowPad, (right - left) + 2 * WindowPad,
                                   (top - bottom) + 2 * WindowPad);
    NSString* borderLabel = label.empty() ? @"" : [NSString stringWithUTF8String:label.c_str()];
    // The strip row above the band is always present: the attached label
    // rides its left, the close and attach buttons its right, and the
    // window's height never changes when a label arrives.
    NSRect labelled = rect;
    labelled.size.height += LabelBand;

    // The host reconciles every frame; an unchanged border must cost
    // nothing. Mid-animation the live frame lags the target, so the
    // comparison is against the target.
    if (g_borderWindow && g_borderWindow.visible && NSEqualRects(g_borderTarget, labelled)) {
        SidescopesBorderView* view = (SidescopesBorderView*)g_borderWindow.contentView;
        NSString* current = view.borderLabel ? view.borderLabel : @"";
        if ([current isEqualToString:borderLabel] && view.regionBinding == binding) {
            return;
        }
        view.borderLabel = borderLabel;
        view.regionBinding = binding;
        view.needsDisplay = YES;

        return;
    }
    // Past the unchanged guard: a real appear or move. The macOS entrance
    // carries no persistent appearing flag, so the Windows appearing field
    // has no analog here.
    SS_DIAG(Border, "show wanted=%ld,%ld,%ld,%ld visible=%d", static_cast<long>(left), static_cast<long>(top),
            static_cast<long>(right), static_cast<long>(bottom), g_borderWindow.visible ? 1 : 0);

    if (!g_borderWindow) {
        g_borderWindow = makeBorderWindow(rect);
    }
    SidescopesBorderView* view = (SidescopesBorderView*)g_borderWindow.contentView;
    NSString* current = view.borderLabel ? view.borderLabel : @"";
    if (![current isEqualToString:borderLabel]) {
        view.borderLabel = borderLabel;
        view.needsDisplay = YES;
    }
    view.labelBand = LabelBand;
    view.regionBinding = binding;
    if (g_borderWindow.visible) {
        // Already shown at another place: snap, never tween position.
        snapBorderFrame(labelled);
        g_borderTarget = labelled;

        return;
    }
    animateBorderAppear(labelled);
}

void hideRegionBorder()
{
    // Guarded on the window like the Windows side, so a down border does not
    // log before one has ever been built. The macOS entrance carries no
    // persistent appearing flag, so that Windows field has no analog here.
    if (g_borderWindow) {
        SS_DIAG(Border, "hide visible=%d", g_borderWindow.visible ? 1 : 0);
    }
    // A click on the band gives the panel key status, so taking the border
    // down can strand the keyboard the same way a finished pick does. Only a
    // panel that actually held it hands it back: the host reconciles to a
    // hidden border every frame, and an unconditional hand-back would pull the
    // keyboard off an open picker overlay thirty times a second.
    const BOOL borderHeldKeyboard = g_borderWindow.keyWindow;
    [g_borderWindow orderOut:nil];
    if (borderHeldKeyboard) {
        handKeyboardToOwnWindow();
    }
    g_borderTarget = NSMakeRect(0, 0, 0, 0);
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
    edit.bindingToggled = g_borderBindingToggled;
    g_borderBindingToggled = false;
    if (g_borderEditChanged) {
        edit.region = g_borderEditRegion;
        g_borderEditChanged = false;
    }
    return edit;
}

}  // namespace sidescopes
