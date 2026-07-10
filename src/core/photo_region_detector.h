#pragma once

#include <vector>

#include "core/frame.h"

namespace sidescopes {

// A candidate photo area within a captured frame.
struct RegionCandidate {
    IntRect rect;
    // 0..1: how confident the detector is that this is a photo canvas.
    // Interior richness times border flatness — a real canvas is dense with
    // varied content and sits inside uniform editor chrome.
    float confidence = 0.0f;
};

// Finds likely photo canvases: large contiguous areas of statistically rich
// content (high variance, many distinct colors) surrounded by the flat,
// uniform chrome that photo editors deliberately draw around images.
//
// This is a heuristic and it is used accordingly: candidates power
// hover-and-confirm region suggestions, never silent automation. Known
// limits, by design: photos containing large flat areas shrink their
// candidate; thumbnail grids merge into one large candidate; content that
// fills the frame edge-to-edge offers no chrome border to detect against.
//
// Returns up to `max_candidates`, strongest first. Deterministic.
std::vector<RegionCandidate> DetectPhotoRegions(const FrameView& frame, int max_candidates = 3);

}  // namespace sidescopes
