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

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "platform/windows/region_overlay_surface.h"

namespace sidescopes {

struct PickerState
{
    HWND window = nullptr;
    uint32_t displayId = 0;
    int originX = 0;  // the covered monitor, virtual-screen pixels
    int originY = 0;
    int width = 0;
    int height = 0;
    bool drawMode = false;
    bool facesMode = false;
    // Whether this display's face scan has finished. Until it has, face mode
    // stays silent about absence; once set, an empty face list means the
    // honest "none found". The streamed display arrives scanned; the others
    // flip it through updatePickerFaces when their background scan lands.
    bool facesScanned = false;
    // The attached draw's clamp: set when a drag starts in window mode
    // over a suggestion - the drag cannot leave constraintRect
    // (overlay-local pixels), everything outside it dims hard, and the
    // label names the target window's application.
    bool constrained = false;
    Gdiplus::RectF constraintRect{};
    std::wstring constraintLabel;
    // A drag in window mode: draws an attached region within the window
    // under the drag's start instead of confirming a whole window.
    bool pickDragging = false;
    // Color pinning: a click reports a point to sample, a drag a rectangle
    // to average, the region is never touched, and a cursor chip previews
    // the sample.
    bool pinMode = false;
    // The pending pin, in overlay-local pixels, left here until the next
    // poll collects it - with the click's Shift state, the per-pin choice
    // to keep picking. pinnedIsPoint says whether pinnedPoint (a click) or
    // pinnedSample (a drag) holds it.
    Gdiplus::PointF pinnedPoint{};
    Gdiplus::RectF pinnedSample{};
    bool pinnedIsPoint = false;
    bool pinnedKeepOpen = false;
    bool pinnedReady = false;
    // The active suggestion list in overlay-local pixels, with labels -
    // the windows or the faces, depending on the mode.
    std::vector<std::pair<Gdiplus::RectF, std::wstring>> windows;
    std::vector<std::pair<Gdiplus::RectF, std::wstring>> faces;
    std::vector<std::pair<Gdiplus::RectF, std::wstring>> suggestions;
    // This application's own windows, local pixels: they float above the
    // overlay, and the banner avoids sitting beneath them.
    std::vector<Gdiplus::RectF> exclusions;
    int hoveredSuggestion = -1;
    bool dragging = false;
    POINT dragStart{};
    POINT dragCurrent{};
    bool picked = false;
    bool finished = false;
    Gdiplus::RectF confirmedRect{};
    // The cached backing store, plus the selection rectangle it last
    // showed: successive drag repaints touch only the union of the two.
    std::unique_ptr<LayeredSurface> surface;
    Gdiplus::RectF paintedSelection{};
    bool selectionPainted = false;
};

extern std::vector<PickerState*> g_pickers;

// The pin cursor, built and owned by region_selection.cpp beside its
// builder; the picker's set-cursor handler only reads it.
extern HCURSOR g_pinCursor;

PickerState* pickerForWindow(HWND window);
std::vector<HWND> ownWindows();
std::vector<Gdiplus::RectF> ownWindowExclusions(const PickerState& picker);
Gdiplus::RectF selectionRect(const PickerState& picker);
void switchPickerMode(int mode);
void paintPicker(PickerState& picker, const Gdiplus::RectF* dirty = nullptr);
LRESULT CALLBACK pickerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

}  // namespace sidescopes
