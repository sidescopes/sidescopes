#pragma once

#include <vector>

#include "core/frame.h"

namespace sidescopes {

// The chrome-fenced content area of the dominant window, and the chrome
// that must be masked inside it.
//
// Chrome that spans an edge of its application's main window - side
// panels, a filmstrip, a top bar - DELIMITS the content rather than
// hiding it: nothing continues beneath, so it bounds the analysis. The
// fence starts from the main window (chrome is laid out against the
// window, not the display - a title bar always sits above it) and
// shrinks until no edge-spanning chrome remains inside. Chrome that
// still overlaps the fenced area - a loupe overlay floating on the
// photograph - genuinely hides content and is returned as an occluder
// mask, in fence-relative coordinates.
//
// All rectangles are frame pixels. `slack` absorbs the title bar or a
// status strip sitting between the window edge and the docked chrome.
struct ContentFence {
    IntRect content;
    std::vector<IntRect> occluders;
};

ContentFence FenceContent(const IntRect& frame_bounds, const IntRect& main_window,
                          const std::vector<IntRect>& chrome, int slack);

}  // namespace sidescopes
