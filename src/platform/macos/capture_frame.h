#pragma once

#import <CoreVideo/CoreVideo.h>

#include <optional>

#include "core/frame.h"

namespace sidescopes {

struct FrameBuffer;
class FrameMailbox;
class PqCaptureDecoder;

/// Validates the delivered encoding, including its color space for HDR.
[[nodiscard]] std::optional<PixelFormat> capturePixelFormat(CVPixelBufferRef image, bool hdrRequested);

// Copies a supported, geometry-checked image into its already stamped frame.
// An unavailable surface or allocation failure drops this delivery. The native
// read lock is always released before return or mailbox publication.
bool deliverCapturePixels(CVPixelBufferRef image, FrameBuffer& buffer, FrameMailbox& mailbox,
                          const PqCaptureDecoder* decoder = nullptr) noexcept;

}  // namespace sidescopes
