#pragma once

#import <AppKit/AppKit.h>

#include <vector>

#include "platform/region_selection.h"

namespace sidescopes {

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
constexpr double TabAttachZone = 18.0;
constexpr double MinimumWidthForClose = 48.0;

// Shared edit state the application polls once per frame.
extern std::vector<BorderKeyPress> g_borderKeyPresses;
extern bool g_borderEditing;
extern bool g_borderEditChanged;
extern bool g_borderDismissed;
extern bool g_borderAttachToggled;
extern RegionOfInterest g_borderEditRegion;

}  // namespace sidescopes

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

@interface SidescopesBorderView : NSView
@property(nonatomic, assign) unsigned dragZone;       // a mask of sidescopes::ZoneBits
@property(nonatomic, assign) NSPoint dragStartMouse;  // global screen coords
@property(nonatomic, assign) NSRect dragStartRegion;  // global screen coords
@property(nonatomic, assign) BOOL closePressed;
@property(nonatomic, assign) BOOL attachPressed;
// Whether the outlined region is attached: picks the toggle's glyph.
@property(nonatomic, assign) BOOL attachedRegion;
// Non-empty for a window-attached region: the attached application's name,
// worn as a small tab above the band, with the measurement dashes taking a
// label being the tell that this region belongs to a window.
@property(nonatomic, copy) NSString* attachedLabel;
// Extra top strip carrying the attached label, zero when unattached; the
// region math below subtracts it so every zone stays anchored to the band.
@property(nonatomic, assign) CGFloat labelBand;
@end

// The attached-edit spotlight: a click-through veil that dims everything
// outside the attached window while its border is being dragged, so the
// resize limit is visible - the attached draw's dress, without a picker.
@interface SidescopesEditDimView : NSView
@property(nonatomic, assign) NSRect holeRect;
@end
