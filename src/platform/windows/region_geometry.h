#pragma once

#include "core/analysis_worker.h"  // RegionOfInterest

namespace sidescopes {

// Which edges a border drag adjusts; Move relocates the whole region,
// Close dismisses it. Shared by the geometry helpers below and the border
// window procedure.
enum ZoneBits : unsigned
{
    ZoneNone = 0,
    ZoneLeft = 1u << 0,
    ZoneRight = 1u << 1,
    ZoneTop = 1u << 2,
    ZoneBottom = 1u << 3,
    ZoneMove = 1u << 4,
    ZoneClose = 1u << 5,
};

// A rectangle in overlay-local points. The region math is expressed
// through this toolkit-independent type - not GDI+ RectF or Win32 RECT -
// so it carries no windowing dependency and can be reasoned about and
// unit-tested on its own.
struct LocalRect
{
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

// Overlay-local points to display-relative percentages, and back. The
// percentages let a selection survive capture-resolution changes.
RegionOfInterest regionFromLocalRect(const LocalRect& rect, double width, double height);
LocalRect localRectFromRegion(const RegionOfInterest& region, double width, double height);

// The drag rectangle between two pointer positions, normalized so it reads
// the same whichever way the drag was made.
LocalRect selectionRectFromDrag(double startX, double startY, double currentX, double currentY);

// The grab zone for a point already known to be outside the region.
// cornerZoneAt returns a two-axis corner zone, or ZoneNone when the point
// is not in a corner. edgeOrMoveZoneAt returns a one-axis edge zone at an
// edge's midpoint, or ZoneMove anywhere else on the band.
unsigned cornerZoneAt(const LocalRect& region, double x, double y, double scale);
unsigned edgeOrMoveZoneAt(const LocalRect& region, double x, double y, double scale);

// A drag delta applied to a region, clamped so no dragged edge crosses
// within the minimum of its opposite. Move offsets the whole rectangle; an
// edge or corner zone moves only those sides.
LocalRect draggedRegionRect(unsigned dragZone, const LocalRect& start, double dx, double dy, double minimum);

}  // namespace sidescopes
