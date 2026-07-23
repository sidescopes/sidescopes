#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// The COM base types GDI+ leans on (IStream, PROPID) are among what
// WIN32_LEAN_AND_MEAN strips out of windows.h.
#include <objidl.h>

// The GDI+ headers also call min and max unqualified, and NOMINMAX
// removed the macros they expect; the standard functions stand in.
#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>

#include <memory>
#include <string>

#include "platform/region_geometry.h"
#include "platform/windows/region_overlay_surface.h"

namespace sidescopes {

// The border window extends this far beyond the region: the grab band,
// wide enough to hit with a cursor, slim enough to stay unobtrusive.
constexpr double BorderPad = 12.0;
// The screenshot-style handle dots protrude past the band's outer edge;
// the window carries a transparent margin so they are not clipped.
constexpr double HandleRadius = 3.5;
// Thickness of the measured-edge ring between the region and the hazard
// stripes. The stripes' cutoff and the ring share this one constant, so
// they abut exactly at every display scale.
constexpr double EdgeRing = 1.0;
constexpr double HandleMargin = HandleRadius + 2.0;
constexpr double WindowPad = BorderPad + HandleMargin;
// Regions cannot shrink beyond this many points per side.
constexpr double MinimumRegionSize = 24.0;
// The hover-revealed close button: a badge on the band's outer corner,
// diagonally off the corner handle, so it visibly belongs to the region
// as a whole. Pulled inward a touch so the disc mostly rides the band;
// tiny regions still yield it to the resize zones.
// Extra window height above the band when the attached label is worn, so
// the name tab clears the handles instead of crowding the top-center one.
constexpr double LabelBand = 20.0;

/// The border's entrance: a short fade with a slight outward settle onto
/// its place, mirroring the macOS constants exactly. Hiding stays instant
/// - a stale border is wrong the moment it is stale. The settle inset is
/// absolute DIPs, capped for tiny regions, so the motion reads the same at
/// every region size.
constexpr double BorderAppearSeconds = 0.12;
constexpr double BorderSettlePoints = 16.0;
constexpr UINT_PTR BorderAppearTimer = 1;
constexpr double CloseRadius = 6.5;
constexpr double CloseHitRadius = 11.0;
constexpr double CloseCornerInset = 2.0;
constexpr double TabAttachZone = 18.0;
constexpr double MinimumWidthForClose = 48.0;

struct BorderState
{
    HWND window = nullptr;
    // The display the region lives on, for percent conversions.
    double displayOriginX = 0.0;
    double displayOriginY = 0.0;
    double displayWidth = 0.0;
    double displayHeight = 0.0;
    // The region in virtual-screen pixels.
    RECT region{};
    unsigned dragZone = ZoneNone;
    POINT dragStartMouse{};
    RECT dragStartRegion{};
    bool closePressed = false;
    bool attachPressed = false;
    // Whether the outlined region is attached: picks the toggle's glyph.
    bool attachedRegion = false;
    // The cached backing store and the geometry it was painted for. The
    // band's look depends on the window's size and scale, never on its
    // position, so a move needs no repaint at all - the common case when
    // the whole region is dragged around.
    std::unique_ptr<LayeredSurface> surface;
    int paintedWidth = 0;
    int paintedHeight = 0;
    double paintedScale = 0.0;
    // Non-empty for a window-attached region: the attached application's
    // name, worn above the band - the tell that this region belongs to a
    // window.
    std::wstring attachedLabel;
    std::wstring paintedLabel;
    // The entrance animation: the rect the border is heading to (region
    // lags it mid-flight), the start tick, and the whole-surface alpha.
    RECT appearTarget{};
    ULONGLONG appearStart = 0;
    bool appearing = false;
    BYTE alpha = 255;
};

extern BorderState g_border;

extern bool g_borderEditing;
extern bool g_borderEditChanged;
extern bool g_borderDismissed;
extern bool g_borderAttachToggled;
extern RegionOfInterest g_borderEditRegion;

void paintBorder();
void beginBorderAppear(double scale);
void snapBorderAppear();
LRESULT CALLBACK borderProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

}  // namespace sidescopes
