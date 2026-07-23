#pragma once

#import <AppKit/AppKit.h>

#include <string>
#include <utility>
#include <vector>

#include "platform/region_selection.h"

namespace sidescopes {

// The picker's live pin cursor, rebuilt by the application as the sampled
// color changes; the picker view draws it in pin mode.
extern NSCursor* g_pinCursor;

}  // namespace sidescopes

// Borderless windows refuse key status unless overridden; the picker needs
// keyDown for ESC-to-cancel.
@interface SidescopesPickerWindow : NSWindow
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
// The attached draw's clamp: an empty rect means unconstrained. Set when a
// drag starts in window mode over a suggestion - the drag cannot leave the
// rect, everything outside it dims hard, and the label names the target.
@property(nonatomic, assign) NSRect constraintRect;
@property(nonatomic, copy) NSString* constraintLabel;
// A drag in window mode: draws an attached region within the window under
// the drag's start instead of confirming a whole window.
@property(nonatomic, assign) BOOL pickDragging;
// In picking mode: whether the face list is active instead of windows.
@property(nonatomic, assign) BOOL facesMode;
// Whether this display's face scan has finished. Until it has, face mode
// stays silent about absence; once set, an empty face list means the honest
// "none found". The streamed display arrives scanned; the others flip it
// through updatePickerFaces when their background scan lands.
@property(nonatomic, assign) BOOL facesScanned;
// Color pinning: a click reports a point to sample, a drag a rectangle
// to average, the region is never touched, and a cursor chip previews
// the sample. pinnedIsPoint says which of the two the pending pin is.
@property(nonatomic, assign) BOOL pinMode;
@property(nonatomic, assign) NSPoint pinnedPoint;
@property(nonatomic, assign) NSRect pinnedSample;
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
- (void)switchToMode:(sidescopes::RegionPickerMode)mode;
- (NSRect)selectionRect;
@end
