#pragma once

#include "core/frame.h"
#include "sidescopes/module.h"

namespace sidescopes {

/// Reject layouts that cannot describe the supplied pixel rows. Zero-sized
/// frames are valid empty input; an unknown format is never guessed as BGRA.
[[nodiscard]] inline bool validBoundaryFrame(const SsFrameView& frame)
{
    const bool knownFormat =
        frame.pixel_format == SS_PIXEL_FORMAT_BGRA8 || frame.pixel_format == SS_PIXEL_FORMAT_ARGB2101010;
    return knownFormat && frame.width >= 0 && frame.height >= 0 &&
           (frame.width == 0 || frame.height == 0 ||
            (frame.pixels && frame.stride_bytes >= static_cast<int64_t>(frame.width) * 4));
}

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
    // Callers validate the format before constructing the engine's view.
    view.format = frame.pixel_format == SS_PIXEL_FORMAT_ARGB2101010 ? PixelFormat::Argb2101010 : PixelFormat::Bgra8;

    return view;
}

}  // namespace sidescopes
