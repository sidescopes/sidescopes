#pragma once

#include "core/frame.h"
#include "sidescopes/module.h"

namespace sidescopes {

/// The engine-side view of a frame the host handed across the boundary.
///
/// Shared rather than repeated in each module: every scope has to agree about
/// the pixel layout, and four copies of the same field list is how one of them
/// eventually keeps reading a ten-bit frame as bytes.
[[nodiscard]] inline FrameView frameFromBoundary(const SsFrameView& frame)
{
    FrameView view;
    view.pixels = frame.pixels;
    view.strideBytes = frame.stride_bytes;
    view.width = frame.width;
    view.height = frame.height;
    view.colorSpace = frame.color_space == SS_COLOR_SPACE_SRGB ? ColorSpaceHint::Srgb : ColorSpaceHint::Unknown;
    view.sequence = frame.sequence;
    // An unrecognized format reads as the eight-bit one every host before ABI
    // minor 4 sent, which is what a zeroed field means anyway.
    view.format = frame.pixel_format == SS_PIXEL_FORMAT_ARGB2101010 ? PixelFormat::Argb2101010 : PixelFormat::Bgra8;

    return view;
}

}  // namespace sidescopes
